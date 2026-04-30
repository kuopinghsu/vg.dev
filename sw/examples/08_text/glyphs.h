/*
 * 08_text/glyphs.h — Hand-authored vector glyphs ("O", "V", "A") in
 * path-space (Y-down) coordinates.  Used by both the RI and cmodel
 * tests so the same outline data is rasterised by both implementations.
 *
 * Each glyph consists of one or more closed sub-paths.  Holes
 * (counter-shapes) are wound opposite to the outer contour and rely
 * on VG_NON_ZERO / VG_REG_FILL_NON_ZERO fill rule.
 *
 * Coordinate system per glyph: origin (0,0) is top-left of the cell,
 * X increases right, Y increases down.  Cell size 60 wide × 80 tall.
 *
 * The renderer provides a GlyphEmitter with callbacks for MOVE/LINE/
 * CUBIC/CLOSE; each glyph function emits its commands by calling
 * those callbacks at world coordinates (ox + lx, oy + ly).
 */
#pragma once

#include <math.h>

typedef struct GlyphEmitter {
    void (*move )(void *u, float x, float y);
    void (*line )(void *u, float x, float y);
    void (*cubic)(void *u, float x1, float y1,
                           float x2, float y2,
                           float x,  float y);
    void (*close)(void *u);
    void *u;
} GlyphEmitter;

#define EMOVE(e, x, y)                                  (e)->move ((e)->u, (x), (y))
#define ELINE(e, x, y)                                  (e)->line ((e)->u, (x), (y))
#define ECUBIC(e, x1, y1, x2, y2, x, y)                 (e)->cubic((e)->u, (x1),(y1),(x2),(y2),(x),(y))
#define ECLOSE(e)                                       (e)->close((e)->u)

/* -------------------------------------------------------------------------
 * Letter O — outer ellipse + reversed inner ellipse (hole).
 *
 * Each ellipse is approximated with 4 cubic Bezier segments using the
 * standard k = 4*(sqrt(2)-1)/3 ≈ 0.55228 control-point offset.
 * ------------------------------------------------------------------------- */
static void glyph_O(const GlyphEmitter *e, float ox, float oy)
{
    const float k = 0.5522847498f;

    /* Outer ellipse: cx=30, cy=40, rx=28, ry=38   (CW in Y-down) */
    {
        float cx = ox + 30.f, cy = oy + 40.f, rx = 28.f, ry = 38.f;
        float kx = rx * k, ky = ry * k;
        EMOVE (e, cx,       cy - ry);
        ECUBIC(e, cx + kx,  cy - ry,
                  cx + rx,  cy - ky,
                  cx + rx,  cy);
        ECUBIC(e, cx + rx,  cy + ky,
                  cx + kx,  cy + ry,
                  cx,       cy + ry);
        ECUBIC(e, cx - kx,  cy + ry,
                  cx - rx,  cy + ky,
                  cx - rx,  cy);
        ECUBIC(e, cx - rx,  cy - ky,
                  cx - kx,  cy - ry,
                  cx,       cy - ry);
        ECLOSE(e);
    }

    /* Inner ellipse (hole): cx=30, cy=40, rx=14, ry=24, REVERSED winding. */
    {
        float cx = ox + 30.f, cy = oy + 40.f, rx = 14.f, ry = 24.f;
        float kx = rx * k, ky = ry * k;
        EMOVE (e, cx,       cy - ry);
        ECUBIC(e, cx - kx,  cy - ry,
                  cx - rx,  cy - ky,
                  cx - rx,  cy);
        ECUBIC(e, cx - rx,  cy + ky,
                  cx - kx,  cy + ry,
                  cx,       cy + ry);
        ECUBIC(e, cx + kx,  cy + ry,
                  cx + rx,  cy + ky,
                  cx + rx,  cy);
        ECUBIC(e, cx + rx,  cy - ky,
                  cx + kx,  cy - ry,
                  cx,       cy - ry);
        ECLOSE(e);
    }
}

/* -------------------------------------------------------------------------
 * Letter V — single closed contour, all line segments, sharp angles.
 * ------------------------------------------------------------------------- */
static void glyph_V(const GlyphEmitter *e, float ox, float oy)
{
    EMOVE(e, ox +  2.f, oy +  2.f);   /* top-left outer  */
    ELINE(e, ox + 18.f, oy +  2.f);   /* top-left inner  */
    ELINE(e, ox + 30.f, oy + 50.f);   /* interior bottom (above outer bottom) */
    ELINE(e, ox + 42.f, oy +  2.f);   /* top-right inner */
    ELINE(e, ox + 58.f, oy +  2.f);   /* top-right outer */
    ELINE(e, ox + 30.f, oy + 76.f);   /* bottom outer point */
    ECLOSE(e);
}

/* -------------------------------------------------------------------------
 * Letter A — outer triangle silhouette with crossbar, plus a triangular
 * counter (hole) above the crossbar.  Crossbar is part of the outer
 * contour to avoid splitting it into a third sub-path.
 * ------------------------------------------------------------------------- */
static void glyph_A(const GlyphEmitter *e, float ox, float oy)
{
    /* Outer outline (single contour) */
    EMOVE(e, ox + 30.f, oy +  2.f);   /* top apex                    */
    ELINE(e, ox + 58.f, oy + 78.f);   /* bottom-right outer          */
    ELINE(e, ox + 48.f, oy + 78.f);   /* bottom-right inner          */
    ELINE(e, ox + 41.f, oy + 58.f);   /* crossbar right outer        */
    ELINE(e, ox + 19.f, oy + 58.f);   /* crossbar left outer         */
    ELINE(e, ox + 12.f, oy + 78.f);   /* bottom-left inner           */
    ELINE(e, ox +  2.f, oy + 78.f);   /* bottom-left outer           */
    ECLOSE(e);

    /* Counter (hole) — REVERSED winding triangle above the crossbar. */
    EMOVE(e, ox + 30.f, oy + 22.f);   /* top of counter              */
    ELINE(e, ox + 22.f, oy + 50.f);   /* counter left  (reversed)    */
    ELINE(e, ox + 38.f, oy + 50.f);   /* counter right (reversed)    */
    ECLOSE(e);
}

/* -------------------------------------------------------------------------
 * Letter P — vertical stem + bowl, with a closed counter (reversed).
 * Block (line-only) form to keep the regression tolerant of AA filter
 * differences.
 * ------------------------------------------------------------------------- */
static void glyph_P(const GlyphEmitter *e, float ox, float oy)
{
    /* Outer outline (CW in Y-down) */
    EMOVE(e, ox +  2.f, oy +  2.f);
    ELINE(e, ox + 36.f, oy +  2.f);
    ELINE(e, ox + 48.f, oy + 14.f);
    ELINE(e, ox + 48.f, oy + 32.f);
    ELINE(e, ox + 36.f, oy + 44.f);
    ELINE(e, ox + 12.f, oy + 44.f);
    ELINE(e, ox + 12.f, oy + 78.f);
    ELINE(e, ox +  2.f, oy + 78.f);
    ECLOSE(e);

    /* Counter (CCW = hole under NON_ZERO) */
    EMOVE(e, ox + 12.f, oy + 12.f);
    ELINE(e, ox + 12.f, oy + 34.f);
    ELINE(e, ox + 34.f, oy + 34.f);
    ELINE(e, ox + 38.f, oy + 30.f);
    ELINE(e, ox + 38.f, oy + 16.f);
    ELINE(e, ox + 34.f, oy + 12.f);
    ECLOSE(e);
}

/* -------------------------------------------------------------------------
 * Letter E — single contour, no counter.
 * ------------------------------------------------------------------------- */
static void glyph_E(const GlyphEmitter *e, float ox, float oy)
{
    EMOVE(e, ox +  2.f, oy +  2.f);
    ELINE(e, ox + 56.f, oy +  2.f);
    ELINE(e, ox + 56.f, oy + 12.f);
    ELINE(e, ox + 12.f, oy + 12.f);
    ELINE(e, ox + 12.f, oy + 36.f);
    ELINE(e, ox + 46.f, oy + 36.f);
    ELINE(e, ox + 46.f, oy + 46.f);
    ELINE(e, ox + 12.f, oy + 46.f);
    ELINE(e, ox + 12.f, oy + 68.f);
    ELINE(e, ox + 56.f, oy + 68.f);
    ELINE(e, ox + 56.f, oy + 78.f);
    ELINE(e, ox +  2.f, oy + 78.f);
    ECLOSE(e);
}

/* -------------------------------------------------------------------------
 * Letter N — single contour, sharp diagonal interior.
 * ------------------------------------------------------------------------- */
static void glyph_N(const GlyphEmitter *e, float ox, float oy)
{
    EMOVE(e, ox +  2.f, oy +  2.f);
    ELINE(e, ox + 14.f, oy +  2.f);
    ELINE(e, ox + 46.f, oy + 60.f);
    ELINE(e, ox + 46.f, oy +  2.f);
    ELINE(e, ox + 58.f, oy +  2.f);
    ELINE(e, ox + 58.f, oy + 78.f);
    ELINE(e, ox + 46.f, oy + 78.f);
    ELINE(e, ox + 14.f, oy + 20.f);
    ELINE(e, ox + 14.f, oy + 78.f);
    ELINE(e, ox +  2.f, oy + 78.f);
    ECLOSE(e);
}

/* -------------------------------------------------------------------------
 * Glyph dispatcher and styled-string renderer with per-glyph affine
 * (rotation + scale + translation around the glyph cell centre).
 * ------------------------------------------------------------------------- */
static void glyph_emit_char(const GlyphEmitter *e, char ch, float ox, float oy)
{
    switch (ch) {
    case 'O': glyph_O(e, ox, oy); break;
    case 'P': glyph_P(e, ox, oy); break;
    case 'E': glyph_E(e, ox, oy); break;
    case 'N': glyph_N(e, ox, oy); break;
    case 'V': glyph_V(e, ox, oy); break;
    case 'A': glyph_A(e, ox, oy); break;
    default: break;
    }
}

/*
 * StringRenderer abstracts the "set affine matrix, emit glyph outlines,
 * issue one draw" cycle so RI and cmodel can share render_styled_string().
 *
 * The matrix is applied as:  out_pt = M * in_pt
 * where  M = [ sx  shx  tx ;
 *              shy sy   ty ;
 *              0   0    1  ]
 *
 * Each renderer translates this into its native call (vgLoadMatrix for
 * RI – composing any base flip itself; six register writes for cmodel).
 */
typedef struct StringRenderer {
    void (*set_matrix)(void *u,
                        float sx,  float shx, float tx,
                        float shy, float sy,  float ty);
    GlyphEmitter ge;
    /* Render currently-buffered path with the active matrix, then clear it. */
    void (*draw_and_reset)(void *u);
    void *u;
} StringRenderer;

/*
 * Compose M = T(ex,ey) · R(angle) · S(scale) · T(-cx,-cy).
 * Rotation pivots around glyph cell centre (cx,cy); the glyph is then
 * placed at (ex,ey) in screen space.
 */
static void compose_TRS(float ex, float ey,
                         float angle, float scale,
                         float cx, float cy,
                         float *out_sx,  float *out_shx, float *out_tx,
                         float *out_shy, float *out_sy,  float *out_ty)
{
    float c = cosf(angle) * scale;
    float s = sinf(angle) * scale;
    *out_sx  =  c;  *out_shx = -s;  *out_tx  = ex - c * cx + s * cy;
    *out_shy =  s;  *out_sy  =  c;  *out_ty  = ey - s * cx - c * cy;
}

/*
 * Render `str` along an arc baseline, with per-glyph rotation and a
 * gentle scale wave.  Each glyph is drawn in its own draw call.
 *
 *   pen_x0,pen_y0 — screen position of first glyph centre
 *   advance       — horizontal step between glyph centres
 *   arc_amp       — vertical amplitude of the arc baseline
 *   tilt_step     — extra rotation (radians) applied per glyph index
 *   base_scale    — baseline scale factor (1.0 = native size)
 *   scale_amp     — sinusoidal scale variation amplitude
 */
static void render_styled_string(const StringRenderer *sr, const char *str,
                                  float pen_x0, float pen_y0,
                                  float advance,
                                  float arc_amp,
                                  float tilt_step,
                                  float base_scale,
                                  float scale_amp)
{
    int i = 0;
    for (const char *p = str; *p; p++, i++) {
        if (*p == ' ') continue;

        /* Arc baseline: parabolic dip (max at the centre). */
        int   n   = 1; for (const char *q = str; *q; q++) n++;
        float mid = (float)(n - 1) * 0.5f;
        float di  = ((float)i - mid) / (mid > 0.f ? mid : 1.f);  /* -1..+1 */
        float ex  = pen_x0 + (float)i * advance;
        float ey  = pen_y0 + arc_amp * (1.f - di * di);

        /* Tangent angle = derivative of the parabola; keeps glyphs
         * aligned to the baseline.  d(ey)/d(ex) = -2*arc_amp*di / advance. */
        float dydx  = (advance > 0.f && mid > 0.f)
                          ? (-2.f * arc_amp * di / (mid * advance)) : 0.f;
        float angle = atanf(dydx) + (float)i * tilt_step;

        float scale = base_scale + scale_amp * sinf((float)i * 0.9f);

        float sx, shx, tx, shy, sy, ty;
        compose_TRS(ex, ey, angle, scale, 30.f, 40.f,
                    &sx, &shx, &tx, &shy, &sy, &ty);

        sr->set_matrix(sr->u, sx, shx, tx, shy, sy, ty);
        /* Emit glyph at path-local origin (matrix supplies position). */
        glyph_emit_char(&sr->ge, *p, 0.f, 0.f);
        sr->draw_and_reset(sr->u);
    }
}
