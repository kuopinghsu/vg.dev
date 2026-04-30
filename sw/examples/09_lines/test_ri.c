/*
 * 09_lines/test_ri.c — line drawing scene rendered via the OpenVG RI.
 *
 * Lines are emitted as filled thin quads (the cmodel does not implement
 * VG_STROKE_PATH, so both renderers must use the same fill geometry).
 */
#include "../ri_helper.h"
#include "lines.h"

#define W 240
#define H 160

typedef struct {
    VGubyte *cmds;  VGfloat *coords;
    int n_cmds, n_coords, cap_cmds, cap_coords;
} RIBuilder;

static void rb_cmd(RIBuilder *b, VGubyte c) { b->cmds[b->n_cmds++] = c; }
static void rb_xy (RIBuilder *b, float x, float y)
{ b->coords[b->n_coords++] = x; b->coords[b->n_coords++] = y; }

static void ri_emit_move (void *u, float x, float y)
{ rb_cmd((RIBuilder*)u, VG_MOVE_TO_ABS); rb_xy((RIBuilder*)u, x, y); }
static void ri_emit_line (void *u, float x, float y)
{ rb_cmd((RIBuilder*)u, VG_LINE_TO_ABS); rb_xy((RIBuilder*)u, x, y); }
static void ri_emit_close(void *u)
{ rb_cmd((RIBuilder*)u, VG_CLOSE_PATH); }

int main(void)
{
    ri_init(W, H);
    ri_clear_white();
    ri_set_screen_matrix();

    static VGubyte cmds  [4096];
    static VGfloat coords[16384];
    RIBuilder b = { cmds, coords, 0, 0, 4096, 16384 };
    LineEmitter e = { ri_emit_move, ri_emit_line, ri_emit_close, &b };
    lines_render_scene(&e);

    VGPath path = vgCreatePath(VG_PATH_FORMAT_STANDARD, VG_PATH_DATATYPE_F,
                               1.f, 0.f, 0, 0, VG_PATH_CAPABILITY_ALL);
    vgAppendPathData(path, b.n_cmds, b.cmds, b.coords);

    VGPaint paint = vgCreatePaint();
    vgSetParameteri(paint, VG_PAINT_TYPE, VG_PAINT_TYPE_COLOR);
    VGfloat black[] = { 0.f, 0.f, 0.f, 1.f };
    vgSetParameterfv(paint, VG_PAINT_COLOR, 4, black);
    vgSetPaint(paint, VG_FILL_PATH);

    vgSeti(VG_BLEND_MODE, VG_BLEND_SRC_OVER);
    vgSeti(VG_FILL_RULE,  VG_NON_ZERO);
    vgDrawPath(path, VG_FILL_PATH);

    vgDestroyPaint(paint);
    vgDestroyPath(path);
    ri_save_ppm("ri.ppm");
    ri_deinit();
    return 0;
}
