/*
 * 10_mask_tex_xform/test_cmodel.c
 *
 * Renders a single rounded-rect path that combines:
 *   - coordinate transform (rotate + scale, applied via MATRIX_*).
 *   - texture (pattern) fill, REPEAT-tiled in screen space.
 *   - alpha mask with a smooth radial falloff.
 *
 * All three features are exercised in one draw call.
 */
#include "../cmodel_helper.h"
#include "scene.h"

/* Append a rounded rectangle (axis-aligned in path-local coords) to pb. */
static void pb_rounded_rect(PathBuf *pb, float x0, float y0,
                                          float x1, float y1, float r)
{
    /* Cubic-bezier corner control offset for a quarter circle. */
    const float k = 0.5522847498f;
    float kr = r * k;

    cm_pb_cmd(pb, VG_PATH_CMD_MOVE_TO);
    cm_pb_float(pb, x0 + r); cm_pb_float(pb, y0);

    cm_pb_cmd(pb, VG_PATH_CMD_LINE_TO);
    cm_pb_float(pb, x1 - r); cm_pb_float(pb, y0);
    cm_pb_cmd(pb, VG_PATH_CMD_CUBIC_TO);
    cm_pb_float(pb, x1 - r + kr); cm_pb_float(pb, y0);
    cm_pb_float(pb, x1);          cm_pb_float(pb, y0 + r - kr);
    cm_pb_float(pb, x1);          cm_pb_float(pb, y0 + r);

    cm_pb_cmd(pb, VG_PATH_CMD_LINE_TO);
    cm_pb_float(pb, x1); cm_pb_float(pb, y1 - r);
    cm_pb_cmd(pb, VG_PATH_CMD_CUBIC_TO);
    cm_pb_float(pb, x1);          cm_pb_float(pb, y1 - r + kr);
    cm_pb_float(pb, x1 - r + kr); cm_pb_float(pb, y1);
    cm_pb_float(pb, x1 - r);      cm_pb_float(pb, y1);

    cm_pb_cmd(pb, VG_PATH_CMD_LINE_TO);
    cm_pb_float(pb, x0 + r); cm_pb_float(pb, y1);
    cm_pb_cmd(pb, VG_PATH_CMD_CUBIC_TO);
    cm_pb_float(pb, x0 + r - kr); cm_pb_float(pb, y1);
    cm_pb_float(pb, x0);          cm_pb_float(pb, y1 - r + kr);
    cm_pb_float(pb, x0);          cm_pb_float(pb, y1 - r);

    cm_pb_cmd(pb, VG_PATH_CMD_LINE_TO);
    cm_pb_float(pb, x0); cm_pb_float(pb, y0 + r);
    cm_pb_cmd(pb, VG_PATH_CMD_CUBIC_TO);
    cm_pb_float(pb, x0);          cm_pb_float(pb, y0 + r - kr);
    cm_pb_float(pb, x0 + r - kr); cm_pb_float(pb, y0);
    cm_pb_float(pb, x0 + r);      cm_pb_float(pb, y0);

    cm_pb_cmd(pb, VG_PATH_CMD_CLOSE_PATH);
}

int main(void)
{
    vg_cmodel_t cm = vg_cmodel_create(SCENE_W, SCENE_H);
    cm_set_identity(cm);
    cm_clear_white(cm, SCENE_W, SCENE_H);

    /* ---------------- Texture (pattern) ---------------- */
    static uint8_t tex[TEX_W * TEX_H * 4];
    scene_make_texture(tex);
    vg_cmodel_set_image_ptr(cm, tex, TEX_W * 4, TEX_W, TEX_H);
    vg_cmodel_reg_write(cm, VG_REG_TEX_STRIDE, TEX_W * 4);
    vg_cmodel_reg_write(cm, VG_REG_TEX_WIDTH,  TEX_W);
    vg_cmodel_reg_write(cm, VG_REG_TEX_HEIGHT, TEX_H);

    /* ---------------- Mask layer ---------------- */
    static uint8_t mask[SCENE_W * SCENE_H];
    scene_make_mask(mask);
    vg_cmodel_set_mask_ptr(cm, mask);
    vg_cmodel_reg_write(cm, VG_REG_MASK_STRIDE, SCENE_W);
    vg_cmodel_reg_write(cm, VG_REG_MASK_EN,     1);

    /* ---------------- Coordinate transform ---------------- */
    float sx, shx, tx, shy, sy, ty;
    scene_compose_matrix(&sx, &shx, &tx, &shy, &sy, &ty);
    cm_reg_f(cm, VG_REG_MATRIX_SX,  sx);
    cm_reg_f(cm, VG_REG_MATRIX_SHX, shx);
    cm_reg_f(cm, VG_REG_MATRIX_TX,  tx);
    cm_reg_f(cm, VG_REG_MATRIX_SHY, shy);
    cm_reg_f(cm, VG_REG_MATRIX_SY,  sy);
    cm_reg_f(cm, VG_REG_MATRIX_TY,  ty);

    /* ---------------- Path ---------------- */
    PathBuf pb = cm_pb_create(2 * 1024);
    pb_rounded_rect(&pb,
                    PATH_LOCAL_X0, PATH_LOCAL_Y0,
                    PATH_LOCAL_X1, PATH_LOCAL_Y1,
                    PATH_CORNER_R);
    vg_cmodel_set_path_ptr(cm, pb.buf, (uint32_t)pb.len);

    /* ---------------- Render ---------------- */
    vg_cmodel_reg_write(cm, VG_REG_FILL_RULE,  VG_REG_FILL_NON_ZERO);
    vg_cmodel_reg_write(cm, VG_REG_BLEND_MODE, VG_REG_BLEND_SRC_OVER);
    vg_cmodel_reg_write(cm, VG_REG_GRAD_TYPE,  VG_GRAD_PATTERN);
    vg_cmodel_reg_write(cm, VG_REG_PATH_KICK,  1);

    vg_cmodel_save_ppm(cm, "cmodel.ppm");
    printf("Saved cmodel.ppm\n");
    cm_pb_free(&pb);
    vg_cmodel_destroy(cm);
    return 0;
}
