// jack-aaudio: play a JACK server's output through Android's AAudio.
//
// termux's own jack driver (opensles) keeps only two buffers queued and has
// no slack for a late cycle, which crackles. Instead jackd runs on the
// `dummy` driver and this client plays what it is given through an AAudio
// callback stream, with a ring buffer in between to absorb late cycles: an
// android app gets no real-time scheduling, except for that callback.
//
//   jack-aaudio [buffer_ms [stats_seconds [device|timer]]]   (default 20, 0, timer)
//
// buffer_ms is how late a jack cycle may be before the output runs dry.
// with stats_seconds, the fill range of the ring is logged that often.
//
// device: the audio device is the clock. jackd is put into freewheel, where
// it runs cycles as fast as its clients allow, and this client holds each
// cycle back until the device has played enough. after a late cycle jack
// catches up by itself. the ports are aaudio:playback_*, marked physical;
// run jackd with `-d dummy -C 0 -P 0`, because in freewheel its own ports
// are never serviced and a client connected to them stalls the graph.
//
// timer: jackd's timer is the clock (`jackd -S -d dummy -m`), this client
// reads system:monitor_*, and the ring is read through a resampler whose
// ratio follows the fill level, the two clocks not being the same. the
// timer driver drops the time it is late by, so this needs a larger buffer.
//
// cc -O2 -o jack-aaudio jack-aaudio.c -ljack -laaudio

#include <aaudio/AAudio.h>
#include <jack/jack.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define CHANNELS 2
#define RING_FRAMES 32768 // power of two

static float ring[RING_FRAMES * CHANNELS];
static atomic_uint ring_w; // frames written, by the jack thread
static atomic_uint ring_r; // frames read, by the aaudio thread

static jack_client_t *client;
static jack_port_t *in_port[CHANNELS];
static unsigned target_frames; // mean fill level to hold
static unsigned start_frames;  // fill level to (re)start playing at
static unsigned max_frames;    // fill level above which audio is dropped
static int rate;

// device clock: jack freewheels and jack_process() holds each cycle back
// until the ring has drained to hold_frames
static bool device_clock;
static atomic_bool freewheeling;
static unsigned hold_frames;
static sem_t room; // posted by the aaudio thread after it has read

// only touched by the aaudio thread
static double phase;   // read position past ring_r, 0..1 frame
static double fill_avg; // smoothed fill level

static atomic_int stat_min = INT32_MAX; // fill range since the last report
static atomic_int stat_max;
static atomic_int stat_avg;
static atomic_int stat_ppm; // resampling ratio - 1

static atomic_bool priming = true; // aaudio plays silence until the ring fills
static atomic_uint underruns;
static atomic_uint overruns;
static atomic_bool stream_error;
static atomic_bool quit;

static int jack_process(jack_nframes_t nframes, void *arg) {
    (void)arg;
    const float *in[CHANNELS];
    for (int c = 0; c < CHANNELS; c++) {
        in[c] = (const float *)jack_port_get_buffer(in_port[c], nframes);
    }
    unsigned w = atomic_load_explicit(&ring_w, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&ring_r, memory_order_acquire);
    if (w - r + nframes > RING_FRAMES) {
        return 0; // reader is stuck; nothing sensible to do
    }
    for (jack_nframes_t i = 0; i < nframes; i++) {
        float *dst = &ring[((w + i) & (RING_FRAMES - 1)) * CHANNELS];
        for (int c = 0; c < CHANNELS; c++) {
            dst[c] = in[c][i];
        }
    }
    atomic_store_explicit(&ring_w, w + nframes, memory_order_release);

    // at most 200 ms: with the stream gone, jack goes on slowly
    struct timespec end;
    clock_gettime(CLOCK_REALTIME, &end);
    end.tv_nsec += 200000000;
    if (end.tv_nsec >= 1000000000) {
        end.tv_nsec -= 1000000000;
        end.tv_sec++;
    }
    while (atomic_load(&freewheeling) && !atomic_load(&quit)) {
        r = atomic_load_explicit(&ring_r, memory_order_acquire);
        if (w + nframes - r <= hold_frames || sem_timedwait(&room, &end)) {
            break;
        }
    }
    return 0;
}

static void on_freewheel(int starting, void *arg) {
    (void)arg;
    atomic_store(&freewheeling, starting != 0);
}

static void note_fill(int before, int after) {
    if (after < atomic_load_explicit(&stat_min, memory_order_relaxed)) {
        atomic_store_explicit(&stat_min, after, memory_order_relaxed);
    }
    if (before > atomic_load_explicit(&stat_max, memory_order_relaxed)) {
        atomic_store_explicit(&stat_max, before, memory_order_relaxed);
    }
}

static inline float ring_at(unsigned frame, int c) {
    return ring[(frame & (RING_FRAMES - 1)) * CHANNELS + c];
}

static aaudio_data_callback_result_t audio_callback(AAudioStream *stream, void *user, void *data,
                                                    int32_t nframes) {
    (void)stream;
    (void)user;
    float *out = (float *)data;
    unsigned r = atomic_load_explicit(&ring_r, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&ring_w, memory_order_acquire);
    unsigned fill = w - r;

    if (atomic_load_explicit(&freewheeling, memory_order_relaxed)) {
        // jack follows this thread, so there is nothing to regulate: play
        // what is there. when a cycle is too late it is silence for a
        // burst, and jack makes up the lost ground by itself
        static bool flowing;
        if (fill < (unsigned)nframes) {
            memset(out, 0, sizeof(float) * CHANNELS * nframes);
            if (flowing) {
                atomic_fetch_add(&underruns, 1);
                flowing = false;
            }
        } else {
            for (int32_t i = 0; i < nframes; i++) {
                for (int c = 0; c < CHANNELS; c++) {
                    float v = ring_at(r + i, c);
                    out[i * CHANNELS + c] = v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
                }
            }
            atomic_store_explicit(&ring_r, r + nframes, memory_order_release);
            note_fill((int)fill, (int)fill - nframes);
            atomic_store_explicit(&stat_avg, (int)fill, memory_order_relaxed);
            flowing = true;
        }
        sem_post(&room);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    if (atomic_load(&priming)) {
        if (fill < start_frames) {
            memset(out, 0, sizeof(float) * CHANNELS * nframes);
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        r = w - start_frames;
        fill = start_frames;
        phase = 0;
        fill_avg = target_frames;
        atomic_store(&priming, false);
    } else if (fill > max_frames) {
        // jack ran far ahead (e.g. this thread was stalled): skip to the target
        r = w - start_frames;
        fill = start_frames;
        fill_avg = target_frames;
        atomic_fetch_add(&overruns, 1);
    }

    // the fill level is a sawtooth (jack writes a period at a time), so it
    // is smoothed over 0.5 s; an error of 10 ms then moves the ratio by
    // 0.25 %, which settles in a few seconds without audible pitch change
    fill_avg += ((double)fill - fill_avg) * nframes / (0.5 * rate);
    double ratio = 1.0 + 0.25 * (fill_avg - target_frames) / rate;
    if (ratio > 1.01) {
        ratio = 1.01;
    } else if (ratio < 0.99) {
        ratio = 0.99;
    }

    // the interpolation reads one frame behind and two ahead
    if (fill < (unsigned)(phase + nframes * ratio) + 3) {
        // ran dry: play silence and refill before resuming
        memset(out, 0, sizeof(float) * CHANNELS * nframes);
        atomic_store(&priming, true);
        atomic_fetch_add(&underruns, 1);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    double pos = phase;
    for (int32_t i = 0; i < nframes; i++) {
        unsigned n = r + (unsigned)pos;
        float t = (float)(pos - (unsigned)pos);
        for (int c = 0; c < CHANNELS; c++) {
            // 4-point hermite
            float y0 = ring_at(n - 1, c), y1 = ring_at(n, c);
            float y2 = ring_at(n + 1, c), y3 = ring_at(n + 2, c);
            float c1 = 0.5f * (y2 - y0);
            float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
            float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            float v = ((c3 * t + c2) * t + c1) * t + y1;
            out[i * CHANNELS + c] = v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
        }
        pos += ratio;
    }
    unsigned used = (unsigned)pos;
    phase = pos - used;
    atomic_store_explicit(&ring_r, r + used, memory_order_release);

    note_fill((int)fill, (int)(fill - used));
    atomic_store_explicit(&stat_avg, (int)fill_avg, memory_order_relaxed);
    atomic_store_explicit(&stat_ppm, (int)((ratio - 1.0) * 1e6), memory_order_relaxed);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// e.g. headphones plugged in: the stream is dead and has to be reopened
static void error_callback(AAudioStream *stream, void *user, aaudio_result_t error) {
    (void)stream;
    (void)user;
    (void)error;
    atomic_store(&stream_error, true);
}

static AAudioStream *open_stream(void) {
    AAudioStreamBuilder *b;
    AAudioStream *s = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) {
        return NULL;
    }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, rate);
    AAudioStreamBuilder_setChannelCount(b, CHANNELS);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audio_callback, NULL);
    AAudioStreamBuilder_setErrorCallback(b, error_callback, NULL);
    aaudio_result_t res = AAudioStreamBuilder_openStream(b, &s);
    AAudioStreamBuilder_delete(b);
    if (res != AAUDIO_OK) {
        fprintf(stderr, "jack-aaudio: open failed: %s\n", AAudio_convertResultToText(res));
        return NULL;
    }
    atomic_store(&priming, true);
    atomic_store(&stream_error, false);
    res = AAudioStream_requestStart(s);
    if (res != AAUDIO_OK) {
        fprintf(stderr, "jack-aaudio: start failed: %s\n", AAudio_convertResultToText(res));
        AAudioStream_close(s);
        return NULL;
    }
    // android's own buffer starts out large. the callback thread is the one
    // real-time thread here and does very little, so two bursts are enough;
    // main() adds a burst whenever the device reports an underrun
    AAudioStream_setBufferSizeInFrames(s, 2 * AAudioStream_getFramesPerBurst(s));
    fprintf(stderr, "jack-aaudio: stream open, rate %d, burst %d frames, buffer %d of %d frames\n",
            AAudioStream_getSampleRate(s), AAudioStream_getFramesPerBurst(s),
            AAudioStream_getBufferSizeInFrames(s), AAudioStream_getBufferCapacityInFrames(s));
    return s;
}

static void on_signal(int sig) {
    (void)sig;
    atomic_store(&quit, true);
}

static void on_jack_shutdown(void *arg) {
    (void)arg;
    atomic_store(&quit, true);
}

int main(int argc, char **argv) {
    int buffer_ms = argc > 1 ? atoi(argv[1]) : 20;
    int stats_s = argc > 2 ? atoi(argv[2]) : 0;
    device_clock = argc > 3 && !strcmp(argv[3], "device");
    if (buffer_ms < 2) {
        buffer_ms = 2;
    }
    // no real-time scheduling for the jack thread; take what there is
    setpriority(PRIO_PROCESS, 0, -20);

    client = jack_client_open("aaudio", JackNoStartServer, NULL);
    if (!client) {
        fprintf(stderr, "jack-aaudio: cannot connect to jack\n");
        return 1;
    }
    rate = (int)jack_get_sample_rate(client);
    unsigned period = jack_get_buffer_size(client);
    // jack delivers a whole period at once, so the level swings by that much
    // around its mean: buffer_ms is what is left at the bottom of the swing
    target_frames = (unsigned)((long)rate * buffer_ms / 1000) + period / 2;
    start_frames = target_frames + period / 2;
    max_frames = target_frames * 2 + period * 2;
    if (max_frames > RING_FRAMES / 2) {
        fprintf(stderr, "jack-aaudio: buffer too large\n");
        return 1;
    }
    hold_frames = (unsigned)((long)rate * buffer_ms / 1000);
    sem_init(&room, 0, 0);
    fprintf(stderr, "jack-aaudio: jack period %u frames, %d ms spare, %s clock\n", period, buffer_ms,
            device_clock ? "device" : "timer");

    jack_set_process_callback(client, jack_process, NULL);
    jack_on_shutdown(client, on_jack_shutdown, NULL);
    jack_set_freewheel_callback(client, on_freewheel, NULL);
    for (int c = 0; c < CHANNELS; c++) {
        // with the device clock these are the playback ports that crone
        // looks for; jack's own would stall the graph (see the top)
        char name[16];
        snprintf(name, sizeof(name), device_clock ? "playback_%d" : "in_%d", c + 1);
        in_port[c] = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE,
                                        device_clock ? JackPortIsInput | JackPortIsPhysical | JackPortIsTerminal
                                                     : JackPortIsInput,
                                        0);
    }
    if (jack_activate(client)) {
        fprintf(stderr, "jack-aaudio: cannot activate\n");
        return 1;
    }
    if (device_clock) {
        if (jack_set_freewheel(client, 1)) {
            fprintf(stderr, "jack-aaudio: cannot start freewheel\n");
            return 1;
        }
    } else {
        for (int c = 0; c < CHANNELS; c++) {
            char src[32];
            snprintf(src, sizeof(src), "system:monitor_%d", c + 1);
            if (jack_connect(client, src, jack_port_name(in_port[c]))) {
                fprintf(stderr, "jack-aaudio: cannot connect %s (jackd needs -d dummy -m)\n", src);
                return 1;
            }
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    AAudioStream *stream = open_stream();
    unsigned last_under = 0, last_over = 0;
    int last_xruns = 0, seconds = 0, ticks = 0;
    while (!atomic_load(&quit)) {
        usleep(100000);
        if (++ticks % 10) {
            continue;
        }
        if (!stream || atomic_load(&stream_error)) {
            if (stream) {
                fprintf(stderr, "jack-aaudio: stream lost, reopening\n");
                AAudioStream_close(stream);
            }
            stream = open_stream();
            last_xruns = 0;
            continue;
        }
        unsigned u = atomic_load(&underruns), o = atomic_load(&overruns);
        if (u != last_under || o != last_over) {
            fprintf(stderr, "jack-aaudio: underruns %u overruns %u\n", u, o);
            last_under = u;
            last_over = o;
        }
        int x = AAudioStream_getXRunCount(stream);
        if (x != last_xruns) {
            int size = AAudioStream_getBufferSizeInFrames(stream) + AAudioStream_getFramesPerBurst(stream);
            size = AAudioStream_setBufferSizeInFrames(stream, size);
            fprintf(stderr, "jack-aaudio: device underruns %d, buffer now %d frames\n", x, size);
            last_xruns = x;
        }
        if (stats_s > 0 && ++seconds % stats_s == 0) {
            fprintf(stderr, "jack-aaudio: fill %d..%d mean %d frames, ratio %+d ppm\n",
                    atomic_exchange(&stat_min, INT32_MAX), atomic_exchange(&stat_max, 0),
                    atomic_load(&stat_avg), atomic_load(&stat_ppm));
        }
    }

    if (device_clock) {
        jack_set_freewheel(client, 0);
    }
    if (stream) {
        AAudioStream_requestStop(stream);
        AAudioStream_close(stream);
    }
    jack_client_close(client);
    return 0;
}
