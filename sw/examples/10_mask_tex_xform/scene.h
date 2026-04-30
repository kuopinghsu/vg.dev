/*
 * 10_mask_tex_xform/scene.h
 *
 * Shared scene parameters and procedural texture/mask generators used
 * by both the cmodel and RI side of the test, so that the two pipelines
 * see the *same* pixel data for the texture and the alpha mask.
 *
 * Scene combines three features in one draw:
 *   1. Coordinate transform : a rotated + scaled rounded-rect path.
 *   2. Texture (pattern)    : 32x32 procedural RGBA image, REPEAT tiled
 *                             in screen space.
 *   3. Alpha mask           : W x H 8-bit alpha buffer with a smooth
 *                             radial falloff centred on the surface.
 *
 * Surface dimensions are chosen so that the texture height divides
 * the surface height evenly (160 = 32 * 5).  This lets us flip the
 * pattern image when uploading to the RI so that cmodel-screen-Y-down
 * sampling and RI-Y-up pattern sampling agree.
 */
#pragma once

#include <math.h>
#include <stdint.h>

#define SCENE_W      240
#define SCENE_H      160
#define TEX_W        32
#define TEX_H        32

/* Path is built in cmodel-screen coordinates (Y-down).  The matrix
 * registers (cmodel) / PATH_USER_TO_SURFACE (RI) apply the affine. */
#define PATH_LOCAL_X0   (-90.f)
#define PATH_LOCAL_Y0   (-50.f)
#define PATH_LOCAL_X1   ( 90.f)
#define PATH_LOCAL_Y1   ( 50.f)
#define PATH_CORNER_R    24.f

/* Rotation pivot = surface centre.  After rotate+scale the path is
 * placed with translation (CX,CY). */
#define SCENE_CX        (SCENE_W * 0.5f)
#define SCENE_CY        (SCENE_H * 0.5f)
#define SCENE_ANGLE     0.35f         /* radians, ~20 degrees           */
#define SCENE_SCALE     1.10f

/* Mask: smoothstep radial falloff centred on the surface.  Inside
 * MASK_R_IN it is fully opaque, outside MASK_R_OUT fully transparent,
 * with a smooth Hermite ramp in between.  This exercises partial-alpha
 * mask blending (which the cmodel matches the RI on by performing
 * sRGB-correct linear-light compositing). */
#define MASK_R_IN      55.f
#define MASK_R_OUT     80.f

/* -------------------------------------------------------------------------
 * Procedural pattern texture: a "tartan" of red/teal/yellow blocks plus a
 * dark grid line every 8 pixels.  Stored row-major in screen-Y-down with
 * RGBA bytes per pixel.
 * ------------------------------------------------------------------------- */
static void scene_make_texture(uint8_t *out /* TEX_W*TEX_H*4 */)
{
    for (int y = 0; y < TEX_H; y++) {
        for (int x = 0; x < TEX_W; x++) {
            uint8_t r, g, b;
            int cx = x / 8;     /* 0..3 */
            int cy = y / 8;     /* 0..3 */
            int idx = (cx + cy * 4) % 3;
            switch (idx) {
            case 0: r = 0xE6; g = 0x3B; b = 0x3B; break; /* red    */
            case 1: r = 0x1F; g = 0x9E; b = 0xA1; break; /* teal   */
            default:r = 0xF2; g = 0xC4; b = 0x1F; break; /* yellow */
            }
            /* dark grid lines */
            if ((x % 8) == 0 || (y % 8) == 0) { r = 0x10; g = 0x10; b = 0x10; }
            uint8_t *p = out + (y * TEX_W + x) * 4;
            p[0] = r; p[1] = g; p[2] = b; p[3] = 0xFF;
        }
    }
}

/* -------------------------------------------------------------------------
 * Alpha mask: smoothstep radial falloff centred on the surface.
 * ------------------------------------------------------------------------- */
static void scene_make_mask(uint8_t *out /* SCENE_W*SCENE_H */)
{
    for (int y = 0; y < SCENE_H; y++) {
        for (int x = 0; x < SCENE_W; x++) {
            float dx = (float)x + 0.5f - SCENE_CX;
            float dy = (float)y + 0.5f - SCENE_CY;
            float d  = sqrtf(dx * dx + dy * dy);
            float t;
            if (d <= MASK_R_IN)       t = 1.f;
            else if (d >= MASK_R_OUT) t = 0.f;
            else {
                float u = (d - MASK_R_IN) / (MASK_R_OUT - MASK_R_IN);
                t = 1.f - (u * u * (3.f - 2.f * u));   /* smoothstep */
            }
            int a = (int)(t * 255.f + 0.5f);
            if (a < 0) a = 0; else if (a > 255) a = 255;
            out[y * SCENE_W + x] = (uint8_t)a;
        }
    }
}

/* -------------------------------------------------------------------------
 * Compose the 3x3 affine transform M used for the path.
 *
 *   M = T(CX,CY) * R(angle) * S(scale)
 *
 * Stored row-major as [sx shx tx ; shy sy ty ; 0 0 1] which is what the
 * cmodel reads from MATRIX_SX/SHX/TX/SHY/SY/TY.
 * ------------------------------------------------------------------------- */
static void scene_compose_matrix(float *sx,  float *shx, float *tx,
                                  float *shy, float *sy,  float *ty)
{
    float c = cosf(SCENE_ANGLE) * SCENE_SCALE;
    float s = sinf(SCENE_ANGLE) * SCENE_SCALE;
    *sx  =  c;  *shx = -s;  *tx  = SCENE_CX;
    *shy =  s;  *sy  =  c;  *ty  = SCENE_CY;
}

/* -------------------------------------------------------------------------
 * Invert the 3x3 affine [a b tx ; c d ty ; 0 0 1] (row-major).
 * Used by the RI side to compute FILL_PAINT_TO_USER such that the pattern
 * stays anchored to the surface (pattern_coord = surface_coord).
 * ------------------------------------------------------------------------- */
static void scene_invert_affine(float a, float b, float tx,
                                 float c, float d, float ty,
                                 float *ia, float *ib, float *itx,
                                 float *ic, float *id, float *ity) __attribute__((unused));
static void scene_invert_affine(float a, float b, float tx,
                                 float c, float d, float ty,
                                 float *ia, float *ib, float *itx,
                                 float *ic, float *id, float *ity)
{
    float det = a * d - b * c;
    float inv = 1.f / det;
    *ia =  d * inv;
    *ib = -b * inv;
    *ic = -c * inv;
    *id =  a * inv;
    *itx = -(*ia * tx + *ib * ty);
    *ity = -(*ic * tx + *id * ty);
}
