// tests for crone/src/MultiTape: chunked RAM tape, record/playback, jobs

#include <cmath>
#include <cstdio>
#include <doctest/doctest.h>
#include <string>
#include <unistd.h>
#include <vector>

#include <sndfile.h>

#include "MultiTape.h"

using namespace crone;

namespace {

constexpr size_t Block = 64;
constexpr float SR = 48000.f;

// drives a MultiTape like the mixer does, with a constant record source
struct Rig {
    MultiTape tape;
    std::vector<float> inL, inR, outL, outR;

    explicit Rig(int channels = 1) : inL(Block, 0.f), inR(Block, 0.f), outL(Block), outR(Block) {
        tape.setMemProbe([] { return size_t(1) << 40; });
        tape.init(SR, channels, false);
    }

    void source(float l, float r) {
        std::fill(inL.begin(), inL.end(), l);
        std::fill(inR.begin(), inR.end(), r);
    }

    void run(size_t blocks) {
        for (size_t b = 0; b < blocks; ++b) {
            std::fill(outL.begin(), outL.end(), 0.f);
            std::fill(outR.begin(), outR.end(), 0.f);
            const float *src[2] = {inL.data(), inR.data()};
            float *out[2] = {outL.data(), outR.data()};
            tape.process(src, out, Block);
        }
    }

    // stop and let the fade-out finish
    void stop() {
        tape.setPlaying(false);
        run(16);
        REQUIRE_FALSE(tape.getStatus().playing);
    }
};

std::string tmpPath(const char *name) {
    char buf[256];
    snprintf(buf, sizeof(buf), "/tmp/multitape-test-%d-%s", static_cast<int>(getpid()), name);
    return buf;
}

} // namespace

TEST_CASE("multitape geometry and initial state") {
    Rig r;
    auto s = r.tape.getStatus();
    CHECK(s.channels == 1);
    CHECK(s.maxFrames == static_cast<size_t>(360 * SR));
    CHECK_FALSE(s.playing);
    CHECK(s.armed == -1);
    CHECK(s.allocatedBytes == 0);
    for (auto e : s.endFrame) {
        CHECK(e == 0);
    }
}

TEST_CASE("multitape stopped transport does not move or output") {
    Rig r;
    r.source(0.5f, 0.5f);
    r.run(4);
    CHECK(r.tape.getStatus().posFrame == 0);
    CHECK(r.outL[10] == 0.f);
}

TEST_CASE("multitape empty tape plays silence and advances") {
    Rig r;
    r.tape.setPlaying(true);
    r.run(10);
    CHECK(r.tape.getStatus().posFrame == 10 * Block);
    CHECK(r.outL[Block - 1] == 0.f);
    CHECK(r.tape.getStatus().allocatedBytes == 0);
}

TEST_CASE("multitape mono record and playback round trip") {
    Rig r;
    r.tape.setArmed(0);
    r.tape.service(); // allocate chunks ahead of the record head
    CHECK(r.tape.getStatus().allocatedBytes > 0);

    r.source(0.5f, 0.5f);
    r.tape.setRecord(true);
    r.tape.setPlaying(true);
    r.run(100);
    CHECK(r.tape.getStatus().recording);
    r.stop();

    const size_t end = r.tape.getStatus().endFrame[0];
    CHECK(end >= 99 * Block);
    // past the punch-in crossfade the take is the source
    CHECK(r.tape.peekSample(0, 1000, 0) == doctest::Approx(0.5f).epsilon(0.001));
    // the first frame is still inside the fade-in
    CHECK(r.tape.peekSample(0, 0, 0) < 0.01f);
    // other tracks untouched
    CHECK(r.tape.peekSample(1, 1000, 0) == 0.f);

    // play it back disarmed: centre pan on a mono track is equal power
    r.tape.setRecord(false);
    r.tape.setArmed(-1);
    r.tape.seek(0);
    r.source(0.f, 0.f);
    r.tape.setPlaying(true);
    r.run(20);
    const float expect = 0.5f * std::cos(static_cast<float>(M_PI) / 4.f);
    CHECK(r.outL[10] == doctest::Approx(expect).epsilon(0.01));
    CHECK(r.outR[10] == doctest::Approx(expect).epsilon(0.01));
}

TEST_CASE("multitape level, mute and pan shape playback") {
    Rig r;
    r.tape.setArmed(2);
    r.tape.service();
    r.source(0.5f, 0.5f);
    r.tape.setRecord(true);
    r.tape.setPlaying(true);
    r.run(50);
    r.stop();
    r.tape.setRecord(false);
    r.tape.setArmed(-1);

    r.tape.setTrackPan(2, -1.f);
    r.tape.seek(0);
    r.tape.setPlaying(true);
    r.run(20);
    CHECK(r.outL[Block - 1] == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(std::fabs(r.outR[Block - 1]) < 1e-4f);

    r.tape.setTrackLevel(2, 0.5f);
    r.run(2);
    CHECK(r.outL[Block - 1] == doctest::Approx(0.25f).epsilon(0.01));

    r.tape.setTrackMute(2, true);
    r.run(2);
    CHECK(std::fabs(r.outL[Block - 1]) < 1e-4f);
}

TEST_CASE("multitape stereo tracks keep channels apart") {
    Rig r(2);
    CHECK(r.tape.getStatus().channels == 2);
    r.tape.setArmed(1);
    r.tape.service();
    r.source(0.25f, -0.5f);
    r.tape.setRecord(true);
    r.tape.setPlaying(true);
    r.run(40);
    r.stop();
    CHECK(r.tape.peekSample(1, 1000, 0) == doctest::Approx(0.25f).epsilon(0.001));
    CHECK(r.tape.peekSample(1, 1000, 1) == doctest::Approx(-0.5f).epsilon(0.001));
}

TEST_CASE("multitape record offset compensates latency") {
    Rig r;
    r.tape.setSource(MultiTape::SourceEngine);
    r.tape.setLatency(1000, 256); // capture latency ignored for engine source
    r.tape.seek(10000);
    r.tape.setArmed(0);
    r.tape.service();
    r.source(0.5f, 0.5f);
    r.tape.setRecord(true);
    r.tape.setPlaying(true);
    r.run(40);
    r.stop();
    // the write head trails the playhead by the playback latency
    const size_t first = 10000 - 256;
    CHECK(r.tape.peekSample(0, first - 1, 0) == 0.f);
    CHECK(r.tape.peekSample(0, first, 0) > 0.f);
    CHECK(r.tape.peekSample(0, first + 300, 0) == doctest::Approx(0.5f).epsilon(0.001));
    const auto s = r.tape.getStatus();
    CHECK(s.endFrame[0] <= s.posFrame - 256);
    CHECK(s.endFrame[0] + 4 > s.posFrame - 256);
}

TEST_CASE("multitape drops record blocks without chunks instead of allocating") {
    Rig r;
    r.tape.setArmed(0); // no service(): nothing allocated
    r.source(0.5f, 0.5f);
    r.tape.setRecord(true);
    r.tape.setPlaying(true);
    r.run(10);
    CHECK(r.tape.getStatus().droppedBlocks > 0);
    CHECK(r.tape.getStatus().allocatedBytes == 0);
    CHECK(r.tape.getStatus().endFrame[0] == 0);
}

TEST_CASE("multitape memory guard refuses allocation") {
    Rig r;
    r.tape.setMemProbe([] { return size_t(10) << 20; });
    r.tape.setArmed(0);
    r.tape.service();
    auto s = r.tape.getStatus();
    CHECK(s.memFull);
    CHECK(s.allocatedBytes == 0);
}

TEST_CASE("multitape overview shows recorded region only") {
    Rig r;
    r.tape.setArmed(0);
    r.tape.service();
    r.source(0.5f, 0.5f);
    r.tape.setRecord(true);
    r.tape.setPlaying(true);
    r.run(160); // 10240 frames
    r.stop();
    uint8_t bins[4];
    r.tape.overview(0, 0, 4096, 4, bins);
    CHECK(bins[0] > 100);
    CHECK(bins[1] > 100);
    CHECK(bins[3] == 0);
    r.tape.overview(1, 0, 4096, 4, bins);
    CHECK(bins[0] == 0);
}

TEST_CASE("multitape clear frees memory and disarm reclaims empty chunks") {
    Rig r;
    r.tape.setArmed(0);
    r.tape.service();
    r.source(0.5f, 0.5f);
    r.tape.setRecord(true);
    r.tape.setPlaying(true);
    r.run(20);
    r.stop();
    r.tape.setRecord(false);
    r.tape.setArmed(-1);
    r.tape.service(); // disarm: unrecorded lookahead chunks are retired
    r.run(2);         // let the audio epoch advance
    r.tape.service();
    // only the chunk holding the take remains
    CHECK(r.tape.getStatus().allocatedBytes == MultiTape::ChunkFrames * sizeof(int16_t));

    r.tape.requestClear(0);
    r.tape.flush();
    r.run(2);
    r.tape.service();
    auto s = r.tape.getStatus();
    CHECK(s.endFrame[0] == 0);
    CHECK(s.allocatedBytes == 0);
    CHECK(r.tape.peekSample(0, 1000, 0) == 0.f);
}

TEST_CASE("multitape seek while playing jumps after the fade") {
    Rig r;
    r.tape.setPlaying(true);
    r.run(4);
    r.tape.seek(100000);
    r.run(20);
    auto p = r.tape.getStatus().posFrame;
    CHECK(p > 100000);
    CHECK(p < 100000 + 20 * Block);
}

TEST_CASE("multitape stops at the end of the tape") {
    Rig r;
    const size_t maxFrames = r.tape.getStatus().maxFrames;
    r.tape.seek(maxFrames - 100);
    r.tape.setPlaying(true);
    r.run(20);
    auto s = r.tape.getStatus();
    CHECK_FALSE(s.playing);
    CHECK(s.posFrame == maxFrames - 1);
}

TEST_CASE("multitape loads a file onto a track and saves it aligned") {
    const std::string in = tmpPath("in.wav");
    const size_t len = 50000;
    {
        SF_INFO info{};
        info.samplerate = 48000;
        info.channels = 2;
        info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        SNDFILE *f = sf_open(in.c_str(), SFM_WRITE, &info);
        REQUIRE(f != nullptr);
        std::vector<float> data(len * 2);
        for (size_t i = 0; i < len; ++i) {
            data[2 * i] = 0.25f;
            data[2 * i + 1] = 0.75f;
        }
        sf_writef_float(f, data.data(), len);
        sf_close(f);
    }

    Rig r;
    const size_t start = 40000; // straddles a chunk boundary
    r.tape.requestLoad(1, in, start);
    r.tape.flush();
    auto s = r.tape.getStatus();
    CHECK(s.endFrame[1] == start + len);
    CHECK(r.tape.peekSample(1, start - 1, 0) == 0.f);
    // stereo file averaged into a mono track
    CHECK(r.tape.peekSample(1, start, 0) == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(r.tape.peekSample(1, start + len - 1, 0) == doctest::Approx(0.5f).epsilon(0.001));

    // loading into an armed track is refused
    r.tape.setArmed(2);
    r.tape.requestLoad(2, in, 0);
    r.tape.flush();
    CHECK(r.tape.getStatus().endFrame[2] == 0);

    const std::string dir = tmpPath("save");
    r.tape.requestSave(dir);
    r.tape.flush();
    SF_INFO info{};
    SNDFILE *f = sf_open((dir + "/track2.wav").c_str(), SFM_READ, &info);
    REQUIRE(f != nullptr);
    CHECK(info.channels == 1);
    CHECK(info.frames == static_cast<sf_count_t>(start + len));
    std::vector<float> back(static_cast<size_t>(info.frames));
    sf_readf_float(f, back.data(), info.frames);
    sf_close(f);
    CHECK(back[start - 1] == 0.f);
    CHECK(back[start + 10] == doctest::Approx(0.5f).epsilon(0.001));
    // empty tracks are not written
    CHECK(access((dir + "/track1.wav").c_str(), F_OK) != 0);

    std::remove((dir + "/track2.wav").c_str());
    rmdir(dir.c_str());
    std::remove(in.c_str());
}

TEST_CASE("multitape worker thread services jobs") {
    MultiTape tape;
    tape.setMemProbe([] { return size_t(1) << 40; });
    tape.init(SR, 1, true);
    tape.requestClear(0);
    tape.flush();
    CHECK(tape.getStatus().jobsPending == 0);
}

TEST_CASE("multitape default channel count honours the env override") {
    setenv("NORNS_MULTITAPE_CHANNELS", "2", 1);
    CHECK(MultiTape::defaultChannels() == 2);
    setenv("NORNS_MULTITAPE_CHANNELS", "1", 1);
    CHECK(MultiTape::defaultChannels() == 1);
    unsetenv("NORNS_MULTITAPE_CHANNELS");
}
