/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Our gdi32.dll (docs/abi/windows.md W8 step 6).
 *
 * GDI's objects without a display: pens, brushes, bitmaps, memory device
 * contexts and the stock objects, kept in one handle table, with selection
 * into a DC and GetObject answering what was created. Nothing is drawn - a
 * call that would put pixels anywhere fails with ERROR_CALL_NOT_IMPLEMENTED -
 * but a bitmap holds its bits, so Get/SetBitmapBits round-trip.
 *
 * A handle is Windows' shape: the table index in the low 16 bits, the object
 * type in bits 16-22, and 0x00800000 for a stock object, which DeleteObject
 * accepts and ignores. Exports are named in gdi32.def. */

#include "vibeos_ntdll.h"

typedef int32_t BOOL32;

#define ERROR_INVALID_HANDLE       6u
#define ERROR_NOT_ENOUGH_MEMORY    8u
#define ERROR_INVALID_PARAMETER    87u
#define ERROR_CALL_NOT_IMPLEMENTED 120u

#define OBJ_PEN     1u
#define OBJ_BRUSH   2u
#define OBJ_DC      3u
#define OBJ_PAL     5u
#define OBJ_FONT    6u
#define OBJ_BITMAP  7u
#define OBJ_MEMDC   10u

#define STOCK_FLAG  0x00800000u
#define TYPE_SHIFT  16u

#define BS_SOLID  0u
#define BS_NULL   1u
#define BS_HATCHED 2u
#define PS_SOLID  0u
#define PS_NULL   5u
#define PS_STYLE_MASK 0xFu

#define WHITE_BRUSH   0
#define LTGRAY_BRUSH  1
#define GRAY_BRUSH    2
#define DKGRAY_BRUSH  3
#define BLACK_BRUSH   4
#define NULL_BRUSH    5
#define WHITE_PEN     6
#define BLACK_PEN     7
#define NULL_PEN      8
#define DEFAULT_PALETTE 15
#define DC_BRUSH      18
#define DC_PEN        19
#define STOCK_LAST    19
#define DEFAULT_BITMAP 20   /* not GetStockObject's: what a memory DC starts with */

#define SLOTS 1024u

typedef struct {
    uint32_t style;
    uint32_t color;
    uint64_t hatch;
} logbrush_t;   /* LOGBRUSH */

typedef struct {
    uint32_t style;
    int32_t width_x;
    int32_t width_y;
    uint32_t color;
} logpen_t;   /* LOGPEN */

typedef struct {
    int32_t type;
    int32_t width;
    int32_t height;
    int32_t width_bytes;
    uint16_t planes;
    uint16_t bpp;
    uint32_t pad;
    void *bits;
} bitmap_t;   /* BITMAP */

typedef struct {
    uint64_t brush, pen, bitmap, palette;
    int32_t bk_mode;
    uint32_t bk_color, text_color;
    int32_t org_x, org_y;
    int32_t char_extra;
} dc_t;

typedef struct {
    uint32_t type;   /* zero: free */
    uint16_t gen;
    union {
        logbrush_t brush;
        logpen_t pen;
        bitmap_t bitmap;
        dc_t dc;
    } u;
} obj_t;

static obj_t g_obj[SLOTS];
static obj_t g_stock[DEFAULT_BITMAP + 1];
static volatile int g_lock;

static void set_error(uint32_t e) {
    __asm__ __volatile__("movl %0, %%gs:%c1" : : "r"(e), "i"(VIBEOS_TEB_LAST_ERROR) : "memory");
}

static void lock(void) {
    while (__sync_lock_test_and_set(&g_lock, 1)) {
        __asm__ __volatile__("pause");
    }
}

static void unlock(void) { __sync_lock_release(&g_lock); }

/* A null handle is a failure that sets no error, as Windows answers: only a
 * handle that names nothing is ERROR_INVALID_HANDLE. */
static void bad_handle(uint64_t h) {
    if (h) {
        set_error(ERROR_INVALID_HANDLE);
    }
}

static void *heap(void) {
    return (void *)(uintptr_t)vibeos_nt_rd64(vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_PEB) +
                                              VIBEOS_PEB_PROCESS_HEAP);
}

static uint64_t stock_handle(int i) {
    return STOCK_FLAG | ((uint64_t)g_stock[i].type << TYPE_SHIFT) | (uint64_t)(0xF000u + (uint32_t)i);
}

static void stock_init(void) {
    static const uint32_t gray[5] = {0xFFFFFFu, 0xC0C0C0u, 0x808080u, 0x404040u, 0u};

    for (int i = 0; i <= BLACK_BRUSH; i++) {
        g_stock[i].type = OBJ_BRUSH;
        g_stock[i].u.brush.color = gray[i];
    }
    g_stock[NULL_BRUSH].type = OBJ_BRUSH;
    g_stock[NULL_BRUSH].u.brush.style = BS_NULL;
    g_stock[WHITE_PEN].type = OBJ_PEN;
    g_stock[WHITE_PEN].u.pen.color = 0xFFFFFFu;
    g_stock[WHITE_PEN].u.pen.width_x = 1;
    g_stock[BLACK_PEN].type = OBJ_PEN;
    g_stock[BLACK_PEN].u.pen.width_x = 1;
    g_stock[NULL_PEN].type = OBJ_PEN;
    g_stock[NULL_PEN].u.pen.style = PS_NULL;
    g_stock[DEFAULT_PALETTE].type = OBJ_PAL;
    g_stock[DC_BRUSH].type = OBJ_BRUSH;
    g_stock[DC_BRUSH].u.brush.color = 0xFFFFFFu;
    g_stock[DC_PEN].type = OBJ_PEN;
    g_stock[DC_PEN].u.pen.width_x = 1;
    g_stock[DEFAULT_BITMAP].type = OBJ_BITMAP;
    g_stock[DEFAULT_BITMAP].u.bitmap.width = 1;
    g_stock[DEFAULT_BITMAP].u.bitmap.height = 1;
    g_stock[DEFAULT_BITMAP].u.bitmap.width_bytes = 2;
    g_stock[DEFAULT_BITMAP].u.bitmap.planes = 1;
    g_stock[DEFAULT_BITMAP].u.bitmap.bpp = 1;
}

/* The object a handle names, or null. A handle whose type bits disagree
 * with the slot, or whose slot was freed and reused, names nothing. */
static obj_t *lookup(uint64_t h) {
    uint32_t idx = (uint32_t)(h & 0xFFFFu);
    uint32_t type = (uint32_t)(h >> TYPE_SHIFT) & 0x7Fu;
    obj_t *o;

    if (h & STOCK_FLAG) {
        idx -= 0xF000u;
        if (idx > DEFAULT_BITMAP || g_stock[idx].type != type) {
            return 0;
        }
        return &g_stock[idx];
    }
    if (idx == 0u || idx >= SLOTS) {
        return 0;
    }
    o = &g_obj[idx];
    if (o->type == 0u || o->type != type || (uint16_t)((h >> 24) & 0x7Fu) != o->gen) {
        return 0;
    }
    return o;
}

/* A new object of `type`, filled from `init`. The generation is bits 24-30,
 * Windows' uniqueness bits: a GDI handle is 32 bits sign-extended, and
 * programs pass one through an int. */
static uint64_t add(uint32_t type, const obj_t *init) {
    uint64_t h = 0;

    lock();
    for (uint32_t i = 1; i < SLOTS; i++) {
        if (g_obj[i].type == 0u) {
            uint16_t gen = (uint16_t)((g_obj[i].gen + 1u) & 0x7Fu);

            g_obj[i] = *init;
            g_obj[i].type = type;
            g_obj[i].gen = gen;
            h = ((uint64_t)gen << 24) | ((uint64_t)type << TYPE_SHIFT) | i;
            break;
        }
    }
    unlock();
    if (!h) {
        set_error(ERROR_NOT_ENOUGH_MEMORY);
    }
    return h;
}

/* ---- objects ----------------------------------------------------------------------------- */

uint64_t GetStockObject(int32_t i) {
    if (i < 0 || i > STOCK_LAST || g_stock[i].type == 0u) {
        return 0;
    }
    return stock_handle(i);
}

uint32_t GetObjectType(uint64_t h) {
    obj_t *o = lookup(h);

    if (!o) {
        set_error(ERROR_INVALID_HANDLE);   /* even for null, unlike the rest */
        return 0;
    }
    return o->type;
}

uint64_t CreateBrushIndirect(const logbrush_t *lb) {
    obj_t o = {0};

    if (!lb) {
        return 0;
    }
    o.u.brush = *lb;
    if (lb->style == BS_NULL) {
        return stock_handle(NULL_BRUSH);   /* Windows hands back the stock one */
    }
    if (lb->style != BS_HATCHED) {
        o.u.brush.hatch = 0;
    }
    return add(OBJ_BRUSH, &o);
}

uint64_t CreateSolidBrush(uint32_t color) {
    logbrush_t lb = {BS_SOLID, color, 0};

    return CreateBrushIndirect(&lb);
}

uint64_t CreateHatchBrush(int32_t hatch, uint32_t color) {
    logbrush_t lb = {BS_HATCHED, color, (uint64_t)(uint32_t)hatch};

    if (hatch < 0 || hatch > 11) {   /* HS_API_MAX: the six of Win32 and Windows' private six */
        return 0;
    }
    if (hatch > 5) {
        lb.style = BS_SOLID;   /* the private six make a solid brush, as Windows does */
        lb.hatch = 0;
    }
    return CreateBrushIndirect(&lb);
}

uint64_t CreatePenIndirect(const logpen_t *lp) {
    obj_t o = {0};

    if (!lp) {
        return 0;
    }
    o.u.pen = *lp;
    if ((lp->style & PS_STYLE_MASK) == PS_NULL) {
        return stock_handle(NULL_PEN);
    }
    if ((lp->style & PS_STYLE_MASK) > 6u) {
        o.u.pen.style = PS_SOLID;   /* an unknown style is solid, as Windows does */
    }
    o.u.pen.width_y = 0;
    if (o.u.pen.width_x < 0) {
        o.u.pen.width_x = -o.u.pen.width_x;
    }
    return add(OBJ_PEN, &o);
}

uint64_t CreatePen(int32_t style, int32_t width, uint32_t color) {
    logpen_t lp = {(uint32_t)style, width, 0, color};

    return CreatePenIndirect(&lp);
}

uint64_t CreateBitmap(int32_t w, int32_t h, uint32_t planes, uint32_t bpp, const void *bits) {
    obj_t o = {0};
    uint64_t size;
    uint64_t r;

    if (w < 0 || h < 0 || planes > 32u || bpp > 32u) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (w == 0 || h == 0) {
        return stock_handle(DEFAULT_BITMAP);   /* 0x0 is the 1x1 default */
    }
    if (bpp != 1u && bpp != 4u && bpp != 8u && bpp != 16u && bpp != 24u && bpp != 32u) {
        bpp = bpp <= 1u ? 1u : bpp <= 4u ? 4u : bpp <= 8u ? 8u : bpp <= 16u ? 16u : bpp <= 24u ? 24u : 32u;
    }
    o.u.bitmap.width = w;
    o.u.bitmap.height = h;
    o.u.bitmap.planes = 1;
    o.u.bitmap.bpp = (uint16_t)bpp;
    o.u.bitmap.width_bytes = (int32_t)((((uint32_t)w * bpp + 15u) / 16u) * 2u);   /* WORD-aligned */
    size = (uint64_t)o.u.bitmap.width_bytes * (uint64_t)h;
    if (size > 0x10000000u) {
        set_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    o.u.bitmap.bits = RtlAllocateHeap(heap(), 8u /* HEAP_ZERO_MEMORY */, (size_t)size);
    if (!o.u.bitmap.bits) {
        set_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (bits) {
        for (uint64_t i = 0; i < size; i++) {
            ((uint8_t *)o.u.bitmap.bits)[i] = ((const uint8_t *)bits)[i];
        }
    }
    r = add(OBJ_BITMAP, &o);
    if (!r) {
        RtlFreeHeap(heap(), 0, o.u.bitmap.bits);
    }
    return r;
}

uint64_t CreateBitmapIndirect(const bitmap_t *bm) {
    if (!bm) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return CreateBitmap(bm->width, bm->height, bm->planes, bm->bpp, bm->bits);
}

/* A bitmap's bits, `count` bytes at most; the answer is what was copied. */
int32_t GetBitmapBits(uint64_t h, int32_t count, void *out) {
    obj_t *o = lookup(h);
    int32_t size;

    if (!o || o->type != OBJ_BITMAP) {
        return 0;
    }
    size = o->u.bitmap.width_bytes * o->u.bitmap.height;
    if (!out) {
        return size;
    }
    if (count > size) {
        count = size;
    }
    for (int32_t i = 0; i < count && o->u.bitmap.bits; i++) {
        ((uint8_t *)out)[i] = ((const uint8_t *)o->u.bitmap.bits)[i];
    }
    return count < 0 ? 0 : count;
}

int32_t SetBitmapBits(uint64_t h, uint32_t count, const void *in) {
    obj_t *o = lookup(h);
    uint32_t size;

    if (!o || o->type != OBJ_BITMAP || !in || !o->u.bitmap.bits) {
        return 0;
    }
    size = (uint32_t)(o->u.bitmap.width_bytes * o->u.bitmap.height);
    if (count > size) {
        count = size;
    }
    for (uint32_t i = 0; i < count; i++) {
        ((uint8_t *)o->u.bitmap.bits)[i] = ((const uint8_t *)in)[i];
    }
    return (int32_t)count;
}

/* GetObject: the object's logical description, as it was created. A null
 * buffer asks for the size. */
int32_t GetObjectW(uint64_t h, int32_t size, void *out) {
    obj_t *o = lookup(h);
    const void *src;
    int32_t n;

    if (!o) {
        bad_handle(h);
        return 0;
    }
    switch (o->type) {
    case OBJ_BRUSH:
        src = &o->u.brush;
        n = (int32_t)sizeof(logbrush_t);
        break;
    case OBJ_PEN:
        src = &o->u.pen;
        n = (int32_t)sizeof(logpen_t);
        break;
    case OBJ_BITMAP: {
        static bitmap_t bm;   /* the bits pointer is not handed out */

        bm = o->u.bitmap;
        bm.bits = 0;
        src = &bm;
        n = (int32_t)sizeof(bitmap_t);
        break;
    }
    case OBJ_PAL: {
        static const uint16_t entries = 20;

        src = &entries;
        n = 2;
        break;
    }
    default:
        return 0;   /* a DC has no logical description */
    }
    if (!out) {
        return n;
    }
    if (size < n) {
        return 0;   /* a buffer too small for the whole description gets none of it */
    }
    /* Windows copies under an exception handler and answers ERROR_NOACCESS for
     * a buffer it cannot write; gdiobj passes a small integer as the buffer on
     * purpose. Nothing is ever mapped in the low 64 KiB, so that much is
     * refused here. A wild pointer elsewhere still faults (WO-100): asking the
     * kernel instead does not work, because NtWriteVirtualMemory refuses the
     * main thread's own stack (WO-099). */
    if ((uintptr_t)out < 0x10000u) {
        set_error(998u);   /* ERROR_NOACCESS */
        return 0;
    }
    for (int32_t i = 0; i < n; i++) {
        ((uint8_t *)out)[i] = ((const uint8_t *)src)[i];
    }
    return n;
}

int32_t GetObjectA(uint64_t h, int32_t size, void *out) { return GetObjectW(h, size, out); }

/* ---- device contexts --------------------------------------------------------------------- */

static uint64_t new_dc(uint32_t type) {
    obj_t o = {0};

    o.u.dc.brush = stock_handle(WHITE_BRUSH);
    o.u.dc.pen = stock_handle(BLACK_PEN);
    o.u.dc.bitmap = stock_handle(DEFAULT_BITMAP);
    o.u.dc.palette = stock_handle(DEFAULT_PALETTE);
    o.u.dc.bk_mode = 2;   /* OPAQUE */
    o.u.dc.bk_color = 0xFFFFFFu;
    return add(type, &o);
}

static int ascii_ieq(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a;
        char y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;

        if (x != y) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

/* The display is the only device: "DISPLAY", or a null driver with
 * "\\.\DISPLAY1" as the device. */
uint64_t CreateDCA(const char *driver, const char *device, const char *port, const void *mode) {
    (void)port, (void)mode;
    if ((driver && ascii_ieq(driver, "DISPLAY")) ||
        (device && (ascii_ieq(device, "DISPLAY") || ascii_ieq(device, "\\\\.\\DISPLAY1")))) {
        return new_dc(OBJ_DC);
    }
    set_error(ERROR_INVALID_PARAMETER);
    return 0;
}

uint64_t CreateDCW(const uint16_t *driver, const uint16_t *device, const uint16_t *port,
                   const void *mode) {
    char d[16] = {0}, v[16] = {0};

    for (int i = 0; driver && i < 15 && driver[i]; i++) {
        d[i] = driver[i] < 0x80u ? (char)driver[i] : '?';
    }
    for (int i = 0; device && i < 15 && device[i]; i++) {
        v[i] = device[i] < 0x80u ? (char)device[i] : '?';
    }
    (void)port;
    return CreateDCA(driver ? d : 0, device ? v : 0, 0, mode);
}

uint64_t CreateCompatibleDC(uint64_t dc) {
    if (dc && !lookup(dc)) {
        return 0;
    }
    return new_dc(OBJ_MEMDC);
}

static dc_t *dc_of(uint64_t h) {
    obj_t *o = lookup(h);

    if (!o || (o->type != OBJ_DC && o->type != OBJ_MEMDC)) {
        bad_handle(h);
        return 0;
    }
    return &o->u.dc;
}

BOOL32 DeleteDC(uint64_t h) {
    obj_t *o = lookup(h);

    if (!o || (o->type != OBJ_DC && o->type != OBJ_MEMDC) || (h & STOCK_FLAG)) {
        return 0;
    }
    lock();
    o->type = 0;
    unlock();
    return 1;
}

/* Is `h` selected into any DC? Windows refuses to delete such an object. */
static int selected(uint64_t h) {
    for (uint32_t i = 1; i < SLOTS; i++) {
        const obj_t *o = &g_obj[i];

        if ((o->type == OBJ_DC || o->type == OBJ_MEMDC) &&
            (o->u.dc.brush == h || o->u.dc.pen == h || o->u.dc.bitmap == h)) {
            return 1;
        }
    }
    return 0;
}

BOOL32 DeleteObject(uint64_t h) {
    obj_t *o = lookup(h);
    void *bits = 0;

    if (!o) {
        bad_handle(h);
        return 0;
    }
    if (h & STOCK_FLAG) {
        return 1;   /* deleting a stock object is allowed and does nothing */
    }
    if (o->type == OBJ_DC || o->type == OBJ_MEMDC) {
        return DeleteDC(h);
    }
    lock();
    if (o->type == OBJ_BITMAP && selected(h)) {
        unlock();
        return 0;   /* a bitmap in a DC cannot go */
    }
    if (o->type == OBJ_BITMAP) {
        bits = o->u.bitmap.bits;
    }
    o->type = 0;   /* a selected pen or brush goes anyway, as on Windows */
    unlock();
    if (bits) {
        RtlFreeHeap(heap(), 0, bits);
    }
    return 1;
}

/* The previous object of the same kind; a bitmap goes into one DC at a time
 * and only into a memory DC. */
uint64_t SelectObject(uint64_t dch, uint64_t h) {
    dc_t *dc = dc_of(dch);
    obj_t *o = lookup(h);
    uint64_t prev = 0;

    if (!dc || !o) {
        bad_handle(dch && h ? 1u : 0u);
        return 0;
    }
    lock();
    switch (o->type) {
    case OBJ_BRUSH:
        prev = dc->brush;
        dc->brush = h;
        break;
    case OBJ_PEN:
        prev = dc->pen;
        dc->pen = h;
        break;
    case OBJ_BITMAP:
        if (lookup(dch)->type != OBJ_MEMDC || (h != dc->bitmap && !(h & STOCK_FLAG) && selected(h))) {
            break;
        }
        prev = dc->bitmap;
        dc->bitmap = h;
        break;
    default:
        break;
    }
    unlock();
    return prev;
}

uint64_t GetCurrentObject(uint64_t dch, uint32_t type) {
    dc_t *dc = dc_of(dch);

    if (!dc) {
        return 0;
    }
    switch (type) {
    case OBJ_BRUSH:
        return dc->brush;
    case OBJ_PEN:
        return dc->pen;
    case OBJ_BITMAP:
        return dc->bitmap;
    case OBJ_PAL:
        return dc->palette;
    default:
        return 0;
    }
}

uint64_t CreateCompatibleBitmap(uint64_t dch, int32_t w, int32_t h) {
    obj_t *o = lookup(dch);
    uint32_t bpp = 32;

    if (!o || (o->type != OBJ_DC && o->type != OBJ_MEMDC)) {
        bad_handle(dch);
        return 0;
    }
    if (o->type == OBJ_MEMDC) {
        obj_t *bm = lookup(o->u.dc.bitmap);

        bpp = bm ? bm->u.bitmap.bpp : 1u;
    }
    return CreateBitmap(w, h, 1, bpp, 0);
}

int32_t SetBkMode(uint64_t dch, int32_t mode) {
    dc_t *dc = dc_of(dch);
    int32_t prev;

    if (!dc || mode < 1 || mode > 2) {
        return 0;
    }
    prev = dc->bk_mode;
    dc->bk_mode = mode;
    return prev;
}

int32_t GetBkMode(uint64_t dch) {
    dc_t *dc = dc_of(dch);

    return dc ? dc->bk_mode : 0;
}

uint32_t SetBkColor(uint64_t dch, uint32_t c) {
    dc_t *dc = dc_of(dch);
    uint32_t prev;

    if (!dc) {
        return 0xFFFFFFFFu;   /* CLR_INVALID */
    }
    prev = dc->bk_color;
    dc->bk_color = c;
    return prev;
}

uint32_t SetTextColor(uint64_t dch, uint32_t c) {
    dc_t *dc = dc_of(dch);
    uint32_t prev;

    if (!dc) {
        return 0xFFFFFFFFu;
    }
    prev = dc->text_color;
    dc->text_color = c;
    return prev;
}

uint32_t GetBkColor(uint64_t dch) {
    dc_t *dc = dc_of(dch);

    return dc ? dc->bk_color : 0xFFFFFFFFu;
}

uint32_t GetTextColor(uint64_t dch) {
    dc_t *dc = dc_of(dch);

    return dc ? dc->text_color : 0xFFFFFFFFu;
}

int32_t SetTextCharacterExtra(uint64_t dch, int32_t extra) {
    dc_t *dc = dc_of(dch);
    int32_t prev;

    if (!dc) {
        return (int32_t)0x80000000u;
    }
    prev = dc->char_extra;
    dc->char_extra = extra;
    return prev;
}

int32_t GetTextCharacterExtra(uint64_t dch) {
    dc_t *dc = dc_of(dch);

    return dc ? dc->char_extra : (int32_t)0x80000000u;
}

BOOL32 SetBrushOrgEx(uint64_t dch, int32_t x, int32_t y, int32_t *prev) {
    dc_t *dc = dc_of(dch);

    if (!dc) {
        return 0;
    }
    if (prev) {
        prev[0] = dc->org_x;
        prev[1] = dc->org_y;
    }
    dc->org_x = x;
    dc->org_y = y;
    return 1;
}

BOOL32 GetBrushOrgEx(uint64_t dch, int32_t *pt) {
    dc_t *dc = dc_of(dch);

    if (!dc || !pt) {
        return 0;
    }
    pt[0] = dc->org_x;
    pt[1] = dc->org_y;
    return 1;
}

/* The display user32 describes: 1024x768, 32 bits. */
#define HORZRES 8
#define VERTRES 10
#define BITSPIXEL 12
#define PLANES 14
#define LOGPIXELSX 88
#define LOGPIXELSY 90

int32_t GetDeviceCaps(uint64_t dch, int32_t index) {
    obj_t *o = lookup(dch);

    if (!o || (o->type != OBJ_DC && o->type != OBJ_MEMDC)) {
        return 0;
    }
    switch (index) {
    case HORZRES:
        return 1024;
    case VERTRES:
        return 768;
    case BITSPIXEL:
        return 32;
    case PLANES:
        return 1;
    case LOGPIXELSX:
    case LOGPIXELSY:
        return 96;
    default:
        return 0;
    }
}

/* ---- drawing ----------------------------------------------------------------------------- */

static BOOL32 no_drawing(void) {
    set_error(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

BOOL32 PatBlt(uint64_t dc, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rop) {
    (void)dc, (void)x, (void)y, (void)w, (void)h, (void)rop;
    return no_drawing();
}

BOOL32 MoveToEx(uint64_t dc, int32_t x, int32_t y, int32_t *prev) {
    (void)dc, (void)x, (void)y, (void)prev;
    return no_drawing();
}

BOOL32 LineTo(uint64_t dc, int32_t x, int32_t y) {
    (void)dc, (void)x, (void)y;
    return no_drawing();
}

uint32_t GetPixel(uint64_t dc, int32_t x, int32_t y) {
    (void)dc, (void)x, (void)y;
    set_error(ERROR_CALL_NOT_IMPLEMENTED);
    return 0xFFFFFFFFu;
}

int vibeos_gdi32_main(uint64_t instance, uint32_t reason, void *reserved) {
    (void)instance;
    (void)reserved;
    if (reason == 1u) {   /* DLL_PROCESS_ATTACH */
        stock_init();
    }
    return 1;
}

/* Nothing is realised, so there is nothing to undo; a null handle fails
 * without an error, as Windows answers. */
BOOL32 UnrealizeObject(uint64_t h) {
    return lookup(h) != 0;
}
