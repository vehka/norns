// see android_compat.h

#ifdef __ANDROID__

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#undef pthread_create

#define CANCEL_SIGNAL SIGUSR2

// One record per live thread created through norns_pthread_create(). A
// pthread_t says nothing about whether its thread still exists (bionic aborts
// in pthread_kill() on a dead one, and reuses the value for later threads),
// so cancellation only ever goes through this list: a thread is on it from
// before its start routine runs until its thread-specific data is destroyed.
struct cancel_rec {
    pthread_t thread;
    pid_t tid;
    bool cancelled;
    void *(*start)(void *);
    void *arg;
    struct cancel_rec *next;
};

static struct cancel_rec *recs = NULL;
static pthread_mutex_t recs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t rec_key;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

// the handler does nothing; its only job is to make blocking calls in the
// target thread return EINTR
static void cancel_signal_handler(int sig) {
    (void)sig;
}

// runs in the exiting thread, whether it returned or called pthread_exit()
static void rec_destroy(void *data) {
    struct cancel_rec *rec = (struct cancel_rec *)data;
    pthread_mutex_lock(&recs_lock);
    for (struct cancel_rec **p = &recs; *p; p = &(*p)->next) {
        if (*p == rec) {
            *p = rec->next;
            break;
        }
    }
    pthread_mutex_unlock(&recs_lock);
    free(rec);
}

static void cancel_init(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = cancel_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART
    sigaction(CANCEL_SIGNAL, &sa, NULL);
    pthread_key_create(&rec_key, rec_destroy);
}

static void *cancel_trampoline(void *data) {
    struct cancel_rec *rec = (struct cancel_rec *)data;
    // wait until the creator has filled in rec->thread and listed the record
    pthread_mutex_lock(&recs_lock);
    rec->tid = (pid_t)syscall(SYS_gettid);
    pthread_mutex_unlock(&recs_lock);
    pthread_setspecific(rec_key, rec);
    return rec->start(rec->arg);
}

int norns_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg) {
    pthread_once(&init_once, cancel_init);

    struct cancel_rec *rec = (struct cancel_rec *)calloc(1, sizeof(*rec));
    if (!rec) {
        return EAGAIN;
    }
    rec->start = start;
    rec->arg = arg;

    pthread_mutex_lock(&recs_lock);
    int ret = pthread_create(thread, attr, cancel_trampoline, rec);
    if (ret == 0) {
        rec->thread = *thread;
        rec->next = recs;
        recs = rec;
    }
    pthread_mutex_unlock(&recs_lock);
    if (ret != 0) {
        free(rec);
    }
    return ret;
}

int pthread_cancel(pthread_t thread) {
    int ret = ESRCH;
    pthread_mutex_lock(&recs_lock);
    for (struct cancel_rec *rec = recs; rec; rec = rec->next) {
        if (pthread_equal(rec->thread, thread)) {
            rec->cancelled = true;
            // the record is listed, so the thread has not finished exiting
            // and its kernel tid is still its own
            if (rec->tid != 0) {
                syscall(SYS_tgkill, getpid(), rec->tid, CANCEL_SIGNAL);
            }
            ret = 0;
            break;
        }
    }
    pthread_mutex_unlock(&recs_lock);
    return ret;
}

void pthread_testcancel(void) {
    pthread_once(&init_once, cancel_init);
    struct cancel_rec *rec = (struct cancel_rec *)pthread_getspecific(rec_key);
    if (!rec) {
        return;
    }
    pthread_mutex_lock(&recs_lock);
    bool cancelled = rec->cancelled;
    pthread_mutex_unlock(&recs_lock);
    if (cancelled) {
        pthread_exit(PTHREAD_CANCELED);
    }
}

#endif // __ANDROID__
