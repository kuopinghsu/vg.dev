/*
 * 03_gradient_linear/test_ri.c
 *
 * Scene: white background + rectangle (10,10)-(90,90) filled with a
 *        horizontal linear gradient: yellow (left) → blue (right)
 * Renders via the OpenVG Reference Implementation → ri.ppm
 */
#include "../ri_helper.h"

#define W 100
#define H 100

int main(void)
{
    ri_init(W, H);
    ri_clear_white();
    ri_set_screen_matrix();

    /* Rectangle */
    static const VGubyte cmds[5] = {
        VG_MOVE_TO_ABS, VG_LINE_TO_ABS, VG_LINE_TO_ABS, VG_LINE_TO_ABS, VG_CLOSE_PATH
    };
    static const VGfloat coords[8] = { 10,10,  90,10,  90,90,  10,90 };

    VGPath path = vgCreatePath(VG_PATH_FORMAT_STANDARD, VG_PATH_DATATYPE_F,
                               1.f, 0.f, 0, 0, VG_PATH_CAPABILITY_ALL);
    vgAppendPathData(path, 5, cmds, coords);

    /* Linear gradient (screen coords: x=10 yellow → x=90 blue) */
    VGPaint paint = vgCreatePaint();
    vgSetParameteri(paint, VG_PAINT_TYPE, VG_PAINT_TYPE_LINEAR_GRADIENT);

    VGfloat linGrad[] = { 10.f, 50.f,  90.f, 50.f };   /* x0,y0, x1,y1 */
    vgSetParameterfv(paint, VG_PAINT_LINEAR_GRADIENT, 4, linGrad);

    /* (offset, r, g, b, a) × 2 */
    VGfloat stops[] = {
        0.0f,  1.f, 1.f, 0.f, 1.f,   /* yellow */
        1.0f,  0.f, 0.f, 1.f, 1.f    /* blue   */
    };
    vgSetParameterfv(paint, VG_PAINT_COLOR_RAMP_STOPS, 10, stops);
    vgSetParameteri(paint, VG_PAINT_COLOR_RAMP_SPREAD_MODE, VG_COLOR_RAMP_SPREAD_PAD);
    vgSetParameteri(paint, VG_PAINT_COLOR_RAMP_PREMULTIPLIED, VG_FALSE);

    vgSetPaint(paint, VG_FILL_PATH);
    vgSeti(VG_BLEND_MODE, VG_BLEND_SRC_OVER);
    vgSeti(VG_FILL_RULE, VG_NON_ZERO);
    vgDrawPath(path, VG_FILL_PATH);

    vgDestroyPaint(paint);
    vgDestroyPath(path);

    ri_save_ppm("ri.ppm");
    ri_deinit();
    return 0;
}
