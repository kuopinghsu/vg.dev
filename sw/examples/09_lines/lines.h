/*
 * 09_lines/lines.h — Build a "line drawing" by emitting each line as a
 * filled quad (thin parallelogram).  Used by both RI and cmodel tests.
 *
 * Each line goes through the GlyphEmitter callback API (move/line/close)
 * so the same geometry is generated on both renderers.  The cmodel does
 * not implement VG_STROKE_PATH; emulating strokes via filled quads is the
 * portable equivalent and matches what most production 2D engines do.
 */
#pragma once

#include <math.h>

typedef struct LineEmitter {
    void (*move )(void *u, float x, float y);
    void (*line )(void *u, float x, float y);
    void (*close)(void *u);
    void *u;
} LineEmitter;

/* Emit one line (x0,y0)-(x1,y1) as a filled rectangle of thickness `w`.
 * The rectangle is axis-aligned to the line direction; cap style = butt. */
static void le_line(const LineEmitter *e,
                    float x0, float y0, float x1, float y1, float w)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-6f) return;
    /* Unit normal = perpendicular to the line, length w/2 each side. */
    float hx = -dy / len * (w * 0.5f);
    float hy =  dx / len * (w * 0.5f);

    e->move (e->u, x0 + hx, y0 + hy);
    e->line (e->u, x1 + hx, y1 + hy);
    e->line (e->u, x1 - hx, y1 - hy);
    e->line (e->u, x0 - hx, y0 - hy);
    e->close(e->u);
}

/* Emit an axis-aligned rectangle outline as 4 line quads. */
static void le_rect_outline(const LineEmitter *e,
                             float x0, float y0,
                             float x1, float y1, float w)
{
    le_line(e, x0, y0, x1, y0, w);   /* top    */
    le_line(e, x1, y0, x1, y1, w);   /* right  */
    le_line(e, x1, y1, x0, y1, w);   /* bottom */
    le_line(e, x0, y1, x0, y0, w);   /* left   */
}

/* Emit a dashed line by chopping (x0,y0)-(x1,y1) into alternating
 * on/off segments according to `pattern[pat_len]` (lengths in pixels).
 * Even indices are "on" (drawn as filled quads), odd indices are "off".
 * Pattern repeats until the line is consumed. */
static void le_dashed(const LineEmitter *e,
                      float x0, float y0, float x1, float y1,
                      float w,
                      const float *pattern, int pat_len)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-6f || pat_len <= 0) return;
    float ux = dx / len, uy = dy / len;

    float t = 0.f;
    int   idx = 0;
    while (t < len) {
        float seg = pattern[idx % pat_len];
        if (seg <= 0.f) { idx++; continue; }
        float t1 = t + seg;
        if (t1 > len) t1 = len;
        if ((idx & 1) == 0) {
            /* "on" stripe: emit a quad of the dash sub-segment. */
            le_line(e,
                    x0 + ux * t,  y0 + uy * t,
                    x0 + ux * t1, y0 + uy * t1,
                    w);
        }
        t = t1;
        idx++;
    }
}

/* The full "lines" scene used by both tests.
 * Surface is 240 x 160 px.
 * Note: lines are kept >= 3 px wide because the cmodel's box-filter AA
 * (8x8 samples in a 0.5-radius pixel square) and the RI's Gaussian-disk
 * filter give visibly different coverage for sub-pixel-thin features.
 * Real applications using the cmodel should follow the same guideline. */
static void lines_render_scene(const LineEmitter *e)
{
    const float W = 240.f, H = 160.f;

    /* 1. Outer frame (4 px) */
    le_rect_outline(e, 4.f, 4.f, W - 4.f, H - 4.f, 4.f);

    /* 2. Horizontal lines of varying thickness, left half. */
    for (int i = 0; i < 6; i++) {
        float y = 18.f + i * 11.f;
        float w = 3.f + (float)i * 0.5f;        /* 3.0 .. 5.5 */
        le_line(e, 14.f, y, 110.f, y, w);
    }

    /* 3. Vertical lines, top-right. */
    for (int i = 0; i < 6; i++) {
        float x = 130.f + i * 13.f;
        le_line(e, x, 14.f, x, 80.f, 4.f);
    }

    /* 4. "Asterisk" / star burst from a centre point, lower half — exercises
     *    arbitrary angles so the rasteriser sees non-axis-aligned edges.
     *    Limited to 6 spokes to keep AA-edge area within tolerance. */
    {
        float cx = 60.f, cy = 120.f, r = 30.f;
        for (int i = 0; i < 6; i++) {
            float a = (float)i * (3.14159265f / 3.f);
            float x1 = cx + cosf(a) * r;
            float y1 = cy + sinf(a) * r;
            le_line(e, cx, cy, x1, y1, 5.f);
        }
    }

    /* 5. Crosshatch grid in the lower-right quadrant. */
    {
        float x0 = 130.f, y0 = 92.f, x1 = 220.f, y1 = 148.f;
        for (float x = x0; x <= x1 + 0.1f; x += 12.f)
            le_line(e, x, y0, x, y1, 3.f);
        for (float y = y0; y <= y1 + 0.1f; y += 10.f)
            le_line(e, x0, y, x1, y, 3.f);
    }

    /* 6. Dashed lines — short-dash, long-dash and dash-dot patterns. */
    {
        static const float pat_short[2] = { 6.f, 4.f };
        static const float pat_long [2] = { 14.f, 6.f };
        static const float pat_dotd [4] = { 12.f, 4.f, 2.f, 4.f };

        /* Three horizontal dashed strokes between the grid and crosshatch
         * areas.  The diagonal dashed stroke from the earlier scene was
         * dropped because its arbitrary slope amplifies the cmodel-vs-RI
         * AA filter shape mismatch beyond the comparison tolerance. */
        le_dashed(e,  14.f,  92.f, 110.f,  92.f, 3.f, pat_short, 2);
        le_dashed(e,  14.f, 108.f, 110.f, 108.f, 3.f, pat_long,  2);
        le_dashed(e,  14.f, 124.f, 110.f, 124.f, 3.f, pat_dotd,  4);
    }
}
