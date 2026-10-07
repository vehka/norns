#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdio.h>

#include "../events.h"

#include "../clocks/clock_midi.h"

#include "device.h"
#include "device_midi.h"

#define DEV_MIDI_INPUT_BUFFER_SIZE 128
#define DEV_MIDI_SEQ_BUFFER_SIZE 256

unsigned int dev_midi_port_count(const char *path) {
    int card;
    int alsa_dev;

    if (sscanf(path, "/dev/snd/midiC%dD%d", &card, &alsa_dev) < 0) {
        // TODO: Insert error message here
        return 0;
    }

    // mostly from amidi.c
    snd_ctl_t *ctl;
    char name[32];

    snd_rawmidi_info_t *info;
    int subs = 0;
    int subs_in = 0;
    int subs_out = 0;

    sprintf(name, "hw:%d", card);
    if (snd_ctl_open(&ctl, name, 0) < 0) {
        // TODO: Insert error message here
        return 0;
    }

    snd_rawmidi_info_alloca(&info);
    snd_rawmidi_info_set_device(info, alsa_dev);

    snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
    if (snd_ctl_rawmidi_info(ctl, info) >= 0) {
        subs_in = snd_rawmidi_info_get_subdevices_count(info);
    }

    snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_OUTPUT);
    if (snd_ctl_rawmidi_info(ctl, info) >= 0) {
        subs_out = snd_rawmidi_info_get_subdevices_count(info);
    }
    snd_ctl_close(ctl);

    subs = subs_in > subs_out ? subs_in : subs_out;
    return subs;
}

int dev_midi_init(void *self, unsigned int port_index, bool multiport_device) {
    struct dev_midi *midi = (struct dev_midi *)self;
    struct dev_common *base = (struct dev_common *)self;

    unsigned int alsa_card;
    unsigned int alsa_dev;
    char *alsa_name;

    sscanf(base->path, "/dev/snd/midiC%uD%u", &alsa_card, &alsa_dev);

    if (asprintf(&alsa_name, "hw:%u,%u,%u", alsa_card, alsa_dev, port_index) < 0) {
        fprintf(stderr, "failed to create alsa device name for card %d,%d\n", alsa_card, alsa_dev);
        return -1;
    }

    if (snd_rawmidi_open(&midi->handle_in, &midi->handle_out, alsa_name, 0) < 0) {
        if (snd_rawmidi_open(NULL, &midi->handle_out, alsa_name, 0) < 0) {
            if (snd_rawmidi_open(&midi->handle_in, NULL, alsa_name, 0) < 0) {
                return -1;
            } else {
                fprintf(stderr, "device opened OK, only input ports\n");
                midi->handle_out = NULL;
            }
        } else {
            fprintf(stderr, "device opened OK, only output ports\n");
            midi->handle_in = NULL;
        }
    } else {
        fprintf(stderr, "device opened (bidirectional)\n");
    }
    // fprintf(stderr, "input handle  : %p\n", midi->handle_in);
    // fprintf(stderr, "output handle : %p\n", midi->handle_out);

    char *name_with_port_index;
    if (multiport_device) {
        if (asprintf(&name_with_port_index, "%s %u", base->name, port_index + 1) < 0) {
            fprintf(stderr, "failed to create human-readable device name for card %d,%d,%d\n", alsa_card, alsa_dev,
                    port_index);
            return -1;
        } else {
            fprintf(stderr, "assigned device name with port: %s\n", name_with_port_index);
        }
        base->name = name_with_port_index;
    }

    if (midi->handle_in != NULL) {
        base->start = &dev_midi_start;
    } else {
        base->start = NULL;
    }
    base->deinit = &dev_midi_deinit;

    midi->clock_enabled = true;

    return 0;
}

int dev_midi_virtual_init(void *self) {
    struct dev_midi *midi = (struct dev_midi *)self;
    struct dev_common *base = (struct dev_common *)self;

    if (snd_rawmidi_open(&midi->handle_in, &midi->handle_out, "virtual", 0) < 0) {
        fprintf(stderr, "failed to open alsa virtual device.\n");
        return -1;
    }

    // trigger reading
    snd_rawmidi_read(midi->handle_in, NULL, 0);

    base->start = &dev_midi_start;
    base->deinit = &dev_midi_deinit;

    return 0;
}

static void *dev_midi_seq_start(void *self);

// open a sequencer client with one port and connect it, both ways, to the
// sequencer port given as "client:port" in the device path
int dev_midi_seq_init(void *self) {
    struct dev_midi *midi = (struct dev_midi *)self;
    struct dev_common *base = (struct dev_common *)self;

    int client;
    int port;

    if (sscanf(base->path, "%d:%d", &client, &port) != 2) {
        fprintf(stderr, "bad alsa sequencer address: %s\n", base->path);
        return -1;
    }

    if (snd_seq_open(&midi->seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        fprintf(stderr, "failed to open alsa sequencer.\n");
        return -1;
    }
    snd_seq_set_client_name(midi->seq, base->name);

    midi->seq_port = snd_seq_create_simple_port(
        midi->seq, base->name,
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (midi->seq_port < 0) {
        fprintf(stderr, "failed to create alsa sequencer port.\n");
        goto err;
    }

    // a port may be input-only or output-only
    {
        int from = snd_seq_connect_from(midi->seq, midi->seq_port, client, port);
        int to = snd_seq_connect_to(midi->seq, midi->seq_port, client, port);
        if (from < 0 && to < 0) {
            fprintf(stderr, "failed to connect to alsa sequencer port %s\n", base->path);
            goto err;
        }
    }

    if (snd_midi_event_new(DEV_MIDI_SEQ_BUFFER_SIZE, &midi->seq_encoder) < 0 ||
        snd_midi_event_new(DEV_MIDI_SEQ_BUFFER_SIZE, &midi->seq_decoder) < 0) {
        fprintf(stderr, "failed to allocate alsa midi event coders.\n");
        goto err;
    }
    // the input parser wants a status byte on every message
    snd_midi_event_no_status(midi->seq_decoder, 1);

    base->start = &dev_midi_seq_start;
    base->deinit = &dev_midi_deinit;

    midi->clock_enabled = true;

    return 0;

err:
    dev_midi_deinit(self);
    return -1;
}

void dev_midi_deinit(void *self) {
    struct dev_midi *midi = (struct dev_midi *)self;
    // struct dev_common *base = (struct dev_common *)self;
    // fprintf(stderr, "midi_deinit   : %s\n", base->name);
    // fprintf(stderr, "input handle  : %p\n", midi->handle_in);
    // fprintf(stderr, "output handle : %p\n", midi->handle_out);

    if (midi->handle_in != NULL) {
        snd_rawmidi_close(midi->handle_in);
    }
    if (midi->handle_out != NULL) {
        snd_rawmidi_close(midi->handle_out);
    }
    if (midi->seq_encoder != NULL) {
        snd_midi_event_free(midi->seq_encoder);
    }
    if (midi->seq_decoder != NULL) {
        snd_midi_event_free(midi->seq_decoder);
    }
    if (midi->seq != NULL) {
        snd_seq_close(midi->seq);
    }
}

static inline bool is_status_byte(uint8_t byte) {
    return (byte & 0xf0) >> 7;
}

static inline bool is_status_real_time(uint8_t byte) {
    return byte >= 0xf8;
}

static inline uint8_t midi_msg_len(uint8_t status) {
    uint8_t upper = status & 0xf0;

    // channel voice messages
    switch (upper) {
    case 0x80: // note off
    case 0x90: // note on
    case 0xa0: // polyphonic key pressure
    case 0xb0: // control change
    case 0xe0: // pitch bend
        return 3;
    case 0xc0: // program change
    case 0xd0: // channel pressure
        return 2;
    }

    // system real-time messages
    switch (status) {
    case 0xf8: // timing clock
    case 0xfa: // start sequence
    case 0xfb: // continue sequence
    case 0xfc: // stop sequence
    case 0xfe: // active sensing
    case 0xff: // system reset
        return 1;
    }

    // system common messages
    switch (status) {
    case 0xf1: // midi timing code
    case 0xf3: // song select
        return 2;
    case 0xf2: // song position pointer
        return 3;
    case 0xf6: // tune request
        return 1;
    }

    // system exclusive messages (variable length)
    switch (status) {
    case 0xf0:    // sysex start
        return 3; // special case, deliver sysex to lua in 3 byte chunks
    case 0xf7:    // sysex stop
        return 1; // special case, allow single sysex stop as isolated event
    }

    // channel mode messages
    return 2;
}

typedef struct {
    uint8_t buffer[DEV_MIDI_INPUT_BUFFER_SIZE];

    uint8_t prior_status;
    uint8_t prior_len;

    uint8_t msg_buf[3];
    uint8_t msg_pos;
    uint8_t msg_len;
    bool msg_started;
    bool msg_sysex;
} midi_input_state_t;

static inline void midi_input_msg_post(midi_input_state_t *state, struct dev_midi *midi) {
    union event_data *ev = event_data_new(EVENT_MIDI_EVENT);
    ev->midi_event.id = midi->dev.id;
    ev->midi_event.nbytes = state->msg_len;
    for (uint8_t n = 0; n < state->msg_len; n++) {
        ev->midi_event.data[n] = state->msg_buf[n];
    }
    event_post(ev);
}

static inline void midi_input_msg_start(midi_input_state_t *state, uint8_t status) {
    state->msg_pos = 0;
    state->msg_len = midi_msg_len(status);
    state->msg_started = true;
    state->msg_buf[state->msg_pos++] = status;
    state->msg_sysex = status == 0xf0;

    // save for running status
    if (!is_status_real_time(status)) {
        state->prior_status = status;
        state->prior_len = state->msg_len;
    }
}

static inline void midi_input_msg_end(midi_input_state_t *state) {
    state->msg_started = false;
    state->msg_pos = 0;
}

static inline void midi_input_msg_acc(midi_input_state_t *state, uint8_t byte) {
    if (!state->msg_started) {
        if (state->msg_sysex) {
            // continue to pass sysex through in 3 byte chunks
            state->msg_started = true;
            state->msg_pos = 0;
            state->msg_len = 3;
        } else {
            // running status, start a new message
            state->msg_started = true;
            state->msg_pos = 0;
            state->msg_len = state->prior_len;
            state->msg_buf[state->msg_pos++] = state->prior_status;
        }
    }

    state->msg_buf[state->msg_pos++] = byte;

    // terminate sysex
    if (byte == 0xf7) {
        state->msg_sysex = false;
        state->msg_len = state->msg_pos;
    }
}

static inline bool midi_input_msg_is_complete(midi_input_state_t *state) {
    return state->msg_started && (state->msg_len == state->msg_pos);
}

static inline ssize_t dev_midi_consume_buffer(midi_input_state_t *state, ssize_t size, struct dev_midi *midi) {
    uint8_t i = 0;
    uint8_t byte = 0;

    for (i = 0; i < size; i++) {
        byte = state->buffer[i];

        if (byte >= 0xf8) {
            if (midi->clock_enabled) {
                clock_midi_handle_message(byte);
            }
        }

        if (is_status_byte(byte) && (byte != 0xf7)) {
            midi_input_msg_start(state, byte);
        } else {
            midi_input_msg_acc(state, byte);
        }

        if (midi_input_msg_is_complete(state)) {
            midi_input_msg_post(state, midi);
            midi_input_msg_end(state);
        }
    }

    return i;
}

void *dev_midi_start(void *self) {
    struct dev_midi *midi = (struct dev_midi *)self;
    struct dev_common *base = (struct dev_common *)self;

    snd_rawmidi_status_t *status = NULL;

    midi_input_state_t state;
    ssize_t read = 0;
    ssize_t xruns;

    if (midi->handle_in == NULL) {
        fprintf(stderr, "starting a reader thread for a non-input MIDI device; shouldn't get here!\n");
        return NULL;
    }

    // fprintf(stderr, "starting input thread for midi device: %s (%p)\n", base->name, midi->handle_in);

    if (snd_rawmidi_status_malloc(&status) != 0) {
        fprintf(stderr, "failed allocating rawmidi status, stopping device: %s\n", base->name);
        return NULL;
    }

    do {
        read = snd_rawmidi_read(midi->handle_in, state.buffer, DEV_MIDI_INPUT_BUFFER_SIZE);
        if (dev_midi_consume_buffer(&state, read, midi) != read) {
            fprintf(stderr, "midi inconsistency for device: %s\n", base->name);
        }

        if (snd_rawmidi_status(midi->handle_in, status) == 0) {
            xruns = snd_rawmidi_status_get_xruns(status);
            if (xruns > 0) {
                fprintf(stderr, "xruns (%d) for midi device: %s\n", (int)xruns, base->name);
                snd_rawmidi_drop(midi->handle_in);
            }
        }

    } while (read > 0);

    if (status) {
        snd_rawmidi_status_free(status);
    }

    return NULL;
}

void *dev_midi_seq_start(void *self) {
    struct dev_midi *midi = (struct dev_midi *)self;
    struct dev_common *base = (struct dev_common *)self;

    midi_input_state_t state = {};
    snd_seq_event_t *ev;
    int err;

    while (true) {
        err = snd_seq_event_input(midi->seq, &ev);
        if (err == -ENOSPC) {
            fprintf(stderr, "input overrun for midi device: %s\n", base->name);
            continue;
        }
        if (err < 0) {
            break;
        }

        if (ev->type == SND_SEQ_EVENT_SYSEX) {
            // sysex has no length limit; feed it through in buffer-sized pieces
            uint8_t *data = (uint8_t *)ev->data.ext.ptr;
            size_t left = ev->data.ext.len;
            while (left > 0) {
                size_t n = left < DEV_MIDI_INPUT_BUFFER_SIZE ? left : DEV_MIDI_INPUT_BUFFER_SIZE;
                memcpy(state.buffer, data, n);
                dev_midi_consume_buffer(&state, n, midi);
                data += n;
                left -= n;
            }
        } else {
            // events that are not midi (port subscriptions &c) decode to nothing
            long n = snd_midi_event_decode(midi->seq_decoder, state.buffer, DEV_MIDI_INPUT_BUFFER_SIZE, ev);
            if (n > 0) {
                dev_midi_consume_buffer(&state, n, midi);
            }
        }
    }

    return NULL;
}

static ssize_t dev_midi_seq_send(struct dev_midi *midi, uint8_t *data, size_t n) {
    snd_seq_event_t ev;
    size_t sent = 0;

    while (sent < n) {
        snd_seq_ev_clear(&ev);
        long used = snd_midi_event_encode(midi->seq_encoder, data + sent, n - sent, &ev);
        if (used <= 0) {
            break;
        }
        sent += used;
        // the encoder returns no event until a message is complete
        if (ev.type != SND_SEQ_EVENT_NONE) {
            snd_seq_ev_set_source(&ev, midi->seq_port);
            snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev);
            if (snd_seq_event_output_direct(midi->seq, &ev) < 0) {
                return -1;
            }
        }
    }

    return sent;
}

ssize_t dev_midi_send(void *self, uint8_t *data, size_t n) {
    struct dev_midi *midi = (struct dev_midi *)self;
    if (midi->seq != NULL) {
        return dev_midi_seq_send(midi, data, n);
    }
    if (midi->handle_out == NULL) {
        return -1;
    } else {
        return snd_rawmidi_write(midi->handle_out, data, n);
    }
}
