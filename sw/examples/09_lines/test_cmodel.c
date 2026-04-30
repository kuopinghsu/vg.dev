/*
 * 09_lines/test_cmodel.c — same line-drawing scene rendered via cmodel.
 */
#include "../cmodel_helper.h"
#include "lines.h"

#define W 240
#define H 160

static void cm_emit_move (void *u, float x, float y)
{ PathBuf *pb = u; cm_pb_cmd(pb, VG_PATH_CMD_MOVE_TO); cm_pb_float(pb, x); cm_pb_float(pb, y); }
static void cm_emit_line (void *u, float x, float y)
{ PathBuf *pb = u; cm_pb_cmd(pb, VG_PATH_CMD_LINE_TO); cm_pb_float(pb, x); cm_pb_float(pb, y); }
static void cm_emit_close(void *u)
{ PathBuf *pb = u; cm_pb_cmd(pb, VG_PATH_CMD_CLOSE_PATH); }

int main(void)
{
    vg_cmodel_t cm = vg_cmodel_create(W, H);
    cm_set_identity(cm);
    cm_clear_white(cm, W, H);

    PathBuf pb = cm_pb_create(64 * 1024);
    LineEmitter e = { cm_emit_move, cm_emit_line, cm_emit_close, &pb };
    lines_render_scene(&e);

    vg_cmodel_set_path_ptr(cm, pb.buf, (uint32_t)pb.len);
    vg_cmodel_reg_write(cm, VG_REG_FILL_RULE,  VG_REG_FILL_NON_ZERO);
    vg_cmodel_reg_write(cm, VG_REG_FILL_COLOR, 0x000000FFu); /* black */
    vg_cmodel_reg_write(cm, VG_REG_BLEND_MODE, VG_REG_BLEND_SRC_OVER);
    vg_cmodel_reg_write(cm, VG_REG_GRAD_TYPE,  VG_GRAD_NONE);
    vg_cmodel_reg_write(cm, VG_REG_PATH_KICK,  1);

    vg_cmodel_save_ppm(cm, "cmodel.ppm");
    printf("Saved cmodel.ppm\n");
    cm_pb_free(&pb);
    vg_cmodel_destroy(cm);
    return 0;
}
