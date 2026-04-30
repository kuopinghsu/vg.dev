/*
 * 06_blend/test_ri.c
 *
 * Scene: white background
 *   Layer 1: red   rectangle (10,10)-(70,70), fully opaque
 *   Layer 2: blue  rectangle (30,30)-(90,90), 50 % alpha (SRC_OVER)
 * Tests Porter-Duff SRC_OVER alpha blending.
 * Renders via the OpenVG Reference Implementation → ri.ppm
 */
#include "../ri_helper.h"

#define W 100
#define H 100

/* Helper: draw a filled rectangle with a solid colour (RGBA floats). */
static void draw_rect(VGfloat x0, VGfloat y0, VGfloat x1, VGfloat y1,
                      VGfloat r, VGfloat g, VGfloat b, VGfloat a)
{
    VGubyte  cmds[5]   = { VG_MOVE_TO_ABS, VG_LINE_TO_ABS, VG_LINE_TO_ABS,
                           VG_LINE_TO_ABS, VG_CLOSE_PATH };
    VGfloat  coords[8] = { x0,y0,  x1,y0,  x1,y1,  x0,y1 };

    VGPath path = vgCreatePath(VG_PATH_FORMAT_STANDARD, VG_PATH_DATATYPE_F,
                               1.f, 0.f, 0, 0, VG_PATH_CAPABILITY_ALL);
    vgAppendPathData(path, 5, cmds, coords);

    VGPaint paint = vgCreatePaint();
    vgSetParameteri(paint, VG_PAINT_TYPE, VG_PAINT_TYPE_COLOR);
    VGfloat color[4] = { r, g, b, a };
    vgSetParameterfv(paint, VG_PAINT_COLOR, 4, color);
    vgSetPaint(paint, VG_FILL_PATH);

    vgSeti(VG_BLEND_MODE, VG_BLEND_SRC_OVER);
    vgSeti(VG_FILL_RULE, VG_NON_ZERO);
    vgDrawPath(path, VG_FILL_PATH);

    vgDestroyPaint(paint);
    vgDestroyPath(path);
}

int main(void)
{
    ri_init(W, H);
    ri_clear_white();
    ri_set_screen_matrix();

    /* Layer 1: fully opaque red */
    draw_rect(10.f, 10.f, 70.f, 70.f,   1.f, 0.f, 0.f, 1.f);
    /* Layer 2: 50% transparent blue (blends over the red) */
    draw_rect(30.f, 30.f, 90.f, 90.f,   0.f, 0.f, 1.f, 0.5f);

    ri_save_ppm("ri.ppm");
    ri_deinit();
    return 0;
}
