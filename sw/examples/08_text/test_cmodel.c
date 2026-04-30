/*
 * 08_text/test_cmodel.c
 *
 * Same scene as test_ri.c — deep-blue "OPENVA" text along an arc with
 * per-glyph rotation and scale wave applied via the matrix registers.
 * Renders through the C-model.
 */
#include "../cmodel_helper.h"
#include "glyphs.h"

#define W 360
#define H 200

/* -------------------------------------------------------------------------
 * Per-renderer state for StringRenderer callbacks.  Holds the cmodel
 * handle and a single working PathBuf reused across all glyph draws.
 * ------------------------------------------------------------------------- */
typedef struct {
    vg_cmodel_t cm;
    PathBuf     pb;
} CMState;

static void cm_emit_move (void *u, float x, float y)
{
    PathBuf *pb = &((CMState *)u)->pb;
    cm_pb_cmd(pb, VG_PATH_CMD_MOVE_TO);
    cm_pb_float(pb, x); cm_pb_float(pb, y);
}
static void cm_emit_line (void *u, float x, float y)
{
    PathBuf *pb = &((CMState *)u)->pb;
    cm_pb_cmd(pb, VG_PATH_CMD_LINE_TO);
    cm_pb_float(pb, x); cm_pb_float(pb, y);
}
static void cm_emit_cubic(void *u, float x1, float y1,
                                    float x2, float y2,
                                    float x,  float y)
{
    PathBuf *pb = &((CMState *)u)->pb;
    cm_pb_cmd(pb, VG_PATH_CMD_CUBIC_TO);
    cm_pb_float(pb, x1); cm_pb_float(pb, y1);
    cm_pb_float(pb, x2); cm_pb_float(pb, y2);
    cm_pb_float(pb, x);  cm_pb_float(pb, y);
}
static void cm_emit_close(void *u)
{
    PathBuf *pb = &((CMState *)u)->pb;
    cm_pb_cmd(pb, VG_PATH_CMD_CLOSE_PATH);
}

/* StringRenderer.set_matrix → six float-register writes consumed by FE. */
static void cm_set_matrix(void *u,
                          float sx,  float shx, float tx,
                          float shy, float sy,  float ty)
{
    vg_cmodel_t cm = ((CMState *)u)->cm;
    cm_reg_f(cm, VG_REG_MATRIX_SX,  sx);
    cm_reg_f(cm, VG_REG_MATRIX_SHX, shx);
    cm_reg_f(cm, VG_REG_MATRIX_TX,  tx);
    cm_reg_f(cm, VG_REG_MATRIX_SHY, shy);
    cm_reg_f(cm, VG_REG_MATRIX_SY,  sy);
    cm_reg_f(cm, VG_REG_MATRIX_TY,  ty);
}

/* StringRenderer.draw_and_reset → kick path then truncate buffer. */
static void cm_draw_and_reset(void *u)
{
    CMState *s = (CMState *)u;
    if (s->pb.len == 0) return;
    vg_cmodel_set_path_ptr(s->cm, s->pb.buf, (uint32_t)s->pb.len);
    vg_cmodel_reg_write(s->cm, VG_REG_PATH_KICK, 1);
    s->pb.len = 0;
}

int main(void)
{
    CMState state;
    state.cm = vg_cmodel_create(W, H);
    state.pb = cm_pb_create(8 * 1024);

    cm_set_identity(state.cm);
    cm_clear_white(state.cm, W, H);

    /* Static fill / blend / rule — matrix is overwritten per glyph. */
    vg_cmodel_reg_write(state.cm, VG_REG_FILL_RULE,  VG_REG_FILL_NON_ZERO);
    vg_cmodel_reg_write(state.cm, VG_REG_FILL_COLOR, 0x1E3A8AFFu);
    vg_cmodel_reg_write(state.cm, VG_REG_BLEND_MODE, VG_REG_BLEND_SRC_OVER);
    vg_cmodel_reg_write(state.cm, VG_REG_GRAD_TYPE,  VG_GRAD_NONE);

    StringRenderer sr = {
        cm_set_matrix,
        { cm_emit_move, cm_emit_line, cm_emit_cubic, cm_emit_close, &state },
        cm_draw_and_reset,
        &state,
    };

    render_styled_string(&sr, "OPENVA",
                          /* pen_x0   */ 50.f,
                          /* pen_y0   */ 70.f,
                          /* advance  */ 50.f,
                          /* arc_amp  */ 30.f,
                          /* tilt_step*/ 0.05f,
                          /* base_scl */ 0.95f,
                          /* scl_amp  */ 0.10f);

    vg_cmodel_save_ppm(state.cm, "cmodel.ppm");
    printf("Saved cmodel.ppm\n");
    cm_pb_free(&state.pb);
    vg_cmodel_destroy(state.cm);
    return 0;
}
