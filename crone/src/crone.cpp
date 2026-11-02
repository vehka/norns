#include "crone.h"
#include "MixerClient.h"
#include "SoftcutClient.h"
#include "oracle.h"

#include <algorithm>

static crone::MixerClient *mixerClient;
static crone::SoftcutClient *softcutClient;

void crone_init(crone::MixerClient *m, crone::SoftcutClient *sc) {
    mixerClient = m;
    softcutClient = sc;
}

void crone_poll_start_vu() {
    mixerClient->getVuPoll()->start();
}

void crone_poll_stop_vu() {
    mixerClient->getVuPoll()->stop();
}

void crone_cut_buffer_read_mono(const char *path,
                                float startSrc = 0.f, float startDst = 0.f, float dur = -1.f, int chSrc = 0, int chDst = 0,
                                float preserve = 0.f, float mix = 1.f) {
    softcutClient->readBufferMono(path, startSrc, startDst, dur, chSrc, chDst,
                                  preserve, mix);
}

void crone_cut_buffer_read_stereo(const char *path,
                                  float startSrc = 0.f,
                                  float startDst = 0.f,
                                  float dur = -1.f,
                                  float preserve = 0.f,
                                  float mix = 1.f) {
    softcutClient->readBufferStereo(path, startSrc, startDst, dur, preserve, mix);
}

void crone_cut_buffer_write_mono(const char *path,
                                 float start = 0.f,
                                 float dur = -1.f,
                                 int chan = 0) {
    softcutClient->writeBufferMono(path, start, dur, chan);
}

void crone_cut_buffer_write_stereo(const char *path,
                                   float start = 0.f,
                                   float dur = -1.f) {
    softcutClient->writeBufferStereo(path, start, dur);
}

void crone_cut_buffer_clear() {
    softcutClient->clearBuffer(0);
    softcutClient->clearBuffer(1);
}

void crone_cut_buffer_clear_channel(int ch = 0) {
    softcutClient->clearBuffer(ch);
}

void crone_cut_buffer_clear_region(float start, float dur, float fadeTime,
                                   float preserve) {
    softcutClient->clearBufferWithFade(0, start, dur, fadeTime, preserve);
    softcutClient->clearBufferWithFade(1, start, dur, fadeTime, preserve);
}

void crone_cut_buffer_clear_region_channel(int ch, float start, float dur,
                                           float fadeTime, float preserve) {
    softcutClient->clearBufferWithFade(ch, start, dur, fadeTime, preserve);
}

void crone_cut_buffer_copy_mono(int srcCh = 0, int dstCh = 0,
                                float srcStart = 0, float dstStart = 0,
                                float dur = -1.f,
                                float fadeTime = 0.f,
                                float preserve = 0.f,
                                bool reverse = false) {
    softcutClient->copyBuffer(srcCh, dstCh, srcStart, dstStart, dur, fadeTime, preserve,
                              reverse);
}

void crone_cut_buffer_copy_stereo(float srcStart = 0, float dstStart = 0,
                                  float dur = -1.f,
                                  float fadeTime = 0.f,
                                  float preserve = 0.f,
                                  bool reverse = false) {
    softcutClient->copyBuffer(0, 0, srcStart, dstStart, dur, fadeTime, preserve, reverse);
    softcutClient->copyBuffer(1, 1, srcStart, dstStart, dur, fadeTime, preserve, reverse);
}

void crone_cut_buffer_render(int ch, float start, float dur, int sampleCt = 128) {
    softcutClient->renderSamples(
        ch, start, dur, sampleCt,
        [=](float secPerSample, float start, size_t count, const float *samples) {
            o_poll_callback_softcut_render(ch, secPerSample, start, count, samples);
        });
}

void crone_cut_query_position(int idx) {
    float pos = softcutClient->getPosition(idx);
    o_poll_callback_softcut_position(idx, pos);
}

void crone_cut_reset() {
    softcutClient->clearBuffer(0, 0, -1);
    softcutClient->clearBuffer(1, 0, -1);

    softcutClient->reset();
    for (int i = 0; i < crone::SoftcutClient::NumVoices; ++i) {
        softcutClient->getPhasePoll()->stop();
    }
}

void crone_set_param_cut_phase_quant(int arg0, float arg1) {
    softcutClient->setPhaseQuant(arg0, arg1);
}

void crone_set_param_cut_phase_offset(int arg0, float arg1) {
    softcutClient->setPhaseOffset(arg0, arg1);
}

void crone_poll_start_cut_phase() {
    softcutClient->getPhasePoll()->start();
}

void crone_poll_stop_cut_phase() {
    softcutClient->getPhasePoll()->stop();
}

void crone_poll_start_tape() {
    mixerClient->getTapePoll()->start();
}

void crone_poll_stop_tape() {
    mixerClient->getTapePoll()->stop();
}

void crone_tape_rec_open(const char *path) {
    mixerClient->openTapeRecord(path);
}

void crone_tape_rec_start() {
    mixerClient->startTapeRecord();
}

void crone_tape_rec_pause(int paused) {
    mixerClient->pauseTapeRecord(paused);
}

void crone_tape_rec_stop() {
    mixerClient->stopTapeRecord();
}

void crone_tape_play_open(const char *path) {
    mixerClient->openTapePlayback(path);
}

void crone_tape_play_start() {
    mixerClient->startTapePlayback();
}

void crone_tape_play_pause(int paused) {
    mixerClient->pauseTapePlayback(paused);
}

void crone_tape_play_stop() {
    mixerClient->stopTapePlayback();
}

void crone_tape_play_loop(int enabled) {
    mixerClient->setTapeLoop(enabled);
}

void crone_tape_pause() {
    mixerClient->pauseTapeRecord(1);
    mixerClient->pauseTapePlayback(1);
}

void crone_tape_resume() {
    mixerClient->pauseTapeRecord(0);
    mixerClient->pauseTapePlayback(0);
}

//--- multitape

static size_t mtape_frames(float sec) {
    const float sr = mixerClient->getMultiTape().getStatus().sampleRate;
    return sec > 0.f ? static_cast<size_t>(sec * sr + 0.5f) : 0;
}

void crone_mtape_play(int on) {
    crone::Commands::mixerCommands.post(crone::Commands::Id::MTAPE_PLAY, on ? 1.f : 0.f);
}

void crone_mtape_seek(float sec) {
    crone::Commands::mixerCommands.post(crone::Commands::Id::MTAPE_SEEK,
                                        static_cast<int>(mtape_frames(sec)), 0);
}

void crone_mtape_arm(int track) {
    crone::Commands::mixerCommands.post(crone::Commands::Id::MTAPE_ARM, track, 0);
}

void crone_mtape_rec(int on) {
    crone::Commands::mixerCommands.post(crone::Commands::Id::MTAPE_REC, on ? 1.f : 0.f);
}

void crone_mtape_track_level(int track, float amp) {
    crone::Commands::mixerCommands.post(crone::Commands::Id::MTAPE_TRACK_LEVEL, track, amp);
}

void crone_mtape_track_pan(int track, float pan) {
    crone::Commands::mixerCommands.post(crone::Commands::Id::MTAPE_TRACK_PAN, track, pan);
}

void crone_mtape_track_mute(int track, int mute) {
    crone::Commands::mixerCommands.post(crone::Commands::Id::MTAPE_TRACK_MUTE, track, mute ? 1.f : 0.f);
}

void crone_mtape_source(int source) {
    crone::Commands::mixerCommands.post(crone::Commands::Id::MTAPE_SOURCE, source, 0);
}

void crone_mtape_level(float amp) {
    crone::Commands::mixerCommands.post(crone::Commands::Id::SET_LEVEL_MTAPE, amp);
}

void crone_mtape_level_rev(float amp) {
    crone::Commands::mixerCommands.post(crone::Commands::Id::SET_LEVEL_MTAPE_AUX, amp);
}

void crone_mtape_load(int track, const char *path, float sec) {
    mixerClient->getMultiTape().requestLoad(track, path, mtape_frames(sec));
}

void crone_mtape_save(const char *dir) {
    mixerClient->getMultiTape().requestSave(dir);
}

void crone_mtape_clear(int track) {
    mixerClient->getMultiTape().requestClear(track);
}

crone::MultiTape::Status crone_mtape_status() {
    return mixerClient->getMultiTape().getStatus();
}

void crone_mtape_overview(int track, float startSec, float binSec, int numBins, uint8_t *out) {
    mixerClient->getMultiTape().overview(track, mtape_frames(startSec),
                                         std::max<size_t>(1, mtape_frames(binSec)), numBins, out);
}

void crone_mtape_rate(float rate) {
    crone::Commands::mixerCommands.post(crone::Commands::Id::MTAPE_RATE, rate);
}

void crone_mtape_rate_slew(float sec) {
    crone::Commands::mixerCommands.post(crone::Commands::Id::MTAPE_RATE_SLEW, sec);
}

void crone_mtape_loop(int on) {
    mixerClient->getMultiTape().setLoop(on != 0);
}

void crone_mtape_loop_points(float startSec, float endSec) {
    mixerClient->getMultiTape().setLoopPoints(mtape_frames(startSec), mtape_frames(endSec));
}

void crone_mtape_mixdown(const char *path) {
    mixerClient->getMultiTape().requestMixdown(path);
}

void crone_mtape_jog(int on) {
    crone::Commands::mixerCommands.post(crone::Commands::Id::MTAPE_JOG, on ? 1.f : 0.f);
}

void crone_mtape_nudge(float rateDelta) {
    crone::Commands::mixerCommands.post(crone::Commands::Id::MTAPE_NUDGE, rateDelta);
}

void crone_mtape_jog_stop(float sec) {
    crone::Commands::mixerCommands.post(crone::Commands::Id::MTAPE_JOG_STOP, sec);
}
