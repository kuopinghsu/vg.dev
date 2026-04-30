/*
 * OpenVG 1.1 Hardware Accelerator Driver Implementation
 */
#include "vg_drv.h"
#include "vg_reg.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Portable float<->uint32 type-pun (avoids strict-aliasing UB) */
static inline uint32_t float_to_u32(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

/* Context structure */
struct vg_ctx {
    volatile uint32_t *base;
    vg_surface_t       surface;
};

static inline void reg_write(vg_ctx_t ctx, uint32_t offset, uint32_t val)
{
    ctx->base[offset / 4] = val;
}

static inline uint32_t reg_read(vg_ctx_t ctx, uint32_t offset)
{
    return ctx->base[offset / 4];
}

static inline void reg_write_f(vg_ctx_t ctx, uint32_t offset, float val)
{
    reg_write(ctx, offset, float_to_u32(val));
}

/* =========================================================================
 * Public API
 * ========================================================================= */

vg_ctx_t vg_drv_init(volatile uint32_t *base)
{
    if (!base)
        return NULL;

    vg_ctx_t ctx = (vg_ctx_t)calloc(1, sizeof(struct vg_ctx));
    if (!ctx)
        return NULL;

    ctx->base = base;

    /* Soft reset then enable FE+BE */
    reg_write(ctx, VG_REG_SOFT_RESET, 1);
    reg_write(ctx, VG_REG_CTRL, VG_CTRL_FE_EN | VG_CTRL_BE_EN | VG_CTRL_TEX_EN);

    return ctx;
}

void vg_drv_release(vg_ctx_t ctx)
{
    if (!ctx)
        return;
    reg_write(ctx, VG_REG_CTRL, 0);
    free(ctx);
}

vg_drv_err_t vg_drv_set_surface(vg_ctx_t ctx, const vg_surface_t *surf)
{
    if (!ctx || !surf)
        return VG_DRV_BAD_HANDLE;

    ctx->surface = *surf;

    reg_write(ctx, VG_REG_SURF_ADDR,   (uint32_t)surf->bus_addr);
    reg_write(ctx, VG_REG_SURF_STRIDE, surf->stride);
    reg_write(ctx, VG_REG_SURF_WIDTH,  surf->width);
    reg_write(ctx, VG_REG_SURF_HEIGHT, surf->height);
    reg_write(ctx, VG_REG_SURF_FORMAT, (uint32_t)surf->format);

    return VG_DRV_OK;
}

vg_drv_err_t vg_drv_clear(vg_ctx_t ctx,
                           int x, int y, int width, int height,
                           uint32_t rgba)
{
    if (!ctx)
        return VG_DRV_BAD_HANDLE;
    if (width <= 0 || height <= 0)
        return VG_DRV_ILLEGAL_ARG;

    /* Programme a full-screen fill using the flat paint path */
    vg_draw_desc_t desc;
    memset(&desc, 0, sizeof(desc));

    /* Identity matrix */
    desc.sx  = 1.0f; desc.sy  = 1.0f;
    desc.shx = 0.0f; desc.shy = 0.0f;
    desc.tx  = 0.0f; desc.ty  = 0.0f;

    desc.fill_rule               = VG_FILL_NON_ZERO;
    desc.fill_paint.type         = VG_DRV_PAINT_FLAT;
    desc.fill_paint.flat_color   = rgba;
    desc.blend_mode              = VG_BLEND_SRC;
    desc.aa_mode                 = VG_AA_NONE;

    /* Scissor to the clear rectangle */
    reg_write(ctx, VG_REG_SCISSOR_X,  (uint32_t)x);
    reg_write(ctx, VG_REG_SCISSOR_Y,  (uint32_t)y);
    reg_write(ctx, VG_REG_SCISSOR_W,  (uint32_t)width);
    reg_write(ctx, VG_REG_SCISSOR_H,  (uint32_t)height);
    reg_write(ctx, VG_REG_SCISSOR_EN, 1);

    /* No path: HW performs a fill of the scissor rect directly */
    reg_write(ctx, VG_REG_FILL_COLOR,  rgba);
    reg_write(ctx, VG_REG_BLEND_MODE,  VG_BLEND_SRC);
    reg_write(ctx, VG_REG_CTRL,
              reg_read(ctx, VG_REG_CTRL) | VG_CTRL_START | VG_CTRL_FLUSH);

    (void)desc; /* desc built above; extend for full HW clear path if needed */
    return VG_DRV_OK;
}

/* --- programme paint registers --- */
static void program_paint(vg_ctx_t ctx, const vg_paint_desc_t *p)
{
    reg_write(ctx, VG_REG_GRAD_TYPE, (uint32_t)p->type);

    if (p->type == VG_DRV_PAINT_FLAT) {
        reg_write(ctx, VG_REG_FILL_COLOR, p->flat_color);
    } else {
        reg_write_f(ctx, VG_REG_GRAD_X0, p->x0);
        reg_write_f(ctx, VG_REG_GRAD_Y0, p->y0);
        reg_write_f(ctx, VG_REG_GRAD_X1, p->x1);
        reg_write_f(ctx, VG_REG_GRAD_Y1, p->y1);
        if (p->type == VG_DRV_PAINT_RADIAL)
            reg_write_f(ctx, VG_REG_GRAD_R, p->r);
        reg_write(ctx, VG_REG_GRAD_SPREAD, p->spread);
        reg_write(ctx, VG_REG_CRAMP_COUNT, (uint32_t)p->num_stops);
        for (int i = 0; i < p->num_stops && i < 16; i++) {
            reg_write_f(ctx, VG_REG_CRAMP_OFFSET(i), p->stops[i].offset);
            reg_write  (ctx, VG_REG_CRAMP_COLOR(i),  p->stops[i].rgba);
        }
    }
}

vg_drv_err_t vg_drv_draw_path(vg_ctx_t ctx, const vg_draw_desc_t *desc)
{
    if (!ctx || !desc)
        return VG_DRV_BAD_HANDLE;

    /* --- Transformation matrix --- */
    reg_write_f(ctx, VG_REG_MATRIX_SX,  desc->sx);
    reg_write_f(ctx, VG_REG_MATRIX_SHX, desc->shx);
    reg_write_f(ctx, VG_REG_MATRIX_TX,  desc->tx);
    reg_write_f(ctx, VG_REG_MATRIX_SHY, desc->shy);
    reg_write_f(ctx, VG_REG_MATRIX_SY,  desc->sy);
    reg_write_f(ctx, VG_REG_MATRIX_TY,  desc->ty);

    /* --- Fill --- */
    reg_write(ctx, VG_REG_FILL_RULE, desc->fill_rule);
    program_paint(ctx, &desc->fill_paint);

    /* --- Stroke --- */
    if (desc->enabled_stroke) {
        const vg_stroke_params_t *sp = &desc->stroke;
        reg_write_f(ctx, VG_REG_STROKE_WIDTH,  sp->line_width);
        reg_write_f(ctx, VG_REG_STROKE_MITER,  sp->miter_limit);
        reg_write  (ctx, VG_REG_STROKE_CAP,    sp->cap_style);
        reg_write  (ctx, VG_REG_STROKE_JOIN,   sp->join_style);
        reg_write_f(ctx, VG_REG_DASH_PHASE,    sp->dash_phase);
        reg_write  (ctx, VG_REG_DASH_COUNT,    (uint32_t)sp->num_dash);
        for (int i = 0; i < sp->num_dash && i < 16; i++)
            reg_write_f(ctx, VG_REG_DASH_PATTERN(i), sp->dash_pattern[i]);
        reg_write(ctx, VG_REG_STROKE_COLOR, desc->stroke_paint.flat_color);
    }

    /* --- Blend / AA --- */
    reg_write(ctx, VG_REG_BLEND_MODE,    desc->blend_mode);
    reg_write(ctx, VG_REG_AA_SAMPLES,    desc->aa_mode);

    /* --- Optional texture --- */
    if (desc->tex_bus_addr) {
        reg_write(ctx, VG_REG_TEX_ADDR,   (uint32_t)desc->tex_bus_addr);
        reg_write(ctx, VG_REG_TEX_STRIDE, desc->tex_stride);
        reg_write(ctx, VG_REG_TEX_WIDTH,  desc->tex_width);
        reg_write(ctx, VG_REG_TEX_HEIGHT, desc->tex_height);
        reg_write(ctx, VG_REG_TEX_FORMAT, (uint32_t)desc->tex_format);
    }

    /* --- Optional mask --- */
    if (desc->mask_enable && desc->mask_bus_addr) {
        reg_write(ctx, VG_REG_MASK_ADDR,   (uint32_t)desc->mask_bus_addr);
        reg_write(ctx, VG_REG_MASK_STRIDE, desc->mask_stride);
        reg_write(ctx, VG_REG_MASK_EN,     1);
    } else {
        reg_write(ctx, VG_REG_MASK_EN, 0);
    }

    /* --- Path DMA --- */
    if (desc->path_bus_addr) {
        reg_write  (ctx, VG_REG_PATH_ADDR,  (uint32_t)desc->path_bus_addr);
        reg_write  (ctx, VG_REG_PATH_SIZE,  desc->path_buf_size);
        reg_write_f(ctx, VG_REG_PATH_SCALE, desc->path_scale == 0.0f ? 1.0f : desc->path_scale);
        reg_write_f(ctx, VG_REG_PATH_BIAS,  desc->path_bias);
        reg_write  (ctx, VG_REG_PATH_KICK,  1);   /* kick FE */
    }

    uint32_t paint_mode = VG_CTRL_FE_EN | VG_CTRL_BE_EN | VG_CTRL_TEX_EN;
    if (desc->enabled_stroke)
        paint_mode |= (1u << 8); /* STROKE_EN private bit, extend as needed */
    reg_write(ctx, VG_REG_CTRL, paint_mode | VG_CTRL_START);

    return VG_DRV_OK;
}

vg_drv_err_t vg_drv_flush(vg_ctx_t ctx)
{
    if (!ctx)
        return VG_DRV_BAD_HANDLE;
    uint32_t ctrl = reg_read(ctx, VG_REG_CTRL);
    reg_write(ctx, VG_REG_CTRL, ctrl | VG_CTRL_FLUSH);
    return VG_DRV_OK;
}

vg_drv_err_t vg_drv_finish(vg_ctx_t ctx, uint32_t timeout_ms)
{
    if (!ctx)
        return VG_DRV_BAD_HANDLE;

    /* Simple polling loop; replace with interrupt-based wait on a real OS */
    uint32_t retries = (timeout_ms == 0) ? 0xFFFFFFFFu : (timeout_ms * 1000u);
    for (uint32_t i = 0; i < retries; i++) {
        uint32_t status = reg_read(ctx, VG_REG_STATUS);
        if (status & VG_STATUS_ERROR)
            return VG_DRV_HW_ERROR;
        if (status & VG_STATUS_IDLE)
            return VG_DRV_OK;
    }
    return VG_DRV_TIMEOUT;
}

uint32_t vg_drv_read_reg(vg_ctx_t ctx, uint32_t offset)
{
    return reg_read(ctx, offset);
}

void vg_drv_write_reg(vg_ctx_t ctx, uint32_t offset, uint32_t val)
{
    reg_write(ctx, offset, val);
}
