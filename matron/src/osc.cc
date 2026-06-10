/*
 * osc.c
 *
 * user OSC device, send/receive arbitrary OSC within lua scripts
 *
 */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NORNS_DESKTOP
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

#include <dns_sd.h>
#include <lo/lo.h>

#include "args.h"
#include "events.h"
#include "oracle.h"

#define OSC_CRONE_HOST "127.0.0.1"
#define OSC_CRONE_PORT "57120"
static lo_address crone_addr;

static DNSServiceRef dnssd_ref;

static int osc_receive(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
static void lo_error_handler(int num, const char *m, const char *path);

#ifdef NORNS_DESKTOP

// On desktop, newer liblo (0.31+) strictly requires OSC paths to start with '/'.
// Some norns scripts relay SC SendReply values via sendMsg("path", ...) without
// the leading slash, which causes liblo to reject and drop the message.
// We replace lo_server_thread with our own recvfrom loop that normalises the
// path before handing the packet to lo_server_dispatch_data.

static lo_server srv;
static pthread_t recv_thread;
static volatile int recv_running;

// Source address of the packet currently being dispatched.
static char last_sender_host[NI_MAXHOST];
static char last_sender_port[NI_MAXSERV];

// Prepend '/' to an OSC path that is missing it.
// The OSC path field is null-padded to a 4-byte boundary; prepending '/' may
// expand that boundary by 4 bytes, which requires shifting the rest of the
// message.  buf must have at least 4 bytes of headroom beyond 'size'.
// Returns the (possibly increased) valid data length.
static size_t osc_normalize_path(uint8_t *buf, size_t size) {
    if (size < 4 || buf[0] == '/' || buf[0] == '#') {
        return size;
    }

    size_t path_len = strnlen((char *)buf, size);
    if (path_len == size) {
        // no terminator within the packet: malformed, let liblo reject it
        // (otherwise orig_padded can exceed size and underflow the memmove).
        return size;
    }
    size_t orig_padded = ((path_len + 1 + 3) / 4) * 4;
    size_t new_padded  = ((path_len + 2 + 3) / 4) * 4;

    if (new_padded > orig_padded) {
        memmove(buf + new_padded, buf + orig_padded, size - orig_padded);
        size += (new_padded - orig_padded);
    }

    memmove(buf + 1, buf, path_len);
    buf[0] = '/';

    size_t path_end = path_len + 2;
    if (new_padded > path_end) {
        memset(buf + path_end, 0, new_padded - path_end);
    }

    return size;
}

static void *osc_recv_loop(void *arg) {
    (void)arg;
    int fd = lo_server_get_socket_fd(srv);

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[65536 + 4]; // +4 headroom for path expansion
    struct sockaddr_storage sa;
    socklen_t sa_len;

    while (recv_running) {
        sa_len = sizeof(sa);
        ssize_t len = recvfrom(fd, buf, sizeof(buf) - 4, 0,
                               (struct sockaddr *)&sa, &sa_len);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { continue; }
            break;
        }

        getnameinfo((struct sockaddr *)&sa, sa_len,
                    last_sender_host, sizeof(last_sender_host),
                    last_sender_port, sizeof(last_sender_port),
                    NI_NUMERICHOST | NI_NUMERICSERV);

        size_t new_len = osc_normalize_path(buf, (size_t)len);
        lo_server_dispatch_data(srv, buf, new_len);
    }

    return NULL;
}

void osc_init(void) {
    srv = lo_server_new(args_remote_port(), lo_error_handler);
    lo_server_add_method(srv, NULL, NULL, osc_receive, NULL);

    int port = lo_server_get_port(srv);
    DNSServiceRegister(&dnssd_ref, 0, 0, "norns", "_osc._udp", NULL, NULL,
                       htons((uint16_t)port), 0, NULL, NULL, NULL);

    crone_addr = lo_address_new(OSC_CRONE_HOST, OSC_CRONE_PORT);

    recv_running = 1;
    pthread_create(&recv_thread, NULL, osc_recv_loop, NULL);
}

void osc_deinit(void) {
    recv_running = 0;
    pthread_join(recv_thread, NULL);
    DNSServiceRefDeallocate(dnssd_ref);
    lo_server_free(srv);
    lo_address_free(crone_addr);
}

int osc_receive(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)types;
    (void)argv;
    (void)argc;
    (void)user_data;

    union event_data *ev = event_data_new(EVENT_OSC);

    ev->osc_event.path = strdup(path);
    ev->osc_event.msg = lo_message_clone(msg);
    ev->osc_event.from_host = strdup(last_sender_host);
    ev->osc_event.from_port = strdup(last_sender_port);

    event_post(ev);

    return 0;
}

#else // !NORNS_DESKTOP — hardware path, unchanged

static lo_server_thread st;

void osc_init(void) {
    st = lo_server_thread_new(args_remote_port(), lo_error_handler);
    lo_server_thread_add_method(st, NULL, NULL, osc_receive, NULL);
    lo_server_thread_start(st);

    DNSServiceRegister(&dnssd_ref, 0, 0, "norns", "_osc._udp", NULL, NULL,
                       htons(lo_server_thread_get_port(st)), 0, NULL, NULL, NULL);

    crone_addr = lo_address_new(OSC_CRONE_HOST, OSC_CRONE_PORT);
}

void osc_deinit(void) {
    DNSServiceRefDeallocate(dnssd_ref);
    lo_server_thread_free(st);
    lo_address_free(crone_addr);
}

int osc_receive(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)types;
    (void)argv;
    (void)argc;
    (void)user_data;

    union event_data *ev = event_data_new(EVENT_OSC);

    ev->osc_event.path = strdup(path);
    ev->osc_event.msg = lo_message_clone(msg);

    lo_address source = lo_message_get_source(msg);
    const char *host = lo_address_get_hostname(source);
    const char *port = lo_address_get_port(source);

    ev->osc_event.from_host = strdup(host);
    ev->osc_event.from_port = strdup(port);

    event_post(ev);

    return 0;
}

#endif // NORNS_DESKTOP

void osc_send(const char *host, const char *port, const char *path, lo_message msg) {
    lo_address address = lo_address_new(host, port);
    if (!address) {
        fprintf(stderr, "failed to create lo_address\n");
        return;
    }
    lo_send_message(address, path, msg);
    lo_address_free(address);
}

void osc_send_crone(const char *path, lo_message msg) {
    lo_send_message(crone_addr, path, msg);
}

void lo_error_handler(int num, const char *m, const char *path) {
    fprintf(stderr, "liblo error %d in path %s: %s\n", num, path, m);
}
