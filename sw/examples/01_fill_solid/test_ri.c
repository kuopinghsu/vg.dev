/*
 * 01_fill_solid/test_ri.c
 *
 * Scene: white background + red filled rectangle (20,20)-(80,80)
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

    /* Build rectangle path (screen-Y-down coords) */
    static const VGubyte cmds[5] = {
        VG_MOVE_TO_ABS, VG_LINE_TO_ABS, VG_LINE_TO_ABS, VG_LINE_TO_ABS, VG_CLOSE_PATH
    };
    static const VGfloat coords[8] = { 20,20,  80,20,  80,80,  20,80 };

    VGPath path = vgCreatePath(VG_PATH_FORMAT_STANDARD, VG_PATH_DATATYPE_F,
                               1.f, 0.f, 0, 0, VG_PATH_CAPABILITY_ALL);
    vgAppendPathData(path, 5, cmds, coords);

    VGPaint paint = vgCreatePaint();
    vgSetParameteri(paint, VG_PAINT_TYPE, VG_PAINT_TYPE_COLOR);
    VGfloat red[] = { 1.f, 0.f, 0.f, 1.f };
    vgSetParameterfv(paint, VG_PAINT_COLOR, 4, red);
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
