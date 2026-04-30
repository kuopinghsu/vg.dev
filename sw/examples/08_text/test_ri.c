/*
 * 08_text/test_ri.c
 *
 * Scene: white background + deep-blue "OPENVA" text rendered along an
 * arc with per-glyph rotation/scale via the matrix registers.
 * Renders via the OpenVG Reference Implementation → ri.ppm.
 */
#include "../ri_helper.h"
#include "glyphs.h"

#define W 360
#define H 200

/* -------------------------------------------------------------------------
 * Per-renderer state for StringRenderer callbacks.  Holds the working
 * VGPath and a cmd/coord scratch buffer.  set_matrix loads (Y-flip *
 * local) so that the same screen-Y-down local matrix is shared with the
 * cmodel test.
 * ------------------------------------------------------------------------- */
typedef struct {
    VGPath   path;
    VGubyte  cmds  [256];
    VGfloat  coords[1024];
    int      n_cmds, n_coords;
} RIState;

static void ri_emit_move (void *u, float x, float y)
{
    RIState *s = u;
    s->cmds[s->n_cmds++] = VG_MOVE_TO_ABS;
    s->coords[s->n_coords++] = x; s->coords[s->n_coords++] = y;
}
static void ri_emit_line (void *u, float x, float y)
{
    RIState *s = u;
    s->cmds[s->n_cmds++] = VG_LINE_TO_ABS;
    s->coords[s->n_coords++] = x; s->coords[s->n_coords++] = y;
}
static void ri_emit_cubic(void *u, float x1, float y1,
                                    float x2, float y2,
                                    float x,  float y)
{
    RIState *s = u;
    s->cmds[s->n_cmds++] = VG_CUBIC_TO_ABS;
    s->coords[s->n_coords++] = x1; s->coords[s->n_coords++] = y1;
    s->coords[s->n_coords++] = x2; s->coords[s->n_coords++] = y2;
    s->coords[s->n_coords++] = x;  s->coords[s->n_coords++] = y;
}
static void ri_emit_close(void *u)
{
    RIState *s = u;
    s->cmds[s->n_cmds++] = VG_CLOSE_PATH;
}

/*
 * Compose flip · local and load as PATH_USER_TO_SURFACE.
 *
 * local (row-major):    flip F (row-major):
 *   [ sx  shx  tx ]       [ 1  0  0 ]
 *   [ shy sy   ty ]       [ 0 -1  H ]
 *   [ 0   0    1  ]       [ 0  0  1 ]
 *
 * F * local =
 *   [  sx    shx    tx       ]
 *   [ -shy  -sy   -ty + H    ]
 *   [  0     0     1         ]
 *
 * OpenVG matrices are column-major:
 *   { sx, -shy, 0,   shx, -sy, 0,   tx, -ty + H, 1 }
 */
static void ri_set_matrix(void *u,
                          float sx,  float shx, float tx,
                          float shy, float sy,  float ty)
{
    (void)u;
    VGfloat m[9] = {
        sx,        -shy,           0.f,
        shx,       -sy,            0.f,
        tx,        -ty + (float)H, 1.f
    };
    vgSeti(VG_MATRIX_MODE, VG_MATRIX_PATH_USER_TO_SURFACE);
    vgLoadMatrix(m);
}

static void ri_draw_and_reset(void *u)
{
    RIState *s = u;
    if (s->n_cmds == 0) return;
    vgClearPath(s->path, VG_PATH_CAPABILITY_ALL);
    vgAppendPathData(s->path, s->n_cmds, s->cmds, s->coords);
    vgDrawPath(s->path, VG_FILL_PATH);
    s->n_cmds    = 0;
    s->n_coords  = 0;
}

int main(void)
{
    ri_init(W, H);
    ri_clear_white();

    RIState state;
    state.n_cmds   = 0;
    state.n_coords = 0;
    state.path = vgCreatePath(VG_PATH_FORMAT_STANDARD, VG_PATH_DATATYPE_F,
                              1.f, 0.f, 0, 0, VG_PATH_CAPABILITY_ALL);

    VGPaint paint = vgCreatePaint();
    vgSetParameteri(paint, VG_PAINT_TYPE, VG_PAINT_TYPE_COLOR);
    /* Deep blue (#1E3A8A) */
    VGfloat fg[] = { 0x1E / 255.f, 0x3A / 255.f, 0x8A / 255.f, 1.f };
    vgSetParameterfv(paint, VG_PAINT_COLOR, 4, fg);
    vgSetPaint(paint, VG_FILL_PATH);

    vgSeti(VG_BLEND_MODE, VG_BLEND_SRC_OVER);
    vgSeti(VG_FILL_RULE,  VG_NON_ZERO);

    StringRenderer sr = {
        ri_set_matrix,
        { ri_emit_move, ri_emit_line, ri_emit_cubic, ri_emit_close, &state },
        ri_draw_and_reset,
        &state,
    };

    /* Spell "OPENVA" along an arc baseline with mild per-glyph tilt and
     * a sinusoidal scale wave.  Glyph cells are 60×80; advance 50 packs
     * them slightly. */
    render_styled_string(&sr, "OPENVA",
                          /* pen_x0   */ 50.f,
                          /* pen_y0   */ 70.f,
                          /* advance  */ 50.f,
                          /* arc_amp  */ 30.f,
                          /* tilt_step*/ 0.05f,
                          /* base_scl */ 0.95f,
                          /* scl_amp  */ 0.10f);

    vgDestroyPaint(paint);
    vgDestroyPath(state.path);

    ri_save_ppm("ri.ppm");
    ri_deinit();
    return 0;
}
