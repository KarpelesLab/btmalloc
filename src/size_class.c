/*
 * size_class.c — size-class table, fast lookup, and per-class tuning.
 *
 * 36 classes, all multiples of 16 (so every slot is >= 16-byte aligned), from
 * 16 B to 16 KiB. Lookup is a single table index: size -> bucket -> class.
 */

#include "internal.h"

const uint32_t btm_sc_to_size[BTM_NUM_SIZE_CLASSES] = {
    16,    32,    48,    64,    80,    96,    112,   128,
    160,   192,   224,   256,
    320,   384,   448,   512,
    640,   768,   896,   1024,
    1280,  1536,  1792,  2048,
    2560,  3072,  3584,  4096,
    5120,  6144,  7168,  8192,
    10240, 12288, 14336, 16384,
};

/* Pages per slab, chosen so each slab holds >= ~7 slots (and >= 15 for the
 * common small classes), keeping the inline slab header's overhead small. */
const uint16_t btm_sc_run_pages[BTM_NUM_SIZE_CLASSES] = {
    1, 1, 1, 1, 1, 1, 1, 1,   /* <=128 */
    1, 1, 1, 1,               /* <=256 */
    2, 2, 2, 2,               /* 320..512 */
    4, 4, 4, 4,               /* 640..1024 */
    8, 8, 8, 8,               /* 1280..2048 */
    16, 16, 16, 16,           /* 2560..4096 */
    24, 24, 24, 24,           /* 5120..8192 */
    32, 32, 32, 32,           /* 10240..16384 */
};

/* size -> class lookup. Index by ceil(size/16)-1 = (size-1)>>4, range 0..1023. */
#define SC_LUT_ENTRIES (BTM_SMALL_MAX_SIZE / 16) /* 1024 */
static uint8_t sc_lut[SC_LUT_ENTRIES];

void btm_size_class_init(void) {
    for (size_t k = 0; k < SC_LUT_ENTRIES; k++) {
        size_t target = 16 * (k + 1); /* bucket max; all classes are mult of 16 */
        int sc = BTM_NUM_SIZE_CLASSES - 1;
        for (int i = 0; i < BTM_NUM_SIZE_CLASSES; i++) {
            if (btm_sc_to_size[i] >= target) { sc = i; break; }
        }
        sc_lut[k] = (uint8_t)sc;
    }
}

int btm_size_to_sc(size_t size) {
    if (BTM_UNLIKELY(size == 0)) return -1;
    if (BTM_UNLIKELY(size > BTM_SMALL_MAX_SIZE)) return -1;
    return sc_lut[(size - 1) >> 4];
}

/* Per-class TLS cache cap: keep roughly 16 KiB cached per bin, but at least 8
 * and at most 256 slots. Small classes cache deeply (hot), large shallowly. */
unsigned btm_cache_max(int sc) {
    size_t sz = btm_sc_to_size[sc];
    unsigned m = (unsigned)(16384 / sz);
    if (m < 8) m = 8;
    if (m > 256) m = 256;
    return m;
}
