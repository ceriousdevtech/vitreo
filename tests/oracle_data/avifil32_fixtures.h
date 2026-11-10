/* avifil32_fixtures.h -- byte-exact AVI/WAVE test files for the avifil32
 * oracle probe (avifil32_probe.c, run on Windows) and Vitreo's
 * tests/winapi/media/test_avifil32.cpp. Both include this one file, so the
 * files Windows measured and the files Vitreo's test parses are identical.
 *
 * Portable C (C99 and C++): no Windows headers. Layouts follow the
 * "AVI RIFF File Reference" (Learn), vfw.h (MainAVIHeader, AVIStreamHeader,
 * AVIINDEXENTRY, AVIPALCHANGE) and aviriff.h (OpenDML AVISUPERINDEX,
 * AVISTDINDEX, AVIEXTHEADER).
 */
#ifndef VITREO_AVIFIL32_FIXTURES_H
#define VITREO_AVIFIL32_FIXTURES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    AVIFX_RGB24 = 0,    /* 5x3 24-bit + PCM audio, idx1 movi-relative, strn/strd */
    AVIFX_PAL8,         /* 6x4 8-bit 16 colours, idx1 absolute, ##pc change     */
    AVIFX_RLE8,         /* 8x4 BI_RLE8 key + delta frame                         */
    AVIFX_RGB16_NOIDX,  /* 4x2 RGB555, no idx1, frames inside 'rec ' lists       */
    AVIFX_ODML32,       /* 3x2 32-bit, indx/ix00 + RIFF AVIX, legacy idx1        */
    AVIFX_WAVE,         /* RIFF WAVE PCM16 mono 8000 Hz, 400 samples             */
    AVIFX_TEXT,         /* not a RIFF file                                       */
    AVIFX_RLE4,         /* 8x2 BI_RLE4 single frame                              */
    AVIFX_ODML_ONLY,    /* as ODML32 without idx1                                */
    AVIFX_MULTI,        /* 2x2 24-bit in '00dc' + 2 PCM + 'txts', header flags   */
    AVIFX_COUNT
};

static const char* const avifx_names[AVIFX_COUNT] = {
    "rgb24.avi", "pal8.avi", "rle8.avi", "rgb16.avi", "odml32.avi",
    "tone.wav",  "notavi.avi", "rle4.avi", "odmlonly.avi", "multi.avi",
};

typedef struct {
    uint8_t* p;
    size_t n;
    size_t cap;
} avifx_buf;

static void avifx_u8(avifx_buf* b, uint32_t v) {
    if (b->n < b->cap) b->p[b->n] = (uint8_t)v;
    b->n++;
}
static void avifx_u16(avifx_buf* b, uint32_t v) {
    avifx_u8(b, v & 0xFF);
    avifx_u8(b, (v >> 8) & 0xFF);
}
static void avifx_u32(avifx_buf* b, uint32_t v) {
    avifx_u16(b, v & 0xFFFF);
    avifx_u16(b, v >> 16);
}
static void avifx_put32_at(avifx_buf* b, size_t at, uint32_t v) {
    if (at + 4 <= b->cap) {
        b->p[at] = (uint8_t)v;
        b->p[at + 1] = (uint8_t)(v >> 8);
        b->p[at + 2] = (uint8_t)(v >> 16);
        b->p[at + 3] = (uint8_t)(v >> 24);
    }
}
static uint32_t avifx_fcc(const char* s) {
    return (uint32_t)(uint8_t)s[0] | ((uint32_t)(uint8_t)s[1] << 8) |
           ((uint32_t)(uint8_t)s[2] << 16) | ((uint32_t)(uint8_t)s[3] << 24);
}
static void avifx_fourcc(avifx_buf* b, const char* s) { avifx_u32(b, avifx_fcc(s)); }
static void avifx_bytes(avifx_buf* b, const uint8_t* d, size_t len) {
    size_t i;
    for (i = 0; i < len; ++i) avifx_u8(b, d[i]);
}
/* Open a LIST/RIFF: returns the offset of its size field. */
static size_t avifx_open(avifx_buf* b, const char* id, const char* type) {
    size_t at;
    avifx_fourcc(b, id);
    at = b->n;
    avifx_u32(b, 0);
    avifx_fourcc(b, type);
    return at;
}
static void avifx_close(avifx_buf* b, size_t size_at) {
    if (b->n & 1) avifx_u8(b, 0);
    avifx_put32_at(b, size_at, (uint32_t)(b->n - size_at - 4));
}
/* Chunk header + data + pad; returns the offset of the chunk header. */
static size_t avifx_chunk(avifx_buf* b, const char* id, const uint8_t* d, size_t len) {
    size_t at = b->n;
    avifx_fourcc(b, id);
    avifx_u32(b, (uint32_t)len);
    avifx_bytes(b, d, len);
    if (len & 1) avifx_u8(b, 0);
    return at;
}

static void avifx_avih(avifx_buf* b, uint32_t usec, uint32_t maxbps, uint32_t flags,
                       uint32_t frames, uint32_t streams, uint32_t sugg, uint32_t w,
                       uint32_t h) {
    avifx_fourcc(b, "avih");
    avifx_u32(b, 56);
    avifx_u32(b, usec);
    avifx_u32(b, maxbps);
    avifx_u32(b, 0);
    avifx_u32(b, flags);
    avifx_u32(b, frames);
    avifx_u32(b, 0);
    avifx_u32(b, streams);
    avifx_u32(b, sugg);
    avifx_u32(b, w);
    avifx_u32(b, h);
    avifx_u32(b, 0);
    avifx_u32(b, 0);
    avifx_u32(b, 0);
    avifx_u32(b, 0);
}

static void avifx_strh(avifx_buf* b, const char* type, uint32_t handler, uint32_t flags,
                       uint32_t scale, uint32_t rate, uint32_t start, uint32_t length,
                       uint32_t sugg, uint32_t quality, uint32_t sample_size, int16_t r,
                       int16_t bt) {
    avifx_fourcc(b, "strh");
    avifx_u32(b, 56);
    avifx_fourcc(b, type);
    avifx_u32(b, handler);
    avifx_u32(b, flags);
    avifx_u16(b, 0); /* wPriority */
    avifx_u16(b, 0); /* wLanguage */
    avifx_u32(b, 0); /* dwInitialFrames */
    avifx_u32(b, scale);
    avifx_u32(b, rate);
    avifx_u32(b, start);
    avifx_u32(b, length);
    avifx_u32(b, sugg);
    avifx_u32(b, quality);
    avifx_u32(b, sample_size);
    avifx_u16(b, 0);
    avifx_u16(b, 0);
    avifx_u16(b, (uint16_t)r);
    avifx_u16(b, (uint16_t)bt);
}

/* BITMAPINFOHEADER (40 bytes) + optional colour table. */
static void avifx_strf_bih(avifx_buf* b, int32_t w, int32_t h, uint16_t bpp, uint32_t comp,
                           uint32_t size_image, uint32_t clr_used, const uint8_t* rgbq) {
    avifx_fourcc(b, "strf");
    avifx_u32(b, 40 + clr_used * 4);
    avifx_u32(b, 40);
    avifx_u32(b, (uint32_t)w);
    avifx_u32(b, (uint32_t)h);
    avifx_u16(b, 1);
    avifx_u16(b, bpp);
    avifx_u32(b, comp);
    avifx_u32(b, size_image);
    avifx_u32(b, 0);
    avifx_u32(b, 0);
    avifx_u32(b, clr_used);
    avifx_u32(b, 0);
    if (clr_used) avifx_bytes(b, rgbq, clr_used * 4);
}

typedef struct {
    uint32_t id, flags, offset, size;
} avifx_idx;

static void avifx_idx1(avifx_buf* b, const avifx_idx* e, int count, uint32_t base) {
    int i;
    avifx_fourcc(b, "idx1");
    avifx_u32(b, (uint32_t)count * 16);
    for (i = 0; i < count; ++i) {
        avifx_u32(b, e[i].id);
        avifx_u32(b, e[i].flags);
        avifx_u32(b, e[i].offset - base);
        avifx_u32(b, e[i].size);
    }
}

static size_t avifx_rgb24(avifx_buf* b) {
    uint8_t frame[48], audio[100], strd[6] = {'v', 'i', 't', 'r', 'e', 'o'};
    avifx_idx idx[6];
    size_t riff, hdrl, strl, movi, info;
    uint32_t movi_base;
    int f, i, n = 0;
    riff = avifx_open(b, "RIFF", "AVI ");
    hdrl = avifx_open(b, "LIST", "hdrl");
    avifx_avih(b, 66666, 12000, 0x10 | 0x100, 3, 2, 112, 5, 3);
    strl = avifx_open(b, "LIST", "strl");
    avifx_strh(b, "vids", 0, 0, 1, 15, 0, 3, 48, 0xFFFFFFFFu, 0, 5, 3);
    avifx_strf_bih(b, 5, 3, 24, 0, 48, 0, NULL);
    avifx_chunk(b, "strd", strd, sizeof strd);
    avifx_chunk(b, "strn", (const uint8_t*)"Video #1", 9);
    avifx_chunk(b, "vtsd", (const uint8_t*)"streamx", 7);
    avifx_close(b, strl);
    strl = avifx_open(b, "LIST", "strl");
    avifx_strh(b, "auds", 0, 0, 1, 11025, 0, 300, 100, 0xFFFFFFFFu, 1, 0, 0);
    {
        /* PCMWAVEFORMAT: 16 bytes, no cbSize. */
        uint8_t wf[16] = {1, 0, 1, 0, 0x11, 0x2B, 0, 0, 0x11, 0x2B, 0, 0, 1, 0, 8, 0};
        avifx_chunk(b, "strf", wf, sizeof wf);
    }
    avifx_close(b, strl);
    avifx_chunk(b, "vtst", (const uint8_t*)"filedata", 8);
    avifx_close(b, hdrl);
    info = avifx_open(b, "LIST", "INFO");
    avifx_chunk(b, "ISFT", (const uint8_t*)"vitreo", 7);
    avifx_close(b, info);
    avifx_chunk(b, "JUNK", (const uint8_t*)"padpadpadpad", 12);
    movi = avifx_open(b, "LIST", "movi");
    movi_base = (uint32_t)(movi + 4);
    for (f = 0; f < 3; ++f) {
        for (i = 0; i < 48; ++i) frame[i] = (uint8_t)(f * 64 + i * 7);
        for (i = 0; i < 100; ++i) audio[i] = (uint8_t)(f * 100 + i);
        idx[n].id = avifx_fcc("00db");
        idx[n].flags = (f == 1) ? 0 : 0x10;
        idx[n].offset = (uint32_t)avifx_chunk(b, "00db", frame, 48);
        idx[n++].size = 48;
        idx[n].id = avifx_fcc("01wb");
        idx[n].flags = 0x10;
        idx[n].offset = (uint32_t)avifx_chunk(b, "01wb", audio, 100);
        idx[n++].size = 100;
    }
    avifx_close(b, movi);
    avifx_idx1(b, idx, n, movi_base);
    avifx_close(b, riff);
    return b->n;
}

static void avifx_pal16(uint8_t* rgbq) {
    int i;
    for (i = 0; i < 16; ++i) {
        rgbq[i * 4 + 0] = (uint8_t)(i * 8);        /* blue  */
        rgbq[i * 4 + 1] = (uint8_t)(255 - i * 16); /* green */
        rgbq[i * 4 + 2] = (uint8_t)(i * 16);       /* red   */
        rgbq[i * 4 + 3] = 0;
    }
}

static size_t avifx_pal8(avifx_buf* b) {
    uint8_t rgbq[64], frame[32];
    uint8_t pc[12] = {1, 2, 0, 0, 255, 0, 0, 0, 0, 0, 255, 0};
    avifx_idx idx[4];
    size_t riff, hdrl, strl, movi;
    int f, x, y, n = 0;
    avifx_pal16(rgbq);
    riff = avifx_open(b, "RIFF", "AVI ");
    hdrl = avifx_open(b, "LIST", "hdrl");
    avifx_avih(b, 100000, 320, 0x10, 3, 1, 32, 6, 4);
    strl = avifx_open(b, "LIST", "strl");
    avifx_strh(b, "vids", avifx_fcc("DIB "), 0x00010000u, 1, 10, 0, 3, 32, 7500, 0, 6, 4);
    avifx_strf_bih(b, 6, 4, 8, 0, 32, 16, rgbq);
    avifx_close(b, strl);
    avifx_close(b, hdrl);
    movi = avifx_open(b, "LIST", "movi");
    for (f = 0; f < 3; ++f) {
        if (f == 2) {
            idx[n].id = avifx_fcc("00pc");
            idx[n].flags = 0x100; /* AVIIF_NOTIME */
            idx[n].offset = (uint32_t)avifx_chunk(b, "00pc", pc, sizeof pc);
            idx[n++].size = sizeof pc;
        }
        memset(frame, 0, sizeof frame);
        for (y = 0; y < 4; ++y)
            for (x = 0; x < 6; ++x) frame[y * 8 + x] = (uint8_t)((x + y + f) & 15);
        idx[n].id = avifx_fcc("00db");
        idx[n].flags = 0x10;
        idx[n].offset = (uint32_t)avifx_chunk(b, "00db", frame, 32);
        idx[n++].size = 32;
    }
    avifx_close(b, movi);
    avifx_idx1(b, idx, n, 0); /* absolute file offsets */
    avifx_close(b, riff);
    return b->n;
}

static size_t avifx_rle8(avifx_buf* b) {
    static const uint8_t key[] = {
        8, 1, 0, 0,                   /* row 0: eight of colour 1, EOL       */
        0, 4, 2, 3, 2, 3, 4, 0, 0, 0, /* row 1: absolute 2 3 2 3, run 4x0    */
        2, 2, 6, 3, 0, 0,             /* row 2: 2x2, 6x3, EOL                */
        8, 0, 0, 1,                   /* row 3: 8x0, end of bitmap           */
    };
    static const uint8_t delta[] = {
        0, 2, 3, 1, /* delta: right 3, up 1 -> row 1, x 3 */
        2, 1,       /* two of colour 1                     */
        0, 1,       /* end of bitmap                       */
    };
    uint8_t rgbq[16] = {0, 0, 0, 0, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 0};
    avifx_idx idx[2];
    size_t riff, hdrl, strl, movi;
    uint32_t movi_base;
    riff = avifx_open(b, "RIFF", "AVI ");
    hdrl = avifx_open(b, "LIST", "hdrl");
    avifx_avih(b, 200000, 120, 0x10, 2, 1, 24, 8, 4);
    strl = avifx_open(b, "LIST", "strl");
    avifx_strh(b, "vids", avifx_fcc("mrle"), 0, 1, 5, 0, 2, 24, 0xFFFFFFFFu, 0, 8, 4);
    avifx_strf_bih(b, 8, 4, 8, 1, 32, 4, rgbq);
    avifx_close(b, strl);
    avifx_close(b, hdrl);
    movi = avifx_open(b, "LIST", "movi");
    movi_base = (uint32_t)(movi + 4);
    idx[0].id = avifx_fcc("00dc");
    idx[0].flags = 0x10;
    idx[0].offset = (uint32_t)avifx_chunk(b, "00dc", key, sizeof key);
    idx[0].size = sizeof key;
    idx[1].id = avifx_fcc("00dc");
    idx[1].flags = 0;
    idx[1].offset = (uint32_t)avifx_chunk(b, "00dc", delta, sizeof delta);
    idx[1].size = sizeof delta;
    avifx_close(b, movi);
    avifx_idx1(b, idx, 2, movi_base);
    avifx_close(b, riff);
    return b->n;
}

static size_t avifx_rle4(avifx_buf* b) {
    static const uint8_t data[] = {
        8, 0x12, 0, 0,             /* row 0: 1 2 1 2 1 2 1 2, EOL          */
        0, 5, 0x31, 0x23, 0x10, 0, /* row 1: absolute 3 1 2 3 1 (padded)   */
        3, 0x00, 0, 1,             /* three of colour 0, end of bitmap     */
    };
    uint8_t rgbq[16] = {0, 0, 0, 0, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 0};
    avifx_idx idx[1];
    size_t riff, hdrl, strl, movi;
    uint32_t movi_base;
    riff = avifx_open(b, "RIFF", "AVI ");
    hdrl = avifx_open(b, "LIST", "hdrl");
    avifx_avih(b, 200000, 80, 0x10, 1, 1, 16, 8, 2);
    strl = avifx_open(b, "LIST", "strl");
    avifx_strh(b, "vids", avifx_fcc("mrle"), 0, 1, 5, 0, 1, 16, 0xFFFFFFFFu, 0, 8, 2);
    avifx_strf_bih(b, 8, 2, 4, 2, 8, 4, rgbq);
    avifx_close(b, strl);
    avifx_close(b, hdrl);
    movi = avifx_open(b, "LIST", "movi");
    movi_base = (uint32_t)(movi + 4);
    idx[0].id = avifx_fcc("00dc");
    idx[0].flags = 0x10;
    idx[0].offset = (uint32_t)avifx_chunk(b, "00dc", data, sizeof data);
    idx[0].size = sizeof data;
    avifx_close(b, movi);
    avifx_idx1(b, idx, 1, movi_base);
    avifx_close(b, riff);
    return b->n;
}

static size_t avifx_rgb16(avifx_buf* b) {
    uint8_t frame[16];
    size_t riff, hdrl, strl, movi, rec;
    int f, i;
    riff = avifx_open(b, "RIFF", "AVI ");
    hdrl = avifx_open(b, "LIST", "hdrl");
    avifx_avih(b, 40000, 400, 0, 2, 1, 16, 4, 2);
    strl = avifx_open(b, "LIST", "strl");
    avifx_strh(b, "vids", 0, 0, 1, 25, 0, 2, 16, 0xFFFFFFFFu, 0, 4, 2);
    avifx_strf_bih(b, 4, 2, 16, 0, 16, 0, NULL);
    avifx_close(b, strl);
    avifx_close(b, hdrl);
    movi = avifx_open(b, "LIST", "movi");
    for (f = 0; f < 2; ++f) {
        for (i = 0; i < 8; ++i) {
            uint32_t px = (uint32_t)(f * 0x1111 + i * 0x0421) & 0x7FFF;
            frame[i * 2] = (uint8_t)px;
            frame[i * 2 + 1] = (uint8_t)(px >> 8);
        }
        rec = avifx_open(b, "LIST", "rec ");
        avifx_chunk(b, "00db", frame, 16);
        avifx_close(b, rec);
    }
    avifx_close(b, movi);
    avifx_close(b, riff);
    return b->n;
}

/* OpenDML: RIFF 'AVI ' (frames 0,1) + RIFF 'AVIX' (frames 2,3). Frames 1 and
 * 3 are delta frames (AVISTDINDEX_DELTAFRAME in their ix00 entries). */
static size_t avifx_odml(avifx_buf* b, int with_idx1) {
    uint8_t frame[24];
    avifx_idx idx[2];
    size_t riff, hdrl, strl, movi, odml, indx_at, ix_at[2], data_at[4];
    uint32_t movi_base = 0;
    int f, i, part;
    riff = avifx_open(b, "RIFF", "AVI ");
    hdrl = avifx_open(b, "LIST", "hdrl");
    avifx_avih(b, 50000, 480, with_idx1 ? 0x10 : 0, 2, 1, 24, 3, 2);
    strl = avifx_open(b, "LIST", "strl");
    avifx_strh(b, "vids", 0, 0, 1, 20, 0, 4, 24, 0xFFFFFFFFu, 0, 3, 2);
    avifx_strf_bih(b, 3, 2, 32, 0, 24, 0, NULL);
    /* indx: AVISUPERINDEX with two entries. */
    avifx_fourcc(b, "indx");
    avifx_u32(b, 24 + 2 * 16);
    indx_at = b->n;
    avifx_u16(b, 4);    /* wLongsPerEntry */
    avifx_u8(b, 0);     /* bIndexSubType */
    avifx_u8(b, 0x00);  /* AVI_INDEX_OF_INDEXES */
    avifx_u32(b, 2);    /* nEntriesInUse */
    avifx_fourcc(b, "00db");
    avifx_u32(b, 0);
    avifx_u32(b, 0);
    avifx_u32(b, 0);
    for (i = 0; i < 2 * 4; ++i) avifx_u32(b, 0); /* patched below */
    avifx_close(b, strl);
    odml = avifx_open(b, "LIST", "odml");
    avifx_fourcc(b, "dmlh");
    avifx_u32(b, 248);
    avifx_u32(b, 4);
    for (i = 0; i < 61; ++i) avifx_u32(b, 0);
    avifx_close(b, odml);
    avifx_close(b, hdrl);
    for (part = 0; part < 2; ++part) {
        if (part == 1) riff = avifx_open(b, "RIFF", "AVIX");
        movi = avifx_open(b, "LIST", "movi");
        if (part == 0) movi_base = (uint32_t)(movi + 4);
        for (f = part * 2; f < part * 2 + 2; ++f) {
            for (i = 0; i < 24; ++i) frame[i] = (uint8_t)(f * 40 + i * 3);
            data_at[f] = avifx_chunk(b, "00db", frame, 24);
        }
        /* ix00: AVISTDINDEX, base = this RIFF's movi list start. */
        ix_at[part] = b->n;
        avifx_fourcc(b, "ix00");
        avifx_u32(b, 24 + 2 * 8);
        avifx_u16(b, 2);
        avifx_u8(b, 0);
        avifx_u8(b, 0x01); /* AVI_INDEX_OF_CHUNKS */
        avifx_u32(b, 2);
        avifx_fourcc(b, "00db");
        avifx_u32(b, (uint32_t)movi); /* qwBaseOffset low */
        avifx_u32(b, 0);              /* qwBaseOffset high */
        avifx_u32(b, 0);              /* dwReserved_3 */
        for (f = part * 2; f < part * 2 + 2; ++f) {
            avifx_u32(b, (uint32_t)(data_at[f] + 8 - movi));
            avifx_u32(b, 24u | ((f & 1) ? 0x80000000u : 0u));
        }
        avifx_close(b, movi);
        if (part == 0 && with_idx1) {
            idx[0].id = avifx_fcc("00db");
            idx[0].flags = 0x10;
            idx[0].offset = (uint32_t)data_at[0];
            idx[0].size = 24;
            idx[1].id = avifx_fcc("00db");
            idx[1].flags = 0;
            idx[1].offset = (uint32_t)data_at[1];
            idx[1].size = 24;
            avifx_idx1(b, idx, 2, movi_base);
        }
        avifx_close(b, riff);
    }
    for (part = 0; part < 2; ++part) {
        size_t e = indx_at + 24 + (size_t)part * 16;
        avifx_put32_at(b, e, (uint32_t)ix_at[part]);
        avifx_put32_at(b, e + 4, 0);
        avifx_put32_at(b, e + 8, 8 + 24 + 2 * 8);
        avifx_put32_at(b, e + 12, 2);
    }
    return b->n;
}

static size_t avifx_wave(avifx_buf* b) {
    size_t riff;
    int i;
    riff = avifx_open(b, "RIFF", "WAVE");
    avifx_fourcc(b, "fmt ");
    avifx_u32(b, 16);
    avifx_u16(b, 1);
    avifx_u16(b, 1);
    avifx_u32(b, 8000);
    avifx_u32(b, 16000);
    avifx_u16(b, 2);
    avifx_u16(b, 16);
    avifx_fourcc(b, "data");
    avifx_u32(b, 800);
    for (i = 0; i < 400; ++i) avifx_u16(b, (uint32_t)(i * 37) & 0xFFFF);
    avifx_close(b, riff);
    return b->n;
}

/* Header passthrough: every avih flag, a disabled video stream with priority,
 * language and dwStart, BI_RGB frames stored as '00dc' with
 * frame 1 not flagged key, two audio streams (no strn) and a text stream. */
static size_t avifx_multi(avifx_buf* b) {
    uint8_t frame[16], audio[32], text[6];
    avifx_idx idx[12];
    size_t riff, hdrl, strl, movi;
    uint32_t movi_base;
    int f, i, n = 0;
    riff = avifx_open(b, "RIFF", "AVI ");
    hdrl = avifx_open(b, "LIST", "hdrl");
    avifx_avih(b, 125000, 999, 0x10 | 0x20 | 0x800 | 0x10000 | 0x20000, 3, 4, 777, 2, 2);
    strl = avifx_open(b, "LIST", "strl");
    avifx_fourcc(b, "strh");
    avifx_u32(b, 56);
    avifx_fourcc(b, "vids");
    avifx_fourcc(b, "abcd");
    avifx_u32(b, 0x00000001u); /* AVISF_DISABLED */
    avifx_u16(b, 3);           /* wPriority */
    avifx_u16(b, 9);           /* wLanguage */
    avifx_u32(b, 0);           /* dwInitialFrames */
    avifx_u32(b, 1);
    avifx_u32(b, 8);
    avifx_u32(b, 2);           /* dwStart */
    avifx_u32(b, 3);
    avifx_u32(b, 16);
    avifx_u32(b, 5000);
    avifx_u32(b, 0);
    avifx_u16(b, 1);
    avifx_u16(b, 2);
    avifx_u16(b, 3);
    avifx_u16(b, 4);
    avifx_strf_bih(b, 2, 2, 24, 0, 16, 0, NULL);
    avifx_close(b, strl);
    strl = avifx_open(b, "LIST", "strl");
    avifx_strh(b, "auds", 0, 0, 1, 8000, 0, 96, 32, 0xFFFFFFFFu, 1, 0, 0);
    {
        uint8_t wf[18] = {1, 0, 1, 0, 0x40, 0x1F, 0, 0, 0x40, 0x1F, 0, 0, 1, 0, 8, 0, 0, 0};
        avifx_chunk(b, "strf", wf, sizeof wf);
    }
    avifx_close(b, strl);
    strl = avifx_open(b, "LIST", "strl");
    avifx_strh(b, "auds", 0, 0, 4, 32000, 0, 24, 32, 0xFFFFFFFFu, 4, 0, 0);
    {
        uint8_t wf[16] = {1, 0, 2, 0, 0x40, 0x1F, 0, 0, 0, 0x7D, 0, 0, 4, 0, 16, 0};
        avifx_chunk(b, "strf", wf, sizeof wf);
    }
    avifx_close(b, strl);
    strl = avifx_open(b, "LIST", "strl");
    avifx_strh(b, "txts", avifx_fcc("txt "), 0, 1, 8, 0, 3, 6, 0xFFFFFFFFu, 0, 0, 0);
    avifx_chunk(b, "strf", (const uint8_t*)"TEXT", 4);
    avifx_close(b, strl);
    avifx_close(b, hdrl);
    movi = avifx_open(b, "LIST", "movi");
    movi_base = (uint32_t)(movi + 4);
    for (f = 0; f < 3; ++f) {
        for (i = 0; i < 16; ++i) frame[i] = (uint8_t)(f * 50 + i * 11);
        for (i = 0; i < 32; ++i) audio[i] = (uint8_t)(f * 32 + i);
        for (i = 0; i < 6; ++i) text[i] = (uint8_t)('a' + f + i);
        idx[n].id = avifx_fcc("00dc");
        idx[n].flags = (f == 1) ? 0 : 0x10;
        idx[n].offset = (uint32_t)avifx_chunk(b, "00dc", frame, 16);
        idx[n++].size = 16;
        idx[n].id = avifx_fcc("01wb");
        idx[n].flags = 0x10;
        idx[n].offset = (uint32_t)avifx_chunk(b, "01wb", audio, 32);
        idx[n++].size = 32;
        idx[n].id = avifx_fcc("02wb");
        idx[n].flags = 0x10;
        idx[n].offset = (uint32_t)avifx_chunk(b, "02wb", audio, 32);
        idx[n++].size = 32;
        idx[n].id = avifx_fcc("03tx");
        idx[n].flags = 0x10;
        idx[n].offset = (uint32_t)avifx_chunk(b, "03tx", text, 6);
        idx[n++].size = 6;
    }
    avifx_close(b, movi);
    avifx_idx1(b, idx, n, movi_base);
    avifx_close(b, riff);
    return b->n;
}

/* Build fixture @p which into @p out (capacity @p cap). Returns the file
 * size; a result larger than @p cap means the buffer was too small. */
static size_t avifx_build(int which, uint8_t* out, size_t cap) {
    avifx_buf b;
    b.p = out;
    b.n = 0;
    b.cap = cap;
    switch (which) {
    case AVIFX_RGB24: return avifx_rgb24(&b);
    case AVIFX_PAL8: return avifx_pal8(&b);
    case AVIFX_RLE8: return avifx_rle8(&b);
    case AVIFX_RGB16_NOIDX: return avifx_rgb16(&b);
    case AVIFX_ODML32: return avifx_odml(&b, 1);
    case AVIFX_WAVE: return avifx_wave(&b);
    case AVIFX_TEXT: {
        const char* t = "This is not a RIFF file at all.\r\n";
        avifx_bytes(&b, (const uint8_t*)t, strlen(t));
        return b.n;
    }
    case AVIFX_RLE4: return avifx_rle4(&b);
    case AVIFX_ODML_ONLY: return avifx_odml(&b, 0);
    case AVIFX_MULTI: return avifx_multi(&b);
    default: return 0;
    }
}

#endif /* VITREO_AVIFIL32_FIXTURES_H */
