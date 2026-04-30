/*
 * 02_fill_curve/test_cmodel.c
 *
 * Scene: white background + green filled ellipse (cx=50,cy=50, rx=40,ry=30)
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

    /* Ellipse via 4 cubic beziers using the helper */
    PathBuf pb = cm_pb_create(1024);
    cm_pb_ellipse(&pb, 50.f, 50.f, 40.f, 30.f);

    vg_cmodel_set_path_ptr(cm, pb.buf, (uint32_t)pb.len);
    vg_cmodel_reg_write(cm, VG_REG_FILL_RULE,  VG_REG_FILL_NON_ZERO);
    vg_cmodel_reg_write(cm, VG_REG_FILL_COLOR, 0x00CC00FFu);  /* green */
    vg_cmodel_reg_write(cm, VG_REG_BLEND_MODE, VG_REG_BLEND_SRC_OVER);
    vg_cmodel_reg_write(cm, VG_REG_GRAD_TYPE,  VG_GRAD_NONE);
    vg_cmodel_reg_write(cm, VG_REG_PATH_KICK,  1);

    cm_pb_free(&pb);
    vg_cmodel_save_ppm(cm, "cmodel.ppm");
    vg_cmodel_destroy(cm);
    printf("Saved cmodel.ppm\n");
    return 0;
}
