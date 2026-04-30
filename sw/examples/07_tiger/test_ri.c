/*
 * 07_tiger/test_ri.c
 *
 * Render the Khronos SVG Tiger using the OpenVG Reference Implementation.
 * Writes to ri.ppm.
 *
 * Uses EGL headless pbuffer (same as main.c) but via ri_helper.h,
 * and applies the same scale+translate matrix as the original sample.
 */
#include "../ri_helper.h"
#include "tiger.h"

#include <stdlib.h>
#include <assert.h>

#define TIGER_W 512

/* -------------------------------------------------------------------------
 * Tiger path-set renderer (adapted from main.c; uses only VG_FILL_PATH since
 * the cmodel does not support stroke rendering).
 * -------------------------------------------------------------------------*/
typedef struct {
    VGFillRule   m_fillRule;
    VGPaintMode  m_paintMode;
    VGPaint      m_fillPaint;
    VGPath       m_path;
} TigerPath;

typedef struct {
    TigerPath *m_paths;
    int        m_numPaths;
} TigerPS;

static TigerPS *tiger_ps_construct(void)
{
    /* First pass: count paths and find max element count. */
    int paths = 0, c = 0, p = 0, maxElems = 0;
    while (c < tigerCommandCount) {
        c += 4;           /* skip fill/stroke/cap/join */
        p += 8;           /* skip miterLimit,strokeWidth,strokeRGB,fillRGB */
        int elems = (int)tigerPoints[p++];
        if (elems > maxElems) maxElems = elems;
        for (int e = 0; e < elems; e++) {
            switch (tigerCommands[c]) {
            case 'M': case 'L': p += 2; break;
            case 'C':           p += 6; break;
            case 'E':                   break;
            default: assert(0);
            }
            c++;
        }
        paths++;
    }

    TigerPS *ps = (TigerPS *)malloc(sizeof(TigerPS));
    ps->m_numPaths = paths;
    ps->m_paths    = (TigerPath *)malloc(paths * sizeof(TigerPath));

    VGubyte *cmd_buf = (VGubyte *)malloc((size_t)maxElems);

    /* Second pass: build VG objects. */
    int i = 0;
    c = 0; p = 0;
    while (c < tigerCommandCount) {
        char fill_ch   = tigerCommands[c++];
        char stroke_ch = tigerCommands[c++];
        c++; c++;  /* skip cap, join */
        (void)stroke_ch;

        p++;  /* miterLimit  – ignored */
        p++;  /* strokeWidth – ignored */
        p += 3;  /* strokeColor – ignored */

        float fr = tigerPoints[p++];
        float fg = tigerPoints[p++];
        float fb = tigerPoints[p++];

        int elems  = (int)tigerPoints[p++];
        int startp = p;

        VGPaintMode mode = 0;
        if (fill_ch == 'F') {
            ps->m_paths[i].m_fillRule = VG_NON_ZERO;
            mode |= VG_FILL_PATH;
        } else if (fill_ch == 'E') {
            ps->m_paths[i].m_fillRule = VG_EVEN_ODD;
            mode |= VG_FILL_PATH;
        } else {
            ps->m_paths[i].m_fillRule = VG_NON_ZERO;
        }
        ps->m_paths[i].m_paintMode = mode;

        /* Build command array and advance p. */
        for (int e = 0; e < elems; e++) {
            switch (tigerCommands[c]) {
            case 'M': cmd_buf[e] = VG_MOVE_TO_ABS;    p += 2; break;
            case 'L': cmd_buf[e] = VG_LINE_TO_ABS;    p += 2; break;
            case 'C': cmd_buf[e] = VG_CUBIC_TO_ABS;   p += 6; break;
            case 'E': cmd_buf[e] = VG_CLOSE_PATH;              break;
            default:  assert(0);
            }
            c++;
        }

        /* Create VG path with the collected commands. */
        ps->m_paths[i].m_path = vgCreatePath(
            VG_PATH_FORMAT_STANDARD, VG_PATH_DATATYPE_F,
            1.0f, 0.0f, 0, 0, VG_PATH_CAPABILITY_ALL);
        if (mode & VG_FILL_PATH)
            vgAppendPathData(ps->m_paths[i].m_path, elems,
                             cmd_buf, tigerPoints + startp);

        /* Create fill paint. */
        VGfloat color[4] = { fr, fg, fb, 1.0f };
        ps->m_paths[i].m_fillPaint = vgCreatePaint();
        vgSetParameteri(ps->m_paths[i].m_fillPaint,
                        VG_PAINT_TYPE, VG_PAINT_TYPE_COLOR);
        vgSetParameterfv(ps->m_paths[i].m_fillPaint,
                         VG_PAINT_COLOR, 4, color);
        i++;
    }

    free(cmd_buf);
    return ps;
}

static void tiger_ps_render(TigerPS *ps)
{
    vgSeti(VG_BLEND_MODE, VG_BLEND_SRC_OVER);
    for (int i = 0; i < ps->m_numPaths; i++) {
        if (!(ps->m_paths[i].m_paintMode & VG_FILL_PATH)) continue;
        vgSeti(VG_FILL_RULE, ps->m_paths[i].m_fillRule);
        vgSetPaint(ps->m_paths[i].m_fillPaint, VG_FILL_PATH);
        vgDrawPath(ps->m_paths[i].m_path, VG_FILL_PATH);
    }
    assert(vgGetError() == VG_NO_ERROR);
}

static void tiger_ps_destruct(TigerPS *ps)
{
    for (int i = 0; i < ps->m_numPaths; i++) {
        vgDestroyPaint(ps->m_paths[i].m_fillPaint);
        vgDestroyPath(ps->m_paths[i].m_path);
    }
    free(ps->m_paths);
    free(ps);
}

/* -------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------*/
int main(void)
{
    const int w = TIGER_W;
    const int h = (int)(TIGER_W * 792.0f / 612.0f + 0.5f);

    ri_init(w, h);

    /* White background */
    VGfloat cc[4] = { 1.f, 1.f, 1.f, 1.f };
    vgSetfv(VG_CLEAR_COLOR, 4, cc);
    vgClear(0, 0, w, h);

    /*
     * Set up PATH_USER_TO_SURFACE matrix in OpenVG Y-up space:
     *   scale  = w / (tigerMaxX - tigerMinX)
     *   tx     = scale * (-tigerMinX)
     *   ty     = scale * (-tigerMinY + 0.5*(h/scale - (tigerMaxY - tigerMinY)))
     *
     * OpenVG column-major 3x3:  [ sx  shx  tx ]
     *                            [ shy  sy  ty ]
     *                            [  0    0   1 ]
     * as array: [sx, shy, 0, shx, sy, 0, tx, ty, 1]
     */
    float scale = (float)w / (tigerMaxX - tigerMinX);
    float tx    = scale * (-tigerMinX);
    float ty    = scale * (-tigerMinY +
                  0.5f * ((float)h / scale - (tigerMaxY - tigerMinY)));
    VGfloat m[9] = {
        scale, 0.f,   0.f,
        0.f,   scale, 0.f,
        tx,    ty,    1.f
    };
    vgSeti(VG_MATRIX_MODE, VG_MATRIX_PATH_USER_TO_SURFACE);
    vgLoadMatrix(m);

    TigerPS *tiger = tiger_ps_construct();
    tiger_ps_render(tiger);
    tiger_ps_destruct(tiger);

    ri_save_ppm("ri.ppm");
    ri_deinit();
    return 0;
}
