/* SPDX-License-Identifier: MIT
 * Frozen Shared ABI Contract between EL0 filmframe.wlm and EL1 z8_film_frame.
 * Keep /Users/ycx/Nikon/build-tools/livediag/feat-film-frame/include/z8_film_frame_abi.h
 * and /Users/ycx/Nikon/fenestra-z8/modules/filmframe/filmframe_abi.h byte-identical.
 */
#ifndef Z8_FILM_FRAME_ABI_H
#define Z8_FILM_FRAME_ABI_H

#include <stdint.h>

#define Z8_FILM_FRAME_ABI          1U
#define Z8_FILM_FRAME_CFG_MAGIC    UINT32_C(0x314d465a) /* 'ZFM1' */
#define Z8_FILM_FRAME_CFG_VERSION  1U
#define Z8_FILM_FRAME_CFG_BYTES    64U

#define Z8_FILM_FRAME_BINDING_NAME "film.frame"
#define Z8_FILM_FRAME_BINDING_VER  1U

#define Z8_FILM_FRAME_CFG_PATH_0   "A:\\FENESTRA\\FLMFRM0.CFG"
#define Z8_FILM_FRAME_CFG_PATH_1   "A:\\FENESTRA\\FLMFRM1.CFG"

enum Z8FilmFrameOp {
    Z8_FF_OP_GET    = 1U,
    Z8_FF_OP_SET    = 2U,
    Z8_FF_OP_STATUS = 3U
};

enum Z8XpanMode {
    Z8_XPAN_OFF          = 0U, /* Native 3:2 aspect ratio */
    Z8_XPAN_MODE_A_MATTE = 1U, /* 3:2 canvas with centered 65:24 XPan window / rebate */
    Z8_XPAN_MODE_B_CROP  = 2U  /* True 65:24 pixel crop + optional rebate + NEF crop */
};

enum Z8BorderStyle {
    Z8_BORDER_OFF          = 0U,
    Z8_BORDER_SPROCKET_LIT = 1U, /* 35mm Rebate (Light-Table Sprockets) */
    Z8_BORDER_SPROCKET_DRK = 2U  /* 35mm Rebate (Dark-Platen Sprockets) */
};

enum Z8EdgeLabel {
    Z8_EDGE_XPAN_6524 = 0U, /* "XPAN 65:24" */
    Z8_EDGE_KODAK_400 = 1U, /* "KODAK 400" */
    Z8_EDGE_FUJI_400  = 2U, /* "FUJI 400" */
    Z8_EDGE_NIKON_Z8  = 3U  /* "NIKON Z8" */
};

enum Z8DateFormat {
    Z8_DATE_OFF       = 0U,
    Z8_DATE_YY_MM_DD  = 1U, /* 'YY MM DD */
    Z8_DATE_YYMMDD_HM = 2U, /* 'YY MM DD HH:MM */
    Z8_DATE_MM_DD_YY  = 3U, /* MM DD 'YY */
    Z8_DATE_DD_MM_YY  = 4U  /* DD MM 'YY */
};

enum Z8DateStyle {
    Z8_DATE_STYLE_CLASSIC = 0U, /* Italic 6-deg 7-Segment Quartz */
    Z8_DATE_STYLE_GR1S    = 1U  /* GR1s 5x7 Dot-Matrix */
};

enum Z8DateGlow {
    Z8_DATE_GLOW_OFF = 0U,
    Z8_DATE_GLOW_ON  = 1U
};

enum Z8VfGuide {
    Z8_VF_GUIDE_OFF   = 0U,
    Z8_VF_GUIDE_DIM50 = 1U, /* 50% Dim Matte */
    Z8_VF_GUIDE_BLK   = 2U, /* 100% Black Matte + Frame */
    Z8_VF_GUIDE_LINES = 3U  /* 2px Frame Lines */
};

enum Z8LiveLink {
    Z8_LIVE_LINK_OFF = 0U,
    Z8_LIVE_LINK_ON  = 1U
};

enum Z8FilmOrientation {
    Z8_ORIENT_LANDSCAPE = 0U, /* 0 deg landscape */
    Z8_ORIENT_CCW90     = 1U, /* 90 deg CCW / EXIF 8 */
    Z8_ORIENT_CW90      = 2U  /* 90 deg CW / EXIF 6 */
};

typedef struct {
    uint32_t magic;        /* Z8_FILM_FRAME_CFG_MAGIC ('ZFM1') */
    uint16_t version;      /* Z8_FILM_FRAME_CFG_VERSION (1) */
    uint16_t bytes;        /* sizeof(Z8FilmFrameConfig) == 64 */
    uint32_t seq;          /* Monotonic save generation for dual-slot selection */
    uint32_t crc32;        /* IEEE 802.3 CRC32 over 64B with crc32 field zeroed */
    uint8_t  xpan_mode;    /* enum Z8XpanMode (0..2) */
    uint8_t  border_style; /* enum Z8BorderStyle (0..2) */
    uint8_t  edge_label;   /* enum Z8EdgeLabel (0..3) */
    uint8_t  date_format;  /* enum Z8DateFormat (0..4) */
    uint8_t  date_style;   /* enum Z8DateStyle (0..1) */
    uint8_t  date_glow;    /* enum Z8DateGlow (0..1) */
    uint8_t  vf_guide;     /* enum Z8VfGuide (0..3) */
    uint8_t  live_link;    /* enum Z8LiveLink (0..1) */
    uint8_t  reserved[40]; /* Must be 0 */
} Z8FilmFrameConfig;

_Static_assert(sizeof(Z8FilmFrameConfig) == Z8_FILM_FRAME_CFG_BYTES,
               "Z8FilmFrameConfig must be 64 bytes");

typedef struct {
    uint16_t year;        /* 2000..2099 */
    uint8_t  month;       /* 1..12 */
    uint8_t  day;         /* 1..31 */
    uint8_t  hour;        /* 0..23 */
    uint8_t  minute;      /* 0..59 */
    uint8_t  second;      /* 0..59 */
    uint8_t  orientation; /* enum Z8FilmOrientation (0..2) */
} Z8FilmTimestamp;

_Static_assert(sizeof(Z8FilmTimestamp) == 8U,
               "Z8FilmTimestamp must be 8 bytes");

#endif /* Z8_FILM_FRAME_ABI_H */
