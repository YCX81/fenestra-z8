/* SPDX-License-Identifier: MIT
 * Dual-slot CRC32 card-selection config for slotcard.wlm (next-boot strict mode).
 * Freestanding C11, zero libc dependencies, zero pointer relocations.
 */
#ifndef SLOTCARD_CFG_H
#define SLOTCARD_CFG_H

#include <stddef.h>
#include <stdint.h>
#include "slotcard.h"

#define SLTCARD_CFG_MAGIC       UINT32_C(0x3143535a) /* 'ZSC1' */
#define SLTCARD_CFG_VERSION     1u
#define SLTCARD_CFG_BYTES       32u
#define SLTCARD_CFG_READ_MAX    33u

#define SLTCARD_SEL_MAGIC       UINT32_C(0x314c535a) /* 'ZSL1' */
#define SLTCARD_SEL_VERSION     1u
#define SLTCARD_SEL_BYTES       64u
#define SLTCARD_SEL_READ_MAX    65u

#define SLTCARD_OPEN_NOT_FOUND  UINT32_C(0x80000010)
#define SLTCARD_OPEN_REFUSED    UINT32_C(0x8000ff01)
#define SLTCARD_DIR_REFUSED     UINT32_C(0x0000ff01)
#define SLTCARD_OPEN_RDONLY     UINT32_C(0x00000000)
#define SLTCARD_OPEN_CREATE_EX  UINT32_C(0x00000101)
#define SLTCARD_OPEN_TRUNC_WR   UINT32_C(0x00000301)

/* cardslot.c runtime latch in C64 reservation tail (read-only probe). */
#define SLTCARD_CS_ADDR         UINT64_C(0xa1171b000)
#define SLTCARD_CS_MAGIC        UINT32_C(0x314c5343) /* 'CSL1' */

enum {
    SC_DRIVE_AUTO = 0u, /* Follow Fenestra boot card via card.files */
    SC_DRIVE_A    = 1u, /* Strict CFexpress (A:\FENESTRA\sc*.rec) */
    SC_DRIVE_B    = 2u  /* Strict SD        (B:\FENESTRA\sc*.rec) */
};

enum {
    SC_MISS_NONE          = 0u,
    SC_MISS_TIMEOUT       = 1u, /* Target card did not mount within wait window */
    SC_MISS_NO_FENESTRA   = 2u, /* Target card mounted, but lacks \FENESTRA dir */
    SC_MISS_BOOTED_SD     = 3u, /* Requested A, but cardslot.c latched drive=2 (SD) */
    SC_MISS_NO_NATIVE_IO  = 4u  /* Native card/memory bindings unavailable */
};

typedef struct {
    uint32_t magic;        /* SLTCARD_CFG_MAGIC ('ZSC1') */
    uint16_t version;      /* SLTCARD_CFG_VERSION (1) */
    uint16_t bytes;        /* SLTCARD_CFG_BYTES (32) */
    uint32_t seq;          /* Monotonic save generation for dual-slot selection */
    uint32_t crc32;        /* IEEE 802.3 CRC32 over 32B with crc32 field zeroed */
    uint8_t  drive_mode;   /* SC_DRIVE_AUTO / SC_DRIVE_A / SC_DRIVE_B */
    uint8_t  reserved[15]; /* Must be 0 */
} SlotCardDriveCfg;

_Static_assert(sizeof(SlotCardDriveCfg) == SLTCARD_CFG_BYTES,
               "SlotCardDriveCfg must be 32 bytes");
_Static_assert(offsetof(SlotCardDriveCfg, magic) == 0u, "cfg.magic offset");
_Static_assert(offsetof(SlotCardDriveCfg, version) == 4u, "cfg.version offset");
_Static_assert(offsetof(SlotCardDriveCfg, bytes) == 6u, "cfg.bytes offset");
_Static_assert(offsetof(SlotCardDriveCfg, seq) == 8u, "cfg.seq offset");
_Static_assert(offsetof(SlotCardDriveCfg, crc32) == 12u, "cfg.crc32 offset");
_Static_assert(offsetof(SlotCardDriveCfg, drive_mode) == 16u, "cfg.drive_mode offset");
_Static_assert(offsetof(SlotCardDriveCfg, reserved) == 17u, "cfg.reserved offset");

static inline uint32_t sc_cfg_crc32(const SlotCardDriveCfg *c) {
    uint8_t raw[SLTCARD_CFG_BYTES];
    const uint8_t *src = (const uint8_t *)c;
    for (uint32_t i = 0u; i < SLTCARD_CFG_BYTES; ++i) {
        raw[i] = src[i];
    }
    raw[12] = 0u;
    raw[13] = 0u;
    raw[14] = 0u;
    raw[15] = 0u;
    return sc_crc32(0u, raw, SLTCARD_CFG_BYTES);
}

static inline void sc_cfg_finalize(SlotCardDriveCfg *c, uint32_t seq, uint8_t drive_mode) {
    uint8_t *p = (uint8_t *)c;
    for (uint32_t i = 0u; i < SLTCARD_CFG_BYTES; ++i) {
        p[i] = 0u;
    }
    c->magic = SLTCARD_CFG_MAGIC;
    c->version = SLTCARD_CFG_VERSION;
    c->bytes = SLTCARD_CFG_BYTES;
    c->seq = seq;
    c->drive_mode = drive_mode <= SC_DRIVE_B ? drive_mode : SC_DRIVE_AUTO;
    c->crc32 = sc_cfg_crc32(c);
}

static inline int sc_cfg_valid(const SlotCardDriveCfg *c) {
    if (!c) {
        return 0;
    }
    if (c->magic != SLTCARD_CFG_MAGIC ||
        c->version != SLTCARD_CFG_VERSION ||
        c->bytes != SLTCARD_CFG_BYTES ||
        c->drive_mode > SC_DRIVE_B) {
        return 0;
    }
    for (uint32_t i = 0u; i < (uint32_t)sizeof(c->reserved); ++i) {
        if (c->reserved[i] != 0u) {
            return 0;
        }
    }
    return c->crc32 == sc_cfg_crc32(c);
}

static inline void sc_cfg_select_dual(const SlotCardDriveCfg *s0, int v0,
                                      const SlotCardDriveCfg *s1, int v1,
                                      SlotCardDriveCfg *out_cfg,
                                      uint32_t *out_active_slot) {
    if (v0 && v1) {
        if (s1->seq > s0->seq) {
            *out_cfg = *s1;
            *out_active_slot = 1u;
        } else {
            *out_cfg = *s0;
            *out_active_slot = 0u;
        }
    } else if (v0) {
        *out_cfg = *s0;
        *out_active_slot = 0u;
    } else if (v1) {
        *out_cfg = *s1;
        *out_active_slot = 1u;
    } else {
        sc_cfg_finalize(out_cfg, 0u, SC_DRIVE_AUTO);
        *out_active_slot = 1u; /* First save targets slot 0 */
    }
}

static inline char sc_drive_letter(uint32_t drive_mode) {
    return drive_mode == SC_DRIVE_B ? 'B' : 'A';
}

static inline void sc_cfg_path(uint32_t slot, char out[64]) {
    const char *src = slot == 0u ? "A:\\FENESTRA\\SLTCARD0.CFG"
                                 : "A:\\FENESTRA\\SLTCARD1.CFG";
    uint32_t i = 0u;
    while (src[i] && i < 63u) {
        out[i] = src[i];
        ++i;
    }
    while (i < 64u) {
        out[i++] = 0;
    }
}

static inline void sc_root_dir_path(uint32_t drive_mode, char out[64]) {
    for (uint32_t i = 0u; i < 64u; ++i) {
        out[i] = 0;
    }
    out[0] = sc_drive_letter(drive_mode);
    out[1] = ':';
    out[2] = '\\';
}

static inline void sc_fenestra_dir_path(uint32_t drive_mode, char out[64]) {
    const char *tail = ":\\FENESTRA";
    for (uint32_t i = 0u; i < 64u; ++i) {
        out[i] = 0;
    }
    out[0] = sc_drive_letter(drive_mode);
    for (uint32_t i = 0u; tail[i]; ++i) {
        out[1u + i] = tail[i];
    }
}

static inline void sc_gen_path(uint32_t drive_mode, uint32_t gen, char out[64]) {
    const char *tail = ":\\FENESTRA\\";
    char name[16];
    sc_name(gen, name);
    for (uint32_t i = 0u; i < 64u; ++i) {
        out[i] = 0;
    }
    out[0] = sc_drive_letter(drive_mode);
    uint32_t at = 1u;
    for (uint32_t i = 0u; tail[i]; ++i) {
        out[at++] = tail[i];
    }
    for (uint32_t i = 0u; name[i] && at < 63u; ++i) {
        out[at++] = name[i];
    }
}

/* ── Dual-slot CRC32 selection persistence (SCSEL0.CFG / SCSEL1.CFG, 64 B) ── */
typedef struct {
    uint32_t magic;        /* 0:  SLTCARD_SEL_MAGIC ('ZSL1') */
    uint16_t version;      /* 4:  SLTCARD_SEL_VERSION (1) */
    uint16_t bytes;        /* 6:  SLTCARD_SEL_BYTES (64) */
    uint32_t seq;          /* 8:  Monotonic save generation for dual-slot A/B */
    uint32_t crc32;        /* 12: IEEE 802.3 CRC32 over 64B with crc32 field zeroed */
    uint32_t base_gen;     /* 16: SC*.REC head generation this selection builds on */
    uint32_t flags;        /* 20: Reserved flags (must be 0 in v1) */
    uint64_t store_id;     /* 24: Matching SC*.REC store_id (0 if empty store) */
    uint32_t still_sel;    /* 32: Photo Picture Control selector (selection[0]) */
    uint32_t video_sel;    /* 36: Video Picture Control selector (selection[1]) */
    uint32_t still_proxy;  /* 40: Stock-legal photo shutdown proxy (proxy[0]) */
    uint32_t video_proxy;  /* 44: Stock-legal video shutdown proxy (proxy[1]) */
    uint8_t  reserved[16]; /* 48: Must be 0 */
} SlotCardSelCfg;

_Static_assert(sizeof(SlotCardSelCfg) == SLTCARD_SEL_BYTES,
               "SlotCardSelCfg must be 64 bytes");
_Static_assert(offsetof(SlotCardSelCfg, magic) == 0u, "sel.magic offset");
_Static_assert(offsetof(SlotCardSelCfg, version) == 4u, "sel.version offset");
_Static_assert(offsetof(SlotCardSelCfg, bytes) == 6u, "sel.bytes offset");
_Static_assert(offsetof(SlotCardSelCfg, seq) == 8u, "sel.seq offset");
_Static_assert(offsetof(SlotCardSelCfg, crc32) == 12u, "sel.crc32 offset");
_Static_assert(offsetof(SlotCardSelCfg, base_gen) == 16u, "sel.base_gen offset");
_Static_assert(offsetof(SlotCardSelCfg, flags) == 20u, "sel.flags offset");
_Static_assert(offsetof(SlotCardSelCfg, store_id) == 24u, "sel.store_id offset");
_Static_assert(offsetof(SlotCardSelCfg, still_sel) == 32u, "sel.still_sel offset");
_Static_assert(offsetof(SlotCardSelCfg, video_sel) == 36u, "sel.video_sel offset");
_Static_assert(offsetof(SlotCardSelCfg, still_proxy) == 40u, "sel.still_proxy offset");
_Static_assert(offsetof(SlotCardSelCfg, video_proxy) == 44u, "sel.video_proxy offset");
_Static_assert(offsetof(SlotCardSelCfg, reserved) == 48u, "sel.reserved offset");

static inline uint32_t sc_sel_crc32(const SlotCardSelCfg *c) {
    uint8_t raw[SLTCARD_SEL_BYTES];
    const uint8_t *src = (const uint8_t *)c;
    for (uint32_t i = 0u; i < SLTCARD_SEL_BYTES; ++i) {
        raw[i] = src[i];
    }
    raw[12] = 0u;
    raw[13] = 0u;
    raw[14] = 0u;
    raw[15] = 0u;
    return sc_crc32(0u, raw, SLTCARD_SEL_BYTES);
}

static inline void sc_sel_finalize(SlotCardSelCfg *c, uint32_t seq, uint32_t base_gen,
                                   uint64_t store_id, uint32_t still_sel,
                                   uint32_t video_sel, uint32_t still_proxy,
                                   uint32_t video_proxy) {
    uint8_t *p = (uint8_t *)c;
    for (uint32_t i = 0u; i < SLTCARD_SEL_BYTES; ++i) {
        p[i] = 0u;
    }
    c->magic = SLTCARD_SEL_MAGIC;
    c->version = SLTCARD_SEL_VERSION;
    c->bytes = SLTCARD_SEL_BYTES;
    c->seq = seq;
    c->base_gen = base_gen;
    c->flags = 0u;
    c->store_id = store_id;
    c->still_sel = still_sel;
    c->video_sel = video_sel;
    c->still_proxy = still_proxy;
    c->video_proxy = video_proxy;
    c->crc32 = sc_sel_crc32(c);
}

static inline int sc_sel_valid(const SlotCardSelCfg *c) {
    if (!c) {
        return 0;
    }
    if (c->magic != SLTCARD_SEL_MAGIC ||
        c->version != SLTCARD_SEL_VERSION ||
        c->bytes != SLTCARD_SEL_BYTES ||
        c->flags != 0u) {
        return 0;
    }
    for (uint32_t i = 0u; i < (uint32_t)sizeof(c->reserved); ++i) {
        if (c->reserved[i] != 0u) {
            return 0;
        }
    }
    return c->crc32 == sc_sel_crc32(c);
}

static inline void sc_sel_select_dual(const SlotCardSelCfg *s0, int v0,
                                      const SlotCardSelCfg *s1, int v1,
                                      SlotCardSelCfg *out_sel,
                                      uint32_t *out_active_slot,
                                      uint32_t *out_has_valid) {
    if (v0 && v1) {
        if (s1->seq > s0->seq) {
            *out_sel = *s1;
            *out_active_slot = 1u;
        } else {
            *out_sel = *s0;
            *out_active_slot = 0u;
        }
        *out_has_valid = 1u;
    } else if (v0) {
        *out_sel = *s0;
        *out_active_slot = 0u;
        *out_has_valid = 1u;
    } else if (v1) {
        *out_sel = *s1;
        *out_active_slot = 1u;
        *out_has_valid = 1u;
    } else {
        sc_sel_finalize(out_sel, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
        *out_active_slot = 1u; /* First save targets slot 0 (SCSEL0.CFG) */
        *out_has_valid = 0u;
    }
}

static inline void sc_sel_path(uint32_t drive_mode, uint32_t slot, char out[64]) {
    const char *tail = slot == 0u ? ":\\FENESTRA\\SCSEL0.CFG"
                                  : ":\\FENESTRA\\SCSEL1.CFG";
    for (uint32_t i = 0u; i < 64u; ++i) {
        out[i] = 0;
    }
    out[0] = sc_drive_letter(drive_mode);
    for (uint32_t i = 0u; tail[i] && (1u + i) < 63u; ++i) {
        out[1u + i] = tail[i];
    }
}

#endif /* SLOTCARD_CFG_H */
