// Android (bionic) compatibility shim, force-included into every C++ unit of
// the norns binary by norns/wscript when building under Termux.
//
// bionic has no thread cancellation. matron uses pthread_cancel() to stop
// metro, device and polling threads, so emulate *deferred* cancellation:
// pthread_cancel() flags the target and interrupts whatever blocking call it
// is in; the target exits the next time it reaches pthread_testcancel().
// Unlike glibc, blocking calls are not cancellation points by themselves, so
// a loop that should be cancellable has to call pthread_testcancel().
#pragma once

#ifdef __ANDROID__

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PTHREAD_CANCELED
#define PTHREAD_CANCELED ((void *)-1)
#endif

int pthread_cancel(pthread_t thread);
void pthread_testcancel(void);

#ifdef __cplusplus
}
#endif

#endif // __ANDROID__
