// see android_compat.h

#ifdef __ANDROID__

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#define CANCEL_SIGNAL SIGUSR2
#define CANCEL_SLOTS 64
// a flagged thread is woken immediately, so it either reaches
// pthread_testcancel() promptly or never will. expire stale requests so they
// can't hit an unrelated thread that later reuses the same pthread_t.
#define CANCEL_EXPIRE_SEC 5

struct cancel_slot {
    bool used;
    pthread_t tid;
    time_t when;
};

static struct cancel_slot slots[CANCEL_SLOTS];
static pthread_mutex_t slots_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t handler_once = PTHREAD_ONCE_INIT;

static time_t now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

// the handler does nothing; its only job is to make blocking calls in the
// target thread return EINTR
static void cancel_signal_handler(int sig) {
    (void)sig;
}

static void install_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = cancel_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART
    sigaction(CANCEL_SIGNAL, &sa, NULL);
}

int pthread_cancel(pthread_t thread) {
    pthread_once(&handler_once, install_handler);

    time_t now = now_sec();
    int free_idx = -1;
    bool found = false;

    pthread_mutex_lock(&slots_lock);
    for (int i = 0; i < CANCEL_SLOTS; ++i) {
        if (slots[i].used && (now - slots[i].when) > CANCEL_EXPIRE_SEC) {
            slots[i].used = false;
        }
        if (slots[i].used && pthread_equal(slots[i].tid, thread)) {
            slots[i].when = now;
            found = true;
        }
        if (!slots[i].used && free_idx < 0) {
            free_idx = i;
        }
    }
    if (!found && free_idx >= 0) {
        slots[free_idx].used = true;
        slots[free_idx].tid = thread;
        slots[free_idx].when = now;
        found = true;
    }
    pthread_mutex_unlock(&slots_lock);

    if (!found) {
        return EAGAIN;
    }
    return pthread_kill(thread, CANCEL_SIGNAL);
}

void pthread_testcancel(void) {
    pthread_t self = pthread_self();
    time_t now = now_sec();
    bool cancelled = false;

    pthread_mutex_lock(&slots_lock);
    for (int i = 0; i < CANCEL_SLOTS; ++i) {
        if (slots[i].used && pthread_equal(slots[i].tid, self)) {
            slots[i].used = false;
            cancelled = (now - slots[i].when) <= CANCEL_EXPIRE_SEC;
            break;
        }
    }
    pthread_mutex_unlock(&slots_lock);

    if (cancelled) {
        pthread_exit(PTHREAD_CANCELED);
    }
}

#endif // __ANDROID__
