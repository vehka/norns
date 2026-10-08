// repl-send: send one line to a ws-wrapper'd REPL and print what comes back.
//
//   repl-send ws4://127.0.0.1:5555 'print(norns.version.update)' 2
//
// args: url, line, seconds to listen. matron is on 5555, sclang on 5556.
// non-interactive counterpart to maiden-repl, for scripts and smoke tests.
// built by termux/build.sh into $NORNS_DEPS/prefix/bin.
#include <nng/nng.h>
#include <nng/protocol/bus0/bus.h>
#include <nng/transport/ws/websocket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    nng_socket s; nng_dialer d; int rv;
    if ((rv = nng_bus0_open(&s)) || (rv = nng_dialer_create(&d, s, argv[1])) || (rv = nng_dialer_set_bool(d, NNG_OPT_WS_SEND_TEXT, true)) || (rv = nng_dialer_set_bool(d, NNG_OPT_WS_RECV_TEXT, true)) || (rv = nng_dialer_start(d, 0))) { fprintf(stderr, "%s\n", nng_strerror(rv)); return 1; }
    nng_msleep(300);
    char line[8192]; snprintf(line, sizeof line, "%s\n", argv[2]);
    nng_send(s, line, strlen(line), 0);
    nng_socket_set_ms(s, NNG_OPT_RECVTIMEO, 200);
    nng_time end = nng_clock() + (nng_time)(atof(argv[3]) * 1000);
    while (nng_clock() < end) { char *b = NULL; size_t n; if (nng_recv(s, &b, &n, NNG_FLAG_ALLOC) == 0) { fwrite(b, 1, n, stdout); nng_free(b, n); } }
    nng_close(s); return 0;
}
