#ifndef LIVE_EVF_DIAG_H
#define LIVE_EVF_DIAG_H
/* Four immutable samples, published once. No callback allocation or file IO. */
#define EVF_DIAG_SAMPLES 4u
#define EVF_DIAG_WORDS 24u
struct evf_diag_sample { uint64_t timestamp_us; uint32_t words[EVF_DIAG_WORDS]; };
struct evf_diag {
    uint64_t next;
    _Atomic uint32_t draws,result,submits,published;
    uint32_t used,drained;
    struct evf_diag_sample samples[EVF_DIAG_SAMPLES];
};
#endif
