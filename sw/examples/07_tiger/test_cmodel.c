/*
 * 07_tiger/test_cmodel.c
 *
 * Render the Khronos SVG Tiger using the OpenVG C-Model.
 * Writes to cmodel.ppm.
 *
 * The hardware matrix registers are programmed to map tiger path-space
 * coordinates directly to screen (Y-down) coordinates:
 *
 *   cmodel_x =  scale * px  + tx
 *   cmodel_y = -scale * py  + ty
 *
 * where:
 *   scale = W / (tigerMaxX - tigerMinX)
 *   tx    = -scale * tigerMinX
 *   ty    = 0.5*H + scale * 0.5*(tigerMinY + tigerMaxY)
 *
 * This produces the same visual result as the RI renderer (which uses the
 * same scale/translate in OpenVG Y-up space and then flips vertically on
 * pixel readback).
 */
#include <stdio.h>
#include <stdint.h>
#include "../cmodel_helper.h"
#include "tiger.h"

#define TIGER_W 512

/* Set the tiger-space → screen (Y-down) affine matrix in cmodel registers. */
static void set_tiger_matrix(vg_cmodel_t cm, int w, int h)
{
    float s  = (float)w / (tigerMaxX - tigerMinX);
    float tx = -s * tigerMinX;
    float ty = 0.5f * (float)h + s * 0.5f * (tigerMinY + tigerMaxY);

    cm_reg_f(cm, VG_REG_MATRIX_SX,  s);
    vg_cmodel_reg_write(cm, VG_REG_MATRIX_SHX, 0u);
    cm_reg_f(cm, VG_REG_MATRIX_TX,  tx);
    vg_cmodel_reg_write(cm, VG_REG_MATRIX_SHY, 0u);
    cm_reg_f(cm, VG_REG_MATRIX_SY,  -s);   /* Y-flip: OpenVG Y-up → screen Y-down */
    cm_reg_f(cm, VG_REG_MATRIX_TY,  ty);
    cm_reg_f(cm, VG_REG_PATH_SCALE, 1.0f);
    vg_cmodel_reg_write(cm, VG_REG_PATH_BIAS, 0u);
}

/* Pack three floats [0,1] into RGBA8888 with alpha=255. */
static uint32_t pack_color(float r, float g, float b)
{
    uint8_t ri = (uint8_t)(r * 255.f + 0.5f);
    uint8_t gi = (uint8_t)(g * 255.f + 0.5f);
    uint8_t bi = (uint8_t)(b * 255.f + 0.5f);
    return ((uint32_t)ri << 24) | ((uint32_t)gi << 16) |
           ((uint32_t)bi <<  8) | 0xFFu;
}

int main(void)
{
    const int w = TIGER_W;
    const int h = (int)(TIGER_W * 792.0f / 612.0f + 0.5f);

    vg_cmodel_t cm = vg_cmodel_create((uint32_t)w, (uint32_t)h);

    /* Step 1: identity matrix + white background. */
    cm_set_identity(cm);
    cm_clear_white(cm, (uint32_t)w, (uint32_t)h);

    /* Step 2: switch to tiger-space transform. */
    set_tiger_matrix(cm, w, h);

    /* Step 3: parse and submit each tiger path. */
    int c = 0;   /* index into tigerCommands */
    int p = 0;   /* index into tigerPoints   */

    while (c < tigerCommandCount) {
        /* --- Metadata (4 command bytes) --- */
        char fill_ch = tigerCommands[c++];   /* 'N', 'F', 'E' */
        c++;   /* stroke mode – ignored (cmodel has no stroke) */
        c++;   /* cap style   – ignored */
        c++;   /* join style  – ignored */

        /* --- Style parameters (8 floats then element count) --- */
        p++;           /* miterLimit  – ignored */
        p++;           /* strokeWidth – ignored */
        p += 3;        /* strokeColor – ignored */
        float fr = tigerPoints[p++];
        float fg = tigerPoints[p++];
        float fb = tigerPoints[p++];
        int   elems = (int)tigerPoints[p++];

        int fill_rule = (fill_ch == 'E') ? VG_REG_FILL_EVEN_ODD
                                         : VG_REG_FILL_NON_ZERO;
        int has_fill  = (fill_ch == 'F' || fill_ch == 'E');

        /* --- Path element data --- */
        if (has_fill) {
            /* Estimate path buffer size (worst case: all cubics). */
            size_t cap = (size_t)(elems * 32 + 64);
            PathBuf pb = cm_pb_create(cap);

            for (int e = 0; e < elems; e++) {
                char seg = tigerCommands[c++];
                switch (seg) {
                case 'M':
                    cm_pb_cmd(&pb, VG_PATH_CMD_MOVE_TO);
                    cm_pb_float(&pb, tigerPoints[p++]);
                    cm_pb_float(&pb, tigerPoints[p++]);
                    break;
                case 'L':
                    cm_pb_cmd(&pb, VG_PATH_CMD_LINE_TO);
                    cm_pb_float(&pb, tigerPoints[p++]);
                    cm_pb_float(&pb, tigerPoints[p++]);
                    break;
                case 'C':
                    cm_pb_cmd(&pb, VG_PATH_CMD_CUBIC_TO);
                    cm_pb_float(&pb, tigerPoints[p++]); /* cp1x */
                    cm_pb_float(&pb, tigerPoints[p++]); /* cp1y */
                    cm_pb_float(&pb, tigerPoints[p++]); /* cp2x */
                    cm_pb_float(&pb, tigerPoints[p++]); /* cp2y */
                    cm_pb_float(&pb, tigerPoints[p++]); /* endx */
                    cm_pb_float(&pb, tigerPoints[p++]); /* endy */
                    break;
                case 'E':
                    cm_pb_cmd(&pb, VG_PATH_CMD_CLOSE_PATH);
                    break;
                default:
                    break;
                }
            }

            vg_cmodel_set_path_ptr(cm, pb.buf, (uint32_t)pb.len);
            vg_cmodel_reg_write(cm, VG_REG_FILL_RULE,  (uint32_t)fill_rule);
            vg_cmodel_reg_write(cm, VG_REG_FILL_COLOR, pack_color(fr, fg, fb));
            vg_cmodel_reg_write(cm, VG_REG_BLEND_MODE, VG_REG_BLEND_SRC_OVER);
            vg_cmodel_reg_write(cm, VG_REG_GRAD_TYPE,  VG_GRAD_NONE);
            vg_cmodel_reg_write(cm, VG_REG_AA_SAMPLES, VG_AA_8X);
            vg_cmodel_reg_write(cm, VG_REG_PATH_KICK,  1);

            cm_pb_free(&pb);
        } else {
            /* No fill: skip element data without building a buffer. */
            for (int e = 0; e < elems; e++) {
                char seg = tigerCommands[c++];
                switch (seg) {
                case 'M': case 'L': p += 2; break;
                case 'C':           p += 6; break;
                case 'E':                   break;
                default:                    break;
                }
            }
        }
    }

    vg_cmodel_save_ppm(cm, "cmodel.ppm");
    vg_cmodel_destroy(cm);
    printf("Saved cmodel.ppm (%dx%d)\n", w, h);
    return 0;
}
