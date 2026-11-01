//
// MultiTape: a RAM-resident 4-track tape. see MultiTape.h
//

#include "MultiTape.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include <sndfile.h>
#include <sys/mman.h>
#include <sys/stat.h>

using namespace crone;

// out-of-class definitions for odr-used constants (c++14)
constexpr int MultiTape::NumTracks;
constexpr size_t MultiTape::ChunkShift;
constexpr size_t MultiTape::ChunkFrames;
constexpr size_t MultiTape::PeakShift;
constexpr size_t MultiTape::PeakFrames;
constexpr size_t MultiTape::PeaksPerChunk;
constexpr float MultiTape::MaxSeconds;
constexpr size_t MultiTape::DefaultMemReserve;

namespace {

// value of a "<key>: <n> kB" line in /proc/meminfo, in bytes; 0 if missing
size_t readMemInfo(const char *key) {
    std::ifstream f("/proc/meminfo");
    if (!f) {
        return 0;
    }
    std::string line;
    const size_t keyLen = strlen(key);
    while (std::getline(f, line)) {
        if (line.compare(0, keyLen, key) == 0 && line.size() > keyLen && line[keyLen] == ':') {
            return static_cast<size_t>(std::strtoull(line.c_str() + keyLen + 1, nullptr, 10)) * 1024;
        }
    }
    return 0;
}

inline uint8_t peakByte(int s) {
    // ceil, so any non-zero sample marks the block as having content
    int a = s < 0 ? -s : s;
    int v = (a + 127) >> 7;
    return static_cast<uint8_t>(v > 255 ? 255 : v);
}

} // namespace

MultiTape::MultiTape() {
    memProbe = []() {
        size_t avail = readMemInfo("MemAvailable");
        return avail > 0 ? avail : SIZE_MAX;
    };
}

MultiTape::~MultiTape() {
    if (worker) {
        quit = true;
        jobCv.notify_all();
        worker->join();
    }
    reclaimRetired(true);
    for (auto &t : tracks) {
        if (!t.chunks) {
            continue;
        }
        for (size_t c = 0; c < numChunks; ++c) {
            freeChunk(t.chunks[c].exchange(nullptr));
        }
    }
}

int MultiTape::defaultChannels() {
    if (const char *env = std::getenv("NORNS_MULTITAPE_CHANNELS")) {
        int n = std::atoi(env);
        if (n == 1 || n == 2) {
            return n;
        }
    }
    size_t total = readMemInfo("MemTotal");
    if (total == 0) {
        return 1;
    }
    return total < (size_t(3) << 29) ? 1 : 2; // 1.5 GiB
}

void MultiTape::setMemProbe(MemProbe p) {
    memProbe = std::move(p);
}

void MultiTape::init(float sampleRate, int channels, bool startWorker) {
    if (numChunks > 0) {
        // already initialized; geometry can't change under existing data
        return;
    }
    sr = sampleRate;
    chans = channels == 2 ? 2 : 1;
    maxFrames = static_cast<size_t>(MaxSeconds * sr);
    numChunks = (maxFrames + ChunkFrames - 1) >> ChunkShift;
    numPeaks = numChunks * PeaksPerChunk;
    chunkSamples = ChunkFrames * chans;
    fadeFrames = std::max<size_t>(1, static_cast<size_t>(0.005f * sr));

    for (auto &t : tracks) {
        t.chunks.reset(new std::atomic<Sample *>[numChunks]);
        for (size_t c = 0; c < numChunks; ++c) {
            t.chunks[c].store(nullptr, std::memory_order_relaxed);
        }
        t.peaks.reset(new std::atomic<uint8_t>[numPeaks]);
        for (size_t i = 0; i < numPeaks; ++i) {
            t.peaks[i].store(0, std::memory_order_relaxed);
        }
        t.endFrame = 0;
    }
    std::cout << "multitape: " << NumTracks << " tracks, " << chans << " ch, "
              << MaxSeconds << " s max" << std::endl;

    if (startWorker) {
        worker = std::make_unique<std::thread>([this] { workLoop(); });
    }
}

//------------------------------------------------------------------------------
//--- audio thread

void MultiTape::setPlaying(bool play) {
    if (play) {
        if (!playing.load(std::memory_order_relaxed)) {
            transportGain = 0.f;
            playing.store(true, std::memory_order_relaxed);
        }
        stopPending = false;
    } else if (playing.load(std::memory_order_relaxed)) {
        stopPending = true;
    }
}

void MultiTape::seek(size_t frame) {
    if (maxFrames == 0) {
        return;
    }
    frame = std::min(frame, maxFrames - 1);
    if (playing.load(std::memory_order_relaxed)) {
        seekPending = true;
        seekTarget = frame;
    } else {
        pos.store(frame, std::memory_order_relaxed);
    }
}

void MultiTape::setArmed(int track) {
    armed.store((track >= 0 && track < NumTracks) ? track : -1, std::memory_order_relaxed);
}

void MultiTape::setRecord(bool rec) {
    recEnabled.store(rec, std::memory_order_relaxed);
}

void MultiTape::setTrackLevel(int track, float amp) {
    if (track >= 0 && track < NumTracks) {
        tracks[track].level = std::max(0.f, amp);
    }
}

void MultiTape::setTrackPan(int track, float pan) {
    if (track >= 0 && track < NumTracks) {
        tracks[track].pan = std::max(-1.f, std::min(1.f, pan));
    }
}

void MultiTape::setTrackMute(int track, bool mute) {
    if (track >= 0 && track < NumTracks) {
        tracks[track].mute = mute;
    }
}

void MultiTape::setSource(int src) {
    if (src >= 0 && src < NumSources) {
        source.store(src, std::memory_order_relaxed);
    }
}

size_t MultiTape::recOffset() const {
    size_t off = latPlayback.load(std::memory_order_relaxed);
    if (source.load(std::memory_order_relaxed) == SourceInput) {
        off += latCapture.load(std::memory_order_relaxed);
    }
    return off;
}

void MultiTape::process(const float *const src[2], float *const out[2], size_t numFrames) {
    // per-block target gains (level, mute, pan); interpolated across the block
    float targetL[NumTracks];
    float targetR[NumTracks];
    for (int t = 0; t < NumTracks; ++t) {
        Track &tr = tracks[t];
        float g = tr.mute ? 0.f : tr.level;
        if (chans == 1) {
            float angle = (tr.pan + 1.f) * 0.25f * static_cast<float>(M_PI);
            targetL[t] = g * std::cos(angle);
            targetR[t] = g * std::sin(angle);
        } else {
            targetL[t] = g * (tr.pan > 0.f ? 1.f - tr.pan : 1.f);
            targetR[t] = g * (tr.pan < 0.f ? 1.f + tr.pan : 1.f);
        }
    }

    if (!playing.load(std::memory_order_relaxed) || numChunks == 0) {
        for (int t = 0; t < NumTracks; ++t) {
            tracks[t].gainL = targetL[t];
            tracks[t].gainR = targetR[t];
        }
        blockCount.fetch_add(1, std::memory_order_release);
        return;
    }

    const float rampInc = 1.f / static_cast<float>(fadeFrames);
    const float blockInc = 1.f / static_cast<float>(numFrames);
    const int arm = armed.load(std::memory_order_relaxed);
    const bool rec = recEnabled.load(std::memory_order_relaxed);
    const size_t offset = recOffset();
    int rt = recTrack.load(std::memory_order_relaxed);
    size_t p = pos.load(std::memory_order_relaxed);
    size_t end = rt >= 0 ? tracks[rt].endFrame.load(std::memory_order_relaxed) : 0;
    bool dropped = false;

    for (size_t i = 0; i < numFrames; ++i) {
        const bool active = !stopPending && !seekPending;

        // transport fade; jumps and stops happen at zero gain
        if (active) {
            transportGain = std::min(1.f, transportGain + rampInc);
        } else {
            transportGain = std::max(0.f, transportGain - rampInc);
            if (transportGain <= 0.f && recGain <= 0.f) {
                if (seekPending) {
                    p = seekTarget;
                    seekPending = false;
                }
                if (stopPending) {
                    stopPending = false;
                    // release the record track so the worker may load/clear/reclaim it
                    flushRecord(rt, end);
                    rt = -1;
                    recTrack.store(-1, std::memory_order_release);
                    peakBlock = SIZE_MAX;
                    playing.store(false, std::memory_order_relaxed);
                    break;
                }
            }
        }

        // record gain; the record track only changes at zero gain
        const bool wantRec = active && rec && arm >= 0;
        if (recGain <= 0.f) {
            int next = wantRec ? arm : -1;
            if (next != rt) {
                flushRecord(rt, end);
                rt = next;
                recTrack.store(rt, std::memory_order_release);
                peakBlock = SIZE_MAX;
                end = rt >= 0 ? tracks[rt].endFrame.load(std::memory_order_relaxed) : 0;
            }
        }
        const float recTarget = (wantRec && arm == rt) ? 1.f : 0.f;
        if (recGain < recTarget) {
            recGain = std::min(recTarget, recGain + rampInc);
        } else if (recGain > recTarget) {
            recGain = std::max(recTarget, recGain - rampInc);
        }

        // playback
        const size_t c = p >> ChunkShift;
        const size_t o = p & (ChunkFrames - 1);
        const float x = static_cast<float>(i) * blockInc;
        float sumL = 0.f;
        float sumR = 0.f;
        for (int t = 0; t < NumTracks; ++t) {
            const Sample *data = tracks[t].chunks[c].load(std::memory_order_acquire);
            if (data == nullptr) {
                continue;
            }
            Track &tr = tracks[t];
            float gl = tr.gainL + (targetL[t] - tr.gainL) * x;
            float gr = tr.gainR + (targetR[t] - tr.gainR) * x;
            if (t == rt) {
                gl *= 1.f - recGain;
                gr *= 1.f - recGain;
            }
            if (chans == 1) {
                float v = toFloat(data[o]);
                sumL += v * gl;
                sumR += v * gr;
            } else {
                sumL += toFloat(data[2 * o]) * gl;
                sumR += toFloat(data[2 * o + 1]) * gr;
            }
        }
        out[0][i] += sumL * transportGain;
        out[1][i] += sumR * transportGain;

        // record: overwrite at the latency-compensated write head
        if (recGain > 0.f && rt >= 0 && p >= offset) {
            const size_t w = p - offset;
            Sample *data = tracks[rt].chunks[w >> ChunkShift].load(std::memory_order_acquire);
            if (data == nullptr) {
                dropped = true;
            } else {
                const size_t wo = w & (ChunkFrames - 1);
                int peak;
                if (chans == 1) {
                    Sample &s = data[wo];
                    float in = 0.5f * (src[0][i] + src[1][i]);
                    float old = toFloat(s);
                    s = toSample(old + (in - old) * recGain);
                    peak = s;
                } else {
                    Sample &sl = data[2 * wo];
                    Sample &sr_ = data[2 * wo + 1];
                    float oldL = toFloat(sl);
                    float oldR = toFloat(sr_);
                    sl = toSample(oldL + (src[0][i] - oldL) * recGain);
                    sr_ = toSample(oldR + (src[1][i] - oldR) * recGain);
                    peak = std::max(std::abs(static_cast<int>(sl)), std::abs(static_cast<int>(sr_)));
                }
                const size_t b = w >> PeakShift;
                if (b != peakBlock) {
                    if (peakBlock != SIZE_MAX) {
                        tracks[rt].peaks[peakBlock].store(static_cast<uint8_t>(peakAcc), std::memory_order_relaxed);
                    }
                    peakBlock = b;
                    // a block entered mid-way keeps the peak of what's already there
                    peakAcc = (w & (PeakFrames - 1)) == 0 ? 0 : tracks[rt].peaks[b].load(std::memory_order_relaxed);
                }
                peakAcc = std::max<uint32_t>(peakAcc, peakByte(peak));
                if (w + 1 > end) {
                    end = w + 1;
                }
            }
        }

        if (++p >= maxFrames) {
            p = maxFrames - 1;
            stopPending = true;
        }
    }

    for (int t = 0; t < NumTracks; ++t) {
        tracks[t].gainL = targetL[t];
        tracks[t].gainR = targetR[t];
    }
    flushRecord(rt, end);
    if (dropped) {
        droppedBlocks.fetch_add(1, std::memory_order_relaxed);
    }
    pos.store(p, std::memory_order_relaxed);
    blockCount.fetch_add(1, std::memory_order_release);
}

// publish the record head's pending peak and content end for a track
void MultiTape::flushRecord(int track, size_t end) {
    if (track < 0) {
        return;
    }
    if (peakBlock != SIZE_MAX) {
        tracks[track].peaks[peakBlock].store(static_cast<uint8_t>(peakAcc), std::memory_order_relaxed);
    }
    if (end > tracks[track].endFrame.load(std::memory_order_relaxed)) {
        tracks[track].endFrame.store(end, std::memory_order_relaxed);
    }
}

//------------------------------------------------------------------------------
//--- any thread

void MultiTape::setLatency(size_t captureFrames, size_t playbackFrames) {
    latCapture.store(captureFrames, std::memory_order_relaxed);
    latPlayback.store(playbackFrames, std::memory_order_relaxed);
}

MultiTape::Status MultiTape::getStatus() const {
    Status s{};
    s.playing = playing.load(std::memory_order_relaxed);
    s.armed = armed.load(std::memory_order_relaxed);
    s.recording = s.playing && recEnabled.load(std::memory_order_relaxed) && s.armed >= 0;
    s.source = source.load(std::memory_order_relaxed);
    s.channels = chans;
    s.posFrame = pos.load(std::memory_order_relaxed);
    s.maxFrames = maxFrames;
    s.sampleRate = sr;
    for (int t = 0; t < NumTracks; ++t) {
        s.endFrame[t] = tracks[t].endFrame.load(std::memory_order_relaxed);
    }
    s.allocatedBytes = allocatedBytes.load(std::memory_order_relaxed);
    s.memFull = memFull.load(std::memory_order_relaxed);
    s.jobsPending = jobsPending.load(std::memory_order_relaxed);
    s.droppedBlocks = droppedBlocks.load(std::memory_order_relaxed);
    return s;
}

void MultiTape::overview(int track, size_t startFrame, size_t binFrames, int numBins, uint8_t *out) const {
    if (track < 0 || track >= NumTracks || numPeaks == 0 || binFrames == 0) {
        std::fill(out, out + std::max(0, numBins), 0);
        return;
    }
    const Track &tr = tracks[track];
    for (int i = 0; i < numBins; ++i) {
        size_t f0 = startFrame + static_cast<size_t>(i) * binFrames;
        size_t b0 = f0 >> PeakShift;
        size_t b1 = std::max(b0 + 1, (f0 + binFrames + PeakFrames - 1) >> PeakShift);
        b1 = std::min(b1, numPeaks);
        uint8_t m = 0;
        for (size_t b = b0; b < b1; ++b) {
            m = std::max(m, tr.peaks[b].load(std::memory_order_relaxed));
        }
        out[i] = m;
    }
}

float MultiTape::peekSample(int track, size_t frame, int channel) const {
    if (track < 0 || track >= NumTracks || frame >= maxFrames) {
        return 0.f;
    }
    const Sample *data = tracks[track].chunks[frame >> ChunkShift].load(std::memory_order_acquire);
    if (data == nullptr) {
        return 0.f;
    }
    const size_t o = frame & (ChunkFrames - 1);
    return toFloat(data[o * chans + (chans == 2 ? (channel & 1) : 0)]);
}

//------------------------------------------------------------------------------
//--- jobs

void MultiTape::requestLoad(int track, const std::string &path, size_t startFrame) {
    std::lock_guard<std::mutex> lock(jobMutex);
    jobs.push_back(Job{Job::Load, track, path, startFrame});
    jobsPending.fetch_add(1);
    jobCv.notify_one();
}

void MultiTape::requestSave(const std::string &dir) {
    std::lock_guard<std::mutex> lock(jobMutex);
    jobs.push_back(Job{Job::Save, -1, dir, 0});
    jobsPending.fetch_add(1);
    jobCv.notify_one();
}

void MultiTape::requestClear(int track) {
    std::lock_guard<std::mutex> lock(jobMutex);
    jobs.push_back(Job{Job::Clear, track, std::string(), 0});
    jobsPending.fetch_add(1);
    jobCv.notify_one();
}

void MultiTape::flush() {
    if (!worker) {
        while (jobsPending.load() > 0) {
            service();
        }
        return;
    }
    std::unique_lock<std::mutex> lock(jobMutex);
    idleCv.wait(lock, [this] { return jobsPending.load() == 0; });
}

//------------------------------------------------------------------------------
//--- worker

void MultiTape::workLoop() {
    while (!quit) {
        {
            std::unique_lock<std::mutex> lock(jobMutex);
            jobCv.wait_for(lock, std::chrono::milliseconds(10),
                           [this] { return quit.load() || !jobs.empty(); });
        }
        if (quit) {
            break;
        }
        service();
    }
}

void MultiTape::service() {
    std::lock_guard<std::mutex> serviceLock(serviceMutex);
    if (numChunks == 0) {
        return;
    }

    ensureRecordChunks();

    // free never-recorded chunks of a track that was just disarmed
    const int arm = armed.load(std::memory_order_relaxed);
    if (lastArmed >= 0 && lastArmed != arm && recTrack.load(std::memory_order_acquire) != lastArmed) {
        reclaimEmptyChunks(lastArmed);
        lastArmed = arm;
    } else if (lastArmed < 0) {
        lastArmed = arm;
    }

    // one job per pass keeps record-chunk lookahead responsive
    Job job;
    bool haveJob = false;
    {
        std::lock_guard<std::mutex> lock(jobMutex);
        if (!jobs.empty()) {
            job = jobs.front();
            jobs.pop_front();
            haveJob = true;
        }
    }
    if (haveJob) {
        runJob(job);
        std::lock_guard<std::mutex> lock(jobMutex);
        jobsPending.fetch_sub(1);
        idleCv.notify_all();
    }

    reclaimRetired(false);
}

MultiTape::Sample *MultiTape::allocChunk() {
    const size_t bytes = chunkSamples * sizeof(Sample);
    if (memProbe && memProbe() < memReserve + bytes) {
        if (!memFull.exchange(true)) {
            std::cerr << "multitape: memory reserve reached; not allocating" << std::endl;
        }
        return nullptr;
    }
    // chunks are mapped directly, so freeing one returns its memory to the
    // system at once (heap-sized blocks would stay with the process);
    // MAP_POPULATE faults the zeroed pages in here, never in the audio thread
    void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (p == MAP_FAILED) {
        memFull = true;
        return nullptr;
    }
    allocatedBytes.fetch_add(bytes, std::memory_order_relaxed);
    memFull = false;
    return static_cast<Sample *>(p);
}

void MultiTape::freeChunk(Sample *p) {
    if (p != nullptr) {
        munmap(p, chunkSamples * sizeof(Sample));
    }
}

void MultiTape::retireChunk(Sample *p) {
    if (p != nullptr) {
        retired.push_back(Retired{p, blockCount.load(std::memory_order_acquire)});
    }
}

void MultiTape::reclaimRetired(bool force) {
    const uint64_t now = blockCount.load(std::memory_order_acquire);
    const size_t bytes = chunkSamples * sizeof(Sample);
    auto it = retired.begin();
    while (it != retired.end()) {
        // two completed audio blocks guarantee no reader still holds the pointer
        if (force || now >= it->epoch + 2) {
            freeChunk(it->ptr);
            allocatedBytes.fetch_sub(bytes, std::memory_order_relaxed);
            it = retired.erase(it);
        } else {
            ++it;
        }
    }
}

void MultiTape::ensureRecordChunks() {
    const int t = armed.load(std::memory_order_relaxed);
    if (t < 0) {
        return;
    }
    const size_t p = pos.load(std::memory_order_relaxed);
    const size_t off = recOffset();
    const size_t w0 = p > off ? p - off : 0;
    const size_t w1 = std::min(maxFrames - 1, p + static_cast<size_t>(3.f * sr));
    for (size_t c = w0 >> ChunkShift; c <= (w1 >> ChunkShift); ++c) {
        if (tracks[t].chunks[c].load(std::memory_order_acquire) != nullptr) {
            continue;
        }
        Sample *data = allocChunk();
        if (data == nullptr) {
            return;
        }
        tracks[t].chunks[c].store(data, std::memory_order_release);
    }
}

void MultiTape::reclaimEmptyChunks(int track) {
    Track &tr = tracks[track];
    for (size_t c = 0; c < numChunks; ++c) {
        if (tr.chunks[c].load(std::memory_order_acquire) == nullptr) {
            continue;
        }
        bool empty = true;
        for (size_t b = c * PeaksPerChunk; b < (c + 1) * PeaksPerChunk; ++b) {
            if (tr.peaks[b].load(std::memory_order_relaxed) != 0) {
                empty = false;
                break;
            }
        }
        if (empty) {
            retireChunk(tr.chunks[c].exchange(nullptr, std::memory_order_acq_rel));
        }
    }
}

void MultiTape::recomputePeaks(int track, size_t chunk, const Sample *data) {
    for (size_t b = 0; b < PeaksPerChunk; ++b) {
        uint8_t m = 0;
        const Sample *s = data + b * PeakFrames * chans;
        for (size_t i = 0; i < PeakFrames * chans; ++i) {
            m = std::max(m, peakByte(s[i]));
        }
        tracks[track].peaks[chunk * PeaksPerChunk + b].store(m, std::memory_order_relaxed);
    }
}

void MultiTape::runJob(const Job &job) {
    switch (job.type) {
    case Job::Load:
        doLoad(job.track, job.path, job.startFrame);
        break;
    case Job::Save:
        doSave(job.path);
        break;
    case Job::Clear:
        doClear(job.track);
        break;
    }
}

void MultiTape::doLoad(int track, const std::string &path, size_t startFrame) {
    if (track < 0 || track >= NumTracks) {
        return;
    }
    if (armed.load() == track || recTrack.load() == track) {
        std::cerr << "multitape: track " << (track + 1) << " is armed; not loading " << path << std::endl;
        return;
    }
    if (startFrame >= maxFrames) {
        return;
    }
    SF_INFO info{};
    SNDFILE *f = sf_open(path.c_str(), SFM_READ, &info);
    if (f == nullptr) {
        std::cerr << "multitape: cannot open " << path << ": " << sf_strerror(nullptr) << std::endl;
        return;
    }
    if (info.channels < 1 || info.frames < 1) {
        sf_close(f);
        return;
    }
    if (static_cast<float>(info.samplerate) != sr) {
        std::cerr << "multitape: " << path << " is " << info.samplerate
                  << " Hz; loading without resampling" << std::endl;
    }

    const size_t endFrame = std::min(maxFrames, startFrame + static_cast<size_t>(info.frames));
    const int fch = info.channels;
    constexpr size_t ioFrames = 4096;
    std::vector<float> io(ioFrames * fch);
    Track &tr = tracks[track];
    size_t loadedEnd = startFrame;

    for (size_t c = startFrame >> ChunkShift; c <= ((endFrame - 1) >> ChunkShift); ++c) {
        const size_t chunkStart = c << ChunkShift;
        const size_t seg0 = std::max(startFrame, chunkStart);
        const size_t seg1 = std::min(endFrame, chunkStart + ChunkFrames);

        // copy-on-write: the audio thread only ever sees complete chunks
        Sample *next = allocChunk();
        if (next == nullptr) {
            std::cerr << "multitape: out of memory loading " << path << std::endl;
            break;
        }
        if (const Sample *old = tr.chunks[c].load(std::memory_order_acquire)) {
            std::memcpy(next, old, chunkSamples * sizeof(Sample));
        }

        size_t frame = seg0;
        bool eof = false;
        while (frame < seg1) {
            const size_t want = std::min(ioFrames, seg1 - frame);
            const sf_count_t got = sf_readf_float(f, io.data(), static_cast<sf_count_t>(want));
            if (got <= 0) {
                eof = true;
                break;
            }
            for (sf_count_t i = 0; i < got; ++i) {
                const float *in = &io[static_cast<size_t>(i) * fch];
                Sample *dst = next + (frame - chunkStart + static_cast<size_t>(i)) * chans;
                if (chans == 1) {
                    dst[0] = toSample(fch == 1 ? in[0] : 0.5f * (in[0] + in[1]));
                } else {
                    dst[0] = toSample(in[0]);
                    dst[1] = toSample(fch == 1 ? in[0] : in[1]);
                }
            }
            frame += static_cast<size_t>(got);
        }

        recomputePeaks(track, c, next);
        retireChunk(tr.chunks[c].exchange(next, std::memory_order_acq_rel));
        loadedEnd = frame;
        if (eof) {
            break;
        }
    }
    sf_close(f);

    if (loadedEnd > tr.endFrame.load()) {
        tr.endFrame.store(loadedEnd);
    }
    std::cout << "multitape: loaded " << path << " to track " << (track + 1) << std::endl;
}

void MultiTape::doSave(const std::string &dir) {
    if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::cerr << "multitape: cannot create " << dir << ": " << strerror(errno) << std::endl;
        return;
    }
    std::vector<Sample> zeros(chunkSamples, 0);
    for (int t = 0; t < NumTracks; ++t) {
        std::ostringstream os;
        os << dir << "/track" << (t + 1) << ".wav";
        const std::string path = os.str();
        const size_t end = tracks[t].endFrame.load();
        if (end == 0) {
            // remove a stale take so reloading the folder matches the tape
            std::remove(path.c_str());
            continue;
        }
        SF_INFO info{};
        info.samplerate = static_cast<int>(sr);
        info.channels = chans;
        info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        SNDFILE *f = sf_open(path.c_str(), SFM_WRITE, &info);
        if (f == nullptr) {
            std::cerr << "multitape: cannot write " << path << ": " << sf_strerror(nullptr) << std::endl;
            continue;
        }
        for (size_t c = 0; (c << ChunkShift) < end; ++c) {
            const size_t frames = std::min(ChunkFrames, end - (c << ChunkShift));
            const Sample *data = tracks[t].chunks[c].load(std::memory_order_acquire);
            sf_writef_short(f, data ? data : zeros.data(), static_cast<sf_count_t>(frames));
        }
        sf_close(f);
    }
    std::cout << "multitape: saved to " << dir << std::endl;
}

void MultiTape::doClear(int track) {
    if (track < 0 || track >= NumTracks) {
        return;
    }
    if (recTrack.load() == track) {
        std::cerr << "multitape: track " << (track + 1) << " is recording; not clearing" << std::endl;
        return;
    }
    Track &tr = tracks[track];
    for (size_t c = 0; c < numChunks; ++c) {
        retireChunk(tr.chunks[c].exchange(nullptr, std::memory_order_acq_rel));
    }
    for (size_t b = 0; b < numPeaks; ++b) {
        tr.peaks[b].store(0, std::memory_order_relaxed);
    }
    tr.endFrame.store(0);
}
