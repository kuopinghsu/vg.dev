/*
 * OpenVG 1.1 Hardware Accelerator – C-Model Header
 *
 * The C-model simulates the hardware behaviour purely in software so that
 * algorithm correctness can be verified before RTL implementation.
 *
 * It reuses the register-level interface defined in driver/vg_reg.h so that
 * the same test-bench code can drive both the C-model and the real driver.
 */
#ifndef VG_CMODEL_H
#define VG_CMODEL_H

#include <stdint.h>
#include <stddef.h>

/* Pull in the register definitions from the driver tree */
#include "../driver/vg_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Cmodel context (opaque)
 * ========================================================================= */
typedef struct vg_cmodel *vg_cmodel_t;

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * vg_cmodel_create – allocate a new C-model instance.
 *
 * @fb_width / fb_height: dimensions of the software frame buffer.
 * Returns NULL on allocation failure.
 */
vg_cmodel_t vg_cmodel_create(uint32_t fb_width, uint32_t fb_height);

/** vg_cmodel_destroy – free all resources. */
void vg_cmodel_destroy(vg_cmodel_t cm);

/* =========================================================================
 * Register interface (mirrors hardware MMIO)
 * ========================================================================= */
void     vg_cmodel_reg_write(vg_cmodel_t cm, uint32_t offset, uint32_t val);
uint32_t vg_cmodel_reg_read (vg_cmodel_t cm, uint32_t offset);

/* =========================================================================
 * Simulation tick
 * =========================================================================
 * Call after writing VG_REG_PATH_KICK (or VG_CTRL_START) to run the
 * simulation synchronously.  Returns 0 on success, non-zero on error.
 */
int vg_cmodel_run(vg_cmodel_t cm);

/* =========================================================================
 * Frame buffer access
 * ========================================================================= */

/** Return pointer to the internal RGBA8888 frame buffer (row-major). */
const uint32_t *vg_cmodel_get_framebuffer(vg_cmodel_t cm,
                                           uint32_t *out_w,
                                           uint32_t *out_h);

/** Return writable pointer to the internal RGBA8888 frame buffer. */
uint32_t *vg_cmodel_get_fb_rw(vg_cmodel_t cm, uint32_t *out_w, uint32_t *out_h);

/**
 * vg_cmodel_save_ppm – write the current frame buffer to a PPM file.
 * Returns 0 on success.
 */
int vg_cmodel_save_ppm(vg_cmodel_t cm, const char *filename);

/* =========================================================================
 * 64-bit host pointer setters
 * =========================================================================
 * On a 64-bit host the path / mask / image pointers cannot be stored in a
 * 32-bit register. Call these instead of writing VG_REG_PATH_ADDR etc.
 * ========================================================================= */
void vg_cmodel_set_path_ptr (vg_cmodel_t cm, const void *ptr, uint32_t size);
void vg_cmodel_set_mask_ptr (vg_cmodel_t cm, const void *ptr);
void vg_cmodel_set_image_ptr(vg_cmodel_t cm, const void *ptr,
                              uint32_t stride, uint32_t w, uint32_t h);

#ifdef __cplusplus
}
#endif

#endif /* VG_CMODEL_H */
