/*
 * 03_gradient_linear/test_cmodel.c
 *
 * Scene: white background + rectangle (10,10)-(90,90) filled with a
 *        horizontal linear gradient: yellow (left) → blue (right)
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

    /* Rectangle (10,10)-(90,90) */
    PathBuf pb = cm_pb_create(512);
    cm_pb_rect(&pb, 10.f, 10.f, 90.f, 90.f);

    vg_cmodel_set_path_ptr(cm, pb.buf, (uint32_t)pb.len);
    vg_cmodel_reg_write(cm, VG_REG_FILL_RULE,  VG_REG_FILL_NON_ZERO);
    vg_cmodel_reg_write(cm, VG_REG_BLEND_MODE, VG_REG_BLEND_SRC_OVER);

    /* Horizontal linear gradient: (10,50) → (90,50) */
    vg_cmodel_reg_write(cm, VG_REG_GRAD_TYPE,    VG_GRAD_LINEAR);
    cm_reg_f(cm, VG_REG_GRAD_X0, 10.f);
    cm_reg_f(cm, VG_REG_GRAD_Y0, 50.f);
    cm_reg_f(cm, VG_REG_GRAD_X1, 90.f);
    cm_reg_f(cm, VG_REG_GRAD_Y1, 50.f);
    vg_cmodel_reg_write(cm, VG_REG_GRAD_SPREAD,  0);  /* PAD */

    /* Stop 0: offset=0.0, yellow (RGBA 0xFFFF00FF) */
    cm_reg_f(cm, VG_REG_CRAMP_OFFSET(0), 0.0f);
    vg_cmodel_reg_write(cm, VG_REG_CRAMP_COLOR(0), 0xFFFF00FFu);
    /* Stop 1: offset=1.0, blue (RGBA 0x0000FFFF) */
    cm_reg_f(cm, VG_REG_CRAMP_OFFSET(1), 1.0f);
    vg_cmodel_reg_write(cm, VG_REG_CRAMP_COLOR(1), 0x0000FFFFu);
    vg_cmodel_reg_write(cm, VG_REG_CRAMP_COUNT, 2);

    vg_cmodel_reg_write(cm, VG_REG_PATH_KICK, 1);

    cm_pb_free(&pb);
    vg_cmodel_save_ppm(cm, "cmodel.ppm");
    vg_cmodel_destroy(cm);
    printf("Saved cmodel.ppm\n");
    return 0;
}
