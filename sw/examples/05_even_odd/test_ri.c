/*
 * 05_even_odd/test_ri.c
 *
 * Scene: white background + two concentric rectangles (outer 10→90, inner 30→70)
 *        drawn as one path with VG_EVEN_ODD fill rule.
 *        Result: a blue rectangular frame with a transparent centre hole.
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

    /*
     * Two sub-paths in one VGPath:
     *  Outer rectangle (10,10)-(90,90) – wound clockwise
     *  Inner rectangle (30,30)-(70,70) – wound counter-clockwise
     * With EVEN_ODD fill rule the inner region is not filled (frame effect).
     */
    VGubyte cmds[10] = {
        /* outer */
        VG_MOVE_TO_ABS, VG_LINE_TO_ABS, VG_LINE_TO_ABS, VG_LINE_TO_ABS, VG_CLOSE_PATH,
        /* inner */
        VG_MOVE_TO_ABS, VG_LINE_TO_ABS, VG_LINE_TO_ABS, VG_LINE_TO_ABS, VG_CLOSE_PATH
    };
    VGfloat coords[16] = {
        /* outer CW */
        10.f,10.f,  90.f,10.f,  90.f,90.f,  10.f,90.f,
        /* inner CCW */
        30.f,30.f,  30.f,70.f,  70.f,70.f,  70.f,30.f
    };

    VGPath path = vgCreatePath(VG_PATH_FORMAT_STANDARD, VG_PATH_DATATYPE_F,
                               1.f, 0.f, 0, 0, VG_PATH_CAPABILITY_ALL);
    vgAppendPathData(path, 10, cmds, coords);

    VGPaint paint = vgCreatePaint();
    vgSetParameteri(paint, VG_PAINT_TYPE, VG_PAINT_TYPE_COLOR);
    VGfloat blue[] = { 0.f, 0.f, 1.f, 1.f };
    vgSetParameterfv(paint, VG_PAINT_COLOR, 4, blue);
    vgSetPaint(paint, VG_FILL_PATH);

    vgSeti(VG_BLEND_MODE, VG_BLEND_SRC_OVER);
    vgSeti(VG_FILL_RULE, VG_EVEN_ODD);
    vgDrawPath(path, VG_FILL_PATH);

    vgDestroyPaint(paint);
    vgDestroyPath(path);

    ri_save_ppm("ri.ppm");
    ri_deinit();
    return 0;
}
