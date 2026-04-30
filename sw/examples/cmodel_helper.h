/*
 * cmodel_helper.h – Shared path-buffer builder and cmodel setup helpers.
 *
 * Include once from a single .c source that defines main().
 * Link with:
 *   gcc ... ../../cmodel/vg_cmodel.c test_cmodel.c -lm
 */
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "vg_cmodel.h"   /* found via -I$(ROOT)/sw/cmodel in Makefile */

/* -------------------------------------------------------------------------
 * Dynamic byte buffer for building cmodel path command streams.
 * -------------------------------------------------------------------------*/
typedef struct { uint8_t *buf; size_t cap, len; } PathBuf;

static PathBuf cm_pb_create(size_t cap)
{
    PathBuf pb;
    pb.buf = (uint8_t *)malloc(cap);
    pb.cap = cap;
    pb.len = 0;
    return pb;
}
static void cm_pb_free(PathBuf *pb) { free(pb->buf); pb->buf = NULL; pb->len = 0; }

/* Append a 4-byte command header (command byte + 3 padding bytes). */
static void cm_pb_cmd(PathBuf *pb, uint8_t cmd)
{
    if (pb->len + 4 > pb->cap) return;
    memset(pb->buf + pb->len, 0, 4);
    pb->buf[pb->len] = cmd;
    pb->len += 4;
}

/* Append a float coordinate value. */
static void cm_pb_float(PathBuf *pb, float v)
{
    if (pb->len + 4 > pb->cap) return;
    memcpy(pb->buf + pb->len, &v, 4);
    pb->len += 4;
}

/* -------------------------------------------------------------------------
 * Register helpers
 * -------------------------------------------------------------------------*/

/* Write a float value to a register (via bit-copy). */
static void cm_reg_f(vg_cmodel_t cm, uint32_t reg, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    vg_cmodel_reg_write(cm, reg, u);
}

/* Set identity affine transform + default path scale/bias + 8x8 AA. */
static void cm_set_identity(vg_cmodel_t cm)
{
    cm_reg_f(cm, VG_REG_MATRIX_SX,  1.0f);
    vg_cmodel_reg_write(cm, VG_REG_MATRIX_SHX, 0u);
    vg_cmodel_reg_write(cm, VG_REG_MATRIX_TX,  0u);
    vg_cmodel_reg_write(cm, VG_REG_MATRIX_SHY, 0u);
    cm_reg_f(cm, VG_REG_MATRIX_SY,  1.0f);
    vg_cmodel_reg_write(cm, VG_REG_MATRIX_TY,  0u);
    cm_reg_f(cm, VG_REG_PATH_SCALE, 1.0f);
    vg_cmodel_reg_write(cm, VG_REG_PATH_BIAS,  0u);
    vg_cmodel_reg_write(cm, VG_REG_AA_SAMPLES, VG_AA_8X);
}

/*
 * Clear the entire canvas to white using VG_BLEND_SRC.
 * Must be called after cm_set_identity().
 */
static void cm_clear_white(vg_cmodel_t cm, uint32_t W, uint32_t H)
{
    PathBuf pb = cm_pb_create(256);
    float fw = (float)W, fh = (float)H;
    cm_pb_cmd(&pb, VG_PATH_CMD_MOVE_TO); cm_pb_float(&pb, 0.f);  cm_pb_float(&pb, 0.f);
    cm_pb_cmd(&pb, VG_PATH_CMD_LINE_TO); cm_pb_float(&pb, fw);   cm_pb_float(&pb, 0.f);
    cm_pb_cmd(&pb, VG_PATH_CMD_LINE_TO); cm_pb_float(&pb, fw);   cm_pb_float(&pb, fh);
    cm_pb_cmd(&pb, VG_PATH_CMD_LINE_TO); cm_pb_float(&pb, 0.f);  cm_pb_float(&pb, fh);
    cm_pb_cmd(&pb, VG_PATH_CMD_CLOSE_PATH);

    vg_cmodel_set_path_ptr(cm, pb.buf, (uint32_t)pb.len);
    vg_cmodel_reg_write(cm, VG_REG_FILL_RULE,  VG_REG_FILL_NON_ZERO);
    vg_cmodel_reg_write(cm, VG_REG_FILL_COLOR, 0xFFFFFFFFu);  /* white */
    vg_cmodel_reg_write(cm, VG_REG_BLEND_MODE, VG_REG_BLEND_SRC);
    vg_cmodel_reg_write(cm, VG_REG_GRAD_TYPE,  VG_GRAD_NONE);
    vg_cmodel_reg_write(cm, VG_REG_PATH_KICK,  1);
    cm_pb_free(&pb);
}

/* -------------------------------------------------------------------------
 * Convenience: add a closed rectangle to a path buffer.
 * -------------------------------------------------------------------------*/
static void cm_pb_rect(PathBuf *pb, float x0, float y0, float x1, float y1) __attribute__((unused));
static void cm_pb_rect(PathBuf *pb, float x0, float y0, float x1, float y1)
{
    cm_pb_cmd(pb, VG_PATH_CMD_MOVE_TO); cm_pb_float(pb, x0); cm_pb_float(pb, y0);
    cm_pb_cmd(pb, VG_PATH_CMD_LINE_TO); cm_pb_float(pb, x1); cm_pb_float(pb, y0);
    cm_pb_cmd(pb, VG_PATH_CMD_LINE_TO); cm_pb_float(pb, x1); cm_pb_float(pb, y1);
    cm_pb_cmd(pb, VG_PATH_CMD_LINE_TO); cm_pb_float(pb, x0); cm_pb_float(pb, y1);
    cm_pb_cmd(pb, VG_PATH_CMD_CLOSE_PATH);
}

/*
 * Append a cubic-bezier ellipse arc centred at (cx,cy) with radii (rx,ry)
 * to the path buffer using 4 bezier segments.
 */
static void cm_pb_ellipse(PathBuf *pb, float cx, float cy, float rx, float ry) __attribute__((unused));
static void cm_pb_ellipse(PathBuf *pb, float cx, float cy, float rx, float ry)
{
    const float k = 0.5522847498f;   /* 4/3 * tan(pi/8) */
    float kx = rx * k, ky = ry * k;

    cm_pb_cmd(pb, VG_PATH_CMD_MOVE_TO);
    cm_pb_float(pb, cx);       cm_pb_float(pb, cy - ry);
    /* top → right */
    cm_pb_cmd(pb, VG_PATH_CMD_CUBIC_TO);
    cm_pb_float(pb, cx + kx);  cm_pb_float(pb, cy - ry);
    cm_pb_float(pb, cx + rx);  cm_pb_float(pb, cy - ky);
    cm_pb_float(pb, cx + rx);  cm_pb_float(pb, cy);
    /* right → bottom */
    cm_pb_cmd(pb, VG_PATH_CMD_CUBIC_TO);
    cm_pb_float(pb, cx + rx);  cm_pb_float(pb, cy + ky);
    cm_pb_float(pb, cx + kx);  cm_pb_float(pb, cy + ry);
    cm_pb_float(pb, cx);       cm_pb_float(pb, cy + ry);
    /* bottom → left */
    cm_pb_cmd(pb, VG_PATH_CMD_CUBIC_TO);
    cm_pb_float(pb, cx - kx);  cm_pb_float(pb, cy + ry);
    cm_pb_float(pb, cx - rx);  cm_pb_float(pb, cy + ky);
    cm_pb_float(pb, cx - rx);  cm_pb_float(pb, cy);
    /* left → top */
    cm_pb_cmd(pb, VG_PATH_CMD_CUBIC_TO);
    cm_pb_float(pb, cx - rx);  cm_pb_float(pb, cy - ky);
    cm_pb_float(pb, cx - kx);  cm_pb_float(pb, cy - ry);
    cm_pb_float(pb, cx);       cm_pb_float(pb, cy - ry);

    cm_pb_cmd(pb, VG_PATH_CMD_CLOSE_PATH);
}
