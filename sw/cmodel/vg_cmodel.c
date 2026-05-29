/*
 * OpenVG 1.1 Hardware Accelerator – C-Model Implementation
 *
 * Models the following hardware blocks:
 *  1. Front-End (FE): curve flattening via recursive De Casteljau,
 *                     affine transform, fill rule.
 *  2. Tiler      : bins flattened edges into 32x32 or 64x64 tiles.
 *  3. Back-End   : scanline rasteriser with 8x8 sub-pixel AA,
 *                  flat / linear-gradient / radial-gradient paint,
 *                  Porter-Duff blending.
 *  4. Texture cache : 4-way set-associative LRU (simulated on the
 *                     internal frame buffer / image buffer).
 */

#include "vg_cmodel.h"
#include "../driver/vg_reg.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>

/* =========================================================================
 * Constants
 * ========================================================================= */
#define MAX_EDGES           524288
#define MAX_TILES_X         128       /* kept for tile_w/tile_h derivation   */
#define MAX_TILES_Y         128
#define MAX_BAND_EDGES      262144    /* per Y-band; bands are tile_h rows   */
#define AA_N                8         /* 8x8 = 64 sub-samples                */
#define MAX_FB_W            2048      /* upper bound on framebuffer width    */
#define CACHE_LINE_BYTES    64
#define CACHE_WAYS          4
#define CACHE_SETS          256       /* total = 64KB / 64B / 4-way          */

/*
 * Band-list memory hierarchy (matches the planned RTL):
 *
 *   DRAM:  dram_band_lists[ty][0..count-1]   <- tiler writes
 *                  |
 *                  | DMA burst of ID_PREFETCH_FIFO_DEPTH IDs
 *                  v
 *   SRAM:  id_prefetch_fifo[ID_PREFETCH_FIFO_DEPTH]   <- BE drains
 *
 * Only the active band's IDs are streamed on-chip; total on-chip pointer
 * storage is the small FIFO + a per-band header table (base, count).
 * The big "band_lists" array in the C-model represents DRAM, not SRAM.
 */
#define ID_PREFETCH_FIFO_DEPTH  256   /* on-chip FIFO of edge-IDs (16 b ea.) */

/* =========================================================================
 * Data types
 * ========================================================================= */

typedef struct {
    float x0, y0, x1, y1;
    int   dir;   /* +1 = upward, -1 = downward (for winding counter)        */
} Edge;

typedef struct {
    uint32_t tag;
    uint8_t  data[CACHE_LINE_BYTES];
    int      valid;
    int      lru;   /* 0 = MRU, CACHE_WAYS-1 = LRU                          */
} CacheSlot;

typedef struct {
    CacheSlot slots[CACHE_SETS][CACHE_WAYS];
} TexCache;

/* Register file */
typedef struct {
    uint32_t regs[0x400 / 4];
} RegFile;

/* =========================================================================
 * C-Model context
 * ========================================================================= */
struct vg_cmodel {
    RegFile  rf;

    /* Internal frame buffer (RGBA8888) */
    uint32_t  fb_w, fb_h;
    uint32_t *fb;

    /* Flattened edge list produced by the FE */
    Edge     edges[MAX_EDGES];
    int      num_edges;

    /*
     * Tiler output: one edge-index list per Y-band (band = tile_h rows tall).
     *
     * RTL memory model:
     *   - dram_band_lists[]  : DRAM-resident ID arrays (one per band).
     *                          In real HW the tiler streams these out via
     *                          AXI; only the address+count is kept on-chip.
     *   - band_hdr[]         : on-chip table (base address + ID count) per
     *                          band; tiny (a few bytes per band).
     *   - id_fifo[]          : small on-chip FIFO into which the active
     *                          band's IDs are DMA'd in bursts.
     *
     * No X-binning: the AET-based scanline march only needs the union of
     * edges that may cross any sub-sample row in the band.
     */
    int  tile_w, tile_h;              /* tile size in pixels                 */
    int  tiles_x, tiles_y;            /* number of tile cols / Y-bands       */

    /* DRAM model (large; in RTL this lives in external DRAM). */
    int *dram_band_lists;             /* [tiles_y][MAX_BAND_EDGES]           */

    /* On-chip per-band header table: base index + count. */
    struct {
        uint32_t base;                /* offset into dram_band_lists[]       */
        uint32_t count;               /* valid IDs in this band              */
    } band_hdr[MAX_TILES_Y];

    /* On-chip ID prefetch FIFO (one band at a time). */
    int       id_fifo[ID_PREFETCH_FIFO_DEPTH];
    int       id_fifo_head;           /* read pointer (consumed by BE)       */
    int       id_fifo_tail;           /* write pointer (filled by DMA)       */
    uint32_t  id_dma_next;            /* next ID index in DRAM to fetch      */
    uint32_t  id_dma_end;             /* last+1 ID index for current band    */

    /* Per-scanline scratch (sized to one band).  In RTL these are SRAM. */
    float *cov_row;                   /* [fb_w]: sub-sample inside-counts    */

    /* Texture cache */
    TexCache tex_cache;

    /* Optional image data pointer (provided by test-bench via bus_addr cast) */
    const uint8_t *image_data;
    uint32_t       image_stride;
    uint32_t       image_w, image_h;

    /* 64-bit host pointers (cannot fit in 32-bit register on 64-bit hosts) */
    const uint8_t *path_ptr;
    uint32_t       path_size;
    const uint8_t *mask_ptr;

    float          ramp_color[16][4];
};

/* =========================================================================
 * Helpers
 * ========================================================================= */
static inline uint32_t rf_read(struct vg_cmodel *cm, uint32_t off)
{
    if (off >= 0x400) return 0;
    return cm->rf.regs[off / 4];
}

static inline void rf_write(struct vg_cmodel *cm, uint32_t off, uint32_t v)
{
    if (off < 0x400)
        cm->rf.regs[off / 4] = v;
}

static inline float rf_readf(struct vg_cmodel *cm, uint32_t off)
{
    uint32_t u = rf_read(cm, off);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static inline uint32_t float_to_u32(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

/* RGBA8888 helpers */
static inline uint8_t rgba_r(uint32_t c) { return (c >> 24) & 0xFF; }
static inline uint8_t rgba_g(uint32_t c) { return (c >> 16) & 0xFF; }
static inline uint8_t rgba_b(uint32_t c) { return (c >>  8) & 0xFF; }
static inline uint8_t rgba_a(uint32_t c) { return (c      ) & 0xFF; }
static inline uint32_t rgba_pack(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) |
           ((uint32_t)b <<  8) | (uint32_t)a;
}

/* Clamp float to [0,1] */
static inline float clampf(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
static inline int   clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* =========================================================================
 * Texture cache implementation
 * ========================================================================= */
static void cache_init(TexCache *tc)
{
    memset(tc, 0, sizeof(*tc));
}

static uint8_t cache_read_byte(TexCache *tc,
                               const uint8_t *mem, size_t mem_size,
                               uint32_t addr)
{
    if (!mem || addr >= (uint32_t)mem_size) return 0;

    int index_bits = 0;
    for (int s = CACHE_SETS; s > 1; s >>= 1) index_bits++;

    uint32_t offset = addr & (CACHE_LINE_BYTES - 1);
    uint32_t index  = (addr >> 6) & (CACHE_SETS - 1);
    uint32_t tag    = addr >> (6 + index_bits);

    CacheSlot *set = tc->slots[index];

    /* Look for a hit */
    for (int w = 0; w < CACHE_WAYS; w++) {
        if (set[w].valid && set[w].tag == tag) {
            /* Update LRU: demote everyone ≤ current, promote this slot */
            int cur_lru = set[w].lru;
            for (int j = 0; j < CACHE_WAYS; j++)
                if (set[j].valid && set[j].lru < cur_lru)
                    set[j].lru++;
            set[w].lru = 0; /* MRU */
            return set[w].data[offset];
        }
    }

    /* Miss: find LRU slot (highest lru value) */
    int victim = 0;
    for (int w = 1; w < CACHE_WAYS; w++)
        if (!set[w].valid || set[w].lru > set[victim].lru)
            victim = w;

    /* Fill cache line from memory */
    uint32_t base = addr & ~(uint32_t)(CACHE_LINE_BYTES - 1);
    for (int b = 0; b < CACHE_LINE_BYTES; b++) {
        uint32_t a = base + (uint32_t)b;
        set[victim].data[b] = (a < (uint32_t)mem_size) ? mem[a] : 0;
    }
    set[victim].tag   = tag;
    set[victim].valid = 1;

    /* Demote all others, this slot becomes MRU */
    for (int j = 0; j < CACHE_WAYS; j++)
        if (j != victim && set[j].valid)
            set[j].lru++;
    set[victim].lru = 0;

    return set[victim].data[offset];
}

/* Sample RGBA8888 from image via cache (used when texture is active) */
static uint32_t tex_sample(struct vg_cmodel *cm, int ix, int iy)
{
    if (!cm->image_data) return 0x00000000u;
    ix = clampi(ix, 0, (int)cm->image_w - 1);
    iy = clampi(iy, 0, (int)cm->image_h - 1);
    uint32_t addr = (uint32_t)iy * cm->image_stride + (uint32_t)ix * 4;
    uint32_t mem_size = cm->image_stride * cm->image_h;
    uint8_t r = cache_read_byte(&cm->tex_cache, cm->image_data, mem_size, addr    );
    uint8_t g = cache_read_byte(&cm->tex_cache, cm->image_data, mem_size, addr + 1);
    uint8_t b = cache_read_byte(&cm->tex_cache, cm->image_data, mem_size, addr + 2);
    uint8_t a = cache_read_byte(&cm->tex_cache, cm->image_data, mem_size, addr + 3);
    return rgba_pack(r, g, b, a);
}

/* =========================================================================
 * Front-End: Curve Flattening (De Casteljau recursive subdivision)
 * ========================================================================= */

/* Affine-transform a point */
static void transform_pt(float sx, float shx, float tx,
                          float shy, float sy, float ty,
                          float ix, float iy,
                          float *ox, float *oy)
{
    *ox = sx * ix + shx * iy + tx;
    *oy = shy * ix + sy  * iy + ty;
}

static void add_edge(struct vg_cmodel *cm,
                     float x0, float y0, float x1, float y1)
{
    if (cm->num_edges >= MAX_EDGES) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "[cmodel] WARN: edge buffer overflow (MAX_EDGES=%d)\n", MAX_EDGES);
            warned = 1;
        }
        return;
    }
    int dir = (y1 > y0) ? 1 : -1;
    if (y0 == y1) return; /* horizontal edges don't contribute */
    Edge *e = &cm->edges[cm->num_edges++];
    e->x0 = x0; e->y0 = y0;
    e->x1 = x1; e->y1 = y1;
    e->dir = dir;
}

/*
 * Iterative quadratic Bezier flattening using an explicit LIFO stack.
 *
 * RTL note:
 *   - The on-chip stack is a small SRAM/regfile of FLATTEN_STACK_DEPTH
 *     entries; each entry holds (x0,y0,x1,y1,x2,y2,depth) ~ 7 words.
 *   - The flatness test uses |cross|^2 < tol^2 * len^2 (not |cross|/len),
 *     avoiding any divide.  The C model still uses the algebraically
 *     equivalent dist^2 form.
 *   - On overflow (depth or stack), emit the chord as-is (graceful).
 */
#define FLATTEN_STACK_DEPTH  24
#define FLATTEN_MAX_DEPTH_Q  16
#define FLATTEN_MAX_DEPTH_C  20

typedef struct {
    float x0, y0, x1, y1, x2, y2;
    int   depth;
} QuadFrame;

typedef struct {
    float x0, y0, x1, y1, x2, y2, x3, y3;
    int   depth;
} CubicFrame;

static void flatten_quad(struct vg_cmodel *cm,
                          float ix0, float iy0,
                          float ix1, float iy1,
                          float ix2, float iy2,
                          int depth0)
{
    QuadFrame stk[FLATTEN_STACK_DEPTH];
    int sp = 0;
    stk[sp++] = (QuadFrame){ix0, iy0, ix1, iy1, ix2, iy2, depth0};

    while (sp > 0) {
        QuadFrame f = stk[--sp];

        /* Flatness: distance of control point from chord (squared form) */
        float dx = f.x2 - f.x0, dy = f.y2 - f.y0;
        float len2 = dx * dx + dy * dy;
        float ex = f.x1 - f.x0, ey = f.y1 - f.y0;
        float t   = (len2 > 1e-10f) ? ((ex * dx + ey * dy) / len2) : 0.f;
        float cx  = f.x0 + t * dx, cy = f.y0 + t * dy;
        float dist2 = (f.x1 - cx) * (f.x1 - cx) + (f.y1 - cy) * (f.y1 - cy);

        if (f.depth > FLATTEN_MAX_DEPTH_Q || dist2 < 0.25f * 0.25f) {
            add_edge(cm, f.x0, f.y0, f.x2, f.y2);
            continue;
        }
        if (sp + 2 > FLATTEN_STACK_DEPTH) {
            /* Stack full: emit chord and stop subdividing this branch */
            add_edge(cm, f.x0, f.y0, f.x2, f.y2);
            continue;
        }

        /* Midpoints (de Casteljau) */
        float mx0 = (f.x0 + f.x1) * .5f, my0 = (f.y0 + f.y1) * .5f;
        float mx1 = (f.x1 + f.x2) * .5f, my1 = (f.y1 + f.y2) * .5f;
        float mx  = (mx0 + mx1) * .5f,   my  = (my0 + my1) * .5f;

        /* Push right half first so left half is processed first (LIFO). */
        stk[sp++] = (QuadFrame){mx,   my,   mx1,  my1,  f.x2, f.y2, f.depth + 1};
        stk[sp++] = (QuadFrame){f.x0, f.y0, mx0,  my0,  mx,   my,   f.depth + 1};
    }
}

static void flatten_cubic(struct vg_cmodel *cm,
                           float ix0, float iy0,
                           float ix1, float iy1,
                           float ix2, float iy2,
                           float ix3, float iy3,
                           int depth0)
{
    CubicFrame stk[FLATTEN_STACK_DEPTH];
    int sp = 0;
    stk[sp++] = (CubicFrame){ix0, iy0, ix1, iy1, ix2, iy2, ix3, iy3, depth0};

    while (sp > 0) {
        CubicFrame f = stk[--sp];

        /*
         * Flatness: |cross_i|^2 < tol^2 * len^2 (no divide / no sqrt).
         * Equivalent to (|cross_i|/len) < tol.
         */
        float dx  = f.x3 - f.x0,  dy  = f.y3 - f.y0;
        float d1x = f.x1 - f.x0,  d1y = f.y1 - f.y0;
        float d2x = f.x2 - f.x0,  d2y = f.y2 - f.y0;
        float len2 = dx * dx + dy * dy;
        float c1   = d1x * dy - d1y * dx;
        float c2   = d2x * dy - d2y * dx;
        const float tol2 = 0.25f * 0.25f;     /* same tol as recursive version */

        if (f.depth > FLATTEN_MAX_DEPTH_C ||
            (c1 * c1 < tol2 * len2 && c2 * c2 < tol2 * len2)) {
            add_edge(cm, f.x0, f.y0, f.x3, f.y3);
            continue;
        }
        if (sp + 2 > FLATTEN_STACK_DEPTH) {
            add_edge(cm, f.x0, f.y0, f.x3, f.y3);
            continue;
        }

        /* de Casteljau midpoint subdivision */
        float mx0 = (f.x0 + f.x1) * .5f, my0 = (f.y0 + f.y1) * .5f;
        float mx1 = (f.x1 + f.x2) * .5f, my1 = (f.y1 + f.y2) * .5f;
        float mx2 = (f.x2 + f.x3) * .5f, my2 = (f.y2 + f.y3) * .5f;
        float nx0 = (mx0 + mx1) * .5f,   ny0 = (my0 + my1) * .5f;
        float nx1 = (mx1 + mx2) * .5f,   ny1 = (my1 + my2) * .5f;
        float pxm = (nx0 + nx1) * .5f,   pym = (ny0 + ny1) * .5f;

        /* Push right then left (process left first). */
        stk[sp++] = (CubicFrame){pxm,  pym,  nx1,  ny1,  mx2,  my2,  f.x3, f.y3, f.depth + 1};
        stk[sp++] = (CubicFrame){f.x0, f.y0, mx0,  my0,  nx0,  ny0,  pxm,  pym,  f.depth + 1};
    }
}

/* Simple arc approximation: subdivide into cubic Bezier segments */
static void flatten_arc(struct vg_cmodel *cm,
                         float cx, float cy,
                         float rx, float ry,
                         float angle_start, float angle_end,
                         float rot_deg)
{
    /* Approximate each arc quadrant with a cubic Bezier */
    float rot = rot_deg * (float)M_PI / 180.f;
    float cos_r = cosf(rot), sin_r = sinf(rot);

    int segs = (int)(ceilf(fabsf(angle_end - angle_start) / ((float)M_PI / 2.f)));
    if (segs < 1) segs = 1;
    float da = (angle_end - angle_start) / segs;
    float k = (4.f / 3.f) * tanf(da / 4.f);

    float prev_x, prev_y;
    {
        float a = angle_start;
        float px = cosf(a) * rx, py = sinf(a) * ry;
        prev_x = cx + px * cos_r - py * sin_r;
        prev_y = cy + px * sin_r + py * cos_r;
    }

    for (int i = 0; i < segs; i++) {
        float a0 = angle_start + da * i;
        float a1 = a0 + da;
        float c0  = cosf(a0), s0 = sinf(a0);
        float c1  = cosf(a1), s1 = sinf(a1);

        float p1x = (c0 - k * s0) * rx, p1y = (s0 + k * c0) * ry;
        float p2x = (c1 + k * s1) * rx, p2y = (s1 - k * c1) * ry;
        float p3x = c1 * rx, p3y = s1 * ry;

        /* Rotate by rot */
        float t1x = cx + p1x * cos_r - p1y * sin_r;
        float t1y = cy + p1x * sin_r + p1y * cos_r;
        float t2x = cx + p2x * cos_r - p2y * sin_r;
        float t2y = cy + p2x * sin_r + p2y * cos_r;
        float t3x = cx + p3x * cos_r - p3y * sin_r;
        float t3y = cy + p3x * sin_r + p3y * cos_r;

        flatten_cubic(cm, prev_x, prev_y, t1x, t1y, t2x, t2y, t3x, t3y, 0);
        prev_x = t3x; prev_y = t3y;
    }
}

/* =========================================================================
 * Path Parser
 * =========================================================================
 * Reads the path command buffer from the emulated address space.
 * Since the C-model runs on the host CPU, path_bus_addr is cast directly
 * to a host pointer (test-bench passes malloc'd buffers).
 */
static void fe_process_path(struct vg_cmodel *cm)
{
    const uint8_t *buf     = cm->path_ptr;
    uint32_t       path_size = cm->path_size;
    float      scale     = rf_readf(cm, VG_REG_PATH_SCALE);
    float      bias      = rf_readf(cm, VG_REG_PATH_BIAS);
    if (scale == 0.f) scale = 1.f;

    float sx  = rf_readf(cm, VG_REG_MATRIX_SX);
    float shx = rf_readf(cm, VG_REG_MATRIX_SHX);
    float tx  = rf_readf(cm, VG_REG_MATRIX_TX);
    float shy = rf_readf(cm, VG_REG_MATRIX_SHY);
    float sy  = rf_readf(cm, VG_REG_MATRIX_SY);
    float ty  = rf_readf(cm, VG_REG_MATRIX_TY);

    if (!buf || !path_size) return;

    const uint8_t *buf_end = buf + path_size;

    /*
     * cur_x / cur_y : current pen position in CMODEL (screen) coordinates.
     * pcur_x / pcur_y : same position in PATH space (before matrix transform).
     * All RF() reads return path-space coordinates.
     * ox / oy are REL offsets in path space so that REL segments work correctly
     * even when the matrix has non-trivial components (e.g. the Y-flip applied
     * by load_matrix_to_cmodel).
     */
    /* The implicit current point in path-space is (0,0).  In cmodel space
     * that maps to the translation component of the matrix: transform_pt(0,0). */
    float cur_x, cur_y;
    transform_pt(sx, shx, tx, shy, sy, ty, 0.f, 0.f, &cur_x, &cur_y);
    float pcur_x = 0.f, pcur_y = 0.f; /* path-space current position */
    float sub_x = cur_x, sub_y = cur_y; /* cmodel-space subpath start for CLOSE */
    float psub_x = 0.f, psub_y = 0.f; /* path-space subpath start */
    float pprev_cp_x = 0.f, pprev_cp_y = 0.f; /* path-space previous control point */
    /*
     * Track whether the current subpath has been explicitly closed.  In
     * OpenVG, an unclosed subpath is still implicitly closed for FILL
     * operations (only stroking treats it as open).  Without the implicit
     * close edge, the polygon's winding count is unbalanced where the
     * open subpath crosses a sample line, producing visible half-coverage
     * stripes (e.g. tiger row 124 / 298 artefacts).
     */
    int subpath_open = 0;

    while (buf + 4 <= buf_end) {
        uint32_t hdr = 0;
        memcpy(&hdr, buf, 4); buf += 4;
        uint8_t cmd = hdr & 0xFF;
        int     rel = cmd & VG_PATH_REL;
        uint8_t seg = cmd & ~VG_PATH_REL;

        /* Read one float from buffer */
#define RF() ({ float _f; if (buf + 4 > buf_end) goto done; memcpy(&_f, buf, 4); buf += 4; _f * scale + bias; })
#define RF_RAW() ({ float _f; if (buf + 4 > buf_end) goto done; memcpy(&_f, buf, 4); buf += 4; _f; })

        /* REL offsets in PATH space (not cmodel space) */
        float ox = rel ? pcur_x : 0.f;
        float oy = rel ? pcur_y : 0.f;

        float tx0, ty0; /* transformed */

        switch (seg) {
        case VG_PATH_CMD_CLOSE_PATH:
            add_edge(cm, cur_x, cur_y, sub_x, sub_y);
            pcur_x = psub_x; pcur_y = psub_y;
            cur_x = sub_x; cur_y = sub_y;
            subpath_open = 0;
            break;

        case VG_PATH_CMD_MOVE_TO: {
            /* Implicitly close any open subpath for FILL semantics. */
            if (subpath_open &&
                (cur_x != sub_x || cur_y != sub_y)) {
                add_edge(cm, cur_x, cur_y, sub_x, sub_y);
            }
            float nx = RF() + ox, ny = RF() + oy;
            transform_pt(sx, shx, tx, shy, sy, ty, nx, ny, &tx0, &ty0);
            pcur_x = nx; pcur_y = ny;
            cur_x = tx0; cur_y = ty0;
            psub_x = nx;  psub_y = ny;
            sub_x = tx0; sub_y = ty0;
            subpath_open = 0;
            break;
        }
        case VG_PATH_CMD_LINE_TO: {
            float nx = RF() + ox, ny = RF() + oy;
            transform_pt(sx, shx, tx, shy, sy, ty, nx, ny, &tx0, &ty0);
            add_edge(cm, cur_x, cur_y, tx0, ty0);
            pprev_cp_x = pcur_x; pprev_cp_y = pcur_y;
            pcur_x = nx; pcur_y = ny;
            cur_x = tx0; cur_y = ty0;
            subpath_open = 1;
            break;
        }
        case VG_PATH_CMD_HLINE_TO: {
            /* x changes, y stays the same in path space */
            float nx = RF() + ox;
            transform_pt(sx, shx, tx, shy, sy, ty, nx, pcur_y, &tx0, &ty0);
            add_edge(cm, cur_x, cur_y, tx0, ty0);
            pprev_cp_x = pcur_x; pprev_cp_y = pcur_y;
            pcur_x = nx;
            cur_x = tx0; cur_y = ty0;
            subpath_open = 1;
            break;
        }
        case VG_PATH_CMD_VLINE_TO: {
            /* y changes, x stays the same in path space */
            float ny = RF() + oy;
            transform_pt(sx, shx, tx, shy, sy, ty, pcur_x, ny, &tx0, &ty0);
            add_edge(cm, cur_x, cur_y, tx0, ty0);
            pprev_cp_x = pcur_x; pprev_cp_y = pcur_y;
            pcur_y = ny;
            cur_x = tx0; cur_y = ty0;
            subpath_open = 1;
            break;
        }
        case VG_PATH_CMD_QUAD_TO: {
            float qx0 = RF() + ox, qy0 = RF() + oy;
            float qx1 = RF() + ox, qy1 = RF() + oy;
            float tc0x, tc0y, tc1x, tc1y;
            transform_pt(sx, shx, tx, shy, sy, ty, qx0, qy0, &tc0x, &tc0y);
            transform_pt(sx, shx, tx, shy, sy, ty, qx1, qy1, &tc1x, &tc1y);
            flatten_quad(cm, cur_x, cur_y, tc0x, tc0y, tc1x, tc1y, 0);
            pprev_cp_x = qx0; pprev_cp_y = qy0;
            pcur_x = qx1; pcur_y = qy1;
            cur_x = tc1x; cur_y = tc1y;
            subpath_open = 1;
            break;
        }
        case VG_PATH_CMD_SQUAD_TO: {
            /* Smooth quadratic: reflect previous control point in path space */
            float rp0x = 2.f * pcur_x - pprev_cp_x;
            float rp0y = 2.f * pcur_y - pprev_cp_y;
            float endx = RF() + ox, endy = RF() + oy;
            float tr0x, tr0y, t1x, t1y;
            transform_pt(sx, shx, tx, shy, sy, ty, rp0x, rp0y, &tr0x, &tr0y);
            transform_pt(sx, shx, tx, shy, sy, ty, endx, endy, &t1x, &t1y);
            flatten_quad(cm, cur_x, cur_y, tr0x, tr0y, t1x, t1y, 0);
            pprev_cp_x = rp0x; pprev_cp_y = rp0y;
            pcur_x = endx; pcur_y = endy;
            cur_x = t1x; cur_y = t1y;
            subpath_open = 1;
            break;
        }
        case VG_PATH_CMD_CUBIC_TO: {
            float p1x = RF() + ox, p1y = RF() + oy;
            float p2x = RF() + ox, p2y = RF() + oy;
            float p3x = RF() + ox, p3y = RF() + oy;
            float t1x, t1y, t2x, t2y, t3x, t3y;
            transform_pt(sx, shx, tx, shy, sy, ty, p1x, p1y, &t1x, &t1y);
            transform_pt(sx, shx, tx, shy, sy, ty, p2x, p2y, &t2x, &t2y);
            transform_pt(sx, shx, tx, shy, sy, ty, p3x, p3y, &t3x, &t3y);
            flatten_cubic(cm, cur_x, cur_y, t1x, t1y, t2x, t2y, t3x, t3y, 0);
            pprev_cp_x = p2x; pprev_cp_y = p2y;
            pcur_x = p3x; pcur_y = p3y;
            cur_x = t3x; cur_y = t3y;
            subpath_open = 1;
            break;
        }
        case VG_PATH_CMD_SCUBIC_TO: {
            /* C1 smooth cubic: reflect previous control point in path space */
            float rp2x = 2.f * pcur_x - pprev_cp_x;
            float rp2y = 2.f * pcur_y - pprev_cp_y;
            float p2x = RF() + ox, p2y = RF() + oy;
            float p3x = RF() + ox, p3y = RF() + oy;
            float t1x, t1y, t2x, t2y, t3x, t3y;
            transform_pt(sx, shx, tx, shy, sy, ty, rp2x, rp2y, &t1x, &t1y);
            transform_pt(sx, shx, tx, shy, sy, ty, p2x, p2y, &t2x, &t2y);
            transform_pt(sx, shx, tx, shy, sy, ty, p3x, p3y, &t3x, &t3y);
            flatten_cubic(cm, cur_x, cur_y, t1x, t1y, t2x, t2y, t3x, t3y, 0);
            pprev_cp_x = p2x; pprev_cp_y = p2y;
            pcur_x = p3x; pcur_y = p3y;
            cur_x = t3x; cur_y = t3y;
            subpath_open = 1;
            break;
        }
        case VG_PATH_CMD_SCCWARC_TO:
        case VG_PATH_CMD_SCWARC_TO:
        case VG_PATH_CMD_LCCWARC_TO:
        case VG_PATH_CMD_LCWARC_TO: {
            float rh  = RF_RAW();
            float rv  = RF_RAW();
            float rot = RF_RAW();
            float ex  = RF() + ox;
            float ey  = RF() + oy;
            /* Endpoint-to-centre arc conversion in path space */
            float rot_r = rot * (float)M_PI / 180.f;
            float cos_r = cosf(-rot_r), sin_r = sinf(-rot_r);
            float mx = (pcur_x - ex) * .5f, my = (pcur_y - ey) * .5f;
            float x1p = cos_r * mx + sin_r * my;
            float y1p = -sin_r * mx + cos_r * my;
            float x1p2 = x1p * x1p, y1p2 = y1p * y1p;
            float rh2 = rh * rh, rv2 = rv * rv;
            float sq = (rh2 * rv2 - rh2 * y1p2 - rv2 * x1p2) /
                       (rh2 * y1p2 + rv2 * x1p2);
            if (sq < 0.f) sq = 0.f;
            float fac = sqrtf(sq);
            int large  = (seg == VG_PATH_CMD_LCCWARC_TO || seg == VG_PATH_CMD_LCWARC_TO);
            int sweep  = (seg == VG_PATH_CMD_SCWARC_TO  || seg == VG_PATH_CMD_LCWARC_TO);
            if (large == sweep) fac = -fac;
            float cxp = fac * rh * y1p / rv;
            float cyp = -fac * rv * x1p / rh;
            float cx_ = cos_r * cxp - sin_r * cyp + (pcur_x + ex) * .5f;
            float cy_ = sin_r * cxp + cos_r * cyp + (pcur_y + ey) * .5f;
            float theta1 = atan2f((y1p - cyp) / rv, (x1p - cxp) / rh);
            float theta2 = atan2f((-y1p - cyp) / rv, (-x1p - cxp) / rh);
            if (!sweep && theta2 > theta1) theta2 -= 2.f * (float)M_PI;
            if ( sweep && theta2 < theta1) theta2 += 2.f * (float)M_PI;
            /* Transform ellipse centre */
            float tcx, tcy;
            transform_pt(sx, shx, tx, shy, sy, ty, cx_, cy_, &tcx, &tcy);
            flatten_arc(cm, tcx, tcy, rh, rv, theta1, theta2, rot);
            transform_pt(sx, shx, tx, shy, sy, ty, ex, ey, &tx0, &ty0);
            pcur_x = ex; pcur_y = ey;
            cur_x = tx0; cur_y = ty0;
            subpath_open = 1;
            break;
        }
        default:
            /* Unknown segment – skip */
            break;
        }
#undef RF
#undef RF_RAW
    }
done:
    /* Implicitly close any final open subpath for FILL semantics. */
    if (subpath_open && (cur_x != sub_x || cur_y != sub_y)) {
        add_edge(cm, cur_x, cur_y, sub_x, sub_y);
    }
    return;
}

/* =========================================================================
 * Tiler: bin edges into Y-bands (no X bins)
 * =========================================================================
 * RTL note:
 *   - The tiler walks the edge stream once and bursts edge-IDs into
 *     per-band ring buffers in DRAM.  Only the band header (base, count)
 *     stays on-chip.
 *   - The 1-pixel guard band keeps edges that lie just inside a neighbour
 *     band visible to the current band's AET (otherwise pixels at the
 *     band boundary would see incomplete winding).
 * ========================================================================= */
static void tiler_bin(struct vg_cmodel *cm)
{
    int th = cm->tile_h;

    /* Initialise per-band headers: base = ty*MAX_BAND_EDGES, count = 0. */
    for (int ty = 0; ty < cm->tiles_y; ty++) {
        cm->band_hdr[ty].base  = (uint32_t)(ty * MAX_BAND_EDGES);
        cm->band_hdr[ty].count = 0;
    }

    const float TILE_BIN_GUARD = 1.0f;

    for (int ei = 0; ei < cm->num_edges; ei++) {
        Edge *e = &cm->edges[ei];
        float ymin = fminf(e->y0, e->y1) - TILE_BIN_GUARD;
        float ymax = fmaxf(e->y0, e->y1) + TILE_BIN_GUARD;

        int ty0 = clampi((int)floorf(ymin / th), 0, cm->tiles_y - 1);
        int ty1 = clampi((int)floorf(ymax / th), 0, cm->tiles_y - 1);

        for (int ty = ty0; ty <= ty1; ty++) {
            uint32_t cnt = cm->band_hdr[ty].count;
            if (cnt < (uint32_t)MAX_BAND_EDGES) {
                /* DRAM write: dram_band_lists[base + cnt] = ei */
                cm->dram_band_lists[cm->band_hdr[ty].base + cnt] = ei;
                cm->band_hdr[ty].count = cnt + 1;
            } else {
                static int warned = 0;
                if (!warned) {
                    fprintf(stderr,
                            "[cmodel] WARN: band %d edges exceed MAX_BAND_EDGES=%d (num_edges=%d)\n",
                            ty, MAX_BAND_EDGES, cm->num_edges);
                    warned = 1;
                }
            }
        }
    }
}

/* =========================================================================
 * ID-prefetch FIFO: streams a band's edge-IDs from DRAM on-chip.
 * =========================================================================
 * RTL: a tiny FIFO (256 entries × 16 b = 0.5 KB) refilled by a DMA engine
 * from dram_band_lists[band_hdr[ty].base .. base+count].  The BE pops one
 * ID per consumer cycle; when the FIFO is empty the DMA refills it with a
 * burst of up to ID_PREFETCH_FIFO_DEPTH IDs.
 *
 * In this C-model we model the same pop/refill semantics so that any
 * future micro-architectural exploration (FIFO depth, burst length) can
 * be done without changing the algorithm.
 * ========================================================================= */
static void id_fifo_open_band(struct vg_cmodel *cm, int ty)
{
    cm->id_fifo_head = 0;
    cm->id_fifo_tail = 0;
    cm->id_dma_next  = cm->band_hdr[ty].base;
    cm->id_dma_end   = cm->band_hdr[ty].base + cm->band_hdr[ty].count;
}

/* Refill the FIFO from DRAM; returns number of IDs newly available. */
static int id_fifo_refill(struct vg_cmodel *cm)
{
    int added = 0;
    while (cm->id_dma_next < cm->id_dma_end &&
           (cm->id_fifo_tail - cm->id_fifo_head) < ID_PREFETCH_FIFO_DEPTH) {
        int slot = cm->id_fifo_tail % ID_PREFETCH_FIFO_DEPTH;
        cm->id_fifo[slot] = cm->dram_band_lists[cm->id_dma_next++];
        cm->id_fifo_tail++;
        added++;
    }
    return added;
}

/* Pop one ID; returns -1 when band exhausted. */
static int id_fifo_pop(struct vg_cmodel *cm)
{
    if (cm->id_fifo_head == cm->id_fifo_tail) {
        if (id_fifo_refill(cm) == 0) return -1;
    }
    int slot = cm->id_fifo_head % ID_PREFETCH_FIFO_DEPTH;
    cm->id_fifo_head++;
    return cm->id_fifo[slot];
}

/* =========================================================================
 * Back-End: AET-based scanline rasteriser
 * =========================================================================
 * RTL pipeline shape modelled here:
 *   For each pixel-row in the band:
 *     For each sub-row sy (1 sample if AA off, AA_N if on):
 *       1) Walk the band's edge list, gather x-intercepts of edges that
 *          straddle the sub-row's y; build a small "Active Intercepts"
 *          list (this is the AET for that sub-row).  In RTL the list
 *          lives in a small SRAM (~256 entries) and the per-edge divide
 *          (1 / dy) is computed once when the edge enters the AET.
 *       2) Insertion-sort the intercepts by x (small N -> RTL bubble sort
 *          or odd-even sorter; predictable latency).
 *       3) March pixel columns left-to-right with a running winding
 *          counter, advancing through the sorted intercepts.  For each
 *          horizontal sub-sample sx of each pixel, a 1-bit "inside"
 *          decision is added to that pixel's sub-sample counter.
 *     End sub-row loop.
 *     For each pixel with non-zero coverage: paint, mask, blend, write.
 * ========================================================================= */

typedef struct { float xi; int dir; } Intercept;

/* RGBA float colour (definition repeated near paint code for locality). */
typedef struct { float r, g, b, a; } RGBA_f;

/* Forward decls: defs appear later in this file. */
static RGBA_f u32_to_rgbaf(uint32_t c);
static RGBA_f eval_paint(struct vg_cmodel *cm, float px, float py);
static RGBA_f blend_pixel(uint32_t blend_mode, RGBA_f src, RGBA_f dst);

/* sRGB <-> linear conversion (IEC 61966-2-1).  The OpenVG spec performs
 * alpha compositing in linear light when the destination surface is
 * sRGB-encoded (which the reference implementation always uses).  The
 * cmodel mirrors that behaviour so partial-alpha blends (smooth masks,
 * gradient edges, glyph anti-aliasing) match the RI bit-for-bit. */
static inline float srgb_to_linear(float c)
{
    if (c <= 0.04045f) return c * (1.f / 12.92f);
    return powf((c + 0.055f) * (1.f / 1.055f), 2.4f);
}
static inline float linear_to_srgb(float c)
{
    if (c <= 0.0031308f) return c * 12.92f;
    return 1.055f * powf(c, 1.f / 2.4f) - 0.055f;
}

static int edge_active_at_y(const Edge *e, float y)
{
    float ymin = fminf(e->y0, e->y1);
    float ymax = fmaxf(e->y0, e->y1);
    /*
     * CTS_TOP_RIGHT rule (matches OpenVG RI): lower boundary INCLUSIVE.
     *   y > ymin  (exclude screen-top)  &&  y <= ymax  (include screen-bottom).
     */
    return (y > ymin && y <= ymax);
}

/* Small insertion sort.  N typically <= a few dozen even for tiger. */
static void sort_intercepts(Intercept *a, int n)
{
    for (int i = 1; i < n; i++) {
        Intercept k = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].xi > k.xi) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = k;
    }
}

static void rasterize_band(struct vg_cmodel *cm, int ty,
                            Intercept *ints_scratch)
{
    int th = cm->tile_h;
    int y0 = ty * th;
    int y1 = clampi(y0 + th, 0, (int)cm->fb_h);
    int fb_w = (int)cm->fb_w;

    uint32_t fill_rule  = rf_read(cm, VG_REG_FILL_RULE);
    uint32_t blend_mode = rf_read(cm, VG_REG_BLEND_MODE);
    uint32_t aa_mode    = rf_read(cm, VG_REG_AA_SAMPLES);
    uint32_t surf_linear = rf_read(cm, VG_REG_SURF_LINEAR);
    uint32_t surf_premul = rf_read(cm, VG_REG_SURF_PREMULT);

    int sc_en = (int)rf_read(cm, VG_REG_SCISSOR_EN);
    int sc_x  = (int)rf_read(cm, VG_REG_SCISSOR_X);
    int sc_y  = (int)rf_read(cm, VG_REG_SCISSOR_Y);
    int sc_w  = (int)rf_read(cm, VG_REG_SCISSOR_W);
    int sc_h  = (int)rf_read(cm, VG_REG_SCISSOR_H);

    int   n_sub;
    float radius;
    if (aa_mode == VG_AA_NONE) { n_sub = 1;     radius = 0.f;  }
    else                       { n_sub = AA_N;  radius = 0.5f; }
    float step          = (n_sub > 1) ? (2.f * radius / (float)n_sub) : 0.f;
    int   total_samples = n_sub * n_sub;
    /* Per-pixel sub-sample inside-counter for the current pixel-row.    */
    /* Stored as float to avoid an extra int->float conversion later.    */
    float *cov = cm->cov_row;

    for (int py = y0; py < y1; py++) {
        /* Reset per-row coverage accumulator. */
        for (int i = 0; i < fb_w; i++) cov[i] = 0.f;

        for (int sy = 0; sy < n_sub; sy++) {
            float spy = (n_sub == 1)
                            ? ((float)py + 0.5f)
                            : ((float)py + 0.5f - radius + ((float)sy + 0.5f) * step);

            /*
             * (1) Build AET for this sub-row by streaming the band's
             *     edge-IDs through the on-chip prefetch FIFO.  Re-open
             *     the FIFO each sub-row pass (DMA cursor rewinds to the
             *     band header's base).
             */
            id_fifo_open_band(cm, ty);
            int n_ints = 0;
            int eid;
            while ((eid = id_fifo_pop(cm)) >= 0) {
                const Edge *e = &cm->edges[eid];
                if (!edge_active_at_y(e, spy)) continue;
                float t  = (spy - e->y0) / (e->y1 - e->y0);
                float xi = e->x0 + t * (e->x1 - e->x0);
                ints_scratch[n_ints].xi  = xi;
                ints_scratch[n_ints].dir = e->dir;
                n_ints++;
            }

            /* (2) Sort by x */
            sort_intercepts(ints_scratch, n_ints);

            /* (3) March pixel columns L->R, advancing intercept index. */
            int winding = 0;
            int idx     = 0;
            for (int px = 0; px < fb_w; px++) {
                int row_inside = 0;
                for (int sx = 0; sx < n_sub; sx++) {
                    float spx = (n_sub == 1)
                                    ? ((float)px + 0.5f)
                                    : ((float)px + 0.5f - radius + ((float)sx + 0.5f) * step);
                    /*
                     * CTS_TOP_RIGHT left-boundary inclusive: cross at xi
                     * counts when xi <= spx.
                     */
                    while (idx < n_ints && ints_scratch[idx].xi <= spx) {
                        winding += ints_scratch[idx].dir;
                        idx++;
                    }
                    int inside = (fill_rule == VG_REG_FILL_EVEN_ODD)
                                     ? (abs(winding) & 1)
                                     : (winding != 0);
                    row_inside += inside;
                }
                cov[px] += (float)row_inside;
            }
        }

        /* Paint + mask + blend pass over pixels with non-zero coverage. */
        for (int px = 0; px < fb_w; px++) {
            if (cov[px] == 0.f) continue;
            if (sc_en) {
                if (px < sc_x || px >= sc_x + sc_w ||
                    py < sc_y || py >= sc_y + sc_h)
                    continue;
            }
            float coverage = cov[px] / (float)total_samples;

            uint32_t grad_type = rf_read(cm, VG_REG_GRAD_TYPE);
            RGBA_f src = eval_paint(cm, px + .5f, py + .5f);

            if (rf_read(cm, VG_REG_COLOR_XFORM_EN)) {
                src.r = clampf(src.r * rf_readf(cm, VG_REG_COLOR_XFORM_0) + rf_readf(cm, VG_REG_COLOR_XFORM_4));
                src.g = clampf(src.g * rf_readf(cm, VG_REG_COLOR_XFORM_1) + rf_readf(cm, VG_REG_COLOR_XFORM_5));
                src.b = clampf(src.b * rf_readf(cm, VG_REG_COLOR_XFORM_2) + rf_readf(cm, VG_REG_COLOR_XFORM_6));
                src.a = clampf(src.a * rf_readf(cm, VG_REG_COLOR_XFORM_3) + rf_readf(cm, VG_REG_COLOR_XFORM_7));
            }

            if (surf_linear && grad_type != VG_GRAD_NONE) {
                src.r = srgb_to_linear(clampf(src.r));
                src.g = srgb_to_linear(clampf(src.g));
                src.b = srgb_to_linear(clampf(src.b));
            }

            /* Mask is folded into the coverage scalar (linear-light
             * lerp below), matching the OpenVG RI which applies mask
             * via maskBuffer->readMaskCoverage() before the blend's
             * linear-space anti-aliasing combine. */
            if (rf_read(cm, VG_REG_MASK_EN)) {
                uint32_t mstride = rf_read(cm, VG_REG_MASK_STRIDE);
                const uint8_t *mdata = cm->mask_ptr;
                if (mdata) {
                    uint8_t mv = mdata[(uint32_t)py * mstride + (uint32_t)px];
                    coverage *= mv / 255.f;
                }
            }
            if (coverage == 0.f) continue;

            uint32_t *dst_px = &cm->fb[(uint32_t)py * cm->fb_w + (uint32_t)px];
            RGBA_f dst = u32_to_rgbaf(*dst_px);
            if (surf_premul && dst.a > 1e-6f) {
                float ia = 1.f / dst.a;
                dst.r *= ia;
                dst.g *= ia;
                dst.b *= ia;
            }

            /* Blend in the destination (sRGB) byte space, exactly like
             * the OpenVG RI does inside Color::blend.  Source is the
             * raw paint sample; coverage/mask are NOT folded in here. */
            RGBA_f r = blend_pixel(blend_mode, src, dst);

            /* Anti-aliasing combine in linear-light space: the RI
             * converts both the blended result and the original dst
             * back to lRGBA_PRE before mixing by coverage, then back
             * to sRGB.  This is what makes partial-coverage / partial
             * mask pixels match RI bit-for-bit. */
            float r_lr = surf_linear ? clampf(r.r) : srgb_to_linear(clampf(r.r));
            float r_lg = surf_linear ? clampf(r.g) : srgb_to_linear(clampf(r.g));
            float r_lb = surf_linear ? clampf(r.b) : srgb_to_linear(clampf(r.b));
            float d_lr = surf_linear ? clampf(dst.r) : srgb_to_linear(dst.r);
            float d_lg = surf_linear ? clampf(dst.g) : srgb_to_linear(dst.g);
            float d_lb = surf_linear ? clampf(dst.b) : srgb_to_linear(dst.b);

            float o_lr = r_lr * coverage + d_lr * (1.f - coverage);
            float o_lg = r_lg * coverage + d_lg * (1.f - coverage);
            float o_lb = r_lb * coverage + d_lb * (1.f - coverage);
            float o_a  = r.a  * coverage + dst.a * (1.f - coverage);

            /* RI stores zero RGB for fully transparent non-premultiplied pixels. */
            if (!surf_premul && o_a <= 1e-6f) {
                o_lr = 0.f;
                o_lg = 0.f;
                o_lb = 0.f;
            }

            float out_r = surf_linear ? clampf(o_lr) : clampf(linear_to_srgb(o_lr));
            float out_g = surf_linear ? clampf(o_lg) : clampf(linear_to_srgb(o_lg));
            float out_b = surf_linear ? clampf(o_lb) : clampf(linear_to_srgb(o_lb));
            uint8_t r8 = (uint8_t)(out_r * 255.f + .5f);
            uint8_t g8 = (uint8_t)(out_g * 255.f + .5f);
            uint8_t b8 = (uint8_t)(out_b * 255.f + .5f);
            uint8_t a8 = (uint8_t)(clampf(o_a) * 255.f + .5f);
            *dst_px = rgba_pack(r8, g8, b8, a8);
        }
    }
}

/* =========================================================================
 * Paint evaluation
 * ========================================================================= */

static RGBA_f u32_to_rgbaf(uint32_t c)
{
    RGBA_f f = {
        rgba_r(c) / 255.f,
        rgba_g(c) / 255.f,
        rgba_b(c) / 255.f,
        rgba_a(c) / 255.f
    };
    return f;
}

static RGBA_f rgba_add(RGBA_f a, RGBA_f b)
{
    RGBA_f r = { a.r + b.r, a.g + b.g, a.b + b.b, a.a + b.a };
    return r;
}

static RGBA_f rgba_sub(RGBA_f a, RGBA_f b)
{
    RGBA_f r = { a.r - b.r, a.g - b.g, a.b - b.b, a.a - b.a };
    return r;
}

static RGBA_f rgba_scale(RGBA_f a, float s)
{
    RGBA_f r = { a.r * s, a.g * s, a.b * s, a.a * s };
    return r;
}

static RGBA_f rgba_lerp(RGBA_f a, RGBA_f b, float t)
{
    RGBA_f r = {
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t,
    };
    return r;
}

static RGBA_f rgba_clamp01(RGBA_f a)
{
    RGBA_f r = { clampf(a.r), clampf(a.g), clampf(a.b), clampf(a.a) };
    return r;
}

static RGBA_f ramp_stop_color(struct vg_cmodel *cm, int i)
{
    RGBA_f c = {
        cm->ramp_color[i][0],
        cm->ramp_color[i][1],
        cm->ramp_color[i][2],
        cm->ramp_color[i][3],
    };
    return c;
}

static float pos_mod(float x, float m)
{
    float r = fmodf(x, m);
    if (r < 0.f) r += m;
    return r;
}

static RGBA_f ramp_integrate(struct vg_cmodel *cm, int num_stops,
                             float gmin, float gmax)
{
    RGBA_f c = (RGBA_f){0, 0, 0, 0};
    if (gmin == 1.f || gmax == 0.f) return c;

    int i = 0;
    for (; i < num_stops - 1; i++) {
        float s = rf_readf(cm, VG_REG_CRAMP_OFFSET(i));
        float e = rf_readf(cm, VG_REG_CRAMP_OFFSET(i + 1));
        if (gmin >= s && gmin < e) {
            float g = (e > s + 1e-10f) ? ((gmin - s) / (e - s)) : 0.f;
            RGBA_f sc = ramp_stop_color(cm, i);
            RGBA_f ec = ramp_stop_color(cm, i + 1);
            RGBA_f rc = rgba_lerp(sc, ec, g);
            c = rgba_sub(c, rgba_scale(rgba_add(sc, rc), 0.5f * (gmin - s)));
            break;
        }
    }

    for (; i < num_stops - 1; i++) {
        float s = rf_readf(cm, VG_REG_CRAMP_OFFSET(i));
        float e = rf_readf(cm, VG_REG_CRAMP_OFFSET(i + 1));
        RGBA_f sc = ramp_stop_color(cm, i);
        RGBA_f ec = ramp_stop_color(cm, i + 1);
        c = rgba_add(c, rgba_scale(rgba_add(sc, ec), 0.5f * (e - s)));

        if (gmax >= s && gmax < e) {
            float g = (e > s + 1e-10f) ? ((gmax - s) / (e - s)) : 0.f;
            RGBA_f rc = rgba_lerp(sc, ec, g);
            c = rgba_sub(c, rgba_scale(rgba_add(rc, ec), 0.5f * (e - gmax)));
            break;
        }
    }

    return c;
}

static RGBA_f ramp_sample_filtered(struct vg_cmodel *cm, int num_stops,
                                   float gradient, float rho,
                                   uint32_t spread_mode,
                                   int ramp_premult)
{
    RGBA_f c = (RGBA_f){0, 0, 0, 0};

    if (rho <= 0.f) {
        float t = gradient;
        switch (spread_mode) {
        case 1: {
            float g = pos_mod(t, 2.f);
            t = (g < 1.f) ? g : (2.f - g);
            break;
        }
        case 2:
            t = t - floorf(t);
            break;
        default:
            t = clampf(t);
            break;
        }

        for (int i = 0; i < num_stops - 1; i++) {
            float s = rf_readf(cm, VG_REG_CRAMP_OFFSET(i));
            float e = rf_readf(cm, VG_REG_CRAMP_OFFSET(i + 1));
            if (t >= s && t < e) {
                float g = (e > s + 1e-10f) ? ((t - s) / (e - s)) : 0.f;
                g = clampf(g);
                c = rgba_lerp(ramp_stop_color(cm, i), ramp_stop_color(cm, i + 1), g);
                goto done;
            }
        }
        c = ramp_stop_color(cm, num_stops - 1);
        goto done;
    }

    {
        float gmin = gradient - 0.5f * rho;
        float gmax = gradient + 0.5f * rho;
        RGBA_f avg;

        switch (spread_mode) {
        case 1: {
            avg = ramp_integrate(cm, num_stops, 0.f, 1.f);
            float gmini = floorf(gmin);
            float gmaxi = floorf(gmax);
            c = rgba_scale(avg, gmaxi + 1.f - gmini);

            if (((int)gmini) & 1) {
                c = rgba_sub(c, ramp_integrate(cm, num_stops,
                                               clampf(1.f - (gmin - gmini)), 1.f));
            } else {
                c = rgba_sub(c, ramp_integrate(cm, num_stops,
                                               0.f, clampf(gmin - gmini)));
            }

            if (((int)gmaxi) & 1) {
                c = rgba_sub(c, ramp_integrate(cm, num_stops,
                                               0.f, clampf(1.f - (gmax - gmaxi))));
            } else {
                c = rgba_sub(c, ramp_integrate(cm, num_stops,
                                               clampf(gmax - gmaxi), 1.f));
            }
            break;
        }
        case 2: {
            avg = ramp_integrate(cm, num_stops, 0.f, 1.f);
            float gmini = floorf(gmin);
            float gmaxi = floorf(gmax);
            c = rgba_scale(avg, gmaxi + 1.f - gmini);
            c = rgba_sub(c, ramp_integrate(cm, num_stops,
                                           0.f, clampf(gmin - gmini)));
            c = rgba_sub(c, ramp_integrate(cm, num_stops,
                                           clampf(gmax - gmaxi), 1.f));
            break;
        }
        default: {
            if (gmin < 0.f)
                c = rgba_add(c, rgba_scale(ramp_stop_color(cm, 0), fminf(gmax, 0.f) - gmin));
            if (gmax > 1.f)
                c = rgba_add(c, rgba_scale(ramp_stop_color(cm, num_stops - 1),
                                           gmax - fmaxf(gmin, 1.f)));
            gmin = clampf(gmin);
            gmax = clampf(gmax);
            c = rgba_add(c, ramp_integrate(cm, num_stops, gmin, gmax));
            c = rgba_clamp01(rgba_scale(c, 1.f / rho));
            goto done;
        }
        }

        c = rgba_clamp01(rgba_scale(c, 1.f / rho));

        if (rho >= 0.5f) {
            float ratio = fminf((rho - 0.5f) * 2.f, 1.f);
            c = rgba_add(rgba_scale(avg, ratio), rgba_scale(c, 1.f - ratio));
        }
    }

done:
    if (ramp_premult) {
        if (c.a > 1e-6f) {
            float ia = 1.f / c.a;
            c.r *= ia;
            c.g *= ia;
            c.b *= ia;
        } else {
            c.r = c.g = c.b = 0.f;
        }
    }
    return c;
}

static RGBA_f eval_paint(struct vg_cmodel *cm, float px, float py)
{
    uint32_t grad_type = rf_read(cm, VG_REG_GRAD_TYPE);

    if (grad_type == VG_GRAD_NONE) {
        RGBA_f f = {
            clampf(rf_readf(cm, VG_REG_FILL_COLOR_R_F)),
            clampf(rf_readf(cm, VG_REG_FILL_COLOR_G_F)),
            clampf(rf_readf(cm, VG_REG_FILL_COLOR_B_F)),
            clampf(rf_readf(cm, VG_REG_FILL_COLOR_A_F)),
        };
        return f;
    }

    if (grad_type == VG_GRAD_PATTERN) {
        /* Sample texture in screen space with REPEAT tiling. */
        uint32_t tw = rf_read(cm, VG_REG_TEX_WIDTH);
        uint32_t th = rf_read(cm, VG_REG_TEX_HEIGHT);
        if (tw == 0 || th == 0 || cm->image_data == NULL)
            return u32_to_rgbaf(rf_read(cm, VG_REG_FILL_COLOR));
        int ix = (int)floorf(px);
        int iy = (int)floorf(py);
        ix %= (int)tw; if (ix < 0) ix += (int)tw;
        iy %= (int)th; if (iy < 0) iy += (int)th;
        return u32_to_rgbaf(tex_sample(cm, ix, iy));
    }

    /* Evaluate gradient */
    float gx0 = rf_readf(cm, VG_REG_GRAD_X0);
    float gy0 = rf_readf(cm, VG_REG_GRAD_Y0);
    float gx1 = rf_readf(cm, VG_REG_GRAD_X1);
    float gy1 = rf_readf(cm, VG_REG_GRAD_Y1);
    float s2p_sx  = rf_readf(cm, VG_REG_SURF2PAINT_SX);
    float s2p_shx = rf_readf(cm, VG_REG_SURF2PAINT_SHX);
    float s2p_tx  = rf_readf(cm, VG_REG_SURF2PAINT_TX);
    float s2p_shy = rf_readf(cm, VG_REG_SURF2PAINT_SHY);
    float s2p_sy  = rf_readf(cm, VG_REG_SURF2PAINT_SY);
    float s2p_ty  = rf_readf(cm, VG_REG_SURF2PAINT_TY);
    float qx = s2p_sx * px + s2p_shx * py + s2p_tx;
    float qy = s2p_shy * px + s2p_sy * py + s2p_ty;
    float t   = 0.f;
    float rho = 0.f;

    if (grad_type == VG_GRAD_LINEAR) {
        float dx = gx1 - gx0, dy = gy1 - gy0;
        float len2 = dx * dx + dy * dy;
        if (len2 > 1e-10f) {
            t = ((qx - gx0) * dx + (qy - gy0) * dy) / len2;
            rho = 1.f / sqrtf(len2);
        }
    } else { /* RADIAL */
        float gr  = rf_readf(cm, VG_REG_GRAD_R);
        float dx  = qx - gx0, dy = qy - gy0;
        t = (gr > 1e-6f) ? (sqrtf(dx * dx + dy * dy) / gr) : 0.f;
    }

    /* Sample colour ramp */
    int num_stops = (int)rf_read(cm, VG_REG_CRAMP_COUNT);
    RGBA_f col = {0, 0, 0, 1};
    if (num_stops == 0) return col;

    int ramp_premult = (int)rf_read(cm, VG_REG_RAMP_PREMULT);
    if (num_stops == 1) {
        col = ramp_stop_color(cm, 0);
        if (ramp_premult && col.a > 1e-6f) {
            float ia = 1.f / col.a;
            col.r *= ia; col.g *= ia; col.b *= ia;
        }
        return col;
    }

    return ramp_sample_filtered(cm, num_stops, t, rho,
                                rf_read(cm, VG_REG_GRAD_SPREAD),
                                ramp_premult);
}

/* =========================================================================
 * Porter-Duff blending
 * ========================================================================= */
static RGBA_f blend_pixel(uint32_t blend_mode,
                           RGBA_f src, RGBA_f dst)
{
    RGBA_f out;
    float sa = src.a, da = dst.a;
    switch (blend_mode) {
    case VG_REG_BLEND_SRC:
        return src;
    case VG_REG_BLEND_SRC_OVER:
        out.r = src.r * sa + dst.r * da * (1.f - sa);
        out.g = src.g * sa + dst.g * da * (1.f - sa);
        out.b = src.b * sa + dst.b * da * (1.f - sa);
        out.a = sa + da * (1.f - sa);
        if (out.a > 1e-6f) { out.r /= out.a; out.g /= out.a; out.b /= out.a; }
        return out;
    case VG_REG_BLEND_DST_OVER:
        out.r = dst.r * da + src.r * sa * (1.f - da);
        out.g = dst.g * da + src.g * sa * (1.f - da);
        out.b = dst.b * da + src.b * sa * (1.f - da);
        out.a = da + sa * (1.f - da);
        if (out.a > 1e-6f) { out.r /= out.a; out.g /= out.a; out.b /= out.a; }
        return out;
    case VG_REG_BLEND_MULTIPLY:
        out.r = src.r * dst.r; out.g = src.g * dst.g;
        out.b = src.b * dst.b; out.a = clampf(sa + da - sa * da);
        return out;
    case VG_REG_BLEND_SCREEN:
        out.r = src.r + dst.r - src.r * dst.r;
        out.g = src.g + dst.g - src.g * dst.g;
        out.b = src.b + dst.b - src.b * dst.b;
        out.a = clampf(sa + da - sa * da);
        return out;
    case VG_REG_BLEND_ADDITIVE:
        out.r = clampf(src.r + dst.r);
        out.g = clampf(src.g + dst.g);
        out.b = clampf(src.b + dst.b);
        out.a = clampf(sa + da);
        return out;
    default:
        return src;
    }
}

/* =========================================================================
 * Top-level simulation run
 * ========================================================================= */
int vg_cmodel_run(vg_cmodel_t cm)
{
    if (!cm) return -1;

    /* Set BUSY status */
    rf_write(cm, VG_REG_STATUS,
             rf_read(cm, VG_REG_STATUS) & ~VG_STATUS_IDLE);
    rf_write(cm, VG_REG_STATUS,
             rf_read(cm, VG_REG_STATUS) | VG_STATUS_FE_BUSY);

    /* Tile size from register */
    if (rf_read(cm, VG_REG_TILE_SIZE) == VG_TILE_64) {
        cm->tile_w = cm->tile_h = 64;
    } else {
        cm->tile_w = cm->tile_h = 32;
    }
    cm->tiles_x = ((int)cm->fb_w  + cm->tile_w - 1) / cm->tile_w;
    cm->tiles_y = ((int)cm->fb_h  + cm->tile_h - 1) / cm->tile_h;
    if (cm->tiles_x > MAX_TILES_X) cm->tiles_x = MAX_TILES_X;
    if (cm->tiles_y > MAX_TILES_Y) cm->tiles_y = MAX_TILES_Y;

    /* --- Front End --- */
    cm->num_edges = 0;
    fe_process_path(cm);

    rf_write(cm, VG_REG_STATUS,
             rf_read(cm, VG_REG_STATUS) & ~VG_STATUS_FE_BUSY);
    rf_write(cm, VG_REG_STATUS,
             rf_read(cm, VG_REG_STATUS) | VG_STATUS_BE_BUSY);

    /* --- Tiler (Y-band binning) --- */
    tiler_bin(cm);

    /* --- Back End: one AET-march per non-empty band --- */
    /*
     * Intercept scratch sized to the worst case (every band edge crosses).
     * In RTL this is a small SRAM (~256 entries) backed by a spill path.
     */
    Intercept *ints_scratch =
        (Intercept *)malloc(sizeof(Intercept) * (MAX_BAND_EDGES + 1));
    if (!ints_scratch) return -1;

    for (int ty = 0; ty < cm->tiles_y; ty++) {
        if (cm->band_hdr[ty].count == 0) continue;
        rasterize_band(cm, ty, ints_scratch);
    }
    free(ints_scratch);

    /* Set IDLE, fire IRQ */
    rf_write(cm, VG_REG_STATUS,
             (rf_read(cm, VG_REG_STATUS) & ~VG_STATUS_BE_BUSY) | VG_STATUS_IDLE);
    rf_write(cm, VG_REG_IRQ_STATUS,
             rf_read(cm, VG_REG_IRQ_STATUS) | VG_IRQ_DONE | VG_IRQ_FE_DONE | VG_IRQ_BE_DONE);

    return 0;
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */
vg_cmodel_t vg_cmodel_create(uint32_t fb_width, uint32_t fb_height)
{
    struct vg_cmodel *cm = (struct vg_cmodel *)calloc(1, sizeof(*cm));
    if (!cm) return NULL;

    if (fb_width > MAX_FB_W) {
        fprintf(stderr, "[cmodel] fb_width %u exceeds MAX_FB_W %d\n",
                fb_width, MAX_FB_W);
        free(cm); return NULL;
    }

    cm->fb_w = fb_width;
    cm->fb_h = fb_height;
    cm->fb   = (uint32_t *)calloc(fb_width * fb_height, sizeof(uint32_t));
    if (!cm->fb) { free(cm); return NULL; }

    cm->tile_w = cm->tile_h = 32;
    cm->tiles_x = ((int)fb_width  + 31) / 32;
    cm->tiles_y = ((int)fb_height + 31) / 32;

    /* DRAM model: in real HW this lives in external DRAM, not on-chip. */
    cm->dram_band_lists = (int *)calloc((size_t)(MAX_TILES_Y * MAX_BAND_EDGES),
                                        sizeof(int));
    if (!cm->dram_band_lists) { free(cm->fb); free(cm); return NULL; }

    cm->cov_row = (float *)calloc(fb_width, sizeof(float));
    if (!cm->cov_row) { free(cm->dram_band_lists); free(cm->fb); free(cm); return NULL; }

    cache_init(&cm->tex_cache);

    /* Default register values */
    rf_write(cm, VG_REG_STATUS, VG_STATUS_IDLE);
    rf_write(cm, VG_REG_SURF_WIDTH,  fb_width);
    rf_write(cm, VG_REG_SURF_HEIGHT, fb_height);
    rf_write(cm, VG_REG_SURF_STRIDE, fb_width * 4);
    rf_write(cm, VG_REG_SURF_FORMAT, VG_FMT_RGBA8888);
    /* Identity matrix */
    rf_write(cm, VG_REG_MATRIX_SX,  0x3F800000u); /* 1.0f */
    rf_write(cm, VG_REG_MATRIX_SY,  0x3F800000u);
    rf_write(cm, VG_REG_BLEND_MODE, VG_REG_BLEND_SRC_OVER);
    rf_write(cm, VG_REG_FILL_COLOR_R_F, 0);
    rf_write(cm, VG_REG_FILL_COLOR_G_F, 0);
    rf_write(cm, VG_REG_FILL_COLOR_B_F, 0);
    rf_write(cm, VG_REG_FILL_COLOR_A_F, float_to_u32(1.f));
    rf_write(cm, VG_REG_SURF_LINEAR, 0);
    rf_write(cm, VG_REG_SURF_PREMULT, 0);
    rf_write(cm, VG_REG_COLOR_XFORM_EN, 0);
    rf_write(cm, VG_REG_RAMP_PREMULT, 0);
    rf_write(cm, VG_REG_AA_SAMPLES, VG_AA_8X);
    for (int i = 0; i < 16; i++) {
        cm->ramp_color[i][0] = 0.f;
        cm->ramp_color[i][1] = 0.f;
        cm->ramp_color[i][2] = 0.f;
        cm->ramp_color[i][3] = 1.f;
    }

    return cm;
}

void vg_cmodel_destroy(vg_cmodel_t cm)
{
    if (!cm) return;
    free(cm->fb);
    free(cm->dram_band_lists);
    free(cm->cov_row);
    free(cm);
}

/* =========================================================================
 * Register access
 * ========================================================================= */
void vg_cmodel_reg_write(vg_cmodel_t cm, uint32_t offset, uint32_t val)
{
    rf_write(cm, offset, val);
    /* Kick simulation when the path start register is written */
    if (offset == VG_REG_PATH_KICK || offset == VG_REG_CTRL) {
        if ((val & (VG_CTRL_START | 1u)) != 0)
            vg_cmodel_run(cm);
    }
}

/* 64-bit pointer setters for use by the test-bench / cmodel driver shim */
void vg_cmodel_set_path_ptr(vg_cmodel_t cm, const void *ptr, uint32_t size)
{
    if (!cm) return;
    cm->path_ptr  = (const uint8_t *)ptr;
    cm->path_size = size;
    rf_write(cm, VG_REG_PATH_SIZE, size);
}

void vg_cmodel_set_mask_ptr(vg_cmodel_t cm, const void *ptr)
{
    if (!cm) return;
    cm->mask_ptr = (const uint8_t *)ptr;
}

void vg_cmodel_set_image_ptr(vg_cmodel_t cm, const void *ptr,
                              uint32_t stride, uint32_t w, uint32_t h)
{
    if (!cm) return;
    cm->image_data   = (const uint8_t *)ptr;
    cm->image_stride = stride;
    cm->image_w      = w;
    cm->image_h      = h;
}

void vg_cmodel_set_ramp_color(vg_cmodel_t cm, uint32_t index, const float rgba[4])
{
    if (!cm || !rgba || index >= 16) return;
    cm->ramp_color[index][0] = rgba[0];
    cm->ramp_color[index][1] = rgba[1];
    cm->ramp_color[index][2] = rgba[2];
    cm->ramp_color[index][3] = rgba[3];
}

uint32_t vg_cmodel_reg_read(vg_cmodel_t cm, uint32_t offset)
{
    return rf_read(cm, offset);
}

/* =========================================================================
 * Frame buffer access
 * ========================================================================= */
const uint32_t *vg_cmodel_get_framebuffer(vg_cmodel_t cm,
                                           uint32_t *out_w,
                                           uint32_t *out_h)
{
    if (!cm) return NULL;
    if (out_w) *out_w = cm->fb_w;
    if (out_h) *out_h = cm->fb_h;
    return cm->fb;
}

uint32_t *vg_cmodel_get_fb_rw(vg_cmodel_t cm, uint32_t *out_w, uint32_t *out_h)
{
    if (!cm) return NULL;
    if (out_w) *out_w = cm->fb_w;
    if (out_h) *out_h = cm->fb_h;
    return cm->fb;
}

int vg_cmodel_save_ppm(vg_cmodel_t cm, const char *filename)
{
    if (!cm || !filename) return -1;
    FILE *fp = fopen(filename, "wb");
    if (!fp) return -1;
    fprintf(fp, "P6\n%u %u\n255\n", cm->fb_w, cm->fb_h);
    for (uint32_t i = 0; i < cm->fb_w * cm->fb_h; i++) {
        uint32_t c = cm->fb[i];
        uint8_t rgb[3] = { rgba_r(c), rgba_g(c), rgba_b(c) };
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
    return 0;
}
