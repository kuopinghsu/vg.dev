/*
 * 04_gradient_radial/test_ri.c
 *
 * Scene: white background + circle (cx=50,cy=50, r=40) filled with a
 *        radial gradient: white (center) → red (edge)
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

    /* Circle path via 4 cubic beziers (screen-Y-down coords) */
    static const float k  = 0.5522847498f;
    static const float cx = 50.f, cy = 50.f, r = 40.f;
    float kxy = r * k;

    VGubyte cmds[6] = {
        VG_MOVE_TO_ABS,
        VG_CUBIC_TO_ABS, VG_CUBIC_TO_ABS, VG_CUBIC_TO_ABS, VG_CUBIC_TO_ABS,
        VG_CLOSE_PATH
    };
    VGfloat coords[26];
    int n = 0;

    coords[n++] = cx;       coords[n++] = cy - r;
    coords[n++] = cx + kxy; coords[n++] = cy - r;
    coords[n++] = cx + r;   coords[n++] = cy - kxy;
    coords[n++] = cx + r;   coords[n++] = cy;
    coords[n++] = cx + r;   coords[n++] = cy + kxy;
    coords[n++] = cx + kxy; coords[n++] = cy + r;
    coords[n++] = cx;       coords[n++] = cy + r;
    coords[n++] = cx - kxy; coords[n++] = cy + r;
    coords[n++] = cx - r;   coords[n++] = cy + kxy;
    coords[n++] = cx - r;   coords[n++] = cy;
    coords[n++] = cx - r;   coords[n++] = cy - kxy;
    coords[n++] = cx - kxy; coords[n++] = cy - r;
    coords[n++] = cx;       coords[n++] = cy - r;

    VGPath path = vgCreatePath(VG_PATH_FORMAT_STANDARD, VG_PATH_DATATYPE_F,
                               1.f, 0.f, 0, 0, VG_PATH_CAPABILITY_ALL);
    vgAppendPathData(path, 6, cmds, coords);

    /* Radial gradient: centre=focal=(50,50), radius=40 */
    VGPaint paint = vgCreatePaint();
    vgSetParameteri(paint, VG_PAINT_TYPE, VG_PAINT_TYPE_RADIAL_GRADIENT);

    VGfloat radGrad[] = { cx, cy, cx, cy, r };  /* cx,cy, fx,fy, r */
    vgSetParameterfv(paint, VG_PAINT_RADIAL_GRADIENT, 5, radGrad);

    VGfloat stops[] = {
        0.0f,  1.f, 1.f, 1.f, 1.f,   /* white at centre */
        1.0f,  1.f, 0.f, 0.f, 1.f    /* red  at edge    */
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
