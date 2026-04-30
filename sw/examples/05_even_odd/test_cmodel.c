/*
 * 05_even_odd/test_cmodel.c
 *
 * Scene: white background + two concentric rectangles with EVEN_ODD fill.
 *        Result: a blue rectangular frame with a white centre hole.
 * Renders via the C-Model → cmodel.ppm
 */
#include <stdio.h>
#include "../cmodel_helper.h"

#define W 100
#define H 100

int main(void)
{
    vg_cmodel_t cm = vg_cmodel_create(W, H);
    cm_set_identity(cm);
    cm_clear_white(cm, W, H);

    /* Two concentric rectangles in one path buffer */
    PathBuf pb = cm_pb_create(1024);

    /* Outer rectangle (10,10)-(90,90) – CW winding */
    cm_pb_rect(&pb, 10.f, 10.f, 90.f, 90.f);

    /* Inner rectangle (30,30)-(70,70) – CCW winding (reverse order) */
    cm_pb_cmd(&pb, VG_PATH_CMD_MOVE_TO); cm_pb_float(&pb, 30.f); cm_pb_float(&pb, 30.f);
    cm_pb_cmd(&pb, VG_PATH_CMD_LINE_TO); cm_pb_float(&pb, 30.f); cm_pb_float(&pb, 70.f);
    cm_pb_cmd(&pb, VG_PATH_CMD_LINE_TO); cm_pb_float(&pb, 70.f); cm_pb_float(&pb, 70.f);
    cm_pb_cmd(&pb, VG_PATH_CMD_LINE_TO); cm_pb_float(&pb, 70.f); cm_pb_float(&pb, 30.f);
    cm_pb_cmd(&pb, VG_PATH_CMD_CLOSE_PATH);

    vg_cmodel_set_path_ptr(cm, pb.buf, (uint32_t)pb.len);
    vg_cmodel_reg_write(cm, VG_REG_FILL_RULE,  VG_REG_FILL_EVEN_ODD);
    vg_cmodel_reg_write(cm, VG_REG_FILL_COLOR, 0x0000FFFFu);  /* blue */
    vg_cmodel_reg_write(cm, VG_REG_BLEND_MODE, VG_REG_BLEND_SRC_OVER);
    vg_cmodel_reg_write(cm, VG_REG_GRAD_TYPE,  VG_GRAD_NONE);
    vg_cmodel_reg_write(cm, VG_REG_PATH_KICK,  1);

    cm_pb_free(&pb);
    vg_cmodel_save_ppm(cm, "cmodel.ppm");
    vg_cmodel_destroy(cm);
    printf("Saved cmodel.ppm\n");
    return 0;
}
