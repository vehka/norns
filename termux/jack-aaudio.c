// jack-aaudio: play a JACK server's output through Android's AAudio.
//
// termux's own jack driver (opensles) keeps only two buffers queued and has
// no slack for a late cycle, which crackles. Instead jackd runs on the
// `dummy` driver (a plain timer) with monitor ports, and this client copies
// system:monitor_* into a ring buffer that an AAudio callback plays from.
// The ring absorbs scheduling jitter; since the timer and the audio device
// run on different clocks it also drifts slowly, which is corrected by
// dropping or repeating a little audio when it gets too full or runs dry.
//
//   jack-aaudio [buffer_ms]    (default 60)
//
// cc -O2 -o jack-aaudio jack-aaudio.c -ljack -laaudio

#include <aaudio/AAudio.h>
#include <jack/jack.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHANNELS 2
#define RING_FRAMES 32768 // power of two

static float ring[RING_FRAMES * CHANNELS];
static atomic_uint ring_w; // frames written, by the jack thread
static atomic_uint ring_r; // frames read, by the aaudio thread

static jack_client_t *client;
static jack_port_t *in_port[CHANNELS];
static unsigned target_frames; // fill level to (re)start playing at
static unsigned max_frames;    // fill level above which audio is dropped

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
    return 0;
}

static aaudio_data_callback_result_t audio_callback(AAudioStream *stream, void *user, void *data,
                                                    int32_t nframes) {
    (void)stream;
    (void)user;
    float *out = (float *)data;
    unsigned r = atomic_load_explicit(&ring_r, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&ring_w, memory_order_acquire);
    unsigned fill = w - r;

    if (atomic_load(&priming)) {
        if (fill < target_frames + (unsigned)nframes) {
            memset(out, 0, sizeof(float) * CHANNELS * nframes);
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        // start from exactly the target level
        r = w - target_frames - (unsigned)nframes;
        fill = target_frames + (unsigned)nframes;
        atomic_store(&priming, false);
    } else if (fill > max_frames) {
        // the device is slower than jack's timer: skip ahead
        r = w - target_frames - (unsigned)nframes;
        fill = target_frames + (unsigned)nframes;
        atomic_fetch_add(&overruns, 1);
    }

    if (fill < (unsigned)nframes) {
        // ran dry: play silence and refill to the target before resuming
        memset(out, 0, sizeof(float) * CHANNELS * nframes);
        atomic_store(&priming, true);
        atomic_fetch_add(&underruns, 1);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    for (int32_t i = 0; i < nframes; i++) {
        const float *src = &ring[((r + i) & (RING_FRAMES - 1)) * CHANNELS];
        for (int c = 0; c < CHANNELS; c++) {
            float v = src[c];
            out[i * CHANNELS + c] = v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
        }
    }
    atomic_store_explicit(&ring_r, r + nframes, memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// e.g. headphones plugged in: the stream is dead and has to be reopened
static void error_callback(AAudioStream *stream, void *user, aaudio_result_t error) {
    (void)stream;
    (void)user;
    (void)error;
    atomic_store(&stream_error, true);
}

static AAudioStream *open_stream(int rate) {
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
    fprintf(stderr, "jack-aaudio: stream open, rate %d, burst %d frames, buffer %d frames\n",
            AAudioStream_getSampleRate(s), AAudioStream_getFramesPerBurst(s),
            AAudioStream_getBufferSizeInFrames(s));
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
    int buffer_ms = argc > 1 ? atoi(argv[1]) : 60;
    if (buffer_ms < 5) {
        buffer_ms = 5;
    }

    client = jack_client_open("aaudio", JackNoStartServer, NULL);
    if (!client) {
        fprintf(stderr, "jack-aaudio: cannot connect to jack\n");
        return 1;
    }
    int rate = (int)jack_get_sample_rate(client);
    unsigned period = jack_get_buffer_size(client);
    // jack delivers a whole period at once, so the level swings by that much
    target_frames = (unsigned)((long)rate * buffer_ms / 1000);
    max_frames = target_frames * 2 + period * 2;
    if (max_frames > RING_FRAMES / 2) {
        fprintf(stderr, "jack-aaudio: buffer too large\n");
        return 1;
    }

    jack_set_process_callback(client, jack_process, NULL);
    jack_on_shutdown(client, on_jack_shutdown, NULL);
    for (int c = 0; c < CHANNELS; c++) {
        char name[16];
        snprintf(name, sizeof(name), "in_%d", c + 1);
        in_port[c] = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    }
    if (jack_activate(client)) {
        fprintf(stderr, "jack-aaudio: cannot activate\n");
        return 1;
    }
    for (int c = 0; c < CHANNELS; c++) {
        char src[32];
        snprintf(src, sizeof(src), "system:monitor_%d", c + 1);
        if (jack_connect(client, src, jack_port_name(in_port[c]))) {
            fprintf(stderr, "jack-aaudio: cannot connect %s (jackd needs -d dummy -m)\n", src);
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    AAudioStream *stream = open_stream(rate);
    unsigned last_under = 0, last_over = 0;
    while (!atomic_load(&quit)) {
        sleep(1);
        if (!stream || atomic_load(&stream_error)) {
            if (stream) {
                fprintf(stderr, "jack-aaudio: stream lost, reopening\n");
                AAudioStream_close(stream);
            }
            stream = open_stream(rate);
            continue;
        }
        unsigned u = atomic_load(&underruns), o = atomic_load(&overruns);
        if (u != last_under || o != last_over) {
            fprintf(stderr, "jack-aaudio: underruns %u overruns %u\n", u, o);
            last_under = u;
            last_over = o;
        }
    }

    if (stream) {
        AAudioStream_requestStop(stream);
        AAudioStream_close(stream);
    }
    jack_client_close(client);
    return 0;
}
