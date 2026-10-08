// Stand-in for avahi's dns_sd compatibility header, which Termux does not
// package. Covers only what matron/src/osc.cc uses; registration reports
// failure, so norns simply isn't advertised over mDNS.
#pragma once

#include <stdint.h>

typedef struct _DNSServiceRef_t *DNSServiceRef;
typedef uint32_t DNSServiceFlags;
typedef int32_t DNSServiceErrorType;
typedef void *DNSServiceRegisterReply;

#define kDNSServiceErr_NoError 0
#define kDNSServiceErr_Unsupported (-65544)

static inline DNSServiceErrorType DNSServiceRegister(DNSServiceRef *ref, DNSServiceFlags flags,
                                                     uint32_t interface_index, const char *name,
                                                     const char *regtype, const char *domain,
                                                     const char *host, uint16_t port, uint16_t txt_len,
                                                     const void *txt_record,
                                                     DNSServiceRegisterReply callback, void *context) {
    (void)flags;
    (void)interface_index;
    (void)name;
    (void)regtype;
    (void)domain;
    (void)host;
    (void)port;
    (void)txt_len;
    (void)txt_record;
    (void)callback;
    (void)context;
    if (ref) {
        *ref = 0;
    }
    return kDNSServiceErr_Unsupported;
}

static inline void DNSServiceRefDeallocate(DNSServiceRef ref) {
    (void)ref;
}
