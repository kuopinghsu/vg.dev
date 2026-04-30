/*
 * 02_fill_curve/test_ri.c
 *
 * Scene: white background + green filled ellipse (cx=50,cy=50, rx=40,ry=30)
 *        approximated with 4 cubic Bezier segments.
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

    /* Ellipse via 4 cubic beziers (screen-Y-down coords)
     * cx=50, cy=50, rx=40, ry=30, k=0.5522847498 */
    static const float k  = 0.5522847498f;
    static const float cx = 50.f, cy = 50.f, rx = 40.f, ry = 30.f;

    VGubyte cmds[9] = {
        VG_MOVE_TO_ABS,
        VG_CUBIC_TO_ABS, VG_CUBIC_TO_ABS, VG_CUBIC_TO_ABS, VG_CUBIC_TO_ABS,
        VG_CLOSE_PATH
    };
    /* 1 MOVE (2) + 4 CUBIC (4×6=24) = 26 floats */
    VGfloat coords[26];
    int n = 0;
    float kx = rx * k, ky = ry * k;

    /* MOVE_TO top-center */
    coords[n++] = cx;       coords[n++] = cy - ry;
    /* top → right */
    coords[n++] = cx + kx;  coords[n++] = cy - ry;
    coords[n++] = cx + rx;  coords[n++] = cy - ky;
    coords[n++] = cx + rx;  coords[n++] = cy;
    /* right → bottom */
    coords[n++] = cx + rx;  coords[n++] = cy + ky;
    coords[n++] = cx + kx;  coords[n++] = cy + ry;
    coords[n++] = cx;       coords[n++] = cy + ry;
    /* bottom → left */
    coords[n++] = cx - kx;  coords[n++] = cy + ry;
    coords[n++] = cx - rx;  coords[n++] = cy + ky;
    coords[n++] = cx - rx;  coords[n++] = cy;
    /* left → top */
    coords[n++] = cx - rx;  coords[n++] = cy - ky;
    coords[n++] = cx - kx;  coords[n++] = cy - ry;
    coords[n++] = cx;       coords[n++] = cy - ry;

    VGPath path = vgCreatePath(VG_PATH_FORMAT_STANDARD, VG_PATH_DATATYPE_F,
                               1.f, 0.f, 0, 0, VG_PATH_CAPABILITY_ALL);
    vgAppendPathData(path, 6, cmds, coords);

    VGPaint paint = vgCreatePaint();
    vgSetParameteri(paint, VG_PAINT_TYPE, VG_PAINT_TYPE_COLOR);
    VGfloat green[] = { 0.f, 0.8f, 0.f, 1.f };
    vgSetParameterfv(paint, VG_PAINT_COLOR, 4, green);
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
