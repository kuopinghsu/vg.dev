/*
 * 04_gradient_radial/test_cmodel.c
 *
 * Scene: white background + circle (cx=50,cy=50, r=40) filled with a
 *        radial gradient: white (center) → red (edge)
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

    /* Circle via 4 cubic beziers using helper */
    PathBuf pb = cm_pb_create(1024);
    cm_pb_ellipse(&pb, 50.f, 50.f, 40.f, 40.f);  /* rx=ry=40 → circle */

    vg_cmodel_set_path_ptr(cm, pb.buf, (uint32_t)pb.len);
    vg_cmodel_reg_write(cm, VG_REG_FILL_RULE,  VG_REG_FILL_NON_ZERO);
    vg_cmodel_reg_write(cm, VG_REG_BLEND_MODE, VG_REG_BLEND_SRC_OVER);

    /* Radial gradient: centre (50,50), radius 40 */
    vg_cmodel_reg_write(cm, VG_REG_GRAD_TYPE, VG_GRAD_RADIAL);
    cm_reg_f(cm, VG_REG_GRAD_X0, 50.f);  /* cx */
    cm_reg_f(cm, VG_REG_GRAD_Y0, 50.f);  /* cy */
    cm_reg_f(cm, VG_REG_GRAD_R,  40.f);  /* radius */
    vg_cmodel_reg_write(cm, VG_REG_GRAD_SPREAD, 0);  /* PAD */

    /* Stop 0: offset=0.0, white */
    cm_reg_f(cm, VG_REG_CRAMP_OFFSET(0), 0.0f);
    vg_cmodel_reg_write(cm, VG_REG_CRAMP_COLOR(0), 0xFFFFFFFFu);
    /* Stop 1: offset=1.0, red */
    cm_reg_f(cm, VG_REG_CRAMP_OFFSET(1), 1.0f);
    vg_cmodel_reg_write(cm, VG_REG_CRAMP_COLOR(1), 0xFF0000FFu);
    vg_cmodel_reg_write(cm, VG_REG_CRAMP_COUNT, 2);

    vg_cmodel_reg_write(cm, VG_REG_PATH_KICK, 1);

    cm_pb_free(&pb);
    vg_cmodel_save_ppm(cm, "cmodel.ppm");
    vg_cmodel_destroy(cm);
    printf("Saved cmodel.ppm\n");
    return 0;
}
