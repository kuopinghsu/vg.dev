/*
 * OpenVG 1.1 Hardware Accelerator Driver
 *
 * Public API header.
 */
#ifndef VG_DRV_H
#define VG_DRV_H

#include <stdint.h>
#include <stddef.h>
#include "vg_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Handle types (opaque)
 * ========================================================================= */
typedef struct vg_ctx  *vg_ctx_t;
typedef struct vg_path *vg_path_t;
typedef struct vg_paint *vg_paint_t;
typedef struct vg_image *vg_image_t;

/* =========================================================================
 * Error codes
 * ========================================================================= */
typedef enum {
    VG_DRV_OK              =  0,
    VG_DRV_BAD_HANDLE      = -1,
    VG_DRV_ILLEGAL_ARG     = -2,
    VG_DRV_OUT_OF_MEMORY   = -3,
    VG_DRV_HW_ERROR        = -4,
    VG_DRV_TIMEOUT         = -5,
    VG_DRV_UNSUPPORTED     = -6,
} vg_drv_err_t;

/* =========================================================================
 * Pixel format (subset of VGImageFormat)
 * ========================================================================= */
typedef enum {
    VG_DRV_FMT_RGBA8888 = VG_FMT_RGBA8888,
    VG_DRV_FMT_RGBX8888 = VG_FMT_RGBX8888,
    VG_DRV_FMT_BGRA8888 = VG_FMT_BGRA8888,
    VG_DRV_FMT_RGB565   = VG_FMT_RGB565,
    VG_DRV_FMT_RGBA4444 = VG_FMT_RGBA4444,
    VG_DRV_FMT_A8       = VG_FMT_A8,
} vg_drv_fmt_t;

/* =========================================================================
 * Surface descriptor
 * ========================================================================= */
typedef struct {
    uintptr_t    bus_addr;   /* Physical / bus address of frame buffer */
    uint32_t     stride;     /* Bytes per row                          */
    uint32_t     width;
    uint32_t     height;
    vg_drv_fmt_t format;
} vg_surface_t;

/* =========================================================================
 * Paint descriptor
 * ========================================================================= */
typedef enum {
    VG_DRV_PAINT_FLAT    = 0,
    VG_DRV_PAINT_LINEAR  = VG_GRAD_LINEAR,
    VG_DRV_PAINT_RADIAL  = VG_GRAD_RADIAL,
} vg_drv_paint_type_t;

typedef struct {
    float offset;
    uint32_t rgba;   /* packed RGBA8888 */
} vg_color_stop_t;

typedef struct {
    vg_drv_paint_type_t type;
    uint32_t            flat_color;     /* RGBA8888, used when type==FLAT    */
    /* Gradient geometry */
    float x0, y0;
    float x1, y1;
    float r;                            /* Radial radius                     */
    /* Color ramp */
    int             num_stops;
    vg_color_stop_t stops[16];
    uint32_t        spread;             /* VGColorRampSpreadMode             */
} vg_paint_desc_t;

/* =========================================================================
 * Stroke parameters
 * ========================================================================= */
typedef struct {
    float    line_width;
    float    miter_limit;
    uint32_t cap_style;     /* VG_CAP_*  */
    uint32_t join_style;    /* VG_JOIN_* */
    float    dash_phase;
    int      num_dash;
    float    dash_pattern[16];
} vg_stroke_params_t;

/* =========================================================================
 * Draw call descriptor
 * ========================================================================= */
typedef struct {
    /* Affine matrix [sx shx tx / shy sy ty] */
    float    sx, shx, tx;
    float    shy, sy, ty;
    /* Fill */
    uint32_t        fill_rule;       /* VG_FILL_* */
    vg_paint_desc_t fill_paint;
    /* Stroke */
    int                enabled_stroke;
    vg_stroke_params_t stroke;
    vg_paint_desc_t    stroke_paint;
    /* Blend / quality */
    uint32_t blend_mode;     /* VG_BLEND_* */
    uint32_t aa_mode;        /* VG_AA_*    */
    /* Path DMA */
    uintptr_t path_bus_addr;
    uint32_t  path_buf_size;
    float     path_scale;
    float     path_bias;
    /* Optional image */
    uintptr_t tex_bus_addr;
    uint32_t  tex_stride;
    uint32_t  tex_width;
    uint32_t  tex_height;
    vg_drv_fmt_t tex_format;
    /* Optional mask */
    uintptr_t mask_bus_addr;
    uint32_t  mask_stride;
    int       mask_enable;
} vg_draw_desc_t;

/* =========================================================================
 * Driver API
 * ========================================================================= */

/**
 * vg_drv_init - Initialize the driver and map MMIO registers.
 * @base: Virtual address mapped to VG_BASE_ADDR.
 * Returns a context handle, or NULL on failure.
 */
vg_ctx_t vg_drv_init(volatile uint32_t *base);

/**
 * vg_drv_release - Release all resources and unmap registers.
 */
void vg_drv_release(vg_ctx_t ctx);

/**
 * vg_drv_set_surface - Configure the render target surface.
 */
vg_drv_err_t vg_drv_set_surface(vg_ctx_t ctx, const vg_surface_t *surf);

/**
 * vg_drv_clear - Fill the surface rectangle with a solid color.
 * @rgba: packed RGBA8888 colour.
 */
vg_drv_err_t vg_drv_clear(vg_ctx_t ctx,
                           int x, int y, int width, int height,
                           uint32_t rgba);

/**
 * vg_drv_draw_path - Submit a path draw call to the hardware.
 * The caller must have prepared the path command buffer in DMA-coherent
 * memory and flushed the CPU cache before calling this function.
 */
vg_drv_err_t vg_drv_draw_path(vg_ctx_t ctx, const vg_draw_desc_t *desc);

/**
 * vg_drv_flush - Signal the hardware to begin rendering.
 * Non-blocking: returns immediately after kicking the hardware.
 */
vg_drv_err_t vg_drv_flush(vg_ctx_t ctx);

/**
 * vg_drv_finish - Block until the hardware is idle.
 * @timeout_ms: milliseconds to wait; 0 = wait forever.
 */
vg_drv_err_t vg_drv_finish(vg_ctx_t ctx, uint32_t timeout_ms);

/**
 * vg_drv_read_reg  / vg_drv_write_reg - Raw register access (debug).
 */
uint32_t     vg_drv_read_reg (vg_ctx_t ctx, uint32_t offset);
void         vg_drv_write_reg(vg_ctx_t ctx, uint32_t offset, uint32_t val);

#ifdef __cplusplus
}
#endif

#endif /* VG_DRV_H */
