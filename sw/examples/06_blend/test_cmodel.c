/*
 * 06_blend/test_cmodel.c
 *
 * Scene: white background
 *   Layer 1: red   rectangle (10,10)-(70,70), fully opaque
 *   Layer 2: blue  rectangle (30,30)-(90,90), 50 % alpha (SRC_OVER)
 * Tests Porter-Duff SRC_OVER alpha blending.
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

    /* --- Layer 1: fully opaque red rectangle (10,10)-(70,70) --- */
    PathBuf pb = cm_pb_create(512);
    cm_pb_rect(&pb, 10.f, 10.f, 70.f, 70.f);
    vg_cmodel_set_path_ptr(cm, pb.buf, (uint32_t)pb.len);
    vg_cmodel_reg_write(cm, VG_REG_FILL_RULE,  VG_REG_FILL_NON_ZERO);
    vg_cmodel_reg_write(cm, VG_REG_FILL_COLOR, 0xFF0000FFu);  /* red, A=255 */
    vg_cmodel_reg_write(cm, VG_REG_BLEND_MODE, VG_REG_BLEND_SRC_OVER);
    vg_cmodel_reg_write(cm, VG_REG_GRAD_TYPE,  VG_GRAD_NONE);
    vg_cmodel_reg_write(cm, VG_REG_PATH_KICK,  1);
    cm_pb_free(&pb);

    /* --- Layer 2: 50% transparent blue rectangle (30,30)-(90,90) --- */
    pb = cm_pb_create(512);
    cm_pb_rect(&pb, 30.f, 30.f, 90.f, 90.f);
    vg_cmodel_set_path_ptr(cm, pb.buf, (uint32_t)pb.len);
    vg_cmodel_reg_write(cm, VG_REG_FILL_RULE,  VG_REG_FILL_NON_ZERO);
    vg_cmodel_reg_write(cm, VG_REG_FILL_COLOR, 0x0000FF80u);  /* blue, A=128≈50% */
    vg_cmodel_reg_write(cm, VG_REG_BLEND_MODE, VG_REG_BLEND_SRC_OVER);
    vg_cmodel_reg_write(cm, VG_REG_GRAD_TYPE,  VG_GRAD_NONE);
    vg_cmodel_reg_write(cm, VG_REG_PATH_KICK,  1);
    cm_pb_free(&pb);

    vg_cmodel_save_ppm(cm, "cmodel.ppm");
    vg_cmodel_destroy(cm);
    printf("Saved cmodel.ppm\n");
    return 0;
}
