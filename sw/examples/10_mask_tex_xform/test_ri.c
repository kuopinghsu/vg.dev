/*
 * 10_mask_tex_xform/test_ri.c
 *
 * Reference render of the same scene as test_cmodel.c, exercising:
 *   - PATH_USER_TO_SURFACE coordinate transform.
 *   - vgPaintPattern with a procedural texture, REPEAT-tiled.
 *   - vgMask with a VG_A_8 mask image.
 *
 * NOTE: For pattern paint the RI routes texel samples through an
 * internal sRGB <-> linear color pipeline that the cmodel (and the
 * solid-color tests) do not model.  That means individual pattern
 * pixel values do not match the cmodel byte-for-byte; the comparison
 * is expected to FAIL on colour channels but PASS on geometry/mask
 * coverage.  The cmodel side is the spec-conformant reference for
 * the pattern paint path being verified here.
 */
#include "../ri_helper.h"
#include "scene.h"
#include <string.h>

#define W SCENE_W
#define H SCENE_H

/* Append a rounded rectangle (axis-aligned in path-local coords). */
static int rrect_append(VGubyte *cmds, VGfloat *coords,
                         float x0, float y0, float x1, float y1, float r,
                         int *out_ncoords)
{
    const float k = 0.5522847498f;
    float kr = r * k;
    int n = 0, c = 0;

    cmds[n++] = VG_MOVE_TO_ABS;  coords[c++] = x0 + r; coords[c++] = y0;

    cmds[n++] = VG_LINE_TO_ABS;  coords[c++] = x1 - r; coords[c++] = y0;
    cmds[n++] = VG_CUBIC_TO_ABS;
    coords[c++] = x1 - r + kr; coords[c++] = y0;
    coords[c++] = x1;          coords[c++] = y0 + r - kr;
    coords[c++] = x1;          coords[c++] = y0 + r;

    cmds[n++] = VG_LINE_TO_ABS;  coords[c++] = x1; coords[c++] = y1 - r;
    cmds[n++] = VG_CUBIC_TO_ABS;
    coords[c++] = x1;          coords[c++] = y1 - r + kr;
    coords[c++] = x1 - r + kr; coords[c++] = y1;
    coords[c++] = x1 - r;      coords[c++] = y1;

    cmds[n++] = VG_LINE_TO_ABS;  coords[c++] = x0 + r; coords[c++] = y1;
    cmds[n++] = VG_CUBIC_TO_ABS;
    coords[c++] = x0 + r - kr; coords[c++] = y1;
    coords[c++] = x0;          coords[c++] = y1 - r + kr;
    coords[c++] = x0;          coords[c++] = y1 - r;

    cmds[n++] = VG_LINE_TO_ABS;  coords[c++] = x0; coords[c++] = y0 + r;
    cmds[n++] = VG_CUBIC_TO_ABS;
    coords[c++] = x0;          coords[c++] = y0 + r - kr;
    coords[c++] = x0 + r - kr; coords[c++] = y0;
    coords[c++] = x0 + r;      coords[c++] = y0;

    cmds[n++] = VG_CLOSE_PATH;
    *out_ncoords = c;
    return n;
}

int main(void)
{
    ri_init(W, H);
    ri_clear_white();

    /* ---------------- Build the rounded-rect path ---------------- */
    VGubyte cmds  [64];
    VGfloat coords[256];
    int ncoords;
    int ncmds = rrect_append(cmds, coords,
                              PATH_LOCAL_X0, PATH_LOCAL_Y0,
                              PATH_LOCAL_X1, PATH_LOCAL_Y1,
                              PATH_CORNER_R, &ncoords);

    VGPath path = vgCreatePath(VG_PATH_FORMAT_STANDARD, VG_PATH_DATATYPE_F,
                               1.f, 0.f, 0, 0, VG_PATH_CAPABILITY_ALL);
    vgAppendPathData(path, ncmds, cmds, coords);
    (void)ncoords;

    /* ---------------- Procedural texture ---------------- */
    static uint8_t tex[TEX_W * TEX_H * 4];
    scene_make_texture(tex);
    /* Upload vertically flipped: pattern image is Y-up while the shared
     * texture is Y-down. */
    static uint8_t tex_flipped[TEX_W * TEX_H * 4];
    for (int r = 0; r < TEX_H; r++)
        memcpy(tex_flipped + r * TEX_W * 4,
               tex          + (TEX_H - 1 - r) * TEX_W * 4,
               TEX_W * 4);

    VGImage texImg = vgCreateImage(VG_sABGR_8888, TEX_W, TEX_H,
                                    VG_IMAGE_QUALITY_NONANTIALIASED);
    vgImageSubData(texImg, tex_flipped, TEX_W * 4, VG_sABGR_8888,
                   0, 0, TEX_W, TEX_H);

    VGPaint paint = vgCreatePaint();
    vgSetParameteri(paint, VG_PAINT_TYPE,           VG_PAINT_TYPE_PATTERN);
    vgSetParameteri(paint, VG_PAINT_PATTERN_TILING_MODE, VG_TILE_REPEAT);
    vgPaintPattern(paint, texImg);
    vgSetPaint(paint, VG_FILL_PATH);

    /* ---------------- Mask layer ---------------- */
    static uint8_t mask[SCENE_W * SCENE_H];
    scene_make_mask(mask);
    static uint8_t mask_flipped[SCENE_W * SCENE_H];
    for (int r = 0; r < SCENE_H; r++)
        memcpy(mask_flipped + r * SCENE_W,
               mask         + (SCENE_H - 1 - r) * SCENE_W,
               SCENE_W);

    VGImage maskImg = vgCreateImage(VG_A_8, SCENE_W, SCENE_H,
                                     VG_IMAGE_QUALITY_NONANTIALIASED);
    vgImageSubData(maskImg, mask_flipped, SCENE_W, VG_A_8,
                   0, 0, SCENE_W, SCENE_H);
    vgMask(maskImg, VG_SET_MASK, 0, 0, SCENE_W, SCENE_H);
    vgSeti(VG_MASKING, VG_TRUE);

    /* ---------------- Coordinate transform ---------------- */
    /* Local affine in cmodel-screen-Y-down (row-major):
     *   [ sx  shx  tx ]
     *   [ shy sy   ty ]
     *
     * PATH_USER_TO_SURFACE = Flip * Local where Flip flips Y about H.
     * Combined (row-major):
     *   [  sx    shx    tx       ]
     *   [ -shy  -sy   -ty + H    ]
     * Column-major OpenVG layout: { sx,-shy,0, shx,-sy,0, tx,-ty+H,1 }.
     */
    float sx, shx, tx, shy, sy, ty;
    scene_compose_matrix(&sx, &shx, &tx, &shy, &sy, &ty);
    VGfloat pus[9] = {
        sx,        -shy,             0.f,
        shx,       -sy,              0.f,
        tx,        -ty + (float)H,   1.f
    };
    vgSeti(VG_MATRIX_MODE, VG_MATRIX_PATH_USER_TO_SURFACE);
    vgLoadMatrix(pus);

    /* FILL_PAINT_TO_USER = inverse(PATH_USER_TO_SURFACE) so that the
     * pattern is sampled in surface space (matches cmodel's screen-space
     * texture sampling). */
    float ia, ib, itx, ic, id, ity;
    scene_invert_affine(pus[0], pus[3], pus[6],   /* row 0 */
                        pus[1], pus[4], pus[7],   /* row 1 */
                        &ia, &ib, &itx,
                        &ic, &id, &ity);
    VGfloat fpt[9] = {
        ia, ic, 0.f,
        ib, id, 0.f,
        itx, ity, 1.f
    };
    vgSeti(VG_MATRIX_MODE, VG_MATRIX_FILL_PAINT_TO_USER);
    vgLoadMatrix(fpt);

    /* Restore active matrix to PUS for path drawing. */
    vgSeti(VG_MATRIX_MODE, VG_MATRIX_PATH_USER_TO_SURFACE);

    /* ---------------- Draw ---------------- */
    vgSeti(VG_BLEND_MODE, VG_BLEND_SRC_OVER);
    vgSeti(VG_FILL_RULE,  VG_NON_ZERO);
    vgDrawPath(path, VG_FILL_PATH);

    vgDestroyImage(maskImg);
    vgDestroyImage(texImg);
    vgDestroyPaint(paint);
    vgDestroyPath(path);

    ri_save_ppm("ri.ppm");
    ri_deinit();
    return 0;
}
