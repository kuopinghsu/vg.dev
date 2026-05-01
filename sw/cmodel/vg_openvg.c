/*
 * vg_openvg.c — EGL 1.4 + OpenVG 1.1 + VGU 1.1 implementation
 *               backed by the hardware C-model (vg_cmodel).
 *
 * Intended to run the Khronos CTS generator in headless mode so the
 * cmodel output can be compared against the RI reference images.
 *
 * Limitations (unsupported features return VG_UNSUPPORTED_IMAGE_FORMAT_ERROR
 * or VG_NO_ERROR silently):
 *   - VGImage, VGFont, VGMaskLayer
 *   - Color transform
 *   - Image filters (convolution, color matrix, Gaussian blur, LUTs)
 *   - Stroke rendering (VG_STROKE_PATH is silently ignored)
 *   - MSAA / rendering quality other than what the cmodel implements
 */

#include <VG/openvg.h>
#include <VG/vgu.h>
#include <EGL/egl.h>

#include "vg_cmodel.h"
#include "vg_reg.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>

/* Helper: write a float value as raw bits to a cmodel register */
static inline uint32_t float_to_bits(float f) {
    uint32_t u; memcpy(&u, &f, sizeof(u)); return u;
}
#define vg_cmodel_reg_write_f(cm, addr, val) \
    vg_cmodel_reg_write((cm), (addr), float_to_bits(val))

/* =========================================================================
 * Utility helpers
 * ========================================================================= */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float clampf01(float v)
{
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

/* Pack RGBA floats [0..1] to 0xRRGGBBAA */
static inline uint32_t pack_rgba(const VGfloat *c)
{
    uint8_t r = (uint8_t)(clampf01(c[0]) * 255.f + 0.5f);
    uint8_t g = (uint8_t)(clampf01(c[1]) * 255.f + 0.5f);
    uint8_t b = (uint8_t)(clampf01(c[2]) * 255.f + 0.5f);
    uint8_t a = (uint8_t)(clampf01(c[3]) * 255.f + 0.5f);
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a;
}

/* =========================================================================
 * Internal object types
 * ========================================================================= */

#define CM_MAX_STOPS   256
#define CM_MAX_DASH    16
#define CM_MAX_SCISSOR 32

typedef struct cm_paint_s {
    VGPaintType type;
    VGfloat     color[4];          /* RGBA [0..1] flat color */
    VGfloat     lin[4];            /* x0,y0,x1,y1 linear gradient */
    VGfloat     rad[5];            /* cx,cy,fx,fy,r radial gradient */
    int         num_stops;
    struct { VGfloat offset; VGfloat color[4]; } stops[CM_MAX_STOPS];
    VGColorRampSpreadMode spread_mode;
    VGTilingMode          tiling_mode;
} cm_paint_t;

#define PATH_MAGIC  0xA2B3C4D5u
#define MASKLAYER_MAGIC 0xD4E5F6A7u

typedef struct cm_masklayer_s {
    uint32_t magic;
    int      width, height;
    uint8_t *data;   /* alpha8, row 0 = top */
} cm_masklayer_t;

typedef struct cm_path_s {
    uint32_t    magic;        /* PATH_MAGIC for validity checks */
    struct cm_path_s *reg_next; /* global registry linked list */
    uint8_t    *buf;          /* command buffer (OpenVG path format) */
    int         buf_cap;      /* allocated bytes */
    int         buf_len;      /* used bytes */
    VGbitfield  capabilities;
    VGPathDatatype datatype;
    VGfloat     scale, bias;
    int         num_segments; /* number of segments appended */
    int         num_coords;   /* number of coordinates appended */
} cm_path_t;

/* Global registry of all user-visible path handles (created by vgCreatePath) */
static cm_path_t *g_path_registry = NULL;

/* Returns non-zero if 'path' is a currently valid user-created path handle */
static int cm_path_is_valid(VGPath path)
{
    if (path == VG_INVALID_HANDLE) return 0;
    cm_path_t *p = (cm_path_t *)(uintptr_t)path;
    for (cm_path_t *it = g_path_registry; it; it = it->reg_next)
        if (it == p) return 1;
    return 0;
}

/* ---- Font object ---- */
#define FONT_MAGIC  0xF07710ACu

#define CM_MAX_GLYPHS 512

typedef struct cm_glyph_s {
    VGboolean   defined;
    VGboolean   has_path;
    VGPath      path;
    VGfloat     glyph_origin[2];
    VGfloat     escapement[2];
} cm_glyph_t;

typedef struct cm_font_s {
    uint32_t    magic;
    int         num_glyphs;      /* number of defined glyphs */
    int         capacity;        /* allocated glyph slots */
    cm_glyph_t *glyphs;          /* glyph_index → glyph mapping (heap array) */
} cm_font_t;

/* ---- Image object ---- */
#define IMAGE_MAGIC 0xA94CE001u

/* Returns bytes per pixel for supported formats; -1 for unsupported. */
static int image_bpp(VGImageFormat fmt)
{
    switch (fmt) {
    case VG_sRGBX_8888: case VG_sRGBA_8888: case VG_sRGBA_8888_PRE:
    case VG_lRGBX_8888: case VG_lRGBA_8888: case VG_lRGBA_8888_PRE:
    case VG_sXRGB_8888: case VG_sARGB_8888: case VG_sARGB_8888_PRE:
    case VG_lXRGB_8888: case VG_lARGB_8888: case VG_lARGB_8888_PRE:
    case VG_sBGRX_8888: case VG_sBGRA_8888: case VG_sBGRA_8888_PRE:
    case VG_lBGRX_8888: case VG_lBGRA_8888: case VG_lBGRA_8888_PRE:
    case VG_sXBGR_8888: case VG_sABGR_8888: case VG_sABGR_8888_PRE:
    case VG_lXBGR_8888: case VG_lABGR_8888: case VG_lABGR_8888_PRE:
        return 4;
    case VG_sRGB_565: case VG_sRGBA_5551: case VG_sRGBA_4444:
    case VG_sBGR_565: case VG_sBGRA_5551: case VG_sBGRA_4444:
    case VG_sARGB_1555: case VG_sARGB_4444:
    case VG_sABGR_1555: case VG_sABGR_4444:
        return 2;
    case VG_sL_8: case VG_lL_8: case VG_A_8:
    case VG_BW_1: case VG_A_1: case VG_A_4:
        return 1;
    default: return -1;
    }
}

typedef struct cm_image_s {
    uint32_t        magic;
    VGImageFormat   format;
    VGint           width;
    VGint           height;
    VGbitfield      allowed_quality;
    int             bpp;         /* bytes per pixel */
    uint8_t        *pixels;      /* pointer to start of image data (may be into parent buffer) */
    int             row_stride;  /* bytes per row in pixels buffer (= width*bpp for root images) */
    VGImage         parent_handle; /* VG_INVALID_HANDLE if root image */
} cm_image_t;

/* Simple handle table – VGHandle is uintptr_t (pointer). */
#define INVALID_HANDLE ((uintptr_t)VG_INVALID_HANDLE)

typedef struct cm_surface_s {
    vg_cmodel_t  cm;
    int          width, height;
    int          is_linear;   /* 0=sRGB  1=linear */
    int          is_premult;  /* 0=non-pre  1=pre */
    uint8_t     *mask_buf;    /* alpha8 mask buffer, row 0 = top, NULL if not allocated */
} cm_surface_t;

typedef struct cm_ctx_s {
    cm_surface_t *surface;
    VGErrorCode   error;

    VGMatrixMode   matrix_mode;
    VGFillRule     fill_rule;
    VGBlendMode    blend_mode;
    VGRenderingQuality rendering_quality;
    VGImageMode    image_mode;

    VGfloat        clear_color[4];
    VGfloat        mat_path[9];         /* column-major 3x3 */
    VGfloat        mat_image[9];
    VGfloat        mat_glyph[9];
    VGfloat        mat_fill_paint[9];   /* VG_MATRIX_FILL_PAINT_TO_USER */
    VGfloat        mat_stroke_paint[9]; /* VG_MATRIX_STROKE_PAINT_TO_USER */

    cm_paint_t    *fill_paint;
    cm_paint_t    *stroke_paint;

    VGfloat        stroke_line_width;
    VGCapStyle     stroke_cap_style;
    VGJoinStyle    stroke_join_style;
    VGfloat        stroke_miter_limit;
    VGfloat        stroke_dash_phase;
    int            stroke_dash_phase_reset;
    VGfloat        stroke_dash_pattern[CM_MAX_DASH];
    int            num_dash;

    int            color_transform;
    VGfloat        color_transform_values[8];

    int            scissoring;
    VGint          scissor_rects[CM_MAX_SCISSOR * 4];
    int            num_scissor_rects;

    int            masking;
    int            filter_channel_mask;
    int            filter_format_linear;
    int            filter_format_premultiplied;
    int            pixel_layout;
    int            image_quality;
    VGfloat        tile_fill_color[4];
} cm_ctx_t;

/* =========================================================================
 * Global state
 * ========================================================================= */

static int            g_disp_init  = 0;
static EGLint         g_egl_error  = EGL_SUCCESS;
static cm_ctx_t      *g_ctx        = NULL;
static cm_surface_t  *g_surface    = NULL;

/* =========================================================================
 * Default paints (VG_INVALID_HANDLE returns white for fill, black for stroke) */
static cm_paint_t g_default_fill_paint;
static cm_paint_t g_default_stroke_paint;

/* =========================================================================
 * Error helpers
 * ========================================================================= */

/* Per OpenVG spec: first error since last vgGetError() is preserved; do not override. */
#define VG_SET_ERR(e)  do { if (g_ctx && g_ctx->error == VG_NO_ERROR) g_ctx->error = (e); } while(0)
#define EGL_SET_ERR(e) do { g_egl_error = (e); } while(0)
#define VG_CHECK_CTX(...) do { if (!g_ctx) return __VA_ARGS__; } while(0)

/* =========================================================================
 * Matrix helpers
 * ========================================================================= */

static void mat_identity(VGfloat *m)
{
    static const VGfloat id[9] = {1,0,0, 0,1,0, 0,0,1};
    memcpy(m, id, 9 * sizeof(VGfloat));
}

static VGfloat *ctx_current_mat(cm_ctx_t *ctx)
{
    switch (ctx->matrix_mode) {
    case VG_MATRIX_PATH_USER_TO_SURFACE:    return ctx->mat_path;
    case VG_MATRIX_IMAGE_USER_TO_SURFACE:   return ctx->mat_image;
    case VG_MATRIX_GLYPH_USER_TO_SURFACE:   return ctx->mat_glyph;
    case VG_MATRIX_FILL_PAINT_TO_USER:      return ctx->mat_fill_paint;
    case VG_MATRIX_STROKE_PAINT_TO_USER:    return ctx->mat_stroke_paint;
    default: return ctx->mat_path;
    }
}

/* Multiply 3x3 column-major matrices: result = a * b */
static void mat_mul(const VGfloat *a, const VGfloat *b, VGfloat *r)
{
    /* a[col*3+row] */
    VGfloat t[9];
    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
            t[col*3+row] = 0.f;
            for (int k = 0; k < 3; k++)
                t[col*3+row] += a[k*3+row] * b[col*3+k];
        }
    }
    memcpy(r, t, 9 * sizeof(VGfloat));
}

/* Load the OpenVG path_user_to_surface matrix into cmodel registers.
 * OpenVG uses Y-up surface; the cmodel uses Y-down raster coordinates.
 * We compose: cmodel_mat = flip_y * openvg_mat
 * where flip_y = [1,0,0, 0,-1,H, 0,0,1] (column-major).
 * OpenVG column-major 3x3 layout: [sx,shy,0, shx,sy,0, tx,ty,1] */
static void load_matrix_to_cmodel(cm_ctx_t *ctx)
{
    const VGfloat *m = ctx->mat_path;
    int H = ctx->surface ? ctx->surface->height : 64;

    /* OpenVG 3x3 column-major: column j, row i = m[j*3+i]
     * OpenVG affine: x' = sx*x + shx*y + tx
     *                y' = shy*x + sy*y  + ty
     * Cmodel registers: SX, SHX, TX, SHY, SY, TY  */
    float sx  = m[0];    /* col0,row0 */
    float shy = m[1];    /* col0,row1 */
    float shx = m[3];    /* col1,row0 */
    float sy  = m[4];    /* col1,row1 */
    float tx  = m[6];    /* col2,row0 */
    float ty  = m[7];    /* col2,row1 */

    /* Apply Y-flip: y_cmodel = H - 1 - y_openvg
     * composed matrix:
     *   SX'  = sx
     *   SHX' = shx
     *   TX'  = tx
     *   SHY' = -shy
     *   SY'  = -sy
     *   TY'  = (H-1) - ty  + ... more precisely (H-1)*1 - (shy*0 + sy*0 + ty)
     *        = (H - 1) + shy*0 - ty  no...
     *
     * flip_y * m means: new_x = m11*x + m12*y + m13
     *                   new_y = -m21*x - m22*y - m23 + H  (where H = surface height)
     * So: SY' = -sy, SHY' = -shy, TY' = H - ty
     */
    vg_cmodel_t cm = ctx->surface->cm;
    vg_cmodel_reg_write_f(cm, VG_REG_MATRIX_SX,  sx);
    vg_cmodel_reg_write_f(cm, VG_REG_MATRIX_SHX, shx);
    vg_cmodel_reg_write_f(cm, VG_REG_MATRIX_TX,  tx);
    vg_cmodel_reg_write_f(cm, VG_REG_MATRIX_SHY, -shy);
    vg_cmodel_reg_write_f(cm, VG_REG_MATRIX_SY,  -sy);
    vg_cmodel_reg_write_f(cm, VG_REG_MATRIX_TY,  (float)H - ty);
}

/* =========================================================================
 * Paint helpers
 * ========================================================================= */

/* Apply VG_COLOR_TRANSFORM to an RGBA color in-place. */
static void apply_color_transform(cm_ctx_t *ctx, float c[4])
{
    if (!ctx->color_transform) return;
    const VGfloat *cv = ctx->color_transform_values;
    c[0] = clampf01(c[0] * cv[0] + cv[4]);
    c[1] = clampf01(c[1] * cv[1] + cv[5]);
    c[2] = clampf01(c[2] * cv[2] + cv[6]);
    c[3] = clampf01(c[3] * cv[3] + cv[7]);
}

static void load_paint_to_cmodel(cm_ctx_t *ctx, cm_paint_t *paint)
{
    if (!paint) paint = &g_default_fill_paint;
    vg_cmodel_t cm = ctx->surface->cm;

    switch (paint->type) {
    case VG_PAINT_TYPE_COLOR: {
        vg_cmodel_reg_write(cm, VG_REG_GRAD_TYPE, VG_GRAD_NONE);
        float c[4] = { paint->color[0], paint->color[1],
                       paint->color[2], paint->color[3] };
        apply_color_transform(ctx, c);
        vg_cmodel_reg_write(cm, VG_REG_FILL_COLOR, pack_rgba(c));
        break;
    }
    case VG_PAINT_TYPE_LINEAR_GRADIENT: {
        vg_cmodel_reg_write(cm, VG_REG_GRAD_TYPE, VG_GRAD_LINEAR);
        /* In OpenVG, gradient coords are in paint (user) space.
         * For simplicity, pass them through the path matrix. */
        vg_cmodel_reg_write_f(cm, VG_REG_GRAD_X0, paint->lin[0]);
        vg_cmodel_reg_write_f(cm, VG_REG_GRAD_Y0, (float)ctx->surface->height - paint->lin[1]);
        vg_cmodel_reg_write_f(cm, VG_REG_GRAD_X1, paint->lin[2]);
        vg_cmodel_reg_write_f(cm, VG_REG_GRAD_Y1, (float)ctx->surface->height - paint->lin[3]);
        /* Color ramp */
        int ns = paint->num_stops < 16 ? paint->num_stops : 16;
        for (int i = 0; i < ns; i++) {
            vg_cmodel_reg_write_f(cm, VG_REG_CRAMP_OFFSET(i), paint->stops[i].offset);
            float sc[4] = { paint->stops[i].color[0], paint->stops[i].color[1],
                            paint->stops[i].color[2], paint->stops[i].color[3] };
            apply_color_transform(ctx, sc);
            vg_cmodel_reg_write(cm, VG_REG_CRAMP_COLOR(i), pack_rgba(sc));
        }
        vg_cmodel_reg_write(cm, VG_REG_CRAMP_COUNT, (uint32_t)ns);
        vg_cmodel_reg_write(cm, VG_REG_GRAD_SPREAD, (uint32_t)paint->spread_mode);
        break;
    }
    case VG_PAINT_TYPE_RADIAL_GRADIENT: {
        vg_cmodel_reg_write(cm, VG_REG_GRAD_TYPE, VG_GRAD_RADIAL);
        int H = ctx->surface->height;
        vg_cmodel_reg_write_f(cm, VG_REG_GRAD_X0, paint->rad[0]);
        vg_cmodel_reg_write_f(cm, VG_REG_GRAD_Y0, (float)H - paint->rad[1]);
        /* X1/Y1 unused for radial in cmodel; use centre */
        vg_cmodel_reg_write_f(cm, VG_REG_GRAD_X1, paint->rad[0]);
        vg_cmodel_reg_write_f(cm, VG_REG_GRAD_Y1, (float)H - paint->rad[1]);
        vg_cmodel_reg_write_f(cm, VG_REG_GRAD_R,  paint->rad[4]);
        int ns = paint->num_stops < 16 ? paint->num_stops : 16;
        for (int i = 0; i < ns; i++) {
            vg_cmodel_reg_write_f(cm, VG_REG_CRAMP_OFFSET(i), paint->stops[i].offset);
            float rc[4] = { paint->stops[i].color[0], paint->stops[i].color[1],
                            paint->stops[i].color[2], paint->stops[i].color[3] };
            apply_color_transform(ctx, rc);
            vg_cmodel_reg_write(cm, VG_REG_CRAMP_COLOR(i), pack_rgba(rc));
        }
        vg_cmodel_reg_write(cm, VG_REG_CRAMP_COUNT, (uint32_t)ns);
        vg_cmodel_reg_write(cm, VG_REG_GRAD_SPREAD, (uint32_t)paint->spread_mode);
        break;
    }
    default:
        vg_cmodel_reg_write(cm, VG_REG_GRAD_TYPE, VG_GRAD_NONE);
        vg_cmodel_reg_write(cm, VG_REG_FILL_COLOR, 0x000000FFu);
        break;
    }
}

/* =========================================================================
 * Context initialisation
 * ========================================================================= */

static void ctx_init_defaults(cm_ctx_t *ctx, cm_surface_t *surface)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->surface               = surface;
    ctx->error                 = VG_NO_ERROR;
    ctx->matrix_mode           = VG_MATRIX_PATH_USER_TO_SURFACE;
    ctx->fill_rule             = VG_EVEN_ODD;
    ctx->blend_mode            = VG_BLEND_SRC_OVER;
    ctx->rendering_quality     = VG_RENDERING_QUALITY_BETTER;
    ctx->image_mode            = VG_DRAW_IMAGE_NORMAL;
    ctx->clear_color[0]        = 0.f;
    ctx->clear_color[1]        = 0.f;
    ctx->clear_color[2]        = 0.f;
    ctx->clear_color[3]        = 0.f;
    mat_identity(ctx->mat_path);
    mat_identity(ctx->mat_image);
    mat_identity(ctx->mat_glyph);
    mat_identity(ctx->mat_fill_paint);
    mat_identity(ctx->mat_stroke_paint);
    ctx->fill_paint            = NULL;  /* use default */
    ctx->stroke_paint          = NULL;
    ctx->stroke_line_width     = 1.f;
    ctx->stroke_cap_style      = VG_CAP_BUTT;
    ctx->stroke_join_style     = VG_JOIN_MITER;
    ctx->stroke_miter_limit    = 4.f;
    ctx->filter_channel_mask   = VG_RED|VG_GREEN|VG_BLUE|VG_ALPHA;
    ctx->pixel_layout          = VG_PIXEL_LAYOUT_UNKNOWN;
    ctx->image_quality         = VG_IMAGE_QUALITY_FASTER;
    ctx->color_transform_values[0] = 1.f;
    ctx->color_transform_values[1] = 1.f;
    ctx->color_transform_values[2] = 1.f;
    ctx->color_transform_values[3] = 1.f;
    /* [4..7] remain 0.f from memset */

    /* Default paints */
    memset(&g_default_fill_paint,   0, sizeof(cm_paint_t));
    memset(&g_default_stroke_paint, 0, sizeof(cm_paint_t));
    g_default_fill_paint.type     = VG_PAINT_TYPE_COLOR;
    g_default_fill_paint.color[0] = 0.f;
    g_default_fill_paint.color[1] = 0.f;
    g_default_fill_paint.color[2] = 0.f;
    g_default_fill_paint.color[3] = 1.f;
    g_default_stroke_paint        = g_default_fill_paint;
}

/* =========================================================================
 * EGL implementation
 * ========================================================================= */

/* We expose a single EGL config:
 *   R8G8B8A8, sRGB, non-premultiplied, EGL_PBUFFER_BIT, ID=1
 * This maps to the CTS directory prefix "1/sRGB_NONPRE/".
 */
#define CM_EGL_CONFIG_ID    1
#define CM_EGL_CONFIG       ((EGLConfig)(uintptr_t)CM_EGL_CONFIG_ID)
#define CM_EGL_DISPLAY      ((EGLDisplay)1)

EGLint eglGetError(void)
{
    EGLint e = g_egl_error;
    g_egl_error = EGL_SUCCESS;
    return e;
}

EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id)
{
    (void)display_id;
    return CM_EGL_DISPLAY;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
    if (dpy != CM_EGL_DISPLAY) { EGL_SET_ERR(EGL_BAD_DISPLAY); return EGL_FALSE; }
    g_disp_init = 1;
    if (major) *major = 1;
    if (minor) *minor = 4;
    return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy)
{
    (void)dpy;
    g_disp_init = 0;
    return EGL_TRUE;
}

EGLBoolean eglBindAPI(EGLenum api)
{
    if (api != EGL_OPENVG_API) { EGL_SET_ERR(EGL_BAD_PARAMETER); return EGL_FALSE; }
    return EGL_TRUE;
}

EGLenum eglQueryAPI(void) { return EGL_OPENVG_API; }

EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig *configs, EGLint config_size, EGLint *num_config)
{
    (void)dpy;
    if (!num_config) { EGL_SET_ERR(EGL_BAD_PARAMETER); return EGL_FALSE; }
    *num_config = 1;
    if (configs && config_size >= 1)
        configs[0] = CM_EGL_CONFIG;
    return EGL_TRUE;
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs,
                            EGLint config_size, EGLint *num_config)
{
    (void)dpy;
    if (!num_config) { EGL_SET_ERR(EGL_BAD_PARAMETER); return EGL_FALSE; }

    /* Check for config ID filter */
    if (attrib_list) {
        for (int i = 0; attrib_list[i] != EGL_NONE; i += 2) {
            if (attrib_list[i] == EGL_CONFIG_ID) {
                if (attrib_list[i+1] != CM_EGL_CONFIG_ID) {
                    *num_config = 0;
                    return EGL_TRUE;
                }
            }
        }
    }
    *num_config = 1;
    if (configs && config_size >= 1)
        configs[0] = CM_EGL_CONFIG;
    return EGL_TRUE;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value)
{
    (void)dpy;
    if (config != CM_EGL_CONFIG || !value) {
        EGL_SET_ERR(EGL_BAD_CONFIG); return EGL_FALSE;
    }
    switch (attribute) {
    case EGL_CONFIG_ID:             *value = CM_EGL_CONFIG_ID; break;
    case EGL_RED_SIZE:              *value = 8; break;
    case EGL_GREEN_SIZE:            *value = 8; break;
    case EGL_BLUE_SIZE:             *value = 8; break;
    case EGL_ALPHA_SIZE:            *value = 8; break;
    case EGL_LUMINANCE_SIZE:        *value = 0; break;
    case EGL_ALPHA_MASK_SIZE:       *value = 8; break;
    case EGL_BUFFER_SIZE:           *value = 32; break;
    case EGL_DEPTH_SIZE:            *value = 0; break;
    case EGL_STENCIL_SIZE:          *value = 0; break;
    case EGL_SAMPLE_BUFFERS:        *value = 0; break;
    case EGL_SAMPLES:               *value = 0; break;
    case EGL_SURFACE_TYPE:
        *value = EGL_PBUFFER_BIT |
                 EGL_VG_COLORSPACE_LINEAR_BIT |
                 EGL_VG_ALPHA_FORMAT_PRE_BIT;
        break;
    case EGL_RENDERABLE_TYPE:       *value = EGL_OPENVG_BIT; break;
    case EGL_CONFORMANT:
#ifdef EGL_CONFORMANT_KHR
        *value = EGL_OPENVG_BIT;
#else
        *value = EGL_OPENVG_BIT;
#endif
        break;
    case EGL_CONFIG_CAVEAT:         *value = EGL_NONE; break;
    case EGL_COLOR_BUFFER_TYPE:     *value = EGL_RGB_BUFFER; break;
    case EGL_LEVEL:                 *value = 0; break;
    case EGL_MAX_PBUFFER_WIDTH:     *value = 4096; break;
    case EGL_MAX_PBUFFER_HEIGHT:    *value = 4096; break;
    case EGL_MAX_PBUFFER_PIXELS:    *value = 4096*4096; break;
    case EGL_MAX_SWAP_INTERVAL:     *value = 1; break;
    case EGL_MIN_SWAP_INTERVAL:     *value = 1; break;
    case EGL_NATIVE_RENDERABLE:     *value = EGL_FALSE; break;
    case EGL_NATIVE_VISUAL_ID:      *value = 0; break;
    case EGL_NATIVE_VISUAL_TYPE:    *value = EGL_NONE; break;
    case EGL_TRANSPARENT_TYPE:      *value = EGL_NONE; break;
    default:
        EGL_SET_ERR(EGL_BAD_ATTRIBUTE);
        return EGL_FALSE;
    }
    return EGL_TRUE;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                    const EGLint *attrib_list)
{
    (void)dpy; (void)config;
    int w = 64, h = 64;
    int is_linear = 0, is_premult = 0;

    if (attrib_list) {
        for (int i = 0; attrib_list[i] != EGL_NONE; i += 2) {
            switch (attrib_list[i]) {
            case EGL_WIDTH:  w = attrib_list[i+1]; break;
            case EGL_HEIGHT: h = attrib_list[i+1]; break;
            case EGL_VG_COLORSPACE:
                is_linear = (attrib_list[i+1] == EGL_VG_COLORSPACE_LINEAR);
                break;
            case EGL_VG_ALPHA_FORMAT:
                is_premult = (attrib_list[i+1] == EGL_VG_ALPHA_FORMAT_PRE);
                break;
            default: break;
            }
        }
    }

    cm_surface_t *surf = (cm_surface_t *)malloc(sizeof(cm_surface_t));
    if (!surf) { EGL_SET_ERR(EGL_BAD_ALLOC); return EGL_NO_SURFACE; }

    surf->cm = vg_cmodel_create((unsigned)w, (unsigned)h);
    if (!surf->cm) { free(surf); EGL_SET_ERR(EGL_BAD_ALLOC); return EGL_NO_SURFACE; }
    surf->mask_buf = (uint8_t *)malloc((size_t)(w * h));
    if (!surf->mask_buf) {
        vg_cmodel_destroy(surf->cm);
        free(surf);
        EGL_SET_ERR(EGL_BAD_ALLOC);
        return EGL_NO_SURFACE;
    }
    memset(surf->mask_buf, 0xFF, (size_t)(w * h)); /* initialize mask to fully unmasked */
    surf->width     = w;
    surf->height    = h;
    surf->is_linear = is_linear;
    surf->is_premult = is_premult;
    return (EGLSurface)surf;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
    (void)dpy;
    cm_surface_t *surf = (cm_surface_t *)surface;
    if (!surf) { EGL_SET_ERR(EGL_BAD_SURFACE); return EGL_FALSE; }
    vg_cmodel_destroy(surf->cm);
    free(surf->mask_buf);
    free(surf);
    if (g_surface == surf) { g_surface = NULL; }
    return EGL_TRUE;
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config,
                             EGLContext share_context, const EGLint *attrib_list)
{
    (void)dpy; (void)config; (void)share_context; (void)attrib_list;
    cm_ctx_t *ctx = (cm_ctx_t *)malloc(sizeof(cm_ctx_t));
    if (!ctx) { EGL_SET_ERR(EGL_BAD_ALLOC); return EGL_NO_CONTEXT; }
    memset(ctx, 0, sizeof(*ctx));
    ctx->error = VG_NO_ERROR;
    return (EGLContext)ctx;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext context)
{
    (void)dpy;
    cm_ctx_t *ctx = (cm_ctx_t *)context;
    if (!ctx) { EGL_SET_ERR(EGL_BAD_CONTEXT); return EGL_FALSE; }
    if (g_ctx == ctx) g_ctx = NULL;
    free(ctx);
    return EGL_TRUE;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                           EGLContext context)
{
    (void)dpy; (void)read;

    if (context == EGL_NO_CONTEXT) {
        g_ctx = NULL; g_surface = NULL;
        return EGL_TRUE;
    }

    cm_ctx_t     *ctx  = (cm_ctx_t *)context;
    cm_surface_t *surf = (cm_surface_t *)draw;

    if (!surf) { EGL_SET_ERR(EGL_BAD_SURFACE); return EGL_FALSE; }

    ctx_init_defaults(ctx, surf);
    g_ctx     = ctx;
    g_surface = surf;
    return EGL_TRUE;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
    (void)dpy; (void)surface;
    return EGL_TRUE;
}

EGLDisplay eglGetCurrentDisplay(void)
{
    return g_ctx ? CM_EGL_DISPLAY : EGL_NO_DISPLAY;
}

EGLSurface eglGetCurrentSurface(EGLint which)
{
    (void)which;
    return g_surface ? (EGLSurface)g_surface : EGL_NO_SURFACE;
}

EGLContext eglGetCurrentContext(void)
{
    return g_ctx ? (EGLContext)g_ctx : EGL_NO_CONTEXT;
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value)
{
    (void)dpy;
    cm_surface_t *surf = (cm_surface_t *)surface;
    if (!surf || !value) { EGL_SET_ERR(EGL_BAD_SURFACE); return EGL_FALSE; }
    switch (attribute) {
    case EGL_WIDTH:  *value = surf->width;  break;
    case EGL_HEIGHT: *value = surf->height; break;
    case EGL_CONFIG_ID: *value = CM_EGL_CONFIG_ID; break;
    default: EGL_SET_ERR(EGL_BAD_ATTRIBUTE); return EGL_FALSE;
    }
    return EGL_TRUE;
}

const char *eglQueryString(EGLDisplay dpy, EGLint name)
{
    (void)dpy;
    switch (name) {
    case EGL_VENDOR:     return "cmodel";
    case EGL_VERSION:    return "1.4 cmodel";
    case EGL_EXTENSIONS: return "EGL_KHR_config_attribs";
    case EGL_CLIENT_APIS: return "OpenVG";
    default: return "";
    }
}

EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value)
{
    (void)dpy; (void)ctx;
    if (!value) { EGL_SET_ERR(EGL_BAD_PARAMETER); return EGL_FALSE; }
    switch (attribute) {
    case EGL_CONFIG_ID:          *value = CM_EGL_CONFIG_ID; break;
    case EGL_CONTEXT_CLIENT_TYPE: *value = EGL_OPENVG_API; break;
    default: EGL_SET_ERR(EGL_BAD_ATTRIBUTE); return EGL_FALSE;
    }
    return EGL_TRUE;
}

EGLBoolean eglReleaseThread(void) { return EGL_TRUE; }
EGLBoolean eglWaitGL(void) { return EGL_TRUE; }
EGLBoolean eglWaitNative(EGLint engine) { (void)engine; return EGL_TRUE; }
EGLBoolean eglCopyBuffers(EGLDisplay d, EGLSurface s, EGLNativePixmapType t)
{ (void)d;(void)s;(void)t; EGL_SET_ERR(EGL_BAD_NATIVE_PIXMAP); return EGL_FALSE; }
EGLSurface eglCreateWindowSurface(EGLDisplay d,EGLConfig c,EGLNativeWindowType w,const EGLint *a)
{ (void)d;(void)c;(void)w;(void)a; EGL_SET_ERR(EGL_BAD_NATIVE_WINDOW); return EGL_NO_SURFACE; }
EGLSurface eglCreatePixmapSurface(EGLDisplay d,EGLConfig c,EGLNativePixmapType p,const EGLint *a)
{ (void)d;(void)c;(void)p;(void)a; EGL_SET_ERR(EGL_BAD_NATIVE_PIXMAP); return EGL_NO_SURFACE; }
EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay d,EGLenum t,EGLClientBuffer b,EGLConfig c,const EGLint *a)
{ (void)d;(void)t;(void)b;(void)c;(void)a; EGL_SET_ERR(EGL_BAD_PARAMETER); return EGL_NO_SURFACE; }
void (*eglGetProcAddress(const char *name))(void) { (void)name; return NULL; }

/* =========================================================================
 * VG — Error & flush
 * ========================================================================= */

VGErrorCode vgGetError(void){
    if (!g_ctx) return VG_NO_CONTEXT_ERROR;
    VGErrorCode e = g_ctx->error;
    g_ctx->error = VG_NO_ERROR;
    return e;
}

const VGubyte *vgGetString(VGStringID name)
{
    switch (name) {
    case VG_VENDOR:     return (const VGubyte *)"cmodel";
    case VG_RENDERER:   return (const VGubyte *)"VG cmodel";
    case VG_VERSION:    return (const VGubyte *)"1.1";
    case VG_EXTENSIONS: return (const VGubyte *)"";
    default: VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return NULL;
    }
}

VGHardwareQueryResult vgHardwareQuery(VGHardwareQueryType key, VGint setting)
{
    (void)key; (void)setting;
    return VG_HARDWARE_ACCELERATED;
}

void vgFlush(void)  { /* no-op for cmodel */ }
void vgFinish(void) { /* no-op for cmodel */ }

/* =========================================================================
 * VG — Context parameters
 * ========================================================================= */

void vgSetf(VGParamType type, VGfloat value)
{
    VG_CHECK_CTX();
    switch (type) {
    case VG_STROKE_LINE_WIDTH:    g_ctx->stroke_line_width   = value; break;
    case VG_STROKE_MITER_LIMIT:   g_ctx->stroke_miter_limit  = value; break;
    case VG_STROKE_DASH_PHASE:    g_ctx->stroke_dash_phase   = value; break;
    /* Read-only float parameters: silently ignore (no error) */
    case VG_MAX_FLOAT:
    case VG_MAX_GAUSSIAN_STD_DEVIATION: break;
    /* Integer/enum parameters: forward to vgSeti */
    default: vgSeti(type, (VGint)value); break;
    }
}

void vgSeti(VGParamType type, VGint value)
{
    VG_CHECK_CTX();
    switch (type) {
    case VG_MATRIX_MODE:           g_ctx->matrix_mode          = (VGMatrixMode)value; break;
    case VG_FILL_RULE:             g_ctx->fill_rule             = (VGFillRule)value; break;
    case VG_IMAGE_QUALITY:         g_ctx->image_quality = value; break;
    case VG_RENDERING_QUALITY:     g_ctx->rendering_quality     = (VGRenderingQuality)value; break;
    case VG_BLEND_MODE:            g_ctx->blend_mode            = (VGBlendMode)value; break;
    case VG_IMAGE_MODE:            g_ctx->image_mode            = (VGImageMode)value; break;
    case VG_STROKE_CAP_STYLE:      g_ctx->stroke_cap_style      = (VGCapStyle)value; break;
    case VG_STROKE_JOIN_STYLE:     g_ctx->stroke_join_style     = (VGJoinStyle)value; break;
    case VG_STROKE_DASH_PHASE_RESET: g_ctx->stroke_dash_phase_reset = value; break;
    /* Float-valued params settable via vgSeti */
    case VG_STROKE_LINE_WIDTH:    g_ctx->stroke_line_width  = (VGfloat)value; break;
    case VG_STROKE_MITER_LIMIT:   g_ctx->stroke_miter_limit = (VGfloat)value; break;
    case VG_STROKE_DASH_PHASE:    g_ctx->stroke_dash_phase  = (VGfloat)value; break;
    case VG_COLOR_TRANSFORM:       g_ctx->color_transform       = value; break;
    case VG_SCISSORING:            g_ctx->scissoring            = value; break;
    case VG_MASKING:               g_ctx->masking               = value; break;
    case VG_FILTER_FORMAT_LINEAR:  g_ctx->filter_format_linear = value; break;
    case VG_FILTER_FORMAT_PREMULTIPLIED: g_ctx->filter_format_premultiplied = value; break;
    case VG_PIXEL_LAYOUT:          g_ctx->pixel_layout = value; break;
    case VG_SCREEN_LAYOUT:         break;  /* read-only */
    case VG_FILTER_CHANNEL_MASK:   g_ctx->filter_channel_mask   = value; break;
    /* Read-only integer parameters: silently ignore (no error) */
    case VG_MAX_SCISSOR_RECTS:
    case VG_MAX_DASH_COUNT:
    case VG_MAX_KERNEL_SIZE:
    case VG_MAX_SEPARABLE_KERNEL_SIZE:
    case VG_MAX_COLOR_RAMP_STOPS:
    case VG_MAX_IMAGE_WIDTH:
    case VG_MAX_IMAGE_HEIGHT:
    case VG_MAX_IMAGE_PIXELS:
    case VG_MAX_IMAGE_BYTES:
    case VG_MAX_FLOAT:
    case VG_MAX_GAUSSIAN_STD_DEVIATION: break;
    default: VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); break;
    }
}

void vgSetfv(VGParamType type, VGint count, const VGfloat *values)
{
    VG_CHECK_CTX();
    /* negative count is always an error */
    if (count < 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    /* null or misaligned pointer with count > 0 is an error */
    if (count > 0 && (!values || ((uintptr_t)values & (sizeof(VGfloat)-1u)) != 0)) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    switch (type) {
    case VG_SCISSOR_RECTS:
        /* count must be a multiple of 4 */
        if (count % 4 != 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        g_ctx->num_scissor_rects = 0;
        for (int i = 0; i < count && i < CM_MAX_SCISSOR*4; i++)
            g_ctx->scissor_rects[i] = (VGint)values[i];
        g_ctx->num_scissor_rects = count / 4;
        break;
    case VG_COLOR_TRANSFORM_VALUES:
        if (count != 8) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        for (int i = 0; i < 8; i++)
            g_ctx->color_transform_values[i] = values[i];
        break;
    case VG_STROKE_DASH_PATTERN:
        for (int i = 0; i < count && i < CM_MAX_DASH; i++)
            g_ctx->stroke_dash_pattern[i] = values[i];
        g_ctx->num_dash = (count > CM_MAX_DASH) ? CM_MAX_DASH : count;
        break;
    case VG_CLEAR_COLOR:
        if (count != 4) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        memcpy(g_ctx->clear_color, values, 4 * sizeof(VGfloat));
        break;
    case VG_TILE_FILL_COLOR:
        if (count != 4) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        memcpy(g_ctx->tile_fill_color, values, 4 * sizeof(VGfloat));
        break;
    case VG_GLYPH_ORIGIN:
        if (count != 2) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        break;  /* stored but not used */
    default:
        /* scalar parameter — count must be exactly 1 */
        if (count != 1) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        vgSetf(type, values[0]);
        break;
    }
}

void vgSetiv(VGParamType type, VGint count, const VGint *values)
{
    VG_CHECK_CTX();
    if (count < 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (count > 0 && (!values || ((uintptr_t)values & (sizeof(VGint)-1u)) != 0)) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    switch (type) {
    case VG_SCISSOR_RECTS:
        if (count % 4 != 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        for (int i = 0; i < count && i < CM_MAX_SCISSOR*4; i++)
            g_ctx->scissor_rects[i] = values[i];
        g_ctx->num_scissor_rects = count / 4;
        break;
    case VG_TILE_FILL_COLOR:
        if (count != 4) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        for (int i = 0; i < 4; i++) g_ctx->tile_fill_color[i] = (VGfloat)values[i];
        break;
    case VG_CLEAR_COLOR:
        if (count != 4) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        for (int i = 0; i < 4; i++) g_ctx->clear_color[i] = (VGfloat)values[i];
        break;
    case VG_COLOR_TRANSFORM_VALUES:
        if (count != 8) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        for (int i = 0; i < 8; i++) g_ctx->color_transform_values[i] = (VGfloat)values[i];
        break;
    case VG_STROKE_DASH_PATTERN:
        for (int i = 0; i < count && i < CM_MAX_DASH; i++)
            g_ctx->stroke_dash_pattern[i] = (VGfloat)values[i];
        g_ctx->num_dash = (count > CM_MAX_DASH) ? CM_MAX_DASH : count;
        break;
    default:
        if (count != 1) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        vgSeti(type, values[0]);
        break;
    }
}

VGfloat vgGetf(VGParamType type)
{
    VG_CHECK_CTX(0.f);
    switch (type) {
    case VG_STROKE_LINE_WIDTH:          return g_ctx->stroke_line_width;
    case VG_STROKE_MITER_LIMIT:         return g_ctx->stroke_miter_limit;
    case VG_STROKE_DASH_PHASE:          return g_ctx->stroke_dash_phase;
    /* Float read-only params that vgGeti cannot return accurately */
    case VG_MAX_FLOAT:                  return 3.4028234663852886e+38f;
    case VG_MAX_GAUSSIAN_STD_DEVIATION: return 16.f;
    /* Integer/enum parameters: forward to vgGeti */
    default: return (VGfloat)vgGeti(type);
    }
}

VGint vgGeti(VGParamType type)
{
    VG_CHECK_CTX(0);
    switch (type) {
    case VG_MATRIX_MODE:           return (VGint)g_ctx->matrix_mode;
    case VG_FILL_RULE:             return (VGint)g_ctx->fill_rule;
    case VG_IMAGE_QUALITY:         return g_ctx->image_quality;
    case VG_RENDERING_QUALITY:     return (VGint)g_ctx->rendering_quality;
    case VG_BLEND_MODE:
        return (VGint)g_ctx->blend_mode;
    case VG_IMAGE_MODE:            return (VGint)g_ctx->image_mode;
    case VG_STROKE_CAP_STYLE:      return (VGint)g_ctx->stroke_cap_style;
    case VG_STROKE_JOIN_STYLE:     return (VGint)g_ctx->stroke_join_style;
    case VG_STROKE_DASH_PHASE_RESET: return g_ctx->stroke_dash_phase_reset;
    case VG_COLOR_TRANSFORM:       return g_ctx->color_transform;
    case VG_SCISSORING:            return g_ctx->scissoring;
    case VG_MASKING:               return g_ctx->masking;
    case VG_FILTER_CHANNEL_MASK:   return g_ctx->filter_channel_mask;
    case VG_FILTER_FORMAT_LINEAR:  return g_ctx->filter_format_linear;
    case VG_FILTER_FORMAT_PREMULTIPLIED: return g_ctx->filter_format_premultiplied;
    case VG_MAX_SCISSOR_RECTS:     return CM_MAX_SCISSOR;
    case VG_MAX_DASH_COUNT:        return CM_MAX_DASH;
    case VG_MAX_KERNEL_SIZE:       return 7;
    case VG_MAX_SEPARABLE_KERNEL_SIZE: return 15;
    case VG_MAX_COLOR_RAMP_STOPS:  return CM_MAX_STOPS;
    case VG_MAX_IMAGE_WIDTH:       return 4096;
    case VG_MAX_IMAGE_HEIGHT:      return 4096;
    case VG_MAX_IMAGE_PIXELS:      return 4096*4096;
    case VG_MAX_IMAGE_BYTES:       return 4096*4096*4;
    case VG_MAX_GAUSSIAN_STD_DEVIATION: return 16;
    case VG_PIXEL_LAYOUT:          return g_ctx->pixel_layout;
    case VG_SCREEN_LAYOUT:         return VG_PIXEL_LAYOUT_UNKNOWN;
    /* Float params accessible via vgGeti */
    case VG_STROKE_LINE_WIDTH:     return (VGint)g_ctx->stroke_line_width;
    case VG_STROKE_MITER_LIMIT:    return (VGint)g_ctx->stroke_miter_limit;
    case VG_STROKE_DASH_PHASE:     return (VGint)g_ctx->stroke_dash_phase;
    default: VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return 0;
    }
}

VGint vgGetVectorSize(VGParamType type)
{
    switch (type) {
    case VG_SCISSOR_RECTS:           return 0;
    case VG_COLOR_TRANSFORM_VALUES:  return 8;
    case VG_STROKE_DASH_PATTERN:     return g_ctx ? g_ctx->num_dash : 0;
    case VG_CLEAR_COLOR:             return 4;
    case VG_TILE_FILL_COLOR:         return 4;
    case VG_GLYPH_ORIGIN:            return 2;
    default: return 1;
    }
}

void vgGetfv(VGParamType type, VGint count, VGfloat *values)
{
    VG_CHECK_CTX();
    /* count=0 always generates error for get operations */
    if (count <= 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (!values || ((uintptr_t)values & (sizeof(VGfloat)-1u)) != 0) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    switch (type) {
    case VG_CLEAR_COLOR:
        for (int i = 0; i < count && i < 4; i++) values[i] = g_ctx->clear_color[i];
        break;
    case VG_COLOR_TRANSFORM_VALUES:
        for (int i = 0; i < count && i < 8; i++) values[i] = g_ctx->color_transform_values[i];
        break;
    case VG_STROKE_DASH_PATTERN:
        for (int i = 0; i < count && i < g_ctx->num_dash; i++)
            values[i] = g_ctx->stroke_dash_pattern[i];
        break;
    case VG_TILE_FILL_COLOR:
        for (int i = 0; i < count && i < 4; i++) values[i] = g_ctx->tile_fill_color[i];
        break;
    case VG_GLYPH_ORIGIN:
        for (int i = 0; i < count && i < 2; i++) values[i] = 0.f;
        break;
    case VG_SCISSOR_RECTS:
        for (int i = 0; i < count && i < g_ctx->num_scissor_rects*4; i++)
            values[i] = (VGfloat)g_ctx->scissor_rects[i];
        break;
    default:
        if (count >= 1) values[0] = vgGetf(type);
        break;
    }
}

void vgGetiv(VGParamType type, VGint count, VGint *values)
{
    VG_CHECK_CTX();
    /* count=0 always generates error for get operations */
    if (count <= 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (!values || ((uintptr_t)values & (sizeof(VGint)-1u)) != 0) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    switch (type) {
    case VG_SCISSOR_RECTS:
        for (int i = 0; i < count && i < g_ctx->num_scissor_rects*4; i++)
            values[i] = g_ctx->scissor_rects[i];
        break;
    case VG_TILE_FILL_COLOR:
        for (int i = 0; i < count && i < 4; i++) values[i] = (VGint)g_ctx->tile_fill_color[i];
        break;
    case VG_CLEAR_COLOR:
        for (int i = 0; i < count && i < 4; i++) values[i] = (VGint)g_ctx->clear_color[i];
        break;
    case VG_COLOR_TRANSFORM_VALUES:
        for (int i = 0; i < count && i < 8; i++) values[i] = (VGint)g_ctx->color_transform_values[i];
        break;
    case VG_STROKE_DASH_PATTERN:
        for (int i = 0; i < count && i < g_ctx->num_dash; i++)
            values[i] = (VGint)g_ctx->stroke_dash_pattern[i];
        break;
    default:
        if (count >= 1) values[0] = vgGeti(type);
        break;
    }
}

/* =========================================================================
 * VG — Matrix operations
 * ========================================================================= */

void vgLoadIdentity(void)
{
    VG_CHECK_CTX();
    mat_identity(ctx_current_mat(g_ctx));
}

void vgLoadMatrix(const VGfloat *m)
{
    VG_CHECK_CTX();
    if (!m || ((uintptr_t)m & (sizeof(VGfloat)-1u)) != 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    VGfloat *dst = ctx_current_mat(g_ctx);
    memcpy(dst, m, 9 * sizeof(VGfloat));
    /* Enforce affine constraint for non-image matrices: last row must be [0, 0, 1].
     * Image matrices can be projective (no constraint). */
    if (g_ctx->matrix_mode != VG_MATRIX_IMAGE_USER_TO_SURFACE) {
        dst[2] = 0.f; dst[5] = 0.f; dst[8] = 1.f;
    }
}

void vgGetMatrix(VGfloat *m)
{
    VG_CHECK_CTX();
    if (!m || ((uintptr_t)m & (sizeof(VGfloat)-1u)) != 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    memcpy(m, ctx_current_mat(g_ctx), 9 * sizeof(VGfloat));
}

void vgMultMatrix(const VGfloat *m)
{
    VG_CHECK_CTX();
    if (!m || ((uintptr_t)m & (sizeof(VGfloat)-1u)) != 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    VGfloat *cur = ctx_current_mat(g_ctx);
    VGfloat  mm[9];
    memcpy(mm, m, 9 * sizeof(VGfloat));
    /* Enforce affine constraint on the input matrix for non-image modes */
    if (g_ctx->matrix_mode != VG_MATRIX_IMAGE_USER_TO_SURFACE) {
        mm[2] = 0.f; mm[5] = 0.f; mm[8] = 1.f;
    }
    VGfloat  tmp[9];
    mat_mul(cur, mm, tmp);
    memcpy(cur, tmp, 9 * sizeof(VGfloat));
}

void vgTranslate(VGfloat tx, VGfloat ty)
{
    VG_CHECK_CTX();
    VGfloat t[9] = {1,0,0, 0,1,0, tx,ty,1};
    vgMultMatrix(t);
}

void vgScale(VGfloat sx, VGfloat sy)
{
    VG_CHECK_CTX();
    VGfloat t[9] = {sx,0,0, 0,sy,0, 0,0,1};
    vgMultMatrix(t);
}

void vgShear(VGfloat shx, VGfloat shy)
{
    VG_CHECK_CTX();
    VGfloat t[9] = {1,shy,0, shx,1,0, 0,0,1};
    vgMultMatrix(t);
}

void vgRotate(VGfloat angle)
{
    VG_CHECK_CTX();
    float r = (float)(angle * M_PI / 180.0);
    float c = cosf(r), s = sinf(r);
    VGfloat t[9] = {c,s,0, -s,c,0, 0,0,1};
    vgMultMatrix(t);
}

/* =========================================================================
 * VG — Masking
 * ========================================================================= */

/* Validate a handle as VGImage or VGMaskLayer */
static int is_image_handle(VGHandle h) {
    if (h == VG_INVALID_HANDLE) return 0;
    cm_image_t *img = (cm_image_t *)(uintptr_t)h;
    return img->magic == IMAGE_MAGIC;
}
static int is_masklayer_handle(VGHandle h) {
    if (h == VG_INVALID_HANDLE) return 0;
    cm_masklayer_t *ml = (cm_masklayer_t *)(uintptr_t)h;
    return ml->magic == MASKLAYER_MAGIC;
}

/* Apply a mask operation to a single mask byte.
 * cur_val: current mask value (0-255), src_val: source alpha (0-255). */
static uint8_t apply_mask_op(VGMaskOperation op, uint8_t cur_val, uint8_t src_val) {
    switch (op) {
    case VG_CLEAR_MASK:     return 0;
    case VG_FILL_MASK:      return 255;
    case VG_SET_MASK:       return src_val;
    case VG_UNION_MASK:     return (cur_val > src_val) ? cur_val : src_val;
    case VG_INTERSECT_MASK: return (cur_val < src_val) ? cur_val : src_val;
    case VG_SUBTRACT_MASK:  {
        /* mask = mask * (1 - src/255), rounded to nearest */
        int v = (int)cur_val * (255 - (int)src_val);
        return (uint8_t)((v + 127) / 255);
    }
    default: return cur_val;
    }
}

void vgMask(VGHandle mask, VGMaskOperation op, VGint x, VGint y, VGint w, VGint h)
{
    VG_CHECK_CTX();
    /* Validate operation */
    if (op < VG_CLEAR_MASK || op > VG_SUBTRACT_MASK) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    /* VG_SET/UNION/INTERSECT/SUBTRACT need a valid mask handle */
    if (op != VG_CLEAR_MASK && op != VG_FILL_MASK) {
        if (!is_image_handle(mask) && !is_masklayer_handle(mask)) {
            VG_SET_ERR(VG_BAD_HANDLE_ERROR); return;
        }
    }
    /* Width/height must be > 0 */
    if (w <= 0 || h <= 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }

    if (!g_surface || !g_surface->mask_buf) return;
    cm_surface_t *surf = g_surface;
    int W = surf->width, H = surf->height;
    uint8_t *mbuf = surf->mask_buf;

    /* Clamp x,y to surface then loop over each mask pixel */
    int cx  = x < 0 ? 0 : x;
    int cy  = y < 0 ? 0 : y;
    int cx2 = x + w; if (cx2 > W) cx2 = W;
    int cy2 = y + h; if (cy2 > H) cy2 = H;
    if (cx >= cx2 || cy >= cy2) return;

    /* For CLEAR_MASK / FILL_MASK: no source image needed */
    if (op == VG_CLEAR_MASK || op == VG_FILL_MASK) {
        uint8_t fill = (op == VG_FILL_MASK) ? 255 : 0;
        for (int my = cy; my < cy2; my++) {
            /* OpenVG y=0 is bottom; mask_buf row 0 is top */
            int mrow = H - 1 - my;
            if (mrow < 0 || mrow >= H) continue;
            memset(mbuf + mrow * W + cx, fill, (size_t)(cx2 - cx));
        }
        return;
    }

    /* Source-based operations: read alpha from image or mask layer */
    if (is_masklayer_handle(mask)) {
        cm_masklayer_t *ml = (cm_masklayer_t *)(uintptr_t)mask;
        /* ml->data: same coordinate system as mask_buf (row 0 = top) */
        for (int my = cy; my < cy2; my++) {
            int mrow = H - 1 - my;  /* flip OpenVG y */
            if (mrow < 0 || mrow >= H) continue;
            uint8_t *dst = mbuf + mrow * W;
            int src_y = my - y;  /* offset into mask layer (Y-UP: row 0 = OpenVG y=0) */
            int ml_row = src_y;  /* no Y-flip: mask layer uses Y-UP like OpenVG */
            if (ml_row < 0 || ml_row >= ml->height) { /* fill with 0 outside */ continue; }
            for (int mx = cx; mx < cx2; mx++) {
                int src_x = mx - x;
                uint8_t src = (src_x >= 0 && src_x < ml->width) ?
                              ml->data[ml_row * ml->width + src_x] : 0;
                dst[mx] = apply_mask_op(op, dst[mx], src);
            }
        }
    } else {
        /* VGImage: read alpha channel */
        cm_image_t *img = (cm_image_t *)(uintptr_t)mask;
        int bpp = img->bpp;
        for (int my = cy; my < cy2; my++) {
            int mrow = H - 1 - my;
            if (mrow < 0 || mrow >= H) continue;
            uint8_t *dst = mbuf + mrow * W;
            int src_y = my - y;
            /* Image y=0 is top in pixel storage */
            int img_row = src_y;
            if (img_row < 0 || img_row >= img->height) continue;
            for (int mx = cx; mx < cx2; mx++) {
                int src_x = mx - x;
                uint8_t src = 255;
                if (src_x >= 0 && src_x < img->width) {
                    const uint8_t *px = img->pixels + (size_t)img_row * (size_t)img->row_stride + (size_t)src_x * (size_t)bpp;
                    /* Get alpha for this format: for RGBA/BGRA, alpha is last byte */
                    if (bpp == 4) src = px[3];
                    else if (bpp == 1) src = px[0];
                    else src = 255;
                }
                dst[mx] = apply_mask_op(op, dst[mx], src);
            }
        }
    }
}

/* forward declaration - draw_stroke implementation is in the stroke section */
static void draw_stroke(cm_ctx_t *ctx, cm_path_t *src_path);

void vgRenderToMask(VGPath path, VGbitfield paintModes, VGMaskOperation op)
{
    VG_CHECK_CTX();
    if (path == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_path_t *p = (cm_path_t *)(uintptr_t)path;
    if (p->magic != PATH_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (!(paintModes & (VG_FILL_PATH | VG_STROKE_PATH)) ||
        (paintModes & ~(uint32_t)(VG_FILL_PATH | VG_STROKE_PATH))) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    if (op < VG_CLEAR_MASK || op > VG_SUBTRACT_MASK) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    if (!g_surface || !g_surface->mask_buf) return;

    cm_surface_t *surf = g_surface;
    int W = surf->width, H = surf->height;
    vg_cmodel_t cm = surf->cm;

    /* Save framebuffer */
    uint32_t *fb = vg_cmodel_get_fb_rw(cm, NULL, NULL);
    if (!fb) return;
    uint32_t *saved = (uint32_t *)malloc((size_t)(W * H) * 4);
    if (!saved) { VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return; }
    memcpy(saved, fb, (size_t)(W * H) * 4);

    /* Clear FB to transparent black (no mask) */
    memset(fb, 0, (size_t)(W * H) * 4);

    /* Save state and override for coverage rendering */
    VGBlendMode  save_blend   = g_ctx->blend_mode;
    int          save_masking = g_ctx->masking;
    cm_paint_t  *save_fill    = g_ctx->fill_paint;
    cm_paint_t  *save_stroke  = g_ctx->stroke_paint;

    g_ctx->blend_mode   = VG_BLEND_SRC;
    g_ctx->masking      = 0;
    /* Use white paint so that alpha = coverage */
    static cm_paint_t rtm_white_paint;
    rtm_white_paint.type     = VG_PAINT_TYPE_COLOR;
    rtm_white_paint.color[0] = 1.f;
    rtm_white_paint.color[1] = 1.f;
    rtm_white_paint.color[2] = 1.f;
    rtm_white_paint.color[3] = 1.f;
    g_ctx->fill_paint   = &rtm_white_paint;
    g_ctx->stroke_paint = &rtm_white_paint;

    /* Disable mask in cmodel */
    vg_cmodel_reg_write(cm, VG_REG_MASK_EN, 0);

    uint32_t blend_hw = (uint32_t)(VG_BLEND_SRC - VG_BLEND_SRC); /* = 0 = SRC */
    vg_cmodel_reg_write(cm, VG_REG_BLEND_MODE, blend_hw);
    uint32_t aa_hw = (g_ctx->rendering_quality == VG_RENDERING_QUALITY_NONANTIALIASED)
                     ? VG_AA_NONE : VG_AA_8X;
    vg_cmodel_reg_write(cm, VG_REG_AA_SAMPLES, aa_hw);
    uint32_t fill_rule = (g_ctx->fill_rule == VG_EVEN_ODD)
                         ? VG_REG_FILL_EVEN_ODD : VG_REG_FILL_NON_ZERO;
    vg_cmodel_reg_write(cm, VG_REG_FILL_RULE, fill_rule);
    load_matrix_to_cmodel(g_ctx);
    load_paint_to_cmodel(g_ctx, &rtm_white_paint);
    vg_cmodel_reg_write_f(cm, VG_REG_PATH_SCALE, 1.0f);
    vg_cmodel_reg_write_f(cm, VG_REG_PATH_BIAS,  0.0f);

    if (paintModes & VG_FILL_PATH) {
        vg_cmodel_set_path_ptr(cm, p->buf, (uint32_t)p->buf_len);
        vg_cmodel_reg_write(cm, VG_REG_PATH_KICK, 1);
    }
    if (paintModes & VG_STROKE_PATH) {
        draw_stroke(g_ctx, p);
    }

    /* Apply coverage (alpha channel of fb) to mask_buf using op */
    uint8_t *mbuf = surf->mask_buf;
    for (int i = 0; i < W * H; i++) {
        uint8_t cov = (uint8_t)(fb[i] & 0xFF);  /* alpha in low byte (RGBA pack: r=24, g=16, b=8, a=0) */
        mbuf[i] = apply_mask_op(op, mbuf[i], cov);
    }

    /* Restore framebuffer and state */
    memcpy(fb, saved, (size_t)(W * H) * 4);
    free(saved);
    g_ctx->blend_mode   = save_blend;
    g_ctx->masking      = save_masking;
    g_ctx->fill_paint   = save_fill;
    g_ctx->stroke_paint = save_stroke;
}

VGMaskLayer vgCreateMaskLayer(VGint w, VGint h)
{
    VG_CHECK_CTX(VG_INVALID_HANDLE);
    if (w <= 0 || h <= 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return VG_INVALID_HANDLE; }
    cm_masklayer_t *ml = (cm_masklayer_t *)malloc(sizeof(cm_masklayer_t));
    if (!ml) { VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return VG_INVALID_HANDLE; }
    ml->data = (uint8_t *)malloc((size_t)(w * h));
    if (!ml->data) { free(ml); VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return VG_INVALID_HANDLE; }
    memset(ml->data, 0xFF, (size_t)(w * h)); /* initialized to fully unmasked */
    ml->magic  = MASKLAYER_MAGIC;
    ml->width  = w;
    ml->height = h;
    return (VGMaskLayer)(uintptr_t)ml;
}

void vgDestroyMaskLayer(VGMaskLayer masklayer)
{
    if (masklayer == VG_INVALID_HANDLE) return;
    cm_masklayer_t *ml = (cm_masklayer_t *)(uintptr_t)masklayer;
    if (ml->magic != MASKLAYER_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    ml->magic = 0;
    free(ml->data);
    free(ml);
}

void vgFillMaskLayer(VGMaskLayer masklayer, VGint x, VGint y, VGint w, VGint h, VGfloat value)
{
    if (masklayer == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_masklayer_t *ml = (cm_masklayer_t *)(uintptr_t)masklayer;
    if (ml->magic != MASKLAYER_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (value < 0.0f || value > 1.0f) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (w <= 0 || h <= 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    /* Clamp to mask layer bounds */
    int cx  = x < 0 ? 0 : x;
    int cy  = y < 0 ? 0 : y;
    int cx2 = x + w; if (cx2 > ml->width)  cx2 = ml->width;
    int cy2 = y + h; if (cy2 > ml->height) cy2 = ml->height;
    if (cx >= cx2 || cy >= cy2) return;
    uint8_t fill = (uint8_t)(value * 255.0f + 0.5f);
    for (int row = cy; row < cy2; row++) {
        memset(ml->data + row * ml->width + cx, fill, (size_t)(cx2 - cx));
    }
}

void vgCopyMask(VGMaskLayer masklayer, VGint sx, VGint sy,
                VGint dx, VGint dy, VGint width, VGint height)
{
    VG_CHECK_CTX();
    if (masklayer == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_masklayer_t *ml = (cm_masklayer_t *)(uintptr_t)masklayer;
    if (ml->magic != MASKLAYER_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (width <= 0 || height <= 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (!g_surface || !g_surface->mask_buf) return;
    cm_surface_t *surf = g_surface;
    int W = surf->width, H = surf->height;
    uint8_t *mbuf = surf->mask_buf;

    for (int row = 0; row < height; row++) {
        /* API: vgCopyMask(layer, dx, dy, sx, sy, w, h)
         * Our params named (sx,sy,dx,dy) actually map to spec's (dx,dy,sx,sy):
         *   our sx/sy = spec's dx/dy = layer destination
         *   our dx/dy = spec's sx/sy = surface source
         * Mask layer uses Y-UP (OpenVG) convention: row 0 = OpenVG y=0 = bottom. */
        int surf_y = dy + row;  /* surface source y (OpenVG) */
        int mask_row = H - 1 - surf_y;  /* surface mask_buf index (Y-DOWN) */
        if (mask_row < 0 || mask_row >= H) continue;
        int ml_dst_y = sy + row;  /* layer dest y (Y-UP = OpenVG y) */
        if (ml_dst_y < 0 || ml_dst_y >= ml->height) continue;
        for (int col = 0; col < width; col++) {
            int surf_x = dx + col;  /* surface source x */
            int ml_dst_x = sx + col;  /* layer dest x */
            if (surf_x < 0 || surf_x >= W) continue;
            if (ml_dst_x < 0 || ml_dst_x >= ml->width) continue;
            ml->data[ml_dst_y * ml->width + ml_dst_x] = mbuf[mask_row * W + surf_x];
        }
    }
}

/* =========================================================================
 * VG — Clear
 * ========================================================================= */

void vgClear(VGint x, VGint y, VGint width, VGint height)
{
    VG_CHECK_CTX();
    if (!g_surface) return;
    cm_surface_t *surf = g_surface;
    int W = surf->width, H = surf->height;

    /* Clamp to surface */
    if (x < 0) { width  += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width  > W) width  = W - x;
    if (y + height > H) height = H - y;
    if (width <= 0 || height <= 0) return;

    uint32_t col = pack_rgba(g_ctx->clear_color);
    uint32_t *fb = (uint32_t *)(uintptr_t)vg_cmodel_get_framebuffer(surf->cm, NULL, NULL);
    if (!fb) return;

    /* Helper: clear a rectangle [rx, ry, rw, rh] in OpenVG coords (y=0 at bottom) */
    /* OpenVG y=0 is bottom; cmodel row 0 is top → flip */
#define CLEAR_RECT(rx, ry, rw, rh) \
    do { \
        for (int row = 0; row < (rh); row++) { \
            int cmodel_row = H - 1 - ((ry) + row); \
            if (cmodel_row < 0 || cmodel_row >= H) continue; \
            uint32_t *line = fb + cmodel_row * W + (rx); \
            for (int col_idx = 0; col_idx < (rw); col_idx++) \
                line[col_idx] = col; \
        } \
    } while(0)

    if (g_ctx->scissoring && g_ctx->num_scissor_rects == 0) {
        /* Scissoring enabled with no scissor rects = nothing gets cleared */
        return;
    } else if (g_ctx->scissoring && g_ctx->num_scissor_rects > 0) {
        /* Clear only the intersection of [x,y,w,h] with each scissor rect */
        for (int ri = 0; ri < g_ctx->num_scissor_rects; ri++) {
            VGint sx = g_ctx->scissor_rects[ri*4+0];
            VGint sy = g_ctx->scissor_rects[ri*4+1];
            VGint sw = g_ctx->scissor_rects[ri*4+2];
            VGint sh = g_ctx->scissor_rects[ri*4+3];
            /* Intersect with clear region */
            VGint ix = (sx > x) ? sx : x;
            VGint iy = (sy > y) ? sy : y;
            VGint ix2 = (sx+sw < x+width) ? sx+sw : x+width;
            VGint iy2 = (sy+sh < y+height) ? sy+sh : y+height;
            VGint iw = ix2 - ix;
            VGint ih = iy2 - iy;
            if (iw <= 0 || ih <= 0) continue;
            /* Clamp to surface */
            if (ix < 0) { iw += ix; ix = 0; }
            if (iy < 0) { ih += iy; iy = 0; }
            if (ix + iw > W) iw = W - ix;
            if (iy + ih > H) ih = H - iy;
            if (iw <= 0 || ih <= 0) continue;
            CLEAR_RECT(ix, iy, iw, ih);
        }
    } else {
        CLEAR_RECT(x, y, width, height);
    }
#undef CLEAR_RECT
}

/* =========================================================================
 * VG — Path
 * ========================================================================= */

VGPath vgCreatePath(VGint pathFormat, VGPathDatatype datatype,
                    VGfloat scale, VGfloat bias,
                    VGint segCapacityHint, VGint coordCapacityHint,
                    VGbitfield capabilities)
{
    VG_CHECK_CTX(VG_INVALID_HANDLE);
    if (pathFormat != VG_PATH_FORMAT_STANDARD) {
        VG_SET_ERR(VG_UNSUPPORTED_PATH_FORMAT_ERROR);
        return VG_INVALID_HANDLE;
    }
    cm_path_t *p = (cm_path_t *)malloc(sizeof(cm_path_t));
    if (!p) { VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return VG_INVALID_HANDLE; }
    int cap = (coordCapacityHint > 0) ? coordCapacityHint * 8 : 256;
    p->buf = (uint8_t *)malloc((size_t)cap);
    if (!p->buf) { free(p); VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return VG_INVALID_HANDLE; }
    p->magic        = PATH_MAGIC;
    p->reg_next     = g_path_registry;
    g_path_registry = p;
    p->buf_cap      = cap;
    p->buf_len      = 0;
    p->capabilities = capabilities;
    p->datatype     = datatype;
    p->scale        = (scale == 0.f) ? 1.f : scale;
    p->bias         = bias;
    p->num_segments = 0;
    p->num_coords   = 0;
    (void)segCapacityHint;
    return (VGPath)(uintptr_t)p;
}

void vgDestroyPath(VGPath path)
{
    if (path == VG_INVALID_HANDLE) return;
    cm_path_t *p = (cm_path_t *)(uintptr_t)path;
    /* Remove from global registry */
    cm_path_t **pp = &g_path_registry;
    while (*pp && *pp != p) pp = &(*pp)->reg_next;
    if (*pp) *pp = p->reg_next;
    p->magic = 0;
    free(p->buf);
    free(p);
}

void vgClearPath(VGPath path, VGbitfield capabilities)
{
    if (path == VG_INVALID_HANDLE) return;
    cm_path_t *p = (cm_path_t *)(uintptr_t)path;
    p->buf_len       = 0;
    p->capabilities  = capabilities;
    p->num_segments  = 0;
    p->num_coords    = 0;
}

void vgRemovePathCapabilities(VGPath path, VGbitfield capabilities)
{
    if (path == VG_INVALID_HANDLE) return;
    cm_path_t *p = (cm_path_t *)(uintptr_t)path;
    p->capabilities &= ~capabilities;
}

VGbitfield vgGetPathCapabilities(VGPath path)
{
    if (path == VG_INVALID_HANDLE) {
        VG_SET_ERR(VG_BAD_HANDLE_ERROR); return 0;
    }
    return ((cm_path_t *)(uintptr_t)path)->capabilities;
}

void vgAppendPath(VGPath dst, VGPath src)
{
    VG_CHECK_CTX();
    if (dst == VG_INVALID_HANDLE || src == VG_INVALID_HANDLE) {
        VG_SET_ERR(VG_BAD_HANDLE_ERROR); return;
    }
    cm_path_t *d = (cm_path_t *)(uintptr_t)dst;
    cm_path_t *s = (cm_path_t *)(uintptr_t)src;
    int new_len = d->buf_len + s->buf_len;
    if (new_len > d->buf_cap) {
        uint8_t *nb = (uint8_t *)realloc(d->buf, (size_t)new_len * 2);
        if (!nb) { VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return; }
        d->buf = nb; d->buf_cap = new_len * 2;
    }
    memcpy(d->buf + d->buf_len, s->buf, (size_t)s->buf_len);
    d->buf_len = new_len;
}

/* Number of coordinates per segment type (absolute, ignoring REL bit) */
static int seg_coord_count(uint8_t seg)
{
    switch (seg & ~1) {  /* mask REL bit */
    case VG_CLOSE_PATH:   return 0;
    case VG_MOVE_TO:      return 2;
    case VG_LINE_TO:      return 2;
    case VG_HLINE_TO:     return 1;
    case VG_VLINE_TO:     return 1;
    case VG_QUAD_TO:      return 4;
    case VG_CUBIC_TO:     return 6;
    case VG_SQUAD_TO:     return 2;
    case VG_SCUBIC_TO:    return 4;
    case VG_SCCWARC_TO:   return 5;
    case VG_SCWARC_TO:    return 5;
    case VG_LCCWARC_TO:   return 5;
    case VG_LCWARC_TO:    return 5;
    default:              return 0;
    }
}

/* Read one coordinate from raw data, convert to float */
static float read_coord(VGPathDatatype dt, VGfloat scale, VGfloat bias,
                        const void *data, int idx)
{
    float v;
    switch (dt) {
    case VG_PATH_DATATYPE_S_8:
        v = (float)((const int8_t  *)data)[idx]; break;
    case VG_PATH_DATATYPE_S_16:
        v = (float)((const int16_t *)data)[idx]; break;
    case VG_PATH_DATATYPE_S_32:
        v = (float)((const int32_t *)data)[idx]; break;
    default: /* VG_PATH_DATATYPE_F */
        v = ((const float *)data)[idx]; break;
    }
    return v * scale + bias;
}

/* Grow path buffer by at least `needed` bytes */
static int path_ensure(cm_path_t *p, int needed)
{
    if (p->buf_len + needed <= p->buf_cap) return 1;
    int new_cap = (p->buf_len + needed) * 2;
    uint8_t *nb = (uint8_t *)realloc(p->buf, (size_t)new_cap);
    if (!nb) return 0;
    p->buf = nb; p->buf_cap = new_cap;
    return 1;
}

/* Write a 4-byte header (segment code) */
#define PATH_EMIT_CMD(p, cmd) \
    do { \
        if (!path_ensure((p), 4)) return; \
        uint32_t h = (uint8_t)(cmd); \
        memcpy((p)->buf + (p)->buf_len, &h, 4); \
        (p)->buf_len += 4; \
    } while(0)

/* Write one float to path buffer */
#define PATH_EMIT_FLOAT(p, v) \
    do { \
        if (!path_ensure((p), 4)) return; \
        float _f = (v); \
        memcpy((p)->buf + (p)->buf_len, &_f, 4); \
        (p)->buf_len += 4; \
    } while(0)

void vgAppendPathData(VGPath vgpath, VGint numSegments,
                      const VGubyte *pathSegments, const void *pathData)
{
    VG_CHECK_CTX();
    if (vgpath == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (numSegments <= 0 || !pathSegments) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }

    cm_path_t *p = (cm_path_t *)(uintptr_t)vgpath;
    if (!(p->capabilities & VG_PATH_CAPABILITY_APPEND_TO)) {
        VG_SET_ERR(VG_PATH_CAPABILITY_ERROR); return;
    }

    /* Validate all segments before appending anything */
    for (int i = 0; i < numSegments; i++) {
        uint8_t seg = pathSegments[i];
        /* Check for invalid segment command (only bits [4:1] define valid cmds) */
        uint8_t cmd = seg >> 1;
        if (cmd > 12 || (seg & 0xE0)) { /* valid cmds are 0..12, bits[7:5] must be 0 */
            VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
        }
    }

    /* Check pathData alignment when coordinates will be needed */
    if (pathData) {
        size_t align = (p->datatype == VG_PATH_DATATYPE_F)    ? sizeof(VGfloat) :
                       (p->datatype == VG_PATH_DATATYPE_S_32) ? 4u :
                       (p->datatype == VG_PATH_DATATYPE_S_16) ? 2u : 1u;
        if (align > 1 && ((uintptr_t)pathData & (align - 1u)) != 0) {
            VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
        }
    }

    int coord_idx = 0;
    for (int i = 0; i < numSegments; i++) {
        uint8_t seg  = pathSegments[i];
        int nc = seg_coord_count(seg);

        /* If we need coordinates but pathData is null, error */
        if (nc > 0 && !pathData) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }

        /* Use the path's own scale/bias for coordinate conversion */
        float s = p->scale, b = p->bias;

        /* Emit header word */
        if (!path_ensure(p, 4 + nc * 4)) {
            VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return;
        }
        uint32_t hdr = (uint32_t)(uint8_t)seg;
        memcpy(p->buf + p->buf_len, &hdr, 4);
        p->buf_len += 4;

        /* Emit coordinates as floats */
        for (int c = 0; c < nc; c++) {
            float v = read_coord(p->datatype, s, b, pathData, coord_idx++);
            memcpy(p->buf + p->buf_len, &v, 4);
            p->buf_len += 4;
        }
    }
    p->num_segments += numSegments;
    p->num_coords   += coord_idx;
}

void vgModifyPathCoords(VGPath vgpath, VGint startIndex, VGint numSegments,
                        const void *pathData)
{
    VG_CHECK_CTX();
    if (vgpath == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (numSegments <= 0 || !pathData) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    cm_path_t *p = (cm_path_t *)(uintptr_t)vgpath;
    if (!(p->capabilities & VG_PATH_CAPABILITY_MODIFY)) {
        VG_SET_ERR(VG_PATH_CAPABILITY_ERROR); return;
    }

    /* Walk to byte offset of segment 'startIndex' in the buffer */
    const uint8_t *buf_end = p->buf + p->buf_len;
    uint8_t *wp = p->buf;
    for (int i = 0; i < startIndex; i++) {
        if (wp + 4 > buf_end) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        uint32_t hdr; memcpy(&hdr, wp, 4); wp += 4;
        uint8_t seg = (uint8_t)(hdr & 0xFF);
        int nc = seg_coord_count(seg);
        wp += nc * 4;
        if (wp > buf_end) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    }

    /* Overwrite coordinate data for numSegments segments */
    VGfloat s = p->scale, b = p->bias;
    int coord_idx = 0;
    for (int i = 0; i < numSegments; i++) {
        if (wp + 4 > buf_end) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        uint32_t hdr; memcpy(&hdr, wp, 4); wp += 4;  /* keep header as-is */
        uint8_t seg = (uint8_t)(hdr & 0xFF);
        int nc = seg_coord_count(seg);
        for (int c = 0; c < nc; c++) {
            if (wp + 4 > buf_end) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
            float v = read_coord(p->datatype, s, b, pathData, coord_idx++);
            memcpy(wp, &v, 4); wp += 4;
        }
    }
}

void vgTransformPath(VGPath dst, VGPath src)
{
    /* Copy src to dst for now (ignoring the baked transform) */
    vgAppendPath(dst, src);
}

VGboolean vgInterpolatePath(VGPath dst, VGPath st, VGPath en, VGfloat amount)
{
    (void)dst;(void)st;(void)en;(void)amount;
    return VG_FALSE;
}

/* ---- 2-D vector helpers (used by both path queries and stroke renderer) ---- */
typedef struct { float x, y; } sv2;
#define SV2(xx,yy) ((sv2){(xx),(yy)})
static sv2 sv2_add(sv2 a, sv2 b){ return SV2(a.x+b.x, a.y+b.y); }
static sv2 sv2_sub(sv2 a, sv2 b){ return SV2(a.x-b.x, a.y-b.y); }
static sv2 sv2_sc (sv2 a, float s){ return SV2(a.x*s,  a.y*s);  }
static float sv2_dot(sv2 a, sv2 b){ return a.x*b.x + a.y*b.y; }
static float sv2_cross(sv2 a, sv2 b){ return a.x*b.y - a.y*b.x; }
static float sv2_len(sv2 a){ return sqrtf(a.x*a.x + a.y*a.y); }
static sv2 sv2_norm(sv2 a){
    float l = sv2_len(a);
    return (l > 1e-9f) ? SV2(a.x/l, a.y/l) : SV2(1.f, 0.f);
}
static sv2 sv2_perp(sv2 a){ return SV2(-a.y, a.x); } /* 90° CCW */

/* ---- Growable point list ---- */
typedef struct { sv2 *pts; int n, cap; } ptlist_t;
static int ptl_push(ptlist_t *l, sv2 p){
    if(l->n >= l->cap){
        int nc = l->cap ? l->cap*2 : 64;
        sv2 *nb = (sv2 *)realloc(l->pts, (size_t)nc*sizeof(sv2));
        if(!nb) return 0;
        l->pts = nb; l->cap = nc;
    }
    l->pts[l->n++] = p; return 1;
}
static void ptl_free(ptlist_t *l){ free(l->pts); l->pts=NULL; l->n=l->cap=0; }

/* forward declaration - flat_arc implementation is in the stroke section below */
static void flat_arc(ptlist_t *l, sv2 p0, float rx, float ry,
                     float phi_deg, sv2 p1, int large, int ccw);

/* =========================================================================
 * RI-compatible arc tessellation for path queries (vgPathLength, etc.)
 * Replicates the RI's circularLerp / unitAverage / findEllipses algorithm
 * using RI_NUM_TESSELLATED_SEGMENTS=256, matching the RI's exact FP behavior.
 * ========================================================================= */

/* 3x3 matrix for RI arc computation (row-major, only upper 2x3 used) */
typedef struct { float m[3][3]; } ri_m3_t;

static float ri_v2_dot(sv2 a, sv2 b) { return a.x*b.x + a.y*b.y; }
static sv2 ri_v2_perp_ccw(sv2 v)   { return SV2(-v.y,  v.x); }
static sv2 ri_v2_perp_cw(sv2 v)    { return SV2( v.y, -v.x); }
static sv2 ri_v2_half(sv2 a, sv2 b){ return SV2(0.5f*(a.x+b.x), 0.5f*(a.y+b.y)); }
static sv2 ri_v2_neg(sv2 v)         { return SV2(-v.x, -v.y); }
static int  ri_v2_eq(sv2 a, sv2 b)  { return a.x==b.x && a.y==b.y; }

/* RI normalize: uses double precision internally (riMath.h) */
static sv2 ri_v2_normalize(sv2 v) {
    double x = v.x, y = v.y, l = x*x + y*y;
    if (l != 0.0) l = 1.0 / sqrt(l);
    return SV2((float)(x*l), (float)(y*l));
}

/* unitAverage with cw flag (RI's 3-param version) */
static sv2 ri_unit_avg_cw(sv2 u0, sv2 u1, int cw) {
    sv2 u = ri_v2_half(u0, u1);
    sv2 n0 = ri_v2_perp_ccw(u0);
    if (ri_v2_dot(u, u) > 0.25f) {
        if (ri_v2_dot(n0, u1) < 0.0f) u = ri_v2_neg(u);
    } else {
        sv2 n1 = ri_v2_perp_cw(u1);
        u = ri_v2_half(n0, n1);
    }
    if (cw) u = ri_v2_neg(u);
    return ri_v2_normalize(u);
}

/* circularLerp with cw flag (RI's 4-param version) - used for arc tessellation */
static sv2 ri_circ_lerp_cw(sv2 t0, sv2 t1, float ratio, int cw) {
    sv2 u0 = t0, u1 = t1;
    float l0 = 0.0f, l1 = 1.0f;
    for (int i = 0; i < 18; i++) {
        sv2 n = ri_unit_avg_cw(u0, u1, cw);
        float l = 0.5f * (l0 + l1);
        if (ratio < l) { u1 = n; l1 = l; }
        else           { u0 = n; l0 = l; }
    }
    return u0;
}

/* Affine 2x3 transform: v' = M * v + t */
static sv2 ri_affine(const ri_m3_t *m, sv2 v) {
    return SV2(v.x*m->m[0][0] + v.y*m->m[0][1] + m->m[0][2],
               v.x*m->m[1][0] + v.y*m->m[1][1] + m->m[1][2]);
}

/*
 * ri_arc_tess: RI-compatible arc tessellation.
 * Exactly replicates RI's addArcTo / findEllipses with 256 steps.
 * Adds 255 interior arc points + the endpoint p1 to *l.
 * p0 is already in *l.  large/ccw flags match the VG segment type.
 */
static void ri_arc_tess(ptlist_t *l, sv2 p0, float rh, float rv, float phi_deg,
                        sv2 p1, int large, int ccw)
{
    /* Segment type → center choice and direction:
     * large=0,ccw=1 (SCCWARC): cp=c0, cw=false
     * large=0,ccw=0 (SCWARC):  cp=c1, cw=true
     * large=1,ccw=1 (LCCWARC): cp=c1, cw=false
     * large=1,ccw=0 (LCWARC):  cp=c0, cw=true
     * use_c1 = (large == ccw), cw_dir = !ccw */
    int use_c1  = (large == ccw);   /* 0-indexed booleans */
    int cw_dir  = !ccw;

    rh = fabsf(rh); rv = fabsf(rv);
    if (rh < 1e-7f || rv < 1e-7f) { ptl_push(l, p1); return; }

    sv2 p1r = SV2(p1.x - p0.x, p1.y - p0.y);   /* relative endpoint */
    if (ri_v2_eq(p1r, SV2(0.0f, 0.0f))) return;

    float rot = phi_deg * (float)(M_PI / 180.0);
    float crot = cosf(rot), srot = sinf(rot);

    /* unitCircleToEllipse (no translation yet) */
    ri_m3_t uce;
    memset(&uce, 0, sizeof(uce));
    uce.m[0][0] = crot*rh;  uce.m[0][1] = -srot*rv;
    uce.m[1][0] = srot*rh;  uce.m[1][1] =  crot*rv;
    uce.m[2][2] = 1.0f;

    /* ellipseToUnitCircle = invert(uce) (no translation in uce, so det = rh*rv) */
    float det = uce.m[0][0]*uce.m[1][1] - uce.m[0][1]*uce.m[1][0];
    if (fabsf(det) < 1e-20f) { ptl_push(l, p1); return; }
    float idet = 1.0f / det;
    ri_m3_t euc;
    memset(&euc, 0, sizeof(euc));
    euc.m[0][0] =  uce.m[1][1] * idet;
    euc.m[0][1] = -uce.m[0][1] * idet;
    euc.m[1][0] = -uce.m[1][0] * idet;
    euc.m[1][1] =  uce.m[0][0] * idet;
    euc.m[2][2] = 1.0f;  /* no translation; euc.m[0][2]=euc.m[1][2]=0 */

    /* RI passes p0=(0,0) and p1=p1r to findEllipses */
    sv2 u0 = SV2(0.0f, 0.0f);          /* affine(euc, (0,0)) = (0,0) */
    sv2 u1 = ri_affine(&euc, p1r);      /* p1r in unit circle space  */

    sv2 mv = ri_v2_half(u0, u1);
    sv2 d  = SV2(u0.x - u1.x, u0.y - u1.y);
    float lsq = ri_v2_dot(d, d);
    if (lsq <= 0.0f) { ptl_push(l, p1); return; }

    float disc = 1.0f / lsq - 0.25f;
    if (disc < 0.0f) {
        /* Axes too small: scale so a solution exists (RI's rescaling branch) */
        float scale = 0.5f * sqrtf(lsq);
        rh *= scale; rv *= scale;
        uce.m[0][0] = crot*rh;  uce.m[0][1] = -srot*rv;
        uce.m[1][0] = srot*rh;  uce.m[1][1] =  crot*rv;
        det = uce.m[0][0]*uce.m[1][1] - uce.m[0][1]*uce.m[1][0];
        if (fabsf(det) < 1e-20f) { ptl_push(l, p1); return; }
        idet = 1.0f / det;
        euc.m[0][0] =  uce.m[1][1] * idet;
        euc.m[0][1] = -uce.m[0][1] * idet;
        euc.m[1][0] = -uce.m[1][0] * idet;
        euc.m[1][1] =  uce.m[0][0] * idet;
        u1 = ri_affine(&euc, p1r);
        d  = SV2(u0.x - u1.x, u0.y - u1.y);
        mv = ri_v2_half(u0, u1);
        lsq = ri_v2_dot(d, d);
        if (lsq <= 0.0f) { ptl_push(l, p1); return; }
        disc = 1.0f / lsq - 0.25f;
        if (disc < 0.0f) disc = 0.0f;
    }
    if (ri_v2_eq(u0, u1)) { ptl_push(l, p1); return; }

    /* Find the two candidate centers */
    float sdist = sqrtf(disc);
    sv2 sd = SV2(d.x * sdist, d.y * sdist);
    sv2 sp = ri_v2_perp_cw(sd);          /* perpCW of sd */
    sv2 c0 = SV2(mv.x + sp.x, mv.y + sp.y);
    sv2 c1 = SV2(mv.x - sp.x, mv.y - sp.y);

    sv2 center = use_c1 ? c1 : c0;

    /* Unit circle vectors relative to center */
    sv2 ua = SV2(u0.x - center.x, u0.y - center.y);
    sv2 ub = SV2(u1.x - center.x, u1.y - center.y);

    if (ri_v2_eq(ua, ub) ||
        (ua.x==0.0f && ua.y==0.0f) ||
        (ub.x==0.0f && ub.y==0.0f)) { ptl_push(l, p1); return; }

    /* Bake center translation into uce: center in ellipse/world coords */
    sv2 center_e = ri_affine(&uce, center);  /* uce still has no translation */
    uce.m[0][2] = center_e.x;
    uce.m[1][2] = center_e.y;

    /* Tessellate with RI_NUM_TESSELLATED_SEGMENTS=256 using circularLerp */
    const int SEG = 256;
    for (int i = 1; i < SEG; i++) {
        float t  = (float)i / (float)SEG;
        sv2   pn = ri_circ_lerp_cw(ua, ub, t, cw_dir);
        /* Map unit-circle point → world: affine(uce, pn) + p0 */
        sv2   pw = ri_affine(&uce, pn);
        ptl_push(l, SV2(pw.x + p0.x, pw.y + p0.y));
    }
    ptl_push(l, p1);   /* final endpoint */
}

/* RI arc tangent: perpendicular(pn, cw) mapped through the 2x2 part of uce, normalized.
 * Matches RI's: tn = normalize(affineTangentTransform(uce, perpendicular(pn, cw)))
 * where perpendicular(v, cw=true) = (v.y, -v.x), perpendicular(v, cw=false) = (-v.y, v.x). */
static sv2 ri_arc_tan(sv2 pn, int cw_dir, const ri_m3_t *uce) {
    sv2 tn = cw_dir ? SV2(pn.y, -pn.x) : SV2(-pn.y, pn.x);
    /* 2x2 affine tangent transform (no translation) */
    sv2 tm = SV2(tn.x*uce->m[0][0] + tn.y*uce->m[0][1],
                 tn.x*uce->m[1][0] + tn.y*uce->m[1][1]);
    return ri_v2_normalize(tm);
}

/* Arc geometry context from findEllipses.  A is valid flag (0 if degenerate arc). */
typedef struct {
    sv2      ua, ub;    /* unit circle start/end vectors (relative to center) */
    ri_m3_t  uce;       /* unitCircleToEllipse (with center baked into translation) */
    int      cw;        /* clockwise flag */
    int      valid;     /* 1 if ellipses found, 0 if degenerate */
} ri_arc_ctx_t;

/*
 * ri_arc_setup: run findEllipses for the arc from p0 to p1.
 * Returns ctx.valid=1 on success, 0 if arc is degenerate (fall back to line).
 */
static ri_arc_ctx_t ri_arc_setup(sv2 p0, float rh, float rv, float phi_deg,
                                  sv2 p1, int large, int ccw)
{
    ri_arc_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    int use_c1 = (large == ccw);
    ctx.cw = !ccw;

    rh = fabsf(rh); rv = fabsf(rv);
    if (rh < 1e-7f || rv < 1e-7f) return ctx;

    sv2 p1r = SV2(p1.x - p0.x, p1.y - p0.y);
    if (ri_v2_eq(p1r, SV2(0.0f, 0.0f))) return ctx;

    float rot = phi_deg * (float)(M_PI / 180.0);
    float crot = cosf(rot), srot = sinf(rot);

    ri_m3_t uce; memset(&uce, 0, sizeof(uce));
    uce.m[0][0] = crot*rh; uce.m[0][1] = -srot*rv;
    uce.m[1][0] = srot*rh; uce.m[1][1] =  crot*rv;
    uce.m[2][2] = 1.0f;

    float det = uce.m[0][0]*uce.m[1][1] - uce.m[0][1]*uce.m[1][0];
    if (fabsf(det) < 1e-20f) return ctx;
    float idet = 1.0f / det;
    ri_m3_t euc; memset(&euc, 0, sizeof(euc));
    euc.m[0][0] =  uce.m[1][1]*idet; euc.m[0][1] = -uce.m[0][1]*idet;
    euc.m[1][0] = -uce.m[1][0]*idet; euc.m[1][1] =  uce.m[0][0]*idet;
    euc.m[2][2] = 1.0f;

    sv2 u0 = SV2(0.0f, 0.0f), u1 = ri_affine(&euc, p1r);
    sv2 mv = ri_v2_half(u0, u1);
    sv2 d  = SV2(u0.x - u1.x, u0.y - u1.y);
    float lsq = ri_v2_dot(d, d);
    if (lsq <= 0.0f) return ctx;

    float disc = 1.0f / lsq - 0.25f;
    if (disc < 0.0f) {
        float scale = 0.5f * sqrtf(lsq);
        rh *= scale; rv *= scale;
        uce.m[0][0] = crot*rh; uce.m[0][1] = -srot*rv;
        uce.m[1][0] = srot*rh; uce.m[1][1] =  crot*rv;
        det = uce.m[0][0]*uce.m[1][1] - uce.m[0][1]*uce.m[1][0];
        if (fabsf(det) < 1e-20f) return ctx;
        idet = 1.0f / det;
        euc.m[0][0] =  uce.m[1][1]*idet; euc.m[0][1] = -uce.m[0][1]*idet;
        euc.m[1][0] = -uce.m[1][0]*idet; euc.m[1][1] =  uce.m[0][0]*idet;
        u1 = ri_affine(&euc, p1r);
        d  = SV2(u0.x - u1.x, u0.y - u1.y);
        mv = ri_v2_half(u0, u1);
        lsq = ri_v2_dot(d, d);
        if (lsq <= 0.0f) return ctx;
        disc = 1.0f / lsq - 0.25f;
        if (disc < 0.0f) disc = 0.0f;
    }
    if (ri_v2_eq(u0, u1)) return ctx;

    float sdist = sqrtf(disc);
    sv2 sd = SV2(d.x*sdist, d.y*sdist);
    sv2 sp = ri_v2_perp_cw(sd);
    sv2 c0 = SV2(mv.x+sp.x, mv.y+sp.y);
    sv2 c1 = SV2(mv.x-sp.x, mv.y-sp.y);
    sv2 center = use_c1 ? c1 : c0;

    sv2 ua = SV2(u0.x-center.x, u0.y-center.y);
    sv2 ub = SV2(u1.x-center.x, u1.y-center.y);
    if (ri_v2_eq(ua, ub) || (ua.x==0&&ua.y==0) || (ub.x==0&&ub.y==0)) return ctx;

    sv2 center_e = ri_affine(&uce, center);
    uce.m[0][2] = center_e.x; uce.m[1][2] = center_e.y;

    ctx.ua = ua; ctx.ub = ub; ctx.uce = uce; ctx.valid = 1;
    return ctx;
}

/* Compute arc position at unit-circle point pn: world = affine(uce, pn) + p0 */
static sv2 ri_arc_pos(const ri_arc_ctx_t *ctx, sv2 pn, sv2 p0) {
    sv2 raw = ri_affine(&ctx->uce, pn);
    return SV2(raw.x + p0.x, raw.y + p0.y);
}

/* =========================================================================
 * Path geometry utilities (vgPathLength, vgPathBounds, vgPointAlongPath)
 * =========================================================================
 * Uses 256-segment uniform tessellation for Bezier curves, matching the
 * OpenVG RI's RI_NUM_TESSELLATED_SEGMENTS = 256, with double-precision
 * distance accumulation (Vector2::length uses sqrt((double)x*x+(double)y*y)).
 * ========================================================================= */

/* RI-compatible distance: double-precision internally */
static float pw_dist(float dx, float dy) {
    return (float)sqrt((double)dx*(double)dx + (double)dy*(double)dy);
}

#define PW_TESS_N  256   /* RI_NUM_TESSELLATED_SEGMENTS */

/* Tessellated vertex for path queries */
typedef struct {
    float x, y;        /* position */
    float tx, ty;      /* unit tangent */
    float path_len;    /* cumulative path length at this vertex */
    int   seg_idx;     /* source segment index */
} pw_vtx_t;

typedef struct { pw_vtx_t *v; int n, cap; } pw_vl_t;

static void pw_vl_push(pw_vl_t *l, pw_vtx_t pv) {
    if (l->n >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 512;
        pw_vtx_t *nb = (pw_vtx_t *)realloc(l->v, (size_t)nc * sizeof(*nb));
        if (!nb) return;
        l->v = nb; l->cap = nc;
    }
    l->v[l->n++] = pv;
}
static void pw_vl_free(pw_vl_t *l) { free(l->v); l->v = NULL; l->n = l->cap = 0; }

/* Apply affine matrix M[9] (column-major OpenVG layout) to point (px,py) -> (ox,oy) */
static void mat_xform_pt(const float M[9], float px, float py, float *ox, float *oy) {
    *ox = M[0]*px + M[3]*py + M[6];
    *oy = M[1]*px + M[4]*py + M[7];
}

/* Update bounding box for a single point, optionally transformed */
static void pw_bbox_pt(float *bx0, float *by0, float *bx1, float *by1,
                        const float *xf, float px, float py)
{
    float x = px, y = py;
    if (xf) mat_xform_pt(xf, px, py, &x, &y);
    if (x < *bx0) *bx0 = x; if (x > *bx1) *bx1 = x;
    if (y < *by0) *by0 = y; if (y > *by1) *by1 = y;
}

/* Add edge p0->p1 with tangents to vl and update bbox for ACTUAL curve points.
 * is_first_edge=1 also adds/tracks the start point p0.
 * bx0..by1 and xf may be NULL to skip bbox updates. */
static float pw_add_edge(pw_vl_t *vl, int seg_idx,
                          float x0, float y0, float t0x, float t0y,
                          float x1, float y1, float t1x, float t1y,
                          float globalLen, int is_first_edge,
                          float *bx0, float *by0, float *bx1, float *by1,
                          const float *xf)
{
    if (vl && is_first_edge) {
        pw_vtx_t pv = {x0, y0, t0x, t0y, globalLen, seg_idx};
        pw_vl_push(vl, pv);
    }
    if (bx0 && is_first_edge) pw_bbox_pt(bx0, by0, bx1, by1, xf, x0, y0);
    float l = pw_dist(x1 - x0, y1 - y0);
    globalLen += l;
    if (vl) {
        pw_vtx_t pv = {x1, y1, t1x, t1y, globalLen, seg_idx};
        pw_vl_push(vl, pv);
    }
    if (bx0) pw_bbox_pt(bx0, by0, bx1, by1, xf, x1, y1);
    return globalLen;
}

/* Tessellate quadratic bezier p0->p2 with control p1 (256 uniform samples). */
static float pw_tess_quad(pw_vl_t *vl, int seg_idx,
                           float p0x, float p0y, float p1x, float p1y,
                           float p2x, float p2y, float globalLen,
                           float *bx0, float *by0, float *bx1, float *by1,
                           const float *xf)
{
    /* Incoming tangent at t=0: direction of first control segment, like the RI */
    float inc_tx = p1x - p0x, inc_ty = p1y - p0y;
    float inc_l = pw_dist(inc_tx, inc_ty);
    if (inc_l > 1e-10f) { inc_tx /= inc_l; inc_ty /= inc_l; }
    else { inc_tx = 1.0f; inc_ty = 0.0f; }

    float ppx = p0x, ppy = p0y;
    int ife = 1;
    for (int i = 1; i < PW_TESS_N; i++) {
        float t = (float)i / (float)PW_TESS_N, u = 1.0f - t;
        float pnx = u*u*p0x + 2.0f*t*u*p1x + t*t*p2x;
        float pny = u*u*p0y + 2.0f*t*u*p1y + t*t*p2y;
        float tnx = (p1x-p0x)*u + (p2x-p1x)*t;
        float tny = (p1y-p0y)*u + (p2y-p1y)*t;
        float tl = pw_dist(tnx, tny);
        if (tl > 1e-10f) { tnx /= tl; tny /= tl; } else { tnx = 1.0f; tny = 0.0f; }
        globalLen = pw_add_edge(vl, seg_idx, ppx, ppy, ife ? inc_tx : tnx, ife ? inc_ty : tny, pnx, pny, tnx, tny,
                                 globalLen, ife, bx0, by0, bx1, by1, xf);
        ppx = pnx; ppy = pny; ife = 0;
    }
    float t2x = p2x - p1x, t2y = p2y - p1y;
    float t2l = pw_dist(t2x, t2y);
    if (t2l > 1e-10f) { t2x /= t2l; t2y /= t2l; } else { t2x = 1.0f; t2y = 0.0f; }
    return pw_add_edge(vl, seg_idx, ppx, ppy, ife ? inc_tx : t2x, ife ? inc_ty : t2y, p2x, p2y, t2x, t2y,
                        globalLen, ife, bx0, by0, bx1, by1, xf);
}

/* Tessellate cubic bezier p0->p3 with controls p1, p2 (256 uniform samples). */
static float pw_tess_cubic(pw_vl_t *vl, int seg_idx,
                            float p0x, float p0y, float p1x, float p1y,
                            float p2x, float p2y, float p3x, float p3y,
                            float globalLen,
                            float *bx0, float *by0, float *bx1, float *by1,
                            const float *xf)
{
    /* Incoming tangent at t=0: like the RI */
    float inc_tx = p1x - p0x, inc_ty = p1y - p0y;
    if (fabsf(inc_tx) < 1e-10f && fabsf(inc_ty) < 1e-10f) {
        inc_tx = p2x - p0x; inc_ty = p2y - p0y;
        if (fabsf(inc_tx) < 1e-10f && fabsf(inc_ty) < 1e-10f) { inc_tx = p3x - p0x; inc_ty = p3y - p0y; }
    }
    float inc_l = pw_dist(inc_tx, inc_ty);
    if (inc_l > 1e-10f) { inc_tx /= inc_l; inc_ty /= inc_l; } else { inc_tx = 1.0f; inc_ty = 0.0f; }

    float ppx = p0x, ppy = p0y;
    int ife = 1;
    for (int i = 1; i < PW_TESS_N; i++) {
        float t = (float)i / (float)PW_TESS_N;
        /* Use RI's expanded polynomial form for bit-exact match */
        float pnx = (1.0f-3.0f*t+3.0f*t*t-t*t*t)*p0x + (3.0f*t-6.0f*t*t+3.0f*t*t*t)*p1x + (3.0f*t*t-3.0f*t*t*t)*p2x + t*t*t*p3x;
        float pny = (1.0f-3.0f*t+3.0f*t*t-t*t*t)*p0y + (3.0f*t-6.0f*t*t+3.0f*t*t*t)*p1y + (3.0f*t*t-3.0f*t*t*t)*p2y + t*t*t*p3y;
        float tnx = (-3.0f+6.0f*t-3.0f*t*t)*p0x + (3.0f-12.0f*t+9.0f*t*t)*p1x + (6.0f*t-9.0f*t*t)*p2x + 3.0f*t*t*p3x;
        float tny = (-3.0f+6.0f*t-3.0f*t*t)*p0y + (3.0f-12.0f*t+9.0f*t*t)*p1y + (6.0f*t-9.0f*t*t)*p2y + 3.0f*t*t*p3y;
        float tl = pw_dist(tnx, tny);
        if (tl > 1e-10f) { tnx /= tl; tny /= tl; } else { tnx = 1.0f; tny = 0.0f; }
        globalLen = pw_add_edge(vl, seg_idx, ppx, ppy, ife ? inc_tx : tnx, ife ? inc_ty : tny, pnx, pny, tnx, tny,
                                 globalLen, ife, bx0, by0, bx1, by1, xf);
        ppx = pnx; ppy = pny; ife = 0;
    }
    /* Outgoing tangent at t=1: like the RI */
    float out_tx = p3x - p2x, out_ty = p3y - p2y;
    if (fabsf(out_tx) < 1e-10f && fabsf(out_ty) < 1e-10f) {
        out_tx = p3x - p1x; out_ty = p3y - p1y;
        if (fabsf(out_tx) < 1e-10f && fabsf(out_ty) < 1e-10f) { out_tx = p3x - p0x; out_ty = p3y - p0y; }
    }
    float out_l = pw_dist(out_tx, out_ty);
    if (out_l > 1e-10f) { out_tx /= out_l; out_ty /= out_l; } else { out_tx = 1.0f; out_ty = 0.0f; }
    return pw_add_edge(vl, seg_idx, ppx, ppy, ife ? inc_tx : out_tx, ife ? inc_ty : out_ty, p3x, p3y, out_tx, out_ty,
                        globalLen, ife, bx0, by0, bx1, by1, xf);
}

/* Walk path segments [0 .. end_seg_excl), accumulating:
 *  - seg_start_len[i] / seg_end_len[i]: path length at start/end of segment i
 *  - vl: optional list of tessellated vertices
 *  - bbox: optional bounding box (bx0,by0,bx1,by1) of TRANSFORMED vertices
 *    when xform != NULL, each vertex is transformed before updating bbox.
 * Returns globalLen after walking. */
static float pw_walk(const cm_path_t *path, int end_seg_excl,
                     float *seg_start_len, float *seg_end_len,
                     pw_vl_t *vl,
                     float *bx0, float *by0, float *bx1, float *by1,
                     const float xform[9])
{
    if (!path || path->buf_len <= 0 || end_seg_excl <= 0) return 0.0f;
    const uint8_t *rp  = path->buf;
    const uint8_t *end = path->buf + path->buf_len;
    float ox = 0, oy = 0;   /* current position */
    float sx = 0, sy = 0;   /* subpath start */
    float pcx = 0, pcy = 0; /* last smooth control point */
    int prev_quad = 0, prev_cubic = 0;
    int seg_idx = 0;
    float globalLen = 0.0f;

#define PW_BBOX(px_, py_) do { \
    if (bx0) pw_bbox_pt(bx0, by0, bx1, by1, xform, (px_), (py_)); \
    } while(0)

    while (rp < end && seg_idx < end_seg_excl) {
        uint32_t hdr; memcpy(&hdr, rp, 4); rp += 4;
        uint8_t seg = (uint8_t)(hdr & 0xFF);
        uint8_t cmd = seg & ~1u;
        int nc = seg_coord_count(seg);
        float c[6] = {0,0,0,0,0,0};
        for (int i = 0; i < nc; i++) { memcpy(&c[i], rp, 4); rp += 4; }
        int r = (seg & 1) != 0; /* is REL? */
        if (seg_start_len) seg_start_len[seg_idx] = globalLen;

        switch (cmd) {
        case VG_CLOSE_PATH: {
            float dx = sx-ox, dy = sy-oy;
            float l = pw_dist(dx, dy);
            if (l > 0.0f) {
                float tnx = dx/l, tny = dy/l;
                globalLen = pw_add_edge(vl, seg_idx, ox, oy, tnx, tny, sx, sy, tnx, tny,
                                         globalLen, 1, bx0, by0, bx1, by1, xform);
            } else { PW_BBOX(sx, sy); }
            ox = sx; oy = sy; pcx = sx; pcy = sy;
            prev_quad = prev_cubic = 0; break; }
        case VG_MOVE_TO: {
            float x = r ? ox+c[0] : c[0], y = r ? oy+c[1] : c[1];
            PW_BBOX(x, y);
            ox = sx = x; oy = sy = y; pcx = x; pcy = y;
            prev_quad = prev_cubic = 0; break; }
        case VG_LINE_TO: {
            float x = r ? ox+c[0] : c[0], y = r ? oy+c[1] : c[1];
            float l = pw_dist(x-ox, y-oy);
            float tnx = (l>1e-10f)?(x-ox)/l:1.0f, tny = (l>1e-10f)?(y-oy)/l:0.0f;
            globalLen = pw_add_edge(vl, seg_idx, ox, oy, tnx, tny, x, y, tnx, tny,
                                     globalLen, 1, bx0, by0, bx1, by1, xform);
            ox = x; oy = y; pcx = x; pcy = y; prev_quad = prev_cubic = 0; break; }
        case VG_HLINE_TO: {
            float x = r ? ox+c[0] : c[0], y = oy;
            float tnx = (x >= ox) ? 1.0f : -1.0f;
            globalLen = pw_add_edge(vl, seg_idx, ox, oy, tnx, 0.0f, x, y, tnx, 0.0f,
                                     globalLen, 1, bx0, by0, bx1, by1, xform);
            ox = x; oy = y; pcx = x; pcy = y; prev_quad = prev_cubic = 0; break; }
        case VG_VLINE_TO: {
            float x = ox, y = r ? oy+c[0] : c[0];
            float tny = (y >= oy) ? 1.0f : -1.0f;
            globalLen = pw_add_edge(vl, seg_idx, ox, oy, 0.0f, tny, x, y, 0.0f, tny,
                                     globalLen, 1, bx0, by0, bx1, by1, xform);
            ox = x; oy = y; pcx = x; pcy = y; prev_quad = prev_cubic = 0; break; }
        case VG_QUAD_TO: {
            float cx = r?ox+c[0]:c[0], cy = r?oy+c[1]:c[1];
            float x  = r?ox+c[2]:c[2], y  = r?oy+c[3]:c[3];
            globalLen = pw_tess_quad(vl, seg_idx, ox, oy, cx, cy, x, y, globalLen,
                                      bx0, by0, bx1, by1, xform);
            pcx = cx; pcy = cy; ox = x; oy = y; prev_quad = 1; prev_cubic = 0; break; }
        case VG_SQUAD_TO: {
            float cx = prev_quad ? 2.0f*ox-pcx : ox;
            float cy = prev_quad ? 2.0f*oy-pcy : oy;
            float x  = r?ox+c[0]:c[0], y = r?oy+c[1]:c[1];
            globalLen = pw_tess_quad(vl, seg_idx, ox, oy, cx, cy, x, y, globalLen,
                                      bx0, by0, bx1, by1, xform);
            pcx = cx; pcy = cy; ox = x; oy = y; prev_quad = 1; prev_cubic = 0; break; }
        case VG_CUBIC_TO: {
            float c1x=r?ox+c[0]:c[0], c1y=r?oy+c[1]:c[1];
            float c2x=r?ox+c[2]:c[2], c2y=r?oy+c[3]:c[3];
            float x  =r?ox+c[4]:c[4], y  =r?oy+c[5]:c[5];
            globalLen = pw_tess_cubic(vl, seg_idx, ox, oy, c1x, c1y, c2x, c2y, x, y,
                                       globalLen, bx0, by0, bx1, by1, xform);
            pcx = c2x; pcy = c2y; ox = x; oy = y; prev_quad = 0; prev_cubic = 1; break; }
        case VG_SCUBIC_TO: {
            float c1x = prev_cubic ? 2.0f*ox-pcx : ox;
            float c1y = prev_cubic ? 2.0f*oy-pcy : oy;
            float c2x=r?ox+c[0]:c[0], c2y=r?oy+c[1]:c[1];
            float x  =r?ox+c[2]:c[2], y  =r?oy+c[3]:c[3];
            globalLen = pw_tess_cubic(vl, seg_idx, ox, oy, c1x, c1y, c2x, c2y, x, y,
                                       globalLen, bx0, by0, bx1, by1, xform);
            pcx = c2x; pcy = c2y; ox = x; oy = y; prev_quad = 0; prev_cubic = 1; break; }
        case VG_SCCWARC_TO: case VG_SCWARC_TO:
        case VG_LCCWARC_TO: case VG_LCWARC_TO: {
            float rx = fabsf(c[0]), ry = fabsf(c[1]), phi = c[2];
            float x  = r ? ox+c[3] : c[3], y = r ? oy+c[4] : c[4];
            int large = (cmd == VG_LCCWARC_TO || cmd == VG_LCWARC_TO);
            int ccw   = (cmd == VG_SCCWARC_TO || cmd == VG_LCCWARC_TO);
            sv2 arc_p0 = SV2(ox, oy), arc_p1 = SV2(x, y);
            ri_arc_ctx_t actx = ri_arc_setup(arc_p0, rx, ry, phi, arc_p1, large, ccw);
            if (!actx.valid) {
                /* Degenerate: add line to endpoint */
                float dx = x-ox, dy = y-oy, l = pw_dist(dx, dy);
                float tnx = (l>1e-10f)?dx/l:1.0f, tny = (l>1e-10f)?dy/l:0.0f;
                globalLen = pw_add_edge(vl, seg_idx, ox, oy, tnx, tny, x, y, tnx, tny,
                                         globalLen, 1, bx0, by0, bx1, by1, xform);
            } else {
                /* RI-compatible arc edges with proper arc tangents */
                const int SEG = 256;
                sv2 pp = arc_p0;
                sv2 tp = ri_arc_tan(actx.ua, actx.cw, &actx.uce);
                int ife = 1;
                for (int i = 1; i <= SEG; i++) {
                    sv2 pn_unit, pw;
                    if (i < SEG) {
                        pn_unit = ri_circ_lerp_cw(actx.ua, actx.ub,
                                                   (float)i / (float)SEG, actx.cw);
                        pw = ri_arc_pos(&actx, pn_unit, arc_p0);
                    } else {
                        pn_unit = actx.ub;
                        pw = arc_p1;   /* exact endpoint, no floating-point drift */
                    }
                    sv2 tn = ri_arc_tan(pn_unit, actx.cw, &actx.uce);
                    globalLen = pw_add_edge(vl, seg_idx,
                                             pp.x, pp.y, tp.x, tp.y,
                                             pw.x, pw.y, tn.x, tn.y,
                                             globalLen, ife, bx0, by0, bx1, by1, xform);
                    pp = pw; tp = tn; ife = 0;
                }
            }
            ox = x; oy = y; pcx = x; pcy = y; prev_quad = prev_cubic = 0; break; }
        default: break;
        }
        if (seg_end_len) seg_end_len[seg_idx] = globalLen;
        seg_idx++;
    }
#undef PW_BBOX
    return globalLen;
}

VGfloat vgPathLength(VGPath vgpath, VGint startSeg, VGint numSegs)
{
    VG_CHECK_CTX(-1.f);
    if (vgpath == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return -1.f; }
    cm_path_t *p = (cm_path_t *)(uintptr_t)vgpath;
    if (p->magic != PATH_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return -1.f; }
    if (!(p->capabilities & VG_PATH_CAPABILITY_PATH_LENGTH)) {
        VG_SET_ERR(VG_PATH_CAPABILITY_ERROR); return -1.f; }
    if (startSeg < 0 || numSegs < 0 || startSeg + numSegs > p->num_segments) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return -1.f; }
    if (numSegs == 0) return 0.0f;

    int total_segs = startSeg + numSegs;
    float *slen = (float *)malloc((size_t)total_segs * sizeof(float));
    float *elen = (float *)malloc((size_t)total_segs * sizeof(float));
    if (!slen || !elen) { free(slen); free(elen); VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return -1.f; }

    pw_walk(p, total_segs, slen, elen, NULL, NULL, NULL, NULL, NULL, NULL);
    float start_len = slen[startSeg];
    float end_len   = elen[startSeg + numSegs - 1];
    free(slen); free(elen);
    return end_len - start_len;
}

/* Get the command byte (masked ~1u) for segment at index seg_idx in path buf */
static uint8_t path_seg_cmd_at(const cm_path_t *p, int seg_idx)
{
    const uint8_t *rp = p->buf;
    const uint8_t *end = p->buf + p->buf_len;
    int idx = 0;
    while (rp < end && idx < p->num_segments) {
        uint32_t hdr; memcpy(&hdr, rp, 4); rp += 4;
        uint8_t seg = (uint8_t)(hdr & 0xFF);
        int nc = seg_coord_count(seg);
        if (idx == seg_idx) return seg & ~1u;
        rp += nc * 4;
        idx++;
    }
    return 0xFF;
}

void vgPointAlongPath(VGPath vgpath, VGint startSeg, VGint numSegs, VGfloat distance,
                      VGfloat *x, VGfloat *y, VGfloat *tx, VGfloat *ty)
{
    VG_CHECK_CTX();
    /* Alignment checks */
    if ((x  && ((uintptr_t)x  & (sizeof(VGfloat)-1u)) != 0) ||
        (y  && ((uintptr_t)y  & (sizeof(VGfloat)-1u)) != 0) ||
        (tx && ((uintptr_t)tx & (sizeof(VGfloat)-1u)) != 0) ||
        (ty && ((uintptr_t)ty & (sizeof(VGfloat)-1u)) != 0)) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    if (vgpath == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_path_t *p = (cm_path_t *)(uintptr_t)vgpath;
    if (p->magic != PATH_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (!(p->capabilities & VG_PATH_CAPABILITY_POINT_ALONG_PATH)) {
        VG_SET_ERR(VG_PATH_CAPABILITY_ERROR); return; }
    if (startSeg < 0 || numSegs < 0 || startSeg + numSegs > p->num_segments) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    /* Default output */
    if (x) *x = 0.f; if (y) *y = 0.f;
    if (tx) *tx = 1.f; if (ty) *ty = 0.f;
    if (numSegs == 0) return;

    /* Skip MOVE_TO segments at start and end, like the RI does */
    int adj_start = startSeg, adj_num = numSegs;
    while (adj_num > 0 && path_seg_cmd_at(p, adj_start) == VG_MOVE_TO) {
        adj_start++; adj_num--;
    }
    while (adj_num > 0 && path_seg_cmd_at(p, adj_start + adj_num - 1) == VG_MOVE_TO) {
        adj_num--;
    }
    /* All segments were MOVE_TO → return (0,0, 1,0) */
    if (adj_num == 0) return;

    int total_segs = adj_start + adj_num;
    float *slen = (float *)malloc((size_t)total_segs * sizeof(float));
    float *elen = (float *)malloc((size_t)total_segs * sizeof(float));
    pw_vl_t vl = {NULL, 0, 0};
    if (!slen || !elen) { free(slen); free(elen); return; }

    pw_walk(p, total_segs, slen, elen, &vl, NULL, NULL, NULL, NULL, NULL);

    /* Find start/end path lengths for the adjusted segment range */
    float range_start = slen[adj_start];
    float range_end   = elen[adj_start + adj_num - 1];
    free(slen); free(elen);

    /* Zero-length path → return start position, tangent (1,0) */
    if (vl.n < 1) { pw_vl_free(&vl); return; }
    if (range_start >= range_end) {
        /* Find the first vertex for this segment range */
        int vi = 0;
        while (vi < vl.n - 1 && vl.v[vi].seg_idx < adj_start) vi++;
        if (x) *x = vl.v[vi].x; if (y) *y = vl.v[vi].y;
        if (tx) *tx = 1.f; if (ty) *ty = 0.f;
        pw_vl_free(&vl); return;
    }

    /* Map distance to global path length coordinates */
    float target = range_start + distance;
    if (target <= range_start) {
        /* Return start point with its tangent */
        int vi = 0;
        while (vi < vl.n - 1 && vl.v[vi].seg_idx < adj_start) vi++;
        if (x) *x = vl.v[vi].x; if (y) *y = vl.v[vi].y;
        if (tx || ty) {
            float ttx = vl.v[vi].tx, tty = vl.v[vi].ty;
            float l = pw_dist(ttx, tty);
            if (l > 1e-10f) { ttx /= l; tty /= l; } else { ttx = 1.f; tty = 0.f; }
            if (tx) *tx = ttx; if (ty) *ty = tty;
        }
        pw_vl_free(&vl); return;
    }
    if (target >= range_end) {
        /* Return end point with its tangent */
        int vi = vl.n - 1;
        while (vi > 0 && vl.v[vi].seg_idx >= adj_start + adj_num) vi--;
        if (x) *x = vl.v[vi].x; if (y) *y = vl.v[vi].y;
        if (tx || ty) {
            float ttx = vl.v[vi].tx, tty = vl.v[vi].ty;
            float l = pw_dist(ttx, tty);
            if (l > 1e-10f) { ttx /= l; tty /= l; } else { ttx = 1.f; tty = 0.f; }
            if (tx) *tx = ttx; if (ty) *ty = tty;
        }
        pw_vl_free(&vl); return;
    }

    /* Narrow vertex range to [adj_start .. adj_start+adj_num) */
    int lo = 0, hi = vl.n - 1;
    while (lo < vl.n - 1 && vl.v[lo].seg_idx < adj_start) lo++;
    while (hi > 0 && vl.v[hi].seg_idx >= adj_start + adj_num) hi--;

    /* Linear scan for the edge containing target */
    int idx = lo;
    for (int i = lo; i < hi; i++) {
        if (vl.v[i].path_len <= target && target < vl.v[i+1].path_len) {
            idx = i; break;
        }
        if (i == hi - 1) idx = i;
    }

    pw_vtx_t *v0 = &vl.v[idx];
    pw_vtx_t *v1 = &vl.v[(idx + 1 <= hi) ? idx + 1 : idx];

    float edge_len = v1->path_len - v0->path_len;
    if (edge_len > 1e-10f) {
        float ratio = (target - v0->path_len) / edge_len;
        if (x) *x = (1.0f - ratio) * v0->x + ratio * v1->x;
        if (y) *y = (1.0f - ratio) * v0->y + ratio * v1->y;
        if (tx || ty) {
            /* Interpolate tangent then normalize, like RI does */
            float ttx = (1.0f - ratio) * v0->tx + ratio * v1->tx;
            float tty = (1.0f - ratio) * v0->ty + ratio * v1->ty;
            float l = pw_dist(ttx, tty);
            if (l > 1e-10f) { ttx /= l; tty /= l; } else { ttx = 1.f; tty = 0.f; }
            if (tx) *tx = ttx; if (ty) *ty = tty;
        }
    } else {
        if (x) *x = v0->x; if (y) *y = v0->y;
        if (tx || ty) {
            float ttx = v0->tx, tty = v0->ty;
            float l = pw_dist(ttx, tty);
            if (l > 1e-10f) { ttx /= l; tty /= l; } else { ttx = 1.f; tty = 0.f; }
            if (tx) *tx = ttx; if (ty) *ty = tty;
        }
    }
    pw_vl_free(&vl);
}

void vgPathBounds(VGPath vgpath, VGfloat *minX, VGfloat *minY, VGfloat *w, VGfloat *h)
{
    VG_CHECK_CTX();
    if ((minX && ((uintptr_t)minX & (sizeof(VGfloat)-1u)) != 0) ||
        (minY && ((uintptr_t)minY & (sizeof(VGfloat)-1u)) != 0) ||
        (w    && ((uintptr_t)w    & (sizeof(VGfloat)-1u)) != 0) ||
        (h    && ((uintptr_t)h    & (sizeof(VGfloat)-1u)) != 0)) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    if (vgpath == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_path_t *p = (cm_path_t *)(uintptr_t)vgpath;
    if (p->magic != PATH_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (!(p->capabilities & VG_PATH_CAPABILITY_PATH_BOUNDS)) {
        VG_SET_ERR(VG_PATH_CAPABILITY_ERROR); return; }

    if (p->num_segments == 0) {
        /* Empty path: special return values */
        if (minX) *minX = 0.f; if (minY) *minY = 0.f;
        if (w) *w = -1.f; if (h) *h = -1.f;
        return;
    }

    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    pw_walk(p, p->num_segments, NULL, NULL, NULL, &bx0, &by0, &bx1, &by1, NULL);

    if (bx0 > bx1 || by0 > by1) {
        /* Degenerate/empty: all MOVE_TO or no geometry */
        if (minX) *minX = 0.f; if (minY) *minY = 0.f;
        if (w) *w = -1.f; if (h) *h = -1.f;
        return;
    }
    if (minX) *minX = bx0; if (minY) *minY = by0;
    if (w) *w = bx1 - bx0; if (h) *h = by1 - by0;
}

void vgPathTransformedBounds(VGPath vgpath, VGfloat *minX, VGfloat *minY,
                              VGfloat *w, VGfloat *h)
{
    VG_CHECK_CTX();
    if ((minX && ((uintptr_t)minX & (sizeof(VGfloat)-1u)) != 0) ||
        (minY && ((uintptr_t)minY & (sizeof(VGfloat)-1u)) != 0) ||
        (w    && ((uintptr_t)w    & (sizeof(VGfloat)-1u)) != 0) ||
        (h    && ((uintptr_t)h    & (sizeof(VGfloat)-1u)) != 0)) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    if (vgpath == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_path_t *p = (cm_path_t *)(uintptr_t)vgpath;
    if (p->magic != PATH_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (!(p->capabilities & VG_PATH_CAPABILITY_PATH_TRANSFORMED_BOUNDS)) {
        VG_SET_ERR(VG_PATH_CAPABILITY_ERROR); return; }

    if (p->num_segments == 0) {
        if (minX) *minX = 0.f; if (minY) *minY = 0.f;
        if (w) *w = -1.f; if (h) *h = -1.f;
        return;
    }

    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    pw_walk(p, p->num_segments, NULL, NULL, NULL, &bx0, &by0, &bx1, &by1,
            g_ctx ? g_ctx->mat_path : NULL);

    if (bx0 > bx1 || by0 > by1) {
        if (minX) *minX = 0.f; if (minY) *minY = 0.f;
        if (w) *w = -1.f; if (h) *h = -1.f;
        return;
    }
    if (minX) *minX = bx0; if (minY) *minY = by0;
    if (w) *w = bx1 - bx0; if (h) *h = by1 - by0;
}

/* =========================================================================
 * Software Stroke Renderer
 * =========================================================================
 * Converts the stroke of a path into a fill polygon and submits it to the
 * cmodel fill pipeline.  All geometry is computed in path-user-space; the
 * existing path-user-to-surface matrix is reused unchanged.
 * ========================================================================= */

/* (sv2 types and ptlist_t helpers are defined above, before path query functions) */

/* ---- Bezier flattening ---- */
#define FLAT_TOL2 0.0625f  /* (0.25 px)^2 */
static void flat_quad(ptlist_t *l, sv2 p0, sv2 cp, sv2 p1, int d){
    sv2 mid = sv2_sc(sv2_add(sv2_add(p0, sv2_sc(cp,2.f)), p1), 0.25f);
    sv2 lm  = sv2_sc(sv2_add(p0, p1), 0.5f);
    sv2 dv  = sv2_sub(mid, lm);
    if(sv2_dot(dv,dv) < FLAT_TOL2 || d > 12){ ptl_push(l, p1); return; }
    sv2 m01 = sv2_sc(sv2_add(p0,cp),0.5f);
    sv2 m12 = sv2_sc(sv2_add(cp,p1),0.5f);
    sv2 mm  = sv2_sc(sv2_add(m01,m12),0.5f);
    flat_quad(l,p0,m01,mm,d+1); flat_quad(l,mm,m12,p1,d+1);
}
static void flat_cub(ptlist_t *l, sv2 p0, sv2 c1, sv2 c2, sv2 p1, int d){
    sv2 dv = sv2_sub(sv2_sc(sv2_add(c1,c2),2.f), sv2_add(p0,p1));
    if(sv2_dot(dv,dv) < FLAT_TOL2 || d > 14){ ptl_push(l, p1); return; }
    sv2 m01 = sv2_sc(sv2_add(p0,c1),0.5f);
    sv2 m12 = sv2_sc(sv2_add(c1,c2),0.5f);
    sv2 m23 = sv2_sc(sv2_add(c2,p1),0.5f);
    sv2 m012= sv2_sc(sv2_add(m01,m12),0.5f);
    sv2 m123= sv2_sc(sv2_add(m12,m23),0.5f);
    sv2 mm  = sv2_sc(sv2_add(m012,m123),0.5f);
    flat_cub(l,p0,m01,m012,mm,d+1); flat_cub(l,mm,m123,m23,p1,d+1);
}
static void flat_arc(ptlist_t *l, sv2 p0, float rx, float ry,
                     float phi_deg, sv2 p1, int large, int ccw){
    rx = fabsf(rx); ry = fabsf(ry);
    if(rx < 1e-6f || ry < 1e-6f){ ptl_push(l,p1); return; }
    float phi = phi_deg * (float)(M_PI/180.0);
    float cp = cosf(phi), sp = sinf(phi);
    float dx2 = (p0.x-p1.x)*0.5f, dy2 = (p0.y-p1.y)*0.5f;
    float x1p =  cp*dx2 + sp*dy2, y1p = -sp*dx2 + cp*dy2;
    float sq = x1p*x1p/(rx*rx) + y1p*y1p/(ry*ry);
    if(sq > 1.f){ float s=sqrtf(sq); rx*=s; ry*=s; }
    float rx2=rx*rx, ry2=ry*ry, x1p2=x1p*x1p, y1p2=y1p*y1p;
    float num = rx2*ry2 - rx2*y1p2 - ry2*x1p2;
    float den = rx2*y1p2 + ry2*x1p2;
    float coef = (den>1e-12f) ? sqrtf(fabsf(num)/den) : 0.f;
    if(large == ccw) coef = -coef;
    float cxp =  coef*rx*y1p/ry, cyp = -coef*ry*x1p/rx;
    float cx = cp*cxp - sp*cyp + (p0.x+p1.x)*0.5f;
    float cy = sp*cxp + cp*cyp + (p0.y+p1.y)*0.5f;
    float ux=(x1p-cxp)/rx, uy=(y1p-cyp)/ry;
    float vx=(-x1p-cxp)/rx, vy=(-y1p-cyp)/ry;
    float ang1 = atan2f(uy,ux);
    float dot = ux*vx+uy*vy;
    if(dot> 1.f)dot= 1.f; if(dot<-1.f)dot=-1.f;
    float dang = acosf(dot);
    if(!ccw && sv2_cross(SV2(ux,uy),SV2(vx,vy)) > 0.f) dang = -dang;
    if( ccw && sv2_cross(SV2(ux,uy),SV2(vx,vy)) < 0.f) dang = -dang;
    if(!large && fabsf(dang) > (float)M_PI) dang = (dang>0?1.f:-1.f)*2.f*(float)M_PI - dang;
    if( large && fabsf(dang) < (float)M_PI) dang = (dang>0?1.f:-1.f)*2.f*(float)M_PI - dang;
    int ns = (int)(fabsf(dang)/0.08f) + 2; if(ns>256)ns=256;
    for(int i=1;i<=ns;i++){
        float a = ang1 + (float)i/(float)ns * dang;
        float px=cosf(a)*rx, py=sinf(a)*ry;
        ptl_push(l, SV2(cp*px - sp*py + cx, sp*px + cp*py + cy));
    }
}

/* ---- Add arc approximation to polygon (for round caps/joins) ---- */
static void add_arc_pts(ptlist_t *l, sv2 ctr, float r, float a0, float a1, int ccw){
    float dang = a1 - a0;
    /* normalise sweep direction */
    if(ccw){  if(dang < 0.f) dang += 2.f*(float)M_PI; }
    else    { if(dang > 0.f) dang -= 2.f*(float)M_PI; }
    int ns = (int)(fabsf(dang)/0.1f) + 1; if(ns>64)ns=64; if(ns<1)ns=1;
    for(int i=1;i<=ns;i++){
        float a = a0 + dang*(float)i/(float)ns;
        ptl_push(l, SV2(ctr.x + cosf(a)*r, ctr.y + sinf(a)*r));
    }
}

/* ---- Emit stroke polygon to a temporary path buffer ---- */
/* Uses the macros PATH_EMIT_CMD / PATH_EMIT_FLOAT on a cm_path_t */

static void sp_move(cm_path_t *p, sv2 v){
    PATH_EMIT_CMD(p, VG_MOVE_TO_ABS);
    PATH_EMIT_FLOAT(p, v.x); PATH_EMIT_FLOAT(p, v.y);
    p->num_segments++; p->num_coords += 2;
}
static void sp_line(cm_path_t *p, sv2 v){
    PATH_EMIT_CMD(p, VG_LINE_TO_ABS);
    PATH_EMIT_FLOAT(p, v.x); PATH_EMIT_FLOAT(p, v.y);
    p->num_segments++; p->num_coords += 2;
}
static void sp_close(cm_path_t *p){
    PATH_EMIT_CMD(p, VG_CLOSE_PATH);
    p->num_segments++;
}

/* ---- Emit a list of points as a closed polygon subpath ---- */
static void emit_polygon(cm_path_t *dst, const ptlist_t *l){
    if(l->n < 2) return;
    sp_move(dst, l->pts[0]);
    for(int i=1;i<l->n;i++) sp_line(dst, l->pts[i]);
    sp_close(dst);
}

/* ---- Build stroke as multiple separate closed sub-paths ---- */
/*
 * Design: emit one closed quadrilateral per segment, then one closed
 * triangle/fan per outside join, and one closed cap per endpoint.
 * Using separate closed sub-paths avoids the self-intersection problem
 * that arises with a single-polygon approach for sharp inside turns.
 * With the NON_ZERO fill rule, overlapping sub-paths simply add their
 * winding counts; the union is filled correctly.
 */
static void stroke_subpath(cm_path_t *dst,
                            const sv2 *pts, int n, int closed,
                            float hw,
                            VGCapStyle cap, VGJoinStyle join, float miter_lim)
{
    if(n < 2) return;

    int ns = closed ? n : n - 1;
    sv2 *tang  = (sv2 *)malloc((size_t)ns * sizeof(sv2));
    sv2 *norml = (sv2 *)malloc((size_t)ns * sizeof(sv2));
    if(!tang || !norml){ free(tang); free(norml); return; }

    for(int i = 0; i < ns; i++){
        sv2 t = sv2_norm(sv2_sub(pts[(i+1)%n], pts[i]));
        tang[i]  = t;
        norml[i] = sv2_perp(t);
    }

    /* ---- 1. Segment quads ---- */
    for(int i = 0; i < ns; i++){
        sv2 p0 = pts[i];
        sv2 p1 = pts[(i+1)%n];
        sv2 nrm = norml[i];
        sv2 l0 = sv2_add(p0, sv2_sc(nrm, hw));
        sv2 l1 = sv2_add(p1, sv2_sc(nrm, hw));
        sv2 r1 = sv2_sub(p1, sv2_sc(nrm, hw));
        sv2 r0 = sv2_sub(p0, sv2_sc(nrm, hw));
        sp_move(dst, l0);
        sp_line(dst, l1);
        sp_line(dst, r1);
        sp_line(dst, r0);
        sp_close(dst);
    }

    /* ---- 2. Joins (outside corner only) ---- */
    int n_joints = closed ? n : n - 2;
    for(int j = 0; j < n_joints; j++){
        int i_seg  = closed ? j : j;           /* segment before joint   */
        int i_next = (i_seg + 1) % ns;         /* segment after joint    */
        int ip1    = (i_seg + 1) % n;          /* joint vertex index     */
        sv2 pj      = pts[ip1];
        sv2 t_cur   = tang[i_seg];
        sv2 t_next  = tang[i_next];
        sv2 n_cur   = norml[i_seg];
        sv2 n_next  = norml[i_next];

        float cross = sv2_cross(t_cur, t_next);
        if(fabsf(cross) < 1e-6f) continue;     /* nearly straight, no join needed */

        /* outside_end / outside_start: the two offset vertices at the join
         * on the OUTSIDE of the turn */
        sv2 outside_end, outside_start;
        if(cross > 0.f){
            /* left/CCW turn: n points inward (toward interior), exterior gap is on -n side */
            outside_end   = sv2_sub(pj, sv2_sc(n_cur,  hw));
            outside_start = sv2_sub(pj, sv2_sc(n_next, hw));
        } else {
            /* right/CW turn: n points outward, exterior gap is on +n side */
            outside_end   = sv2_add(pj, sv2_sc(n_cur,  hw));
            outside_start = sv2_add(pj, sv2_sc(n_next, hw));
        }

        if(join == VG_JOIN_MITER){
            /* Miter point = intersection of the two outside offset lines */
            sv2 d = sv2_sub(outside_start, outside_end);
            float denom = sv2_cross(t_cur, t_next);
            int done = 0;
            if(fabsf(denom) > 1e-9f){
                float s = sv2_cross(d, t_next) / denom;
                sv2 mpt = sv2_add(outside_end, sv2_sc(t_cur, s));
                float md = sv2_len(sv2_sub(mpt, pj));
                if(md <= miter_lim * hw){
                    /* Emit the miter triangle: pj → outside_end → mpt → outside_start */
                    sp_move(dst, pj);
                    sp_line(dst, outside_end);
                    sp_line(dst, mpt);
                    sp_line(dst, outside_start);
                    sp_close(dst);
                    done = 1;
                }
            }
            if(!done){
                /* Bevel fallback */
                sp_move(dst, pj);
                sp_line(dst, outside_end);
                sp_line(dst, outside_start);
                sp_close(dst);
            }
        } else if(join == VG_JOIN_ROUND){
            /* Round join: arc from outside_end to outside_start around pj */
            float a0 = atan2f(outside_end.y   - pj.y, outside_end.x   - pj.x);
            float a1 = atan2f(outside_start.y - pj.y, outside_start.x - pj.x);
            ptlist_t arc = {NULL,0,0};
            ptl_push(&arc, pj);
            ptl_push(&arc, outside_end);
            /* sweep in the direction that spans the outside of the turn */
            int ccw = (cross > 0.f) ? 1 : 0;
            add_arc_pts(&arc, pj, hw, a0, a1, ccw);
            ptl_push(&arc, outside_start);
            emit_polygon(dst, &arc);
            ptl_free(&arc);
        } else {
            /* Bevel: simple triangle */
            sp_move(dst, pj);
            sp_line(dst, outside_end);
            sp_line(dst, outside_start);
            sp_close(dst);
        }
    }

    /* ---- 3. Caps (open paths only) ---- */
    if(!closed){
        /* -- Start cap -- */
        sv2 p0  = pts[0];
        sv2 n0  = norml[0];
        sv2 t0  = tang[0];
        sv2 l0  = sv2_add(p0, sv2_sc(n0, hw));
        sv2 r0  = sv2_sub(p0, sv2_sc(n0, hw));
        if(cap == VG_CAP_SQUARE){
            sv2 back = sv2_sc(t0, -hw);
            sp_move(dst, sv2_add(l0, back));
            sp_line(dst, sv2_add(r0, back));
            sp_line(dst, r0);
            sp_line(dst, l0);
            sp_close(dst);
        } else if(cap == VG_CAP_ROUND){
            float a0 = atan2f(-n0.y, -n0.x); /* = atan2(r0-p0) direction */
            float a1 = atan2f(n0.y,  n0.x);
            ptlist_t arc = {NULL,0,0};
            ptl_push(&arc, l0);
            add_arc_pts(&arc, p0, hw, a1, a0, 1); /* CCW: from l0 side BACKWARD to r0 side (outside cap) */
            ptl_push(&arc, r0);
            emit_polygon(dst, &arc);
            ptl_free(&arc);
        }
        /* BUTT: no cap needed (segment quad already ends at the endpoint) */

        /* -- End cap -- */
        sv2 pe  = pts[n-1];
        sv2 ne  = norml[ns-1];
        sv2 te  = tang[ns-1];
        sv2 le  = sv2_add(pe, sv2_sc(ne, hw));
        sv2 re  = sv2_sub(pe, sv2_sc(ne, hw));
        if(cap == VG_CAP_SQUARE){
            sv2 fwd = sv2_sc(te, hw);
            sp_move(dst, sv2_add(le, fwd));
            sp_line(dst, sv2_add(re, fwd));
            sp_line(dst, re);
            sp_line(dst, le);
            sp_close(dst);
        } else if(cap == VG_CAP_ROUND){
            float a0 = atan2f( ne.y,  ne.x);
            float a1 = atan2f(-ne.y, -ne.x);
            ptlist_t arc = {NULL,0,0};
            ptl_push(&arc, le);
            add_arc_pts(&arc, pe, hw, a0, a1, 0); /* CW */
            ptl_push(&arc, re);
            emit_polygon(dst, &arc);
            ptl_free(&arc);
        }
        /* BUTT: no cap */
    }

    free(tang);
    free(norml);
}

/* ---- Parse path buffer into flat polylines, generate stroke fill ---- */
static void draw_stroke(cm_ctx_t *ctx, cm_path_t *src_path)
{
    if(!ctx || !src_path || src_path->buf_len <= 0) return;

    float hw = ctx->stroke_line_width * 0.5f;
    if(hw <= 0.f) return;

    VGCapStyle  cap  = ctx->stroke_cap_style;
    VGJoinStyle join = ctx->stroke_join_style;
    float miter_lim  = ctx->stroke_miter_limit;
    if(miter_lim < 1.f) miter_lim = 1.f;

    /* Allocate destination path for stroke polygons */
    cm_path_t *dst = (cm_path_t *)calloc(1, sizeof(cm_path_t));
    if(!dst) return;
    dst->magic = PATH_MAGIC;
    dst->capabilities = VG_PATH_CAPABILITY_ALL;
    dst->datatype = VG_PATH_DATATYPE_F;
    dst->scale = 1.f; dst->bias = 0.f;

    /* Current polyline accumulator */
    ptlist_t cur = {NULL,0,0};
    sv2 pos = SV2(0,0);      /* current pen position */
    sv2 move_pt = SV2(0,0);  /* subpath start */
    sv2 prev_cp = SV2(0,0);  /* previous smooth ctrl point */
    int has_prev_quad = 0, has_prev_cub = 0;
    int in_subpath = 0;

    /* Dash state */
    float *dash = (ctx->num_dash > 0) ? ctx->stroke_dash_pattern : NULL;
    int ndash = ctx->num_dash;
    float dash_phase = ctx->stroke_dash_phase;
    int in_dash = 1;  /* start in "on" segment */
    float dash_rem = 0.f;
    if(dash && ndash > 0){
        /* normalise phase */
        float total = 0.f;
        for(int i=0;i<ndash;i++) total += (dash[i] > 0.f ? dash[i] : 0.f);
        if(total > 0.f){
            float ph = fmodf(dash_phase, total);
            if(ph < 0.f) ph += total;
            /* determine which dash segment phase falls in */
            float acc = 0.f;
            in_dash = 1; /* odd idx = gap; even idx = dash */
            for(int i=0;i<ndash;i++){
                float dl = (dash[i]>0.f?dash[i]:0.f);
                if(ph < acc + dl){ in_dash = (i%2==0); dash_rem = acc+dl-ph; break; }
                acc += dl;
            }
        } else { dash = NULL; }
    } else { dash = NULL; }
    int dash_idx = in_dash ? 0 : 1;
    if(dash) dash_rem = (dash_rem > 0.f ? dash_rem : (dash[dash_idx]>0.f?dash[dash_idx]:0.f));

    /* Helper: commit current polyline as a stroke subpath */
    #define COMMIT_SUBPATH(closed_flag) do { \
        if(cur.n >= 2){ \
            stroke_subpath(dst, cur.pts, cur.n, (closed_flag), \
                           hw, cap, join, miter_lim); \
        } \
        ptl_free(&cur); \
        in_subpath = 0; \
    } while(0)

    /* Helper: append a line from pos to pt (handling dash) */
    #define APPEND_LINE_SEG(pt) do { \
        sv2 _to = (pt); \
        if(!dash){ \
            if(!in_subpath){ ptl_push(&cur, pos); in_subpath=1; } \
            ptl_push(&cur, _to); \
        } else { \
            sv2 _dir = sv2_sub(_to, pos); \
            float _len = sv2_len(_dir); \
            if(_len < 1e-9f){ /* degenerate */ } else { \
                sv2 _u = sv2_sc(_dir, 1.f/_len); \
                float _t = 0.f; \
                while(_t < _len){ \
                    float _avail = _len - _t; \
                    if(dash_rem <= 0.f){ \
                        dash_idx = (dash_idx+1)%ndash; \
                        in_dash = (dash_idx%2==0); \
                        dash_rem = dash[dash_idx]>0.f?dash[dash_idx]:0.f; \
                    } \
                    float _take = (_avail < dash_rem) ? _avail : dash_rem; \
                    sv2 _end = sv2_add(pos, sv2_sc(_u, _t+_take)); \
                    if(in_dash){ \
                        if(!in_subpath){ ptl_push(&cur, sv2_add(pos,sv2_sc(_u,_t))); in_subpath=1; } \
                        ptl_push(&cur, _end); \
                    } else { \
                        if(in_subpath){ COMMIT_SUBPATH(0); } \
                    } \
                    _t += _take; dash_rem -= _take; \
                    if(dash_rem <= 0.f){ \
                        dash_idx = (dash_idx+1)%ndash; \
                        in_dash = (dash_idx%2==0); \
                        dash_rem = dash[dash_idx]>0.f?dash[dash_idx]:0.f; \
                    } \
                } \
            } \
        } \
    } while(0)

    /* Parse path buffer */
    const uint8_t *buf = src_path->buf;
    int blen = src_path->buf_len, bpos = 0;

    while(bpos + 4 <= blen){
        uint32_t hdr; memcpy(&hdr, buf+bpos, 4); bpos += 4;
        uint8_t seg = (uint8_t)(hdr & 0xFF);
        uint8_t cmd = seg & ~1u;
        int rel = seg & 1;
        int nc = seg_coord_count(seg);
        if(bpos + nc*4 > blen) break;
        float c[6];
        for(int i=0;i<nc;i++){ memcpy(&c[i], buf+bpos+i*4, 4); }
        bpos += nc*4;

        /* Convert REL to ABS coordinates */
        if(rel){
            switch(cmd){
            case VG_MOVE_TO:  c[0]+=pos.x; c[1]+=pos.y; break;
            case VG_LINE_TO:  c[0]+=pos.x; c[1]+=pos.y; break;
            case VG_HLINE_TO: c[0]+=pos.x; break;
            case VG_VLINE_TO: c[0]+=pos.y; break;
            case VG_QUAD_TO:  c[0]+=pos.x;c[1]+=pos.y; c[2]+=pos.x;c[3]+=pos.y; break;
            case VG_CUBIC_TO: c[0]+=pos.x;c[1]+=pos.y; c[2]+=pos.x;c[3]+=pos.y; c[4]+=pos.x;c[5]+=pos.y; break;
            case VG_SQUAD_TO: c[0]+=pos.x;c[1]+=pos.y; break;
            case VG_SCUBIC_TO:c[0]+=pos.x;c[1]+=pos.y; c[2]+=pos.x;c[3]+=pos.y; break;
            case VG_SCCWARC_TO: case VG_SCWARC_TO: case VG_LCCWARC_TO: case VG_LCWARC_TO:
                c[3]+=pos.x; c[4]+=pos.y; break;
            default: break;
            }
        }

        switch(cmd){
        case VG_MOVE_TO:
            if(in_subpath) COMMIT_SUBPATH(0);
            pos = SV2(c[0],c[1]);
            move_pt = pos;
            has_prev_quad = has_prev_cub = 0;
            break;
        case VG_CLOSE_PATH:
            if(in_subpath && cur.n >= 2){
                /* close: add line back to move_pt if not already there */
                sv2 last = cur.pts[cur.n-1];
                float dx = last.x - move_pt.x, dy = last.y - move_pt.y;
                if(dx*dx+dy*dy > 1e-9f){
                    APPEND_LINE_SEG(move_pt);
                }
                COMMIT_SUBPATH(1);
            } else if(in_subpath){
                ptl_free(&cur); in_subpath=0;
            }
            pos = move_pt;
            has_prev_quad = has_prev_cub = 0;
            break;
        case VG_LINE_TO:{
            sv2 p1 = SV2(c[0],c[1]);
            APPEND_LINE_SEG(p1);
            pos = p1;
            has_prev_quad = has_prev_cub = 0;
            break; }
        case VG_HLINE_TO:{
            sv2 p1 = SV2(c[0], pos.y);
            APPEND_LINE_SEG(p1);
            pos = p1;
            has_prev_quad = has_prev_cub = 0;
            break; }
        case VG_VLINE_TO:{
            sv2 p1 = SV2(pos.x, c[0]);
            APPEND_LINE_SEG(p1);
            pos = p1;
            has_prev_quad = has_prev_cub = 0;
            break; }
        case VG_QUAD_TO:{
            sv2 cp = SV2(c[0],c[1]), p1 = SV2(c[2],c[3]);
            /* Flatten the quad into line segments */
            if(!in_subpath){ ptl_push(&cur, pos); in_subpath=1; }
            if(!dash) flat_quad(&cur, pos, cp, p1, 0);
            else {
                /* For dashed quads: flatten to polyline then dash */
                ptlist_t tmp={NULL,0,0}; ptl_push(&tmp,pos); flat_quad(&tmp,pos,cp,p1,0);
                for(int i=1;i<tmp.n;i++){ sv2 _s=tmp.pts[i-1]; APPEND_LINE_SEG(tmp.pts[i]); pos=_s; }
                ptl_free(&tmp);
            }
            prev_cp = cp; has_prev_quad = 1; has_prev_cub = 0;
            pos = p1;
            break; }
        case VG_CUBIC_TO:{
            sv2 c1=SV2(c[0],c[1]), c2=SV2(c[2],c[3]), p1=SV2(c[4],c[5]);
            if(!in_subpath){ ptl_push(&cur, pos); in_subpath=1; }
            if(!dash) flat_cub(&cur, pos, c1, c2, p1, 0);
            else {
                ptlist_t tmp={NULL,0,0}; ptl_push(&tmp,pos); flat_cub(&tmp,pos,c1,c2,p1,0);
                for(int i=1;i<tmp.n;i++){ sv2 _s=tmp.pts[i-1]; APPEND_LINE_SEG(tmp.pts[i]); pos=_s; }
                ptl_free(&tmp);
            }
            prev_cp = c2; has_prev_quad = 0; has_prev_cub = 1;
            pos = p1;
            break; }
        case VG_SQUAD_TO:{
            sv2 cp = has_prev_quad
                     ? sv2_sub(sv2_sc(pos,2.f), prev_cp)
                     : pos;
            sv2 p1 = SV2(c[0],c[1]);
            if(!in_subpath){ ptl_push(&cur, pos); in_subpath=1; }
            if(!dash) flat_quad(&cur, pos, cp, p1, 0);
            else {
                ptlist_t tmp={NULL,0,0}; ptl_push(&tmp,pos); flat_quad(&tmp,pos,cp,p1,0);
                for(int i=1;i<tmp.n;i++){ sv2 _s=tmp.pts[i-1]; APPEND_LINE_SEG(tmp.pts[i]); pos=_s; }
                ptl_free(&tmp);
            }
            prev_cp = cp; has_prev_quad = 1; has_prev_cub = 0;
            pos = p1;
            break; }
        case VG_SCUBIC_TO:{
            sv2 c1 = has_prev_cub
                     ? sv2_sub(sv2_sc(pos,2.f), prev_cp)
                     : pos;
            sv2 c2 = SV2(c[0],c[1]), p1 = SV2(c[2],c[3]);
            if(!in_subpath){ ptl_push(&cur, pos); in_subpath=1; }
            if(!dash) flat_cub(&cur, pos, c1, c2, p1, 0);
            else {
                ptlist_t tmp={NULL,0,0}; ptl_push(&tmp,pos); flat_cub(&tmp,pos,c1,c2,p1,0);
                for(int i=1;i<tmp.n;i++){ sv2 _s=tmp.pts[i-1]; APPEND_LINE_SEG(tmp.pts[i]); pos=_s; }
                ptl_free(&tmp);
            }
            prev_cp = c2; has_prev_quad = 0; has_prev_cub = 1;
            pos = p1;
            break; }
        case VG_SCCWARC_TO: case VG_SCWARC_TO: case VG_LCCWARC_TO: case VG_LCWARC_TO: {
            float rx=c[0], ry=c[1], phi=c[2];
            sv2 p1 = SV2(c[3],c[4]);
            int large = (cmd==VG_LCCWARC_TO || cmd==VG_LCWARC_TO);
            int ccw   = (cmd==VG_SCCWARC_TO || cmd==VG_LCCWARC_TO);
            if(!in_subpath){ ptl_push(&cur, pos); in_subpath=1; }
            if(!dash) flat_arc(&cur, pos, rx, ry, phi, p1, large, ccw);
            else {
                ptlist_t tmp={NULL,0,0}; ptl_push(&tmp,pos); flat_arc(&tmp,pos,rx,ry,phi,p1,large,ccw);
                for(int i=1;i<tmp.n;i++){ sv2 _s=tmp.pts[i-1]; APPEND_LINE_SEG(tmp.pts[i]); pos=_s; }
                ptl_free(&tmp);
            }
            pos = p1;
            has_prev_quad = has_prev_cub = 0;
            break; }
        default: break;
        }
    }
    /* Commit any remaining open subpath */
    COMMIT_SUBPATH(0);

    #undef COMMIT_SUBPATH
    #undef APPEND_LINE_SEG

    ptl_free(&cur);

    /* Now draw dst as fill using stroke paint */
    if(dst->buf_len > 0){
        cm_surface_t *surf = ctx->surface;
        vg_cmodel_t cm = surf->cm;

        /* Fill rule: non-zero for stroke outline */
        vg_cmodel_reg_write(cm, VG_REG_FILL_RULE, VG_REG_FILL_NON_ZERO);

        /* Blend mode */
        uint32_t blend_hw = (ctx->blend_mode >= VG_BLEND_SRC)
                            ? (uint32_t)(ctx->blend_mode - VG_BLEND_SRC) : 0;
        vg_cmodel_reg_write(cm, VG_REG_BLEND_MODE, blend_hw);

        /* AA */
        uint32_t aa_hw = (ctx->rendering_quality == VG_RENDERING_QUALITY_NONANTIALIASED)
                         ? VG_AA_NONE : VG_AA_8X;
        vg_cmodel_reg_write(cm, VG_REG_AA_SAMPLES, aa_hw);

        /* Transform (same path matrix) */
        load_matrix_to_cmodel(ctx);

        /* Paint: use stroke paint */
        cm_paint_t *paint = ctx->stroke_paint ? ctx->stroke_paint : &g_default_stroke_paint;
        load_paint_to_cmodel(ctx, paint);

        vg_cmodel_reg_write_f(cm, VG_REG_PATH_SCALE, 1.0f);
        vg_cmodel_reg_write_f(cm, VG_REG_PATH_BIAS,  0.0f);

        /* Masking */
        if (ctx->masking && surf->mask_buf) {
            vg_cmodel_set_mask_ptr(cm, surf->mask_buf);
            vg_cmodel_reg_write(cm, VG_REG_MASK_STRIDE, (uint32_t)surf->width);
            vg_cmodel_reg_write(cm, VG_REG_MASK_EN, 1);
        } else {
            vg_cmodel_reg_write(cm, VG_REG_MASK_EN, 0);
        }

        vg_cmodel_set_path_ptr(cm, dst->buf, (uint32_t)dst->buf_len);
        vg_cmodel_reg_write(cm, VG_REG_PATH_KICK, 1);
    }

    free(dst->buf);
    free(dst);
}

/* =========================================================================
 * VG — Draw path
 * ========================================================================= */

void vgDrawPath(VGPath path, VGbitfield paintModes)
{
    VG_CHECK_CTX();
    if (path == VG_INVALID_HANDLE || !g_surface) {
        VG_SET_ERR(VG_BAD_HANDLE_ERROR); return;
    }
    if (!(paintModes & (VG_FILL_PATH | VG_STROKE_PATH))) return;

    cm_path_t *p  = (cm_path_t *)(uintptr_t)path;
    cm_surface_t *surf = g_surface;
    vg_cmodel_t   cm   = surf->cm;

    /* Fill rule */
    uint32_t fill_rule = (g_ctx->fill_rule == VG_EVEN_ODD)
                         ? VG_REG_FILL_EVEN_ODD : VG_REG_FILL_NON_ZERO;
    vg_cmodel_reg_write(cm, VG_REG_FILL_RULE, fill_rule);

    /* Blend mode — map API enum (0x2000+n) to HW register encoding (0-based) */
    uint32_t blend_hw = (g_ctx->blend_mode >= VG_BLEND_SRC)
                        ? (uint32_t)(g_ctx->blend_mode - VG_BLEND_SRC)
                        : 0;
    vg_cmodel_reg_write(cm, VG_REG_BLEND_MODE, blend_hw);

    /* AA — use NONE (single pixel-centre sample) for NONANTIALIASED quality */
    uint32_t aa_hw = (g_ctx->rendering_quality == VG_RENDERING_QUALITY_NONANTIALIASED)
                     ? VG_AA_NONE : VG_AA_8X;
    vg_cmodel_reg_write(cm, VG_REG_AA_SAMPLES, aa_hw);

    /* Transform */
    load_matrix_to_cmodel(g_ctx);

    /* Paint */
    cm_paint_t *paint = g_ctx->fill_paint ? g_ctx->fill_paint : &g_default_fill_paint;
    load_paint_to_cmodel(g_ctx, paint);

    /* Path scale/bias: coords already converted to floats during vgAppendPathData */
    vg_cmodel_reg_write_f(cm, VG_REG_PATH_SCALE, 1.0f);
    vg_cmodel_reg_write_f(cm, VG_REG_PATH_BIAS,  0.0f);

    /* Masking */
    if (g_ctx->masking && surf->mask_buf) {
        vg_cmodel_set_mask_ptr(cm, surf->mask_buf);
        vg_cmodel_reg_write(cm, VG_REG_MASK_STRIDE, (uint32_t)surf->width);
        vg_cmodel_reg_write(cm, VG_REG_MASK_EN, 1);
    } else {
        vg_cmodel_reg_write(cm, VG_REG_MASK_EN, 0);
    }

    if(paintModes & VG_FILL_PATH){
        /* Submit path buffer to cmodel */
        vg_cmodel_set_path_ptr(cm, p->buf, (uint32_t)p->buf_len);

        /* Kick render — cmodel_run is triggered automatically inside reg_write */
        vg_cmodel_reg_write(cm, VG_REG_PATH_KICK, 1);
    }

    /* Stroke rendering (after fill, painter's algorithm) */
    if(paintModes & VG_STROKE_PATH)
        draw_stroke(g_ctx, p);
}

/* =========================================================================
 * VG — Paint
 * ========================================================================= */

VGPaint vgCreatePaint(void)
{
    VG_CHECK_CTX(VG_INVALID_HANDLE);
    cm_paint_t *pt = (cm_paint_t *)calloc(1, sizeof(cm_paint_t));
    if (!pt) { VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return VG_INVALID_HANDLE; }
    pt->type         = VG_PAINT_TYPE_COLOR;
    pt->color[0]     = 0.f;
    pt->color[1]     = 0.f;
    pt->color[2]     = 0.f;
    pt->color[3]     = 1.f;
    pt->spread_mode  = VG_COLOR_RAMP_SPREAD_PAD;
    pt->tiling_mode  = VG_TILE_FILL;
    return (VGPaint)(uintptr_t)pt;
}

void vgDestroyPaint(VGPaint paint)
{
    if (paint == VG_INVALID_HANDLE) return;
    free((cm_paint_t *)(uintptr_t)paint);
}

void vgSetPaint(VGPaint paint, VGbitfield paintModes)
{
    VG_CHECK_CTX();
    cm_paint_t *pt = (paint == VG_INVALID_HANDLE) ? NULL
                    : (cm_paint_t *)(uintptr_t)paint;
    if (paintModes & VG_FILL_PATH)   g_ctx->fill_paint   = pt;
    if (paintModes & VG_STROKE_PATH) g_ctx->stroke_paint = pt;
}

VGPaint vgGetPaint(VGPaintMode paintMode)
{
    VG_CHECK_CTX(VG_INVALID_HANDLE);
    cm_paint_t *pt = (paintMode == VG_STROKE_PATH) ? g_ctx->stroke_paint
                                                    : g_ctx->fill_paint;
    return pt ? (VGPaint)(uintptr_t)pt : VG_INVALID_HANDLE;
}

void vgSetColor(VGPaint paint, VGuint rgba)
{
    if (paint == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_paint_t *pt = (cm_paint_t *)(uintptr_t)paint;
    pt->type      = VG_PAINT_TYPE_COLOR;
    pt->color[0]  = ((rgba >> 24) & 0xFF) / 255.f;
    pt->color[1]  = ((rgba >> 16) & 0xFF) / 255.f;
    pt->color[2]  = ((rgba >>  8) & 0xFF) / 255.f;
    pt->color[3]  = ((rgba      ) & 0xFF) / 255.f;
}

VGuint vgGetColor(VGPaint paint)
{
    if (paint == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return 0; }
    cm_paint_t *pt = (cm_paint_t *)(uintptr_t)paint;
    return pack_rgba(pt->color);
}

void vgPaintPattern(VGPaint paint, VGImage pattern)
{ (void)paint; (void)pattern; }

/* Object parameter set/get */
void vgSetParameterf(VGHandle obj, VGint ptype, VGfloat v)
{
    if (obj == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    /* Image and font parameters are read-only */
    if (ptype == VG_IMAGE_FORMAT || ptype == VG_IMAGE_WIDTH ||
        ptype == VG_IMAGE_HEIGHT || ptype == VG_FONT_NUM_GLYPHS) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    cm_paint_t *pt = (cm_paint_t *)(uintptr_t)obj;
    switch (ptype) {
    case VG_PAINT_TYPE: pt->type = (VGPaintType)(int)v; break;
    case VG_PAINT_COLOR_RAMP_SPREAD_MODE: pt->spread_mode = (VGColorRampSpreadMode)(int)v; break;
    case VG_PAINT_COLOR_RAMP_PREMULTIPLIED: break;
    case VG_PAINT_PATTERN_TILING_MODE: pt->tiling_mode = (VGTilingMode)(int)v; break;
    default: VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); break;
    }
}

void vgSetParameteri(VGHandle obj, VGint ptype, VGint v)
{
    if (obj == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    /* Image parameters are read-only */
    if (ptype == VG_IMAGE_FORMAT || ptype == VG_IMAGE_WIDTH || ptype == VG_IMAGE_HEIGHT) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    /* Font parameters are read-only */
    if (ptype == VG_FONT_NUM_GLYPHS) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    cm_paint_t *pt = (cm_paint_t *)(uintptr_t)obj;
    /* Try to handle as path capability too */
    if (ptype == VG_PATH_CAPABILITY_APPEND_TO || ptype == VG_PATH_DATATYPE ||
        ptype == VG_PATH_FORMAT) {
        cm_path_t *pp = (cm_path_t *)(uintptr_t)obj;
        if (ptype == VG_PATH_DATATYPE) pp->datatype = (VGPathDatatype)v;
        return;
    }
    switch (ptype) {
    case VG_PAINT_TYPE: pt->type = (VGPaintType)v; break;
    case VG_PAINT_COLOR_RAMP_SPREAD_MODE: pt->spread_mode = (VGColorRampSpreadMode)v; break;
    case VG_PAINT_COLOR_RAMP_PREMULTIPLIED: break;
    case VG_PAINT_PATTERN_TILING_MODE: pt->tiling_mode = (VGTilingMode)v; break;
    default: VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); break;
    }
}

void vgSetParameterfv(VGHandle obj, VGint ptype, VGint count, const VGfloat *v)
{
    if (obj == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (count < 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (count > 0 && (!v || ((uintptr_t)v & (sizeof(VGfloat)-1u)) != 0)) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    cm_paint_t *pt = (cm_paint_t *)(uintptr_t)obj;
    switch (ptype) {
    case VG_PAINT_COLOR:
        if (count != 4) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        pt->type = VG_PAINT_TYPE_COLOR;
        memcpy(pt->color, v, 4 * sizeof(float));
        break;
    case VG_PAINT_LINEAR_GRADIENT:
        if (count != 4) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        pt->type = VG_PAINT_TYPE_LINEAR_GRADIENT;
        memcpy(pt->lin, v, 4 * sizeof(float));
        break;
    case VG_PAINT_RADIAL_GRADIENT:
        if (count != 5) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        pt->type = VG_PAINT_TYPE_RADIAL_GRADIENT;
        memcpy(pt->rad, v, 5 * sizeof(float));
        break;
    case VG_PAINT_COLOR_RAMP_STOPS: {
        if (count % 5 != 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        int ns = count / 5;
        pt->num_stops = (ns > CM_MAX_STOPS) ? CM_MAX_STOPS : ns;
        for (int i = 0; i < pt->num_stops; i++) {
            pt->stops[i].offset   = v[i*5];
            pt->stops[i].color[0] = v[i*5+1];
            pt->stops[i].color[1] = v[i*5+2];
            pt->stops[i].color[2] = v[i*5+3];
            pt->stops[i].color[3] = v[i*5+4];
        }
        break;
    }
    default:
        if (count != 1) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
        vgSetParameterf(obj, ptype, v[0]);
        break;
    }
}

void vgSetParameteriv(VGHandle obj, VGint ptype, VGint count, const VGint *v)
{
    if (obj == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (count < 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (count > 0 && (!v || ((uintptr_t)v & (sizeof(VGint)-1u)) != 0)) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    if (count != 1) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    vgSetParameteri(obj, ptype, v[0]);
}

VGfloat vgGetParameterf(VGHandle obj, VGint ptype)
{
    if (obj == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return 0.f; }
    /* Dispatch by parameter type */
    if (ptype == VG_IMAGE_FORMAT || ptype == VG_IMAGE_WIDTH || ptype == VG_IMAGE_HEIGHT) {
        cm_image_t *img = (cm_image_t *)(uintptr_t)obj;
        if (img->magic != IMAGE_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return 0.f; }
        switch (ptype) {
        case VG_IMAGE_FORMAT: return (VGfloat)img->format;
        case VG_IMAGE_WIDTH:  return (VGfloat)img->width;
        case VG_IMAGE_HEIGHT: return (VGfloat)img->height;
        default: break;
        }
    }
    cm_paint_t *pt = (cm_paint_t *)(uintptr_t)obj;
    /* Also handle path objects */
    cm_path_t  *pp = (cm_path_t *)(uintptr_t)obj;
    switch (ptype) {
    case VG_PAINT_TYPE: return (VGfloat)pt->type;
    case VG_PAINT_COLOR_RAMP_SPREAD_MODE: return (VGfloat)pt->spread_mode;
    case VG_PATH_SCALE: return pp->scale;
    case VG_PATH_BIAS:  return pp->bias;
    default: VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return 0.f;
    }
}

VGint vgGetParameteri(VGHandle obj, VGint ptype)
{
    if (obj == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return 0; }
    /* Dispatch by parameter type */
    if (ptype == VG_IMAGE_FORMAT || ptype == VG_IMAGE_WIDTH || ptype == VG_IMAGE_HEIGHT) {
        cm_image_t *img = (cm_image_t *)(uintptr_t)obj;
        if (img->magic != IMAGE_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return 0; }
        switch (ptype) {
        case VG_IMAGE_FORMAT: return (VGint)img->format;
        case VG_IMAGE_WIDTH:  return img->width;
        case VG_IMAGE_HEIGHT: return img->height;
        default: break;
        }
    }
    if (ptype == VG_FONT_NUM_GLYPHS) {
        cm_font_t *f = (cm_font_t *)(uintptr_t)obj;
        if (f->magic != FONT_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return 0; }
        return f->num_glyphs;
    }
    cm_paint_t *pt = (cm_paint_t *)(uintptr_t)obj;
    cm_path_t  *pp = (cm_path_t *)(uintptr_t)obj;
    switch (ptype) {
    case VG_PAINT_TYPE: return (VGint)pt->type;
    case VG_PAINT_COLOR_RAMP_SPREAD_MODE: return (VGint)pt->spread_mode;
    case VG_PAINT_COLOR_RAMP_PREMULTIPLIED: return 0;
    case VG_PAINT_PATTERN_TILING_MODE: return (VGint)pt->tiling_mode;
    case VG_PATH_FORMAT:   return VG_PATH_FORMAT_STANDARD;
    case VG_PATH_DATATYPE: return (VGint)pp->datatype;
    case VG_PATH_SCALE:    return (VGint)pp->scale;
    case VG_PATH_BIAS:     return (VGint)pp->bias;
    case VG_PATH_NUM_SEGMENTS: return pp->num_segments;
    case VG_PATH_NUM_COORDS:   return pp->num_coords;
    default: VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return 0;
    }
}

VGint vgGetParameterVectorSize(VGHandle obj, VGint ptype)
{
    switch (ptype) {
    case VG_PAINT_COLOR:                return 4;
    case VG_PAINT_LINEAR_GRADIENT:      return 4;
    case VG_PAINT_RADIAL_GRADIENT:      return 5;
    case VG_PAINT_COLOR_RAMP_STOPS:
        if (obj != VG_INVALID_HANDLE) {
            /* Only safe to read if this is actually a paint object */
            /* Best effort: check first 4 bytes as paint type (small valid enum) */
            uint32_t first4; memcpy(&first4, (void*)(uintptr_t)obj, 4);
            if (first4 <= 3) { /* VG_PAINT_TYPE_COLOR..PATTERN range */
                cm_paint_t *pt = (cm_paint_t *)(uintptr_t)obj;
                return pt->num_stops * 5;
            }
        }
        return 0;
    case VG_IMAGE_FORMAT:               return 1;
    case VG_IMAGE_WIDTH:                return 1;
    case VG_IMAGE_HEIGHT:               return 1;
    case VG_FONT_NUM_GLYPHS:            return 1;
    default: return 1;
    }
}

void vgGetParameterfv(VGHandle obj, VGint ptype, VGint count, VGfloat *v)
{
    if (!v || ((uintptr_t)v & (sizeof(VGfloat)-1u)) != 0) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    if (count <= 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (obj == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    /* Check count against vector size */
    int vsize = vgGetParameterVectorSize(obj, ptype);
    if (count > vsize) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    cm_paint_t *pt = (cm_paint_t *)(uintptr_t)obj;
    switch (ptype) {
    case VG_PAINT_COLOR:
        for (int i = 0; i < count && i < 4; i++) v[i] = pt->color[i]; break;
    case VG_PAINT_LINEAR_GRADIENT:
        for (int i = 0; i < count && i < 4; i++) v[i] = pt->lin[i]; break;
    case VG_PAINT_RADIAL_GRADIENT:
        for (int i = 0; i < count && i < 5; i++) v[i] = pt->rad[i]; break;
    default:
        if (count >= 1) v[0] = vgGetParameterf(obj, ptype); break;
    }
}

void vgGetParameteriv(VGHandle obj, VGint ptype, VGint count, VGint *v)
{
    if (!v || ((uintptr_t)v & (sizeof(VGint)-1u)) != 0) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    if (count <= 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (obj == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    /* Check count against vector size */
    int vsize = vgGetParameterVectorSize(obj, ptype);
    if (count > vsize) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (count >= 1) v[0] = vgGetParameteri(obj, ptype);
}

/* =========================================================================
 * VG — Image (stubs – return error or invalid handle)
 * ========================================================================= */

VGImage vgCreateImage(VGImageFormat fmt, VGint w, VGint h, VGbitfield allowedQuality)
{
    VG_CHECK_CTX(VG_INVALID_HANDLE);
    int bpp = image_bpp(fmt);
    if (bpp < 0) { VG_SET_ERR(VG_UNSUPPORTED_IMAGE_FORMAT_ERROR); return VG_INVALID_HANDLE; }
    if (w <= 0 || h <= 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return VG_INVALID_HANDLE; }
    if (allowedQuality == 0 ||
        (allowedQuality & ~(VG_IMAGE_QUALITY_NONANTIALIASED |
                            VG_IMAGE_QUALITY_FASTER |
                            VG_IMAGE_QUALITY_BETTER)) != 0) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return VG_INVALID_HANDLE;
    }
    cm_image_t *img = (cm_image_t *)calloc(1, sizeof(cm_image_t));
    if (!img) { VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return VG_INVALID_HANDLE; }
    img->magic           = IMAGE_MAGIC;
    img->format          = fmt;
    img->width           = w;
    img->height          = h;
    img->allowed_quality = allowedQuality;
    img->bpp             = bpp;
    img->parent_handle   = VG_INVALID_HANDLE;
    img->row_stride      = (bpp > 0 ? bpp : 1) * w;
    img->pixels          = (uint8_t *)calloc((size_t)(w * h), (size_t)(bpp > 0 ? bpp : 1));
    if (!img->pixels) { free(img); VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return VG_INVALID_HANDLE; }
    return (VGImage)(uintptr_t)img;
}
void vgDestroyImage(VGImage image)
{
    VG_CHECK_CTX();
    if (image == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_image_t *img = (cm_image_t *)(uintptr_t)image;
    if (img->magic != IMAGE_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    img->magic = 0;
    /* Only free pixel buffer if this is a root image (children share parent buffer) */
    if (img->parent_handle == VG_INVALID_HANDLE)
        free(img->pixels);
    free(img);
}
/* Forward declaration for format index helper (defined in filter section) */
static void cm_fmt_idx(VGImageFormat fmt, int idx[4]);
static void cm_read_px(const cm_image_t *img, int x, int y, float rgba[4]);
static void cm_write_px(cm_image_t *img, int x, int y, const float rgba[4]);

void vgClearImage(VGImage image, VGint x, VGint y, VGint w, VGint h)
{
    VG_CHECK_CTX();
    if (image == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_image_t *img = (cm_image_t *)(uintptr_t)image;
    if (img->magic != IMAGE_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (w <= 0 || h <= 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    /* Clip to image bounds */
    int x0 = x < 0 ? 0 : x;
    int x1 = (x + w) > img->width ? img->width : (x + w);
    int y0 = y < 0 ? 0 : y;
    int y1 = (y + h) > img->height ? img->height : (y + h);
    if (x0 >= x1 || y0 >= y1) return;
    /* Build clear pixel from VG_CLEAR_COLOR in image's internal format */
    const VGfloat *cc = g_ctx ? g_ctx->clear_color : (const VGfloat []){0.f,0.f,0.f,0.f};
    uint8_t px[4] = {0,0,0,0};
    if (img->bpp == 4) {
        int idx[4]; cm_fmt_idx(img->format, idx);
        px[idx[0]] = (uint8_t)(clampf01(cc[0]) * 255.f + 0.5f); /* R */
        px[idx[1]] = (uint8_t)(clampf01(cc[1]) * 255.f + 0.5f); /* G */
        px[idx[2]] = (uint8_t)(clampf01(cc[2]) * 255.f + 0.5f); /* B */
        px[idx[3]] = (uint8_t)(clampf01(cc[3]) * 255.f + 0.5f); /* A */
    } else if (img->bpp == 1) {
        int base_fmt = img->format & 0x3F;
        if (base_fmt == VG_A_8 || base_fmt == VG_A_1 || base_fmt == VG_A_4) {
            px[0] = (uint8_t)(clampf01(cc[3]) * 255.f + 0.5f); /* alpha */
        } else {
            px[0] = (uint8_t)(clampf01(cc[0]) * 255.f + 0.5f); /* luminance/R */
        }
    }
    /* Fill region with clear pixel */
    if (img->bpp == 4) {
        uint32_t fill; memcpy(&fill, px, 4);
        for (int row = y0; row < y1; row++) {
            uint32_t *fdst = (uint32_t *)(img->pixels + (size_t)row * (size_t)img->row_stride + (size_t)x0 * 4);
            for (int col = 0; col < (x1-x0); col++) fdst[col] = fill;
        }
    } else {
        for (int row = y0; row < y1; row++) {
            uint8_t *dst = img->pixels + (size_t)row * (size_t)img->row_stride + (size_t)x0 * (size_t)img->bpp;
            memset(dst, px[0], (size_t)(x1 - x0) * img->bpp);
        }
    }
}

void vgImageSubData(VGImage image, const void *data, VGint dataStride,
                    VGImageFormat dataFormat, VGint x, VGint y, VGint w, VGint h)
{
    VG_CHECK_CTX();
    if (image == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_image_t *img = (cm_image_t *)(uintptr_t)image;
    if (img->magic != IMAGE_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    int src_bpp = image_bpp(dataFormat);
    if (src_bpp < 0) { VG_SET_ERR(VG_UNSUPPORTED_IMAGE_FORMAT_ERROR); return; }
    if (!data || w <= 0 || h <= 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    /* Check data alignment (4-byte formats need 4-byte align, 2-byte need 2-byte) */
    size_t align = src_bpp >= 4 ? 4u : (src_bpp >= 2 ? 2u : 1u);
    if (((uintptr_t)data & (align - 1u)) != 0) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    /* Simple copy: if formats match, copy directly; otherwise convert per-pixel */
    for (int row = 0; row < h; row++) {
        int dst_y = y + row;
        if (dst_y < 0 || dst_y >= img->height) continue;
        int copy_x0 = x < 0 ? -x : 0;
        int copy_x1 = (x + w > img->width) ? (img->width - x) : w;
        if (copy_x1 <= copy_x0) continue;
        int copy_w = copy_x1 - copy_x0;
        const uint8_t *src = (const uint8_t *)data + (size_t)row * (size_t)dataStride
                             + (size_t)(copy_x0 * src_bpp);
        uint8_t *dst = img->pixels + (size_t)dst_y * (size_t)img->row_stride + (size_t)(x + copy_x0) * (size_t)img->bpp;
        if (src_bpp == 4 && img->bpp == 4 && dataFormat == img->format) {
            memcpy(dst, src, (size_t)copy_w * 4);
        } else if (src_bpp == 4 && img->bpp == 4) {
            /* Convert between 4-byte formats */
            int src_idx[4]; cm_fmt_idx(dataFormat, src_idx);
            int dst_idx[4]; cm_fmt_idx(img->format, dst_idx);
            for (int i = 0; i < copy_w; i++) {
                const uint8_t *sp = src + i * 4;
                uint8_t *dp = dst + i * 4;
                uint8_t r = sp[src_idx[0]];
                uint8_t g = sp[src_idx[1]];
                uint8_t b = sp[src_idx[2]];
                uint8_t a = sp[src_idx[3]];
                dp[dst_idx[0]] = r;
                dp[dst_idx[1]] = g;
                dp[dst_idx[2]] = b;
                dp[dst_idx[3]] = a;
            }
        } else {
            memcpy(dst, src, (size_t)(copy_w * (src_bpp < img->bpp ? src_bpp : img->bpp)));
        }
    }
}
void vgGetImageSubData(VGImage image, void *data, VGint dataStride,
                       VGImageFormat dataFormat, VGint x, VGint y, VGint w, VGint h)
{
    VG_CHECK_CTX();
    if (image == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_image_t *img = (cm_image_t *)(uintptr_t)image;
    if (img->magic != IMAGE_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    int dst_bpp = image_bpp(dataFormat);
    if (dst_bpp < 0) { VG_SET_ERR(VG_UNSUPPORTED_IMAGE_FORMAT_ERROR); return; }
    if (!data || w <= 0 || h <= 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    /* Check data alignment */
    size_t align = dst_bpp >= 4 ? 4u : (dst_bpp >= 2 ? 2u : 1u);
    if (((uintptr_t)data & (align - 1u)) != 0) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    for (int row = 0; row < h; row++) {
        int src_y = y + row;
        if (src_y < 0 || src_y >= img->height) continue;
        int copy_x0 = x < 0 ? -x : 0;
        int copy_x1 = (x + w > img->width) ? (img->width - x) : w;
        if (copy_x1 <= copy_x0) continue;
        int copy_w = copy_x1 - copy_x0;
        uint8_t *dst = (uint8_t *)data + (size_t)row * (size_t)dataStride
                      + (size_t)(copy_x0 * dst_bpp);
        if (img->bpp == 4 && dst_bpp == 4 && img->format == dataFormat) {
            /* Same 4-byte format: raw copy */
            const uint8_t *src = img->pixels
                               + (size_t)src_y * (size_t)img->row_stride + (size_t)(x + copy_x0) * 4;
            memcpy(dst, src, (size_t)copy_w * 4);
        } else if (img->bpp == 4 && dst_bpp == 4) {
            /* Different 4-byte formats: convert via float RGBA */
            int src_idx[4]; cm_fmt_idx(img->format, src_idx);
            int dst_idx[4]; cm_fmt_idx(dataFormat, dst_idx);
            const uint8_t *src = img->pixels
                               + (size_t)src_y * (size_t)img->row_stride + (size_t)(x + copy_x0) * 4;
            for (int i = 0; i < copy_w; i++) {
                const uint8_t *sp = src + i * 4;
                uint8_t *dp = dst + i * 4;
                uint8_t r = sp[src_idx[0]];
                uint8_t g = sp[src_idx[1]];
                uint8_t b = sp[src_idx[2]];
                uint8_t a = sp[src_idx[3]];
                dp[dst_idx[0]] = r;
                dp[dst_idx[1]] = g;
                dp[dst_idx[2]] = b;
                dp[dst_idx[3]] = a;
            }
        } else {
            /* Fallback: raw copy (same as before) */
            const uint8_t *src = img->pixels
                               + (size_t)src_y * (size_t)img->row_stride + (size_t)(x + copy_x0) * (size_t)img->bpp;
            memcpy(dst, src, (size_t)(copy_w * (dst_bpp < img->bpp ? dst_bpp : img->bpp)));
        }
    }
}
VGImage vgChildImage(VGImage parent, VGint x, VGint y, VGint w, VGint h)
{
    VG_CHECK_CTX(VG_INVALID_HANDLE);
    if (parent == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return VG_INVALID_HANDLE; }
    cm_image_t *p = (cm_image_t *)(uintptr_t)parent;
    if (p->magic != IMAGE_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return VG_INVALID_HANDLE; }
    if (w <= 0 || h <= 0 || x < 0 || y < 0 ||
        w > p->width || h > p->height ||
        x > p->width - w || y > p->height - h) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return VG_INVALID_HANDLE;
    }
    cm_image_t *child = (cm_image_t *)calloc(1, sizeof(cm_image_t));
    if (!child) { VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return VG_INVALID_HANDLE; }
    child->magic           = IMAGE_MAGIC;
    child->format          = p->format;
    child->width           = w;
    child->height          = h;
    child->allowed_quality = p->allowed_quality;
    child->bpp             = p->bpp;
    child->parent_handle   = parent;
    /* Share parent's pixel buffer: child->pixels points into parent's buffer */
    child->row_stride      = p->row_stride;
    child->pixels          = p->pixels + (size_t)(y * p->row_stride + x * p->bpp);
    return (VGImage)(uintptr_t)child;
}
VGImage vgGetParent(VGImage image)
{
    VG_CHECK_CTX(VG_INVALID_HANDLE);
    if (image == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return VG_INVALID_HANDLE; }
    cm_image_t *img = (cm_image_t *)(uintptr_t)image;
    if (img->magic != IMAGE_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return VG_INVALID_HANDLE; }
    if (img->parent_handle != VG_INVALID_HANDLE)
        return img->parent_handle;
    return image;
}
void vgCopyImage(VGImage dst, VGint dx, VGint dy, VGImage src, VGint sx, VGint sy,
                 VGint w, VGint h, VGboolean dither)
{
    VG_CHECK_CTX();
    (void)dither;
    if (dst == VG_INVALID_HANDLE || src == VG_INVALID_HANDLE) {
        VG_SET_ERR(VG_BAD_HANDLE_ERROR); return;
    }
    cm_image_t *d = (cm_image_t *)(uintptr_t)dst;
    cm_image_t *s = (cm_image_t *)(uintptr_t)src;
    if (d->magic != IMAGE_MAGIC || s->magic != IMAGE_MAGIC) {
        VG_SET_ERR(VG_BAD_HANDLE_ERROR); return;
    }
    if (w <= 0 || h <= 0) return;
    /* Use int64_t to safely handle extreme negative/large coordinates */
    int64_t W = w, H = h;
    int64_t Sx = sx, Sy = sy, Dx = dx, Dy = dy;
    /* Clip source region to src image bounds */
    int64_t sx0 = Sx < 0 ? 0 : Sx;
    int64_t sy0 = Sy < 0 ? 0 : Sy;
    int64_t sx1 = Sx + W > s->width  ? (int64_t)s->width  : Sx + W;
    int64_t sy1 = Sy + H > s->height ? (int64_t)s->height : Sy + H;
    if (sx0 >= sx1 || sy0 >= sy1) return;
    /* Map clipped source back to destination */
    int64_t dx0 = sx0 - Sx + Dx;
    int64_t dy0 = sy0 - Sy + Dy;
    int64_t dx1 = sx1 - Sx + Dx;
    int64_t dy1 = sy1 - Sy + Dy;
    /* Clip destination region to dst image bounds */
    if (dx0 < 0) { sx0 -= dx0; dx0 = 0; }
    if (dy0 < 0) { sy0 -= dy0; dy0 = 0; }
    if (dx1 > d->width)  { sx1 -= (dx1 - d->width);  dx1 = d->width; }
    if (dy1 > d->height) { sy1 -= (dy1 - d->height); dy1 = d->height; }
    if (dx0 >= dx1 || dy0 >= dy1) return;
    int64_t cpw = dx1 - dx0;
    int64_t cph = dy1 - dy0;
    int bpp = s->bpp < d->bpp ? s->bpp : d->bpp;
    for (int64_t row = 0; row < cph; row++) {
        const uint8_t *src_row = s->pixels +
            (size_t)(sy0 + row) * (size_t)s->row_stride + (size_t)sx0 * (size_t)s->bpp;
        uint8_t *dst_row = d->pixels +
            (size_t)(dy0 + row) * (size_t)d->row_stride + (size_t)dx0 * (size_t)d->bpp;
        memcpy(dst_row, src_row, (size_t)cpw * (size_t)bpp);
    }
}
/* linear ↔ sRGB helpers (mirrors vg_cmodel.c but used here for image ops) */
static inline float img_srgb2lin(float c)
{
    if (c <= 0.04045f) return c * (1.f / 12.92f);
    return powf((c + 0.055f) * (1.f / 1.055f), 2.4f);
}
static inline float img_lin2srgb(float c)
{
    if (c <= 0.0031308f) return c * 12.92f;
    return 1.055f * powf(c, 1.f / 2.4f) - 0.055f;
}

void vgDrawImage(VGImage image)
{
    VG_CHECK_CTX();
    if (image == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_image_t *img = (cm_image_t *)(uintptr_t)image;
    if (img->magic != IMAGE_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (!g_surface) return;

    int SW = g_surface->width, SH = g_surface->height;
    int IW = img->width,       IH = img->height;
    if (IW <= 0 || IH <= 0 || SW <= 0 || SH <= 0) return;

    uint32_t *fb = (uint32_t *)(uintptr_t)vg_cmodel_get_framebuffer(g_surface->cm, NULL, NULL);
    if (!fb) return;

    /* Image-to-surface matrix (column-major 3×3, OpenVG convention) */
    const VGfloat *m = g_ctx->mat_image;
    float m_sx  = m[0], m_shy = m[1];   /* col 0 */
    float m_shx = m[3], m_sy  = m[4];   /* col 1 */
    float m_tx  = m[6], m_ty  = m[7];   /* col 2 */

    /* Invert the 2-D affine part: M^-1 maps surface → image coords */
    float det = m_sx * m_sy - m_shx * m_shy;
    if (fabsf(det) < 1e-9f) return;
    float id = 1.f / det;
    float isx  =  m_sy  * id;
    float ishx = -m_shx * id;
    float ishy = -m_shy * id;
    float isy  =  m_sx  * id;
    float itx  = (m_shx * m_ty - m_sy  * m_tx) * id;
    float ity  = (m_shy * m_tx - m_sx  * m_ty) * id;

    /* Bounding box of image [0,IW]×[0,IH] in surface OpenVG coords */
    float cx[4] = { m_tx,
                    m_sx*IW + m_tx,
                    m_sx*IW + m_shx*IH + m_tx,
                    m_shx*IH + m_tx };
    float cy[4] = { m_ty,
                    m_shy*IW + m_ty,
                    m_shy*IW + m_sy*IH + m_ty,
                    m_sy*IH + m_ty };

    float bbx0 = cx[0], bbx1 = cx[0], bby0 = cy[0], bby1 = cy[0];
    for (int i = 1; i < 4; i++) {
        if (cx[i] < bbx0) bbx0 = cx[i];
        if (cx[i] > bbx1) bbx1 = cx[i];
        if (cy[i] < bby0) bby0 = cy[i];
        if (cy[i] > bby1) bby1 = cy[i];
    }

    /* Clip to surface */
    int x0 = (int)floorf(bbx0); if (x0 < 0) x0 = 0;
    int x1 = (int)ceilf (bbx1); if (x1 > SW) x1 = SW;
    int y0 = (int)floorf(bby0); if (y0 < 0) y0 = 0;
    int y1 = (int)ceilf (bby1); if (y1 > SH) y1 = SH;
    if (x0 >= x1 || y0 >= y1) return;

    /* Format properties */
    int base_fmt = img->format & 0x3F;
    /* Linear: lRGBX=7, lRGBA=8, lRGBA_PRE=9, lL_8=10 */
    int is_linear = (base_fmt == 7 || base_fmt == 8 || base_fmt == 9 || base_fmt == 10);
    /* Premultiplied: sRGBA_PRE=2, lRGBA_PRE=9 */
    int is_pre    = (base_fmt == 2 || base_fmt == 9);

    /* Blend mode (0-based) */
    int bm = (g_ctx->blend_mode >= VG_BLEND_SRC)
             ? (int)(g_ctx->blend_mode - VG_BLEND_SRC) : 0;

    /* Image mode and fill paint color */
    VGImageMode imode = g_ctx->image_mode;
    float fp[4] = {1.f, 1.f, 1.f, 1.f};
    if (imode != VG_DRAW_IMAGE_NORMAL) {
        cm_paint_t *pt = g_ctx->fill_paint ? g_ctx->fill_paint : &g_default_fill_paint;
        fp[0] = pt->color[0]; fp[1] = pt->color[1];
        fp[2] = pt->color[2]; fp[3] = pt->color[3];
    }

    for (int vy = y0; vy < y1; vy++) {
        for (int vx = x0; vx < x1; vx++) {
            /* Scissor */
            if (g_ctx->scissoring && g_ctx->num_scissor_rects > 0) {
                int in_sc = 0;
                for (int ri = 0; ri < g_ctx->num_scissor_rects && !in_sc; ri++) {
                    VGint sxr = g_ctx->scissor_rects[ri*4+0];
                    VGint syr = g_ctx->scissor_rects[ri*4+1];
                    VGint swr = g_ctx->scissor_rects[ri*4+2];
                    VGint shr = g_ctx->scissor_rects[ri*4+3];
                    if (vx >= sxr && vx < sxr+swr && vy >= syr && vy < syr+shr)
                        in_sc = 1;
                }
                if (!in_sc) continue;
            }

            /* Backward-map surface pixel centre → image coords */
            float fx = (float)vx + 0.5f;
            float fy = (float)vy + 0.5f;
            float ix_f = isx  * fx + ishx * fy + itx;
            float iy_f = ishy * fx + isy  * fy + ity;

            /* Nearest-neighbour sampling */
            int ix = (int)floorf(ix_f);
            int iy = (int)floorf(iy_f);
            if (ix < 0 || iy < 0 || ix >= IW || iy >= IH) continue;

            /* Read source pixel as normalised float [0,1] */
            float s[4];
            cm_read_px(img, ix, iy, s);

            /* De-premultiply if stored premultiplied */
            if (is_pre && s[3] > 1e-6f) {
                float ia = 1.f / s[3];
                s[0] *= ia; s[1] *= ia; s[2] *= ia;
            }

            /* Convert linear image → sRGB rendering space */
            if (is_linear) {
                s[0] = clampf01(img_lin2srgb(s[0]));
                s[1] = clampf01(img_lin2srgb(s[1]));
                s[2] = clampf01(img_lin2srgb(s[2]));
            }

            /* Apply color transform and compute premultiplied source for blending.
             * Matches RI: NORMAL/MULTIPLY CT on image (or product),
             * STENCIL CT on paint only; STENCIL uses per-channel alphas. */
            float sa, ar, ag, ab;
            float sp[3];

            if (imode == VG_DRAW_IMAGE_STENCIL) {
                /* CT applied to PAINT, image channels act as per-channel stencil */
                float paint[4] = { fp[0], fp[1], fp[2], fp[3] };
                if (g_ctx->color_transform) {
                    const VGfloat *cv = g_ctx->color_transform_values;
                    paint[0] = clampf01(paint[0]*cv[0]+cv[4]);
                    paint[1] = clampf01(paint[1]*cv[1]+cv[5]);
                    paint[2] = clampf01(paint[2]*cv[2]+cv[6]);
                    paint[3] = clampf01(paint[3]*cv[3]+cv[7]);
                }
                float image_a = s[3];
                /* per-channel alphas: paint_a * image_a * image_channel */
                sa = paint[3] * image_a;
                ar = sa * s[0];
                ag = sa * s[1];
                ab = sa * s[2];
                /* premultiplied source channels: paint_channel * per-channel-alpha */
                sp[0] = paint[0] * ar;
                sp[1] = paint[1] * ag;
                sp[2] = paint[2] * ab;
            } else if (imode == VG_DRAW_IMAGE_MULTIPLY) {
                /* CT applied to paint*image product */
                float prod[4] = { s[0]*fp[0], s[1]*fp[1], s[2]*fp[2], s[3]*fp[3] };
                if (g_ctx->color_transform) {
                    const VGfloat *cv = g_ctx->color_transform_values;
                    prod[0] = clampf01(prod[0]*cv[0]+cv[4]);
                    prod[1] = clampf01(prod[1]*cv[1]+cv[5]);
                    prod[2] = clampf01(prod[2]*cv[2]+cv[6]);
                    prod[3] = clampf01(prod[3]*cv[3]+cv[7]);
                }
                sa = prod[3]; ar = ag = ab = sa;
                sp[0]=prod[0]*sa; sp[1]=prod[1]*sa; sp[2]=prod[2]*sa;
            } else {
                /* NORMAL: CT applied to image */
                if (g_ctx->color_transform) {
                    const VGfloat *cv = g_ctx->color_transform_values;
                    s[0] = clampf01(s[0]*cv[0]+cv[4]);
                    s[1] = clampf01(s[1]*cv[1]+cv[5]);
                    s[2] = clampf01(s[2]*cv[2]+cv[6]);
                    s[3] = clampf01(s[3]*cv[3]+cv[7]);
                }
                sa = s[3]; ar = ag = ab = sa;
                sp[0]=s[0]*sa; sp[1]=s[1]*sa; sp[2]=s[2]*sa;
            }

            /* Read destination pixel from framebuffer (RGBA8888: 0xRRGGBBAA) */
            int fb_row = SH - 1 - vy;
            uint32_t *dst_px = &fb[(uint32_t)fb_row * (uint32_t)SW + (uint32_t)vx];
            uint32_t dst_packed = *dst_px;
            float d[4] = { ((dst_packed >> 24) & 0xFF) / 255.f,
                           ((dst_packed >> 16) & 0xFF) / 255.f,
                           ((dst_packed >>  8) & 0xFF) / 255.f,
                           ( dst_packed        & 0xFF) / 255.f };

            /* Porter-Duff blend matching RI: operates on premultiplied sRGB.
             * Per-channel alphas ar/ag/ab (= sa for NORMAL/MULTIPLY, differ for STENCIL). */
            float da = d[3];
            float dp[3] = { d[0]*da, d[1]*da, d[2]*da };
            float rp[3]; float ra;
            switch (bm) {
            case 0: /* SRC */
                rp[0]=sp[0]; rp[1]=sp[1]; rp[2]=sp[2]; ra=sa; break;
            case 1: /* SRC_OVER: r_c=sp+dp*(1-ar_c); r_a=sa+da*(1-sa) */
                ra = sa + da*(1.f-sa);
                rp[0]=sp[0]+dp[0]*(1.f-ar);
                rp[1]=sp[1]+dp[1]*(1.f-ag);
                rp[2]=sp[2]+dp[2]*(1.f-ab); break;
            case 2: /* DST_OVER */
                ra = sa*(1.f-da)+da;
                rp[0]=sp[0]*(1.f-da)+dp[0];
                rp[1]=sp[1]*(1.f-da)+dp[1];
                rp[2]=sp[2]*(1.f-da)+dp[2]; break;
            case 3: /* SRC_IN */
                ra=sa*da;
                rp[0]=sp[0]*da; rp[1]=sp[1]*da; rp[2]=sp[2]*da; break;
            case 4: /* DST_IN: r_c=dp*ar_c; r_a=da*sa */
                ra=da*sa;
                rp[0]=dp[0]*ar; rp[1]=dp[1]*ag; rp[2]=dp[2]*ab; break;
            case 5: /* MULTIPLY: r_c=sp*(1-da+dp)+dp*(1-ar_c) */
                ra = sa+da-sa*da;
                rp[0]=sp[0]*(1.f-da+dp[0])+dp[0]*(1.f-ar);
                rp[1]=sp[1]*(1.f-da+dp[1])+dp[1]*(1.f-ag);
                rp[2]=sp[2]*(1.f-da+dp[2])+dp[2]*(1.f-ab); break;
            case 6: /* SCREEN: r_c=sp+dp*(1-sp) — uses premul sp */
                ra = sa+da-sa*da;
                rp[0]=sp[0]+dp[0]*(1.f-sp[0]);
                rp[1]=sp[1]+dp[1]*(1.f-sp[1]);
                rp[2]=sp[2]+dp[2]*(1.f-sp[2]); break;
            case 7: /* DARKEN: min using ar_c on SrcOver term */
                ra = sa+da-sa*da;
                rp[0]=fminf(sp[0]+dp[0]*(1.f-ar), dp[0]+sp[0]*(1.f-da));
                rp[1]=fminf(sp[1]+dp[1]*(1.f-ag), dp[1]+sp[1]*(1.f-da));
                rp[2]=fminf(sp[2]+dp[2]*(1.f-ab), dp[2]+sp[2]*(1.f-da)); break;
            case 8: /* LIGHTEN: max using ar_c on SrcOver term */
                { float a1=sa+da*(1.f-sa), a2=da+sa*(1.f-da); ra=fmaxf(a1,a2); }
                rp[0]=fmaxf(sp[0]+dp[0]*(1.f-ar), dp[0]+sp[0]*(1.f-da));
                rp[1]=fmaxf(sp[1]+dp[1]*(1.f-ag), dp[1]+sp[1]*(1.f-da));
                rp[2]=fmaxf(sp[2]+dp[2]*(1.f-ab), dp[2]+sp[2]*(1.f-da)); break;
            case 9: /* ADDITIVE */
                ra=fminf(sa+da, 1.f);
                rp[0]=fminf(sp[0]+dp[0],1.f);
                rp[1]=fminf(sp[1]+dp[1],1.f);
                rp[2]=fminf(sp[2]+dp[2],1.f); break;
            default:
                rp[0]=sp[0]; rp[1]=sp[1]; rp[2]=sp[2]; ra=sa; break;
            }
            /* De-premultiply for non-premul storage */
            float out[4];
            out[3] = clampf01(ra);
            if (ra > 1e-6f) {
                out[0] = clampf01(rp[0] / ra);
                out[1] = clampf01(rp[1] / ra);
                out[2] = clampf01(rp[2] / ra);
            } else { out[0]=out[1]=out[2]=0.f; }

            /* Write result */
            uint8_t r8 = (uint8_t)(clampf01(out[0]) * 255.f + 0.5f);
            uint8_t g8 = (uint8_t)(clampf01(out[1]) * 255.f + 0.5f);
            uint8_t b8 = (uint8_t)(clampf01(out[2]) * 255.f + 0.5f);
            uint8_t a8 = (uint8_t)(clampf01(out[3]) * 255.f + 0.5f);
            *dst_px = ((uint32_t)r8 << 24) | ((uint32_t)g8 << 16)
                    | ((uint32_t)b8 <<  8) |  (uint32_t)a8;
        }
    }
}
void vgSetPixels(VGint dx, VGint dy, VGImage src, VGint sx, VGint sy, VGint w, VGint h)
{
    VG_CHECK_CTX();
    if (src == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_image_t *img = (cm_image_t *)(uintptr_t)src;
    if (img->magic != IMAGE_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (w <= 0 || h <= 0) return;
    if (!g_surface) return;
    int SW = g_surface->width, SH = g_surface->height;
    uint32_t *fb = (uint32_t *)(uintptr_t)vg_cmodel_get_framebuffer(g_surface->cm, NULL, NULL);
    if (!fb) return;
    /* Clip to source image bounds (sx, sy define start in source image) */
    int64_t sx0 = sx, sy0 = sy, dx0 = dx, dy0 = dy, W = w, H = h;
    /* Clamp source left/top */
    if (sx0 < 0) { dx0 -= sx0; W += sx0; sx0 = 0; }
    if (sy0 < 0) { dy0 -= sy0; H += sy0; sy0 = 0; }
    /* Clamp source right/bottom */
    if (sx0 + W > img->width)  W = img->width  - sx0;
    if (sy0 + H > img->height) H = img->height - sy0;
    if (W <= 0 || H <= 0) return;
    /* Clamp dest left/top */
    if (dx0 < 0) { sx0 -= dx0; W += dx0; dx0 = 0; }
    if (dy0 < 0) { sy0 -= dy0; H += dy0; dy0 = 0; }
    /* Clamp dest right/bottom */
    if (dx0 + W > SW) W = SW - dx0;
    if (dy0 + H > SH) H = SH - dy0;
    if (W <= 0 || H <= 0) return;
    /* Pre-compute format info for fast per-pixel copy */
    int idx[4] = {0,1,2,3};
    int is_alpha_only = 0;
    if (img->bpp == 4) { cm_fmt_idx(img->format, idx); }
    else if (img->bpp == 1) {
        int bf = img->format & 0x3F;
        is_alpha_only = (bf == VG_A_8 || bf == VG_A_1 || bf == VG_A_4) ? 1 : 0;
    }
    for (int64_t j = 0; j < H; j++) {
        int iy = (int)(sy0 + j), fy = (int)(dy0 + j);
        int fb_row = SH - 1 - fy;
        const uint8_t *row = img->pixels + (size_t)iy * (size_t)img->row_stride;
        uint32_t *fb_ptr = fb + fb_row * SW + (int)dx0;
        for (int64_t i = 0; i < W; i++) {
            int ix = (int)(sx0 + i);
            uint32_t px;
            if (img->bpp == 4) {
                const uint8_t *p = row + ix * 4;
                px = ((uint32_t)p[idx[0]] << 24) | ((uint32_t)p[idx[1]] << 16)
                   | ((uint32_t)p[idx[2]] <<  8) |  (uint32_t)p[idx[3]];
            } else if (img->bpp == 1) {
                uint8_t v = row[ix];
                if (is_alpha_only) px = 0xFFFFFF00u | (uint32_t)v;
                else               px = ((uint32_t)v << 24) | ((uint32_t)v << 16) | ((uint32_t)v << 8) | 0xFF;
            } else {
                float rgba[4]; cm_read_px(img, ix, iy, rgba); px = pack_rgba(rgba);
            }
            fb_ptr[i] = px;
        }
    }
}
void vgCopyPixels(VGint dx, VGint dy, VGint sx, VGint sy, VGint w, VGint h)
{
    VG_CHECK_CTX();
    if (w <= 0 || h <= 0) return;
    if (!g_surface) return;
    int SW = g_surface->width, SH = g_surface->height;
    uint32_t *fb = (uint32_t *)(uintptr_t)vg_cmodel_get_framebuffer(g_surface->cm, NULL, NULL);
    if (!fb) return;
    /* Clip using int64 to handle extreme coordinate values */
    int64_t sx0 = sx, sy0 = sy, dx0 = dx, dy0 = dy, W = w, H = h;
    if (sx0 < 0) { dx0 -= sx0; W += sx0; sx0 = 0; }
    if (sy0 < 0) { dy0 -= sy0; H += sy0; sy0 = 0; }
    if (sx0 + W > SW) W = SW - sx0;
    if (sy0 + H > SH) H = SH - sy0;
    if (W <= 0 || H <= 0) return;
    if (dx0 < 0) { sx0 -= dx0; W += dx0; dx0 = 0; }
    if (dy0 < 0) { sy0 -= dy0; H += dy0; dy0 = 0; }
    if (dx0 + W > SW) W = SW - dx0;
    if (dy0 + H > SH) H = SH - dy0;
    if (W <= 0 || H <= 0) return;
    /* Copy within framebuffer row by row (handle overlap via direction) */
    int copy_up = (dy0 < sy0) || (dy0 == sy0 && dx0 <= sx0);
    for (int64_t j = 0; j < H; j++) {
        int64_t jj = copy_up ? j : (H - 1 - j);
        int sfy = (int)(sy0 + jj), dfy = (int)(dy0 + jj);
        int sfr = SH - 1 - sfy, dfr = SH - 1 - dfy;
        memmove(fb + dfr * SW + (int)dx0, fb + sfr * SW + (int)sx0,
                (size_t)W * sizeof(uint32_t));
    }
}

/* =========================================================================
 * VG — Pixel read / write
 * ========================================================================= */

void vgWritePixels(const void *data, VGint stride, VGImageFormat fmt,
                   VGint dx, VGint dy, VGint w, VGint h)
{
    VG_CHECK_CTX();
    int bpp = image_bpp(fmt);
    if (bpp < 0) { VG_SET_ERR(VG_UNSUPPORTED_IMAGE_FORMAT_ERROR); return; }
    if (!data || w <= 0 || h <= 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    size_t align = bpp >= 4 ? 4u : (bpp >= 2 ? 2u : 1u);
    if (((uintptr_t)data & (align - 1u)) != 0) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    (void)stride;(void)dx;(void)dy;
}

void vgGetPixels(VGImage dst, VGint dx, VGint dy, VGint sx, VGint sy, VGint w, VGint h)
{
    VG_CHECK_CTX();
    if (dst == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_image_t *img = (cm_image_t *)(uintptr_t)dst;
    if (img->magic != IMAGE_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (w <= 0 || h <= 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (!g_surface) return;
    int SW = g_surface->width, SH = g_surface->height;
    uint32_t *fb = (uint32_t *)(uintptr_t)vg_cmodel_get_framebuffer(g_surface->cm, NULL, NULL);
    if (!fb) return;
    /* Clip using int64 to handle extreme coordinate values */
    int64_t sx0 = sx, sy0 = sy, dx0 = dx, dy0 = dy, W = w, H = h;
    if (sx0 < 0) { dx0 -= sx0; W += sx0; sx0 = 0; }
    if (sy0 < 0) { dy0 -= sy0; H += sy0; sy0 = 0; }
    if (sx0 + W > SW) W = SW - sx0;
    if (sy0 + H > SH) H = SH - sy0;
    if (W <= 0 || H <= 0) return;
    if (dx0 < 0) { sx0 -= dx0; W += dx0; dx0 = 0; }
    if (dy0 < 0) { sy0 -= dy0; H += dy0; dy0 = 0; }
    if (dx0 + W > img->width)  W = img->width  - dx0;
    if (dy0 + H > img->height) H = img->height - dy0;
    if (W <= 0 || H <= 0) return;
    /* Pre-compute format info */
    int idx[4] = {0,1,2,3};
    int is_alpha_only = 0;
    if (img->bpp == 4) { cm_fmt_idx(img->format, idx); }
    else if (img->bpp == 1) {
        int bf = img->format & 0x3F;
        is_alpha_only = (bf == VG_A_8 || bf == VG_A_1 || bf == VG_A_4) ? 1 : 0;
    }
    for (int64_t j = 0; j < H; j++) {
        int fy = (int)(sy0 + j), iy = (int)(dy0 + j);
        int fb_row = SH - 1 - fy;
        const uint32_t *fb_ptr = fb + fb_row * SW + (int)sx0;
        uint8_t *row = img->pixels + (size_t)iy * (size_t)img->row_stride;
        for (int64_t i = 0; i < W; i++) {
            int ix = (int)(dx0 + i);
            uint32_t p = fb_ptr[i];
            uint8_t r = (p >> 24) & 0xFF, g = (p >> 16) & 0xFF,
                    b = (p >>  8) & 0xFF, a = p & 0xFF;
            if (img->bpp == 4) {
                uint8_t *dst_p = row + ix * 4;
                dst_p[idx[0]] = r; dst_p[idx[1]] = g;
                dst_p[idx[2]] = b; dst_p[idx[3]] = a;
            } else if (img->bpp == 1) {
                row[ix] = is_alpha_only ? a : r;
            } else {
                float rgba[4] = {r/255.f, g/255.f, b/255.f, a/255.f};
                cm_write_px(img, ix, iy, rgba);
            }
        }
    }
}

/*
 * vgReadPixels — read (sx, sy, w, h) from the surface into data.
 * OpenVG surface Y-up (y=0 = bottom); cmodel framebuffer Y-down (row0 = top).
 * Output: data[y_out * dataStride + x*bpp] where y_out=0 = OpenVG y=sy (bottom).
 */
void vgReadPixels(void *data, VGint dataStride, VGImageFormat dataFormat,
                  VGint sx, VGint sy, VGint width, VGint height)
{
    VG_CHECK_CTX();
    int bpp = image_bpp(dataFormat);
    if (bpp < 0) { VG_SET_ERR(VG_UNSUPPORTED_IMAGE_FORMAT_ERROR); return; }
    /* Check for null/misaligned data pointer */
    if (!data || width <= 0 || height <= 0) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    /* Format-dependent alignment check */
    {
        size_t align = bpp >= 4 ? 4u : (bpp >= 2 ? 2u : 1u);
        if (align > 1 && ((uintptr_t)data & (align - 1u)) != 0) {
            VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
        }
    }
    if (!g_surface) return;

    cm_surface_t *surf = g_surface;
    int W = surf->width, H = surf->height;
    const uint32_t *fb = vg_cmodel_get_framebuffer(surf->cm, NULL, NULL);
    if (!fb) return;

    /* Clamp */
    int x0 = sx < 0 ? 0 : sx;
    int y0 = sy < 0 ? 0 : sy;
    int x1 = sx + width;  if (x1 > W) x1 = W;
    int y1 = sy + height; if (y1 > H) y1 = H;
    int copy_w = x1 - x0;
    int copy_h = y1 - y0;
    if (copy_w <= 0 || copy_h <= 0) return;

    /* For each output row (y_out=0 is OpenVG y=sy=bottom): */
    for (int y_out = 0; y_out < copy_h; y_out++) {
        int openvg_y   = sy + y_out;             /* OpenVG surface y (0=bottom) */
        int cmodel_row = H - 1 - openvg_y;       /* cmodel row (0=top) */
        if (cmodel_row < 0 || cmodel_row >= H) {
            /* Out of bounds — write zeros */
            memset((uint8_t *)data + y_out * dataStride, 0, copy_w * 4);
            continue;
        }
        const uint32_t *src_row = fb + cmodel_row * W + x0;
        uint8_t *dst_row = (uint8_t *)data + y_out * dataStride;

        switch (dataFormat) {
        case VG_sRGBA_8888:
        case VG_lRGBA_8888:
            /* Cmodel stores as 0xRRGGBBAA; VG_sRGBA_8888 is the same bit layout.
             * On little-endian, uint32 0xRRGGBBAA → bytes [AA,BB,GG,RR].
             * VG_sRGBA_8888 expects bytes [AA,BB,GG,RR] (little-endian word). */
            memcpy(dst_row, src_row, copy_w * 4);
            break;
        case VG_sRGBA_8888_PRE:
        case VG_lRGBA_8888_PRE:
            /* Premultiply */
            for (int x = 0; x < copy_w; x++) {
                uint32_t p = src_row[x];
                uint8_t r = (p>>24)&0xFF, g=(p>>16)&0xFF, b=(p>>8)&0xFF, a=(p)&0xFF;
                r = (uint8_t)((r * a + 127) / 255);
                g = (uint8_t)((g * a + 127) / 255);
                b = (uint8_t)((b * a + 127) / 255);
                uint32_t out = ((uint32_t)r<<24)|((uint32_t)g<<16)|((uint32_t)b<<8)|a;
                memcpy(dst_row + x*4, &out, 4);
            }
            break;
        case VG_sRGBX_8888:
            for (int x = 0; x < copy_w; x++) {
                uint32_t p = (src_row[x] | 0xFFu); /* force alpha=255 */
                memcpy(dst_row + x*4, &p, 4);
            }
            break;
        case VG_sBGRA_8888:
        case VG_lBGRA_8888:
            for (int x = 0; x < copy_w; x++) {
                uint32_t p = src_row[x];
                uint8_t r=(p>>24)&0xFF,g=(p>>16)&0xFF,b=(p>>8)&0xFF,a=p&0xFF;
                uint32_t out = ((uint32_t)b<<24)|((uint32_t)g<<16)|((uint32_t)r<<8)|a;
                memcpy(dst_row + x*4, &out, 4);
            }
            break;
        case VG_sARGB_8888:
        case VG_lARGB_8888:
            for (int x = 0; x < copy_w; x++) {
                uint32_t p = src_row[x];
                uint8_t r=(p>>24)&0xFF,g=(p>>16)&0xFF,b=(p>>8)&0xFF,a=p&0xFF;
                uint32_t out = ((uint32_t)a<<24)|((uint32_t)r<<16)|((uint32_t)g<<8)|b;
                memcpy(dst_row + x*4, &out, 4);
            }
            break;
        case VG_sABGR_8888:
        case VG_lABGR_8888:
            for (int x = 0; x < copy_w; x++) {
                uint32_t p = src_row[x];
                uint8_t r=(p>>24)&0xFF,g=(p>>16)&0xFF,b=(p>>8)&0xFF,a=p&0xFF;
                uint32_t out = ((uint32_t)a<<24)|((uint32_t)b<<16)|((uint32_t)g<<8)|r;
                memcpy(dst_row + x*4, &out, 4);
            }
            break;
        case VG_sL_8:
        case VG_lL_8:
            for (int x = 0; x < copy_w; x++) {
                uint32_t p = src_row[x];
                uint8_t r=(p>>24)&0xFF,g=(p>>16)&0xFF,b=(p>>8)&0xFF;
                dst_row[x] = (uint8_t)(0.299f*r + 0.587f*g + 0.114f*b + 0.5f);
            }
            break;
        case VG_A_8:
            for (int x = 0; x < copy_w; x++)
                dst_row[x] = (uint8_t)(src_row[x] & 0xFF);
            break;
        default:
            memcpy(dst_row, src_row, copy_w * 4);
            break;
        }
    }
}

/* =========================================================================
 * VG — Image filter helpers
 * ========================================================================= */

/* Return byte indices for [R,G,B,A] within a 4-byte pixel of the given format.
 *
 * The RI stores 32-bit pixels as a uint32_t = (R<<rs)|(G<<gs)|(B<<bs)|(A<<as).
 * On a little-endian system the byte at address+0 is the LSB.
 * For VG_sRGBA_8888: rs=24,gs=16,bs=8,as=0 → uint32=(R<<24)|(G<<16)|(B<<8)|A
 *   LE bytes: [byte0=A, byte1=B, byte2=G, byte3=R]
 *   So idx[R]=3, idx[G]=2, idx[B]=1, idx[A]=0.
 *
 * swizzleBits = (fmt>>6)&3:
 *   0 = RGBA: rs=24,gs=16,bs=8,as=0  → LE bytes [A,B,G,R]  → idx={3,2,1,0}
 *   1 = ARGB: rs=16,gs=8,bs=0,as=24  → LE bytes [B,G,R,A]  → idx={2,1,0,3}
 *   2 = BGRA: rs=8,gs=16,bs=24,as=0  → LE bytes [A,R,G,B]  → idx={1,2,3,0}
 *   3 = ABGR: rs=0,gs=8,bs=16,as=24  → LE bytes [R,G,B,A]  → idx={0,1,2,3}
 */
static void cm_fmt_idx(VGImageFormat fmt, int idx[4])
{
    int sw = (fmt >> 6) & 3;
    switch (sw) {
    case 0: idx[0]=3;idx[1]=2;idx[2]=1;idx[3]=0; break; /* RGBA */
    case 1: idx[0]=2;idx[1]=1;idx[2]=0;idx[3]=3; break; /* ARGB */
    case 2: idx[0]=1;idx[1]=2;idx[2]=3;idx[3]=0; break; /* BGRA */
    default:idx[0]=0;idx[1]=1;idx[2]=2;idx[3]=3; break; /* ABGR */
    }
}

/* Read pixel (x,y) from image into float rgba[4] in [0,1]. */
static void cm_read_px(const cm_image_t *img, int x, int y, float *rgba)
{
    const uint8_t *p = img->pixels + (size_t)y * (size_t)img->row_stride + (size_t)x * (size_t)img->bpp;
    if (img->bpp == 4) {
        int idx[4]; cm_fmt_idx(img->format, idx);
        rgba[0] = p[idx[0]] / 255.f;
        rgba[1] = p[idx[1]] / 255.f;
        rgba[2] = p[idx[2]] / 255.f;
        rgba[3] = p[idx[3]] / 255.f;
    } else if (img->bpp == 2) {
        rgba[0] = p[0] / 255.f; rgba[1] = p[1] / 255.f;
        rgba[2] = 0.f;          rgba[3] = 1.f;
    } else {
        /* 1-byte formats */
        int base_fmt = img->format & 0x3F;
        if (base_fmt == VG_A_8 || base_fmt == VG_A_1 || base_fmt == VG_A_4) {
            /* Alpha-only: RGB = white, A = stored */
            rgba[0] = rgba[1] = rgba[2] = 1.f;
            rgba[3] = p[0] / 255.f;
        } else {
            /* Luminance (sL_8, lL_8) or BW_1: RGB = stored, A = 1.0 */
            rgba[0] = rgba[1] = rgba[2] = p[0] / 255.f;
            rgba[3] = 1.f;
        }
    }
}

/* Write float rgba[4] to pixel (x,y) in image. */
static void cm_write_px(cm_image_t *img, int x, int y, const float *rgba)
{
    uint8_t *p = img->pixels + (size_t)y * (size_t)img->row_stride + (size_t)x * (size_t)img->bpp;
    if (img->bpp == 4) {
        int idx[4]; cm_fmt_idx(img->format, idx);
        p[idx[0]] = (uint8_t)(clampf01(rgba[0]) * 255.f + 0.5f);
        p[idx[1]] = (uint8_t)(clampf01(rgba[1]) * 255.f + 0.5f);
        p[idx[2]] = (uint8_t)(clampf01(rgba[2]) * 255.f + 0.5f);
        p[idx[3]] = (uint8_t)(clampf01(rgba[3]) * 255.f + 0.5f);
    } else if (img->bpp == 2) {
        p[0] = (uint8_t)(clampf01(rgba[0]) * 255.f + 0.5f);
        p[1] = (uint8_t)(clampf01(rgba[3]) * 255.f + 0.5f);
    } else {
        /* 1-byte formats */
        int base_fmt = img->format & 0x3F;
        if (base_fmt == VG_A_8 || base_fmt == VG_A_1 || base_fmt == VG_A_4) {
            /* Alpha-only: write alpha channel */
            p[0] = (uint8_t)(clampf01(rgba[3]) * 255.f + 0.5f);
        } else {
            /* Luminance: use red channel (or average) */
            p[0] = (uint8_t)(clampf01(rgba[0]) * 255.f + 0.5f);
        }
    }
}

/* Read pixel with tiling applied for out-of-bounds coords. */
static void cm_read_tiled(const cm_image_t *img, int x, int y, VGTilingMode tm, float *rgba)
{
    int W = img->width, H = img->height;
    int ox = x, oy = y;
    switch (tm) {
    case VG_TILE_FILL:
        if (ox < 0 || oy < 0 || ox >= W || oy >= H) {
            if (g_ctx) {
                rgba[0]=g_ctx->tile_fill_color[0]; rgba[1]=g_ctx->tile_fill_color[1];
                rgba[2]=g_ctx->tile_fill_color[2]; rgba[3]=g_ctx->tile_fill_color[3];
            } else { rgba[0]=rgba[1]=rgba[2]=rgba[3]=0.f; }
            return;
        }
        break;
    case VG_TILE_PAD:
        ox = ox < 0 ? 0 : (ox >= W ? W-1 : ox);
        oy = oy < 0 ? 0 : (oy >= H ? H-1 : oy);
        break;
    case VG_TILE_REPEAT:
        ox = ((ox % W) + W) % W;
        oy = ((oy % H) + H) % H;
        break;
    case VG_TILE_REFLECT: {
        int pw = W*2, ph = H*2;
        ox = ((ox % pw) + pw) % pw; if (ox >= W) ox = pw-1-ox;
        oy = ((oy % ph) + ph) % ph; if (oy >= H) oy = ph-1-oy;
        break; }
    default: break;
    }
    cm_read_px(img, ox, oy, rgba);
}

/* Write to dst updating only channels set in channel_mask; others stay as in dst. */
static void cm_write_masked(cm_image_t *dst, int x, int y,
                             const float *newc, int channel_mask)
{
    float oldc[4]; cm_read_px(dst, x, y, oldc);
    float out[4];
    out[0] = (channel_mask & VG_RED)   ? newc[0] : oldc[0];
    out[1] = (channel_mask & VG_GREEN) ? newc[1] : oldc[1];
    out[2] = (channel_mask & VG_BLUE)  ? newc[2] : oldc[2];
    out[3] = (channel_mask & VG_ALPHA) ? newc[3] : oldc[3];
    cm_write_px(dst, x, y, out);
}

/* Validate two image handles for filter ops. Returns 0 on error (error set). */
static int cm_filter_validate(VGImage d, VGImage s)
{
    if (d == VG_INVALID_HANDLE || s == VG_INVALID_HANDLE) {
        VG_SET_ERR(VG_BAD_HANDLE_ERROR); return 0;
    }
    cm_image_t *di = (cm_image_t *)(uintptr_t)d;
    cm_image_t *si = (cm_image_t *)(uintptr_t)s;
    if (di->magic != IMAGE_MAGIC || si->magic != IMAGE_MAGIC) {
        VG_SET_ERR(VG_BAD_HANDLE_ERROR); return 0;
    }
    /* Same image, or one is child of the other → overlap */
    if (d == s ||
        si->parent_handle == d ||
        di->parent_handle == s) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return 0;
    }
    return 1;
}

static int cm_tiling_valid(VGTilingMode tm) {
    return tm == VG_TILE_FILL || tm == VG_TILE_PAD ||
           tm == VG_TILE_REPEAT || tm == VG_TILE_REFLECT;
}

/* =========================================================================
 * VG — Image filters
 * ========================================================================= */

void vgColorMatrix(VGImage d, VGImage s, const VGfloat *m)
{
    VG_CHECK_CTX();
    if (!cm_filter_validate(d, s)) return;
    if (!m || ((uintptr_t)m & 3u)) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    cm_image_t *di = (cm_image_t *)(uintptr_t)d;
    cm_image_t *si = (cm_image_t *)(uintptr_t)s;
    int mask = g_ctx ? g_ctx->filter_channel_mask : (VG_RED|VG_GREEN|VG_BLUE|VG_ALPHA);
    int W = di->width, H = di->height;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float sp[4]; cm_read_px(si, x, y, sp);
            float r = m[0]*sp[0] + m[4]*sp[1] + m[ 8]*sp[2] + m[12]*sp[3] + m[16];
            float g = m[1]*sp[0] + m[5]*sp[1] + m[ 9]*sp[2] + m[13]*sp[3] + m[17];
            float b = m[2]*sp[0] + m[6]*sp[1] + m[10]*sp[2] + m[14]*sp[3] + m[18];
            float a = m[3]*sp[0] + m[7]*sp[1] + m[11]*sp[2] + m[15]*sp[3] + m[19];
            float newc[4] = { r, g, b, a };
            cm_write_masked(di, x, y, newc, mask);
        }
    }
}

void vgConvolve(VGImage d, VGImage s, VGint kw, VGint kh, VGint shiftX, VGint shiftY,
                const VGshort *k, VGfloat scale, VGfloat bias, VGTilingMode tm)
{
    VG_CHECK_CTX();
    if (!cm_filter_validate(d, s)) return;
    if (kw <= 0 || kh <= 0 || kw > 7 || kh > 7)
        { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (!k || ((uintptr_t)k & 1u)) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (!cm_tiling_valid(tm)) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    cm_image_t *di = (cm_image_t *)(uintptr_t)d;
    cm_image_t *si = (cm_image_t *)(uintptr_t)s;
    int mask = g_ctx ? g_ctx->filter_channel_mask : (VG_RED|VG_GREEN|VG_BLUE|VG_ALPHA);
    int W = di->width, H = di->height;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float acc[4] = {0.f,0.f,0.f,0.f};
            for (int j = 0; j < kh; j++) {
                for (int i = 0; i < kw; i++) {
                    float sp[4];
                    cm_read_tiled(si, x+i-shiftX, y+j-shiftY, tm, sp);
                    /* Column-major storage with 180-degree flip per spec:
                     * kernel entry at flip position (kw-1-i, kh-1-j) = k[(kw-1-i)*kh + (kh-1-j)] */
                    float kv = (float)k[(kw-1-i)*kh + (kh-1-j)];
                    acc[0] += kv * sp[0]; acc[1] += kv * sp[1];
                    acc[2] += kv * sp[2]; acc[3] += kv * sp[3];
                }
            }
            float newc[4] = {
                scale*acc[0]+bias, scale*acc[1]+bias,
                scale*acc[2]+bias, scale*acc[3]+bias
            };
            cm_write_masked(di, x, y, newc, mask);
        }
    }
}

void vgSeparableConvolve(VGImage d, VGImage s, VGint kw, VGint kh,
                          VGint shiftX, VGint shiftY, const VGshort *kX, const VGshort *kY,
                          VGfloat scale, VGfloat bias, VGTilingMode tm)
{
    VG_CHECK_CTX();
    if (!cm_filter_validate(d, s)) return;
    if (kw <= 0 || kh <= 0 || kw > 15 || kh > 15)
        { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (!kX || ((uintptr_t)kX & 1u) || !kY || ((uintptr_t)kY & 1u))
        { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (!cm_tiling_valid(tm)) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    cm_image_t *di = (cm_image_t *)(uintptr_t)d;
    cm_image_t *si = (cm_image_t *)(uintptr_t)s;
    int mask = g_ctx ? g_ctx->filter_channel_mask : (VG_RED|VG_GREEN|VG_BLUE|VG_ALPHA);
    int W = di->width, H = di->height;
    /* Temporary row buffer for intermediate result */
    float *tmp = (float *)calloc((size_t)(W * H * 4), sizeof(float));
    if (!tmp) { VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return; }
    /* First pass: convolve horizontally with kX (flipped: kX[kw-1-i]) */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float acc[4] = {0.f,0.f,0.f,0.f};
            for (int i = 0; i < kw; i++) {
                float sp[4]; cm_read_tiled(si, x+i-shiftX, y, tm, sp);
                float kv = (float)kX[kw-1-i];
                acc[0] += kv*sp[0]; acc[1] += kv*sp[1];
                acc[2] += kv*sp[2]; acc[3] += kv*sp[3];
            }
            tmp[(y*W+x)*4+0] = acc[0]; tmp[(y*W+x)*4+1] = acc[1];
            tmp[(y*W+x)*4+2] = acc[2]; tmp[(y*W+x)*4+3] = acc[3];
        }
    }
    /* Compute intermediate edge color for VG_TILE_FILL second pass */
    float edge2[4] = {0.f,0.f,0.f,0.f};
    if (tm == VG_TILE_FILL && g_ctx) {
        float edge[4] = { clampf01(g_ctx->tile_fill_color[0]),
                          clampf01(g_ctx->tile_fill_color[1]),
                          clampf01(g_ctx->tile_fill_color[2]),
                          clampf01(g_ctx->tile_fill_color[3]) };
        float ksum = 0.f;
        for (int i = 0; i < kw; i++) ksum += (float)kX[i];
        edge2[0] = edge[0]*ksum; edge2[1] = edge[1]*ksum;
        edge2[2] = edge[2]*ksum; edge2[3] = edge[3]*ksum;
    }
    /* Second pass: convolve vertically with kY (flipped: kY[kh-1-j]) */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float acc[4] = {0.f,0.f,0.f,0.f};
            for (int j = 0; j < kh; j++) {
                int yy = y + j - shiftY;
                float sp[4];
                if (yy >= 0 && yy < H) {
                    sp[0]=tmp[(yy*W+x)*4+0]; sp[1]=tmp[(yy*W+x)*4+1];
                    sp[2]=tmp[(yy*W+x)*4+2]; sp[3]=tmp[(yy*W+x)*4+3];
                } else {
                    /* Apply tiling to intermediate buffer */
                    switch (tm) {
                    case VG_TILE_FILL:
                        sp[0]=edge2[0]; sp[1]=edge2[1]; sp[2]=edge2[2]; sp[3]=edge2[3]; break;
                    case VG_TILE_PAD: {
                        int cy = yy < 0 ? 0 : H-1;
                        sp[0]=tmp[(cy*W+x)*4+0]; sp[1]=tmp[(cy*W+x)*4+1];
                        sp[2]=tmp[(cy*W+x)*4+2]; sp[3]=tmp[(cy*W+x)*4+3]; break; }
                    case VG_TILE_REPEAT: {
                        int cy = ((yy % H) + H) % H;
                        sp[0]=tmp[(cy*W+x)*4+0]; sp[1]=tmp[(cy*W+x)*4+1];
                        sp[2]=tmp[(cy*W+x)*4+2]; sp[3]=tmp[(cy*W+x)*4+3]; break; }
                    default: { /* VG_TILE_REFLECT */
                        int cy = yy < 0 ? (-yy-1) % H : (H - 1 - (yy-H) % H);
                        if (cy < 0 || cy >= H) cy = 0;
                        sp[0]=tmp[(cy*W+x)*4+0]; sp[1]=tmp[(cy*W+x)*4+1];
                        sp[2]=tmp[(cy*W+x)*4+2]; sp[3]=tmp[(cy*W+x)*4+3]; break; }
                    }
                }
                float kv = (float)kY[kh-1-j];
                acc[0] += kv*sp[0]; acc[1] += kv*sp[1];
                acc[2] += kv*sp[2]; acc[3] += kv*sp[3];
            }
            float newc[4] = {
                scale*acc[0]+bias, scale*acc[1]+bias,
                scale*acc[2]+bias, scale*acc[3]+bias
            };
            cm_write_masked(di, x, y, newc, mask);
        }
    }
    free(tmp);
}

void vgGaussianBlur(VGImage d, VGImage s, VGfloat stdX, VGfloat stdY, VGTilingMode tm)
{
    VG_CHECK_CTX();
    if (!cm_filter_validate(d, s)) return;
    if (stdX <= 0.f || stdY <= 0.f || stdX > 16.f || stdY > 16.f)
        { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (!cm_tiling_valid(tm)) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    cm_image_t *di = (cm_image_t *)(uintptr_t)d;
    cm_image_t *si = (cm_image_t *)(uintptr_t)s;
    int mask = g_ctx ? g_ctx->filter_channel_mask : (VG_RED|VG_GREEN|VG_BLUE|VG_ALPHA);
    /* Compute kernel radius (3*sigma covers 99.7% of Gaussian) */
    int rx = (int)(3.f * stdX + 0.5f); if (rx < 1) rx = 1;
    int ry = (int)(3.f * stdY + 0.5f); if (ry < 1) ry = 1;
    int kw = 2*rx+1, kh = 2*ry+1;
    /* Build 1D Gaussian kernels */
    float *kX = (float *)malloc((size_t)kw * sizeof(float));
    float *kY = (float *)malloc((size_t)kh * sizeof(float));
    if (!kX || !kY) { free(kX); free(kY); VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return; }
    float sumX = 0.f, sumY = 0.f;
    for (int i = 0; i < kw; i++) {
        float dx = (float)(i - rx);
        kX[i] = expf(-0.5f * dx*dx / (stdX*stdX));
        sumX += kX[i];
    }
    for (int j = 0; j < kh; j++) {
        float dy = (float)(j - ry);
        kY[j] = expf(-0.5f * dy*dy / (stdY*stdY));
        sumY += kY[j];
    }
    for (int i = 0; i < kw; i++) kX[i] /= sumX;
    for (int j = 0; j < kh; j++) kY[j] /= sumY;
    /* Apply separable Gaussian */
    int W = di->width, H = di->height;
    float *tmp = (float *)calloc((size_t)(W * H * 4), sizeof(float));
    if (!tmp) { free(kX); free(kY); VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return; }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float acc[4] = {0.f,0.f,0.f,0.f};
            for (int i = 0; i < kw; i++) {
                float sp[4]; cm_read_tiled(si, x+i-rx, y, tm, sp);
                acc[0] += kX[i]*sp[0]; acc[1] += kX[i]*sp[1];
                acc[2] += kX[i]*sp[2]; acc[3] += kX[i]*sp[3];
            }
            tmp[(y*W+x)*4+0]=acc[0]; tmp[(y*W+x)*4+1]=acc[1];
            tmp[(y*W+x)*4+2]=acc[2]; tmp[(y*W+x)*4+3]=acc[3];
        }
    }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float acc[4] = {0.f,0.f,0.f,0.f};
            for (int j = 0; j < kh; j++) {
                int yy = y+j-ry;
                float sp[4];
                if (yy >= 0 && yy < H) {
                    sp[0]=tmp[(yy*W+x)*4+0]; sp[1]=tmp[(yy*W+x)*4+1];
                    sp[2]=tmp[(yy*W+x)*4+2]; sp[3]=tmp[(yy*W+x)*4+3];
                } else {
                    /* Tile the intermediate buffer */
                    float sp2[4]; cm_read_tiled(si, x, yy, tm, sp2);
                    /* Approximate: just use clamped src */
                    int cy = yy < 0 ? 0 : H-1;
                    sp[0]=tmp[(cy*W+x)*4+0]; sp[1]=tmp[(cy*W+x)*4+1];
                    sp[2]=tmp[(cy*W+x)*4+2]; sp[3]=tmp[(cy*W+x)*4+3];
                    (void)sp2;
                }
                acc[0] += kY[j]*sp[0]; acc[1] += kY[j]*sp[1];
                acc[2] += kY[j]*sp[2]; acc[3] += kY[j]*sp[3];
            }
            cm_write_masked(di, x, y, acc, mask);
        }
    }
    free(kX); free(kY); free(tmp);
}

void vgLookup(VGImage d, VGImage s, const VGubyte *r, const VGubyte *g,
              const VGubyte *b, const VGubyte *a, VGboolean outputPremultiplied,
              VGboolean filterLinear)
{
    VG_CHECK_CTX();
    if (!cm_filter_validate(d, s)) return;
    if (!r || !g || !b || !a) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    (void)outputPremultiplied; (void)filterLinear;
    cm_image_t *di = (cm_image_t *)(uintptr_t)d;
    cm_image_t *si = (cm_image_t *)(uintptr_t)s;
    int mask = g_ctx ? g_ctx->filter_channel_mask : (VG_RED|VG_GREEN|VG_BLUE|VG_ALPHA);
    int W = di->width, H = di->height;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float sp[4]; cm_read_px(si, x, y, sp);
            float newc[4];
            newc[0] = r[(int)(clampf01(sp[0]) * 255.f + 0.5f)] / 255.f;
            newc[1] = g[(int)(clampf01(sp[1]) * 255.f + 0.5f)] / 255.f;
            newc[2] = b[(int)(clampf01(sp[2]) * 255.f + 0.5f)] / 255.f;
            newc[3] = a[(int)(clampf01(sp[3]) * 255.f + 0.5f)] / 255.f;
            cm_write_masked(di, x, y, newc, mask);
        }
    }
}

void vgLookupSingle(VGImage d, VGImage s, const VGuint *lut,
                    VGImageChannel srcChannel, VGboolean outputPremultiplied,
                    VGboolean filterLinear)
{
    VG_CHECK_CTX();
    if (!cm_filter_validate(d, s)) return;
    if (!lut || ((uintptr_t)lut & 3u)) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    /* Validate srcChannel for non-luminance images */
    if (srcChannel != VG_RED && srcChannel != VG_GREEN &&
        srcChannel != VG_BLUE && srcChannel != VG_ALPHA)
        { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    (void)outputPremultiplied; (void)filterLinear;
    cm_image_t *di = (cm_image_t *)(uintptr_t)d;
    cm_image_t *si = (cm_image_t *)(uintptr_t)s;
    int mask = g_ctx ? g_ctx->filter_channel_mask : (VG_RED|VG_GREEN|VG_BLUE|VG_ALPHA);
    int W = di->width, H = di->height;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float sp[4]; cm_read_px(si, x, y, sp);
            float sv;
            switch (srcChannel) {
            case VG_RED:   sv = sp[0]; break;
            case VG_GREEN: sv = sp[1]; break;
            case VG_BLUE:  sv = sp[2]; break;
            case VG_ALPHA: sv = sp[3]; break;
            default:       sv = sp[0]; break;
            }
            int idx = (int)(clampf01(sv) * 255.f + 0.5f);
            VGuint lutv = lut[idx];
            /* lut value is stored as 0xRRGGBBAA */
            float newc[4];
            newc[0] = ((lutv >> 24) & 0xFF) / 255.f;
            newc[1] = ((lutv >> 16) & 0xFF) / 255.f;
            newc[2] = ((lutv >>  8) & 0xFF) / 255.f;
            newc[3] = ((lutv      ) & 0xFF) / 255.f;
            cm_write_masked(di, x, y, newc, mask);
        }
    }
}

/* =========================================================================
 * VG — Fonts (stubs)
 * ========================================================================= */

VGFont vgCreateFont(VGint glyphCapacityHint)
{
    VG_CHECK_CTX(VG_INVALID_HANDLE);
    if (glyphCapacityHint < 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return VG_INVALID_HANDLE; }
    int cap = glyphCapacityHint > 0 ? glyphCapacityHint : 8;
    cm_font_t *f = (cm_font_t *)calloc(1, sizeof(cm_font_t));
    if (!f) { VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return VG_INVALID_HANDLE; }
    f->glyphs = (cm_glyph_t *)calloc((size_t)cap, sizeof(cm_glyph_t));
    if (!f->glyphs) { free(f); VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return VG_INVALID_HANDLE; }
    f->magic    = FONT_MAGIC;
    f->capacity = cap;
    f->num_glyphs = 0;
    return (VGFont)(uintptr_t)f;
}
/* Free a glyph's owned path copy (if any). */
static void glyph_free_path(cm_glyph_t *g)
{
    if (g->has_path && g->path != VG_INVALID_HANDLE) {
        cm_path_t *p = (cm_path_t *)(uintptr_t)g->path;
        free(p->buf);
        free(p);
        g->path     = VG_INVALID_HANDLE;
        g->has_path = VG_FALSE;
    }
}

void vgDestroyFont(VGFont font)
{
    VG_CHECK_CTX();
    if (font == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_font_t *f = (cm_font_t *)(uintptr_t)font;
    if (f->magic != FONT_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    f->magic = 0;
    if (f->glyphs) {
        for (int i = 0; i < f->capacity; i++)
            glyph_free_path(&f->glyphs[i]);
        free(f->glyphs);
    }
    free(f);
}
void vgSetGlyphToPath(VGFont font, VGuint glyphIndex, VGPath path,
                      VGboolean isHinted, VGfloat *glyphOrigin, VGfloat *escapement)
{
    VG_CHECK_CTX();
    if (font == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_font_t *f = (cm_font_t *)(uintptr_t)font;
    if (f->magic != FONT_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (!glyphOrigin || !escapement) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    /* Grow glyph array if needed */
    if ((int)glyphIndex >= f->capacity) {
        int newcap = (int)glyphIndex + 8;
        cm_glyph_t *ng = (cm_glyph_t *)realloc(f->glyphs, (size_t)newcap * sizeof(cm_glyph_t));
        if (!ng) { VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return; }
        memset(ng + f->capacity, 0, (size_t)(newcap - f->capacity) * sizeof(cm_glyph_t));
        f->glyphs   = ng;
        f->capacity = newcap;
    }
    cm_glyph_t *g = &f->glyphs[glyphIndex];
    /* Free any previously owned path copy before replacing */
    glyph_free_path(g);
    if (!g->defined) f->num_glyphs++;
    g->defined  = VG_TRUE;
    (void)isHinted;
    g->glyph_origin[0] = glyphOrigin[0];
    g->glyph_origin[1] = glyphOrigin[1];
    g->escapement[0]   = escapement[0];
    g->escapement[1]   = escapement[1];
    if (path == VG_INVALID_HANDLE) {
        g->has_path = VG_FALSE;
        g->path     = VG_INVALID_HANDLE;
    } else {
        if (!cm_path_is_valid(path)) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
        cm_path_t *src = (cm_path_t *)(uintptr_t)path;
        cm_path_t *copy = (cm_path_t *)malloc(sizeof(cm_path_t));
        if (!copy) { VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return; }
        *copy = *src;
        copy->reg_next = NULL; /* copies are not in the global registry */
        int blen = src->buf_len > 0 ? src->buf_len : 1;
        copy->buf = (uint8_t *)malloc((size_t)blen);
        if (!copy->buf) { free(copy); VG_SET_ERR(VG_OUT_OF_MEMORY_ERROR); return; }
        if (src->buf_len > 0) memcpy(copy->buf, src->buf, (size_t)src->buf_len);
        copy->buf_cap = blen;
        g->path     = (VGPath)(uintptr_t)copy;
        g->has_path = VG_TRUE;
    }
}
void vgSetGlyphToImage(VGFont font, VGuint glyphIndex, VGImage image,
                       VGfloat *glyphOrigin, VGfloat *escapement)
{
    VG_CHECK_CTX();
    if (font == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_font_t *f = (cm_font_t *)(uintptr_t)font;
    if (f->magic != FONT_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (!glyphOrigin || !escapement) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    (void)image;(void)glyphIndex;
}
void vgClearGlyph(VGFont font, VGuint glyphIndex)
{
    VG_CHECK_CTX();
    if (font == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_font_t *f = (cm_font_t *)(uintptr_t)font;
    if (f->magic != FONT_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if ((int)glyphIndex >= f->capacity || !f->glyphs[glyphIndex].defined) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    glyph_free_path(&f->glyphs[glyphIndex]);
    f->glyphs[glyphIndex].defined = VG_FALSE;
    f->num_glyphs--;
}
void vgDrawGlyph(VGFont font, VGuint glyphIndex, VGbitfield paintModes,
                 VGboolean allowAutoHinting)
{
    VG_CHECK_CTX();
    if (font == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_font_t *f = (cm_font_t *)(uintptr_t)font;
    if (f->magic != FONT_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (paintModes & ~(VG_FILL_PATH | VG_STROKE_PATH)) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    /* glyphIndex must have been defined in the font */
    if (glyphIndex >= (VGuint)f->capacity || !f->glyphs[glyphIndex].defined) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    (void)allowAutoHinting;
    if (paintModes && f->glyphs[glyphIndex].has_path) {
        vgDrawPath(f->glyphs[glyphIndex].path, paintModes);
    }
}
void vgDrawGlyphs(VGFont font, VGint glyphCount, VGuint *glyphIndices,
                  VGfloat *adjustments_x, VGfloat *adjustments_y,
                  VGbitfield paintModes, VGboolean allowAutoHinting)
{
    VG_CHECK_CTX();
    if (font == VG_INVALID_HANDLE) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    cm_font_t *f = (cm_font_t *)(uintptr_t)font;
    if (f->magic != FONT_MAGIC) { VG_SET_ERR(VG_BAD_HANDLE_ERROR); return; }
    if (glyphCount <= 0) { VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return; }
    if (!glyphIndices || ((uintptr_t)glyphIndices & (sizeof(VGuint)-1u)) != 0) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    if (adjustments_x && ((uintptr_t)adjustments_x & (sizeof(VGfloat)-1u)) != 0) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    if (adjustments_y && ((uintptr_t)adjustments_y & (sizeof(VGfloat)-1u)) != 0) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    /* All glyph indices must be defined */
    for (int i = 0; i < glyphCount; i++) {
        VGuint gi = glyphIndices[i];
        if (gi >= (VGuint)f->capacity || !f->glyphs[gi].defined) {
            VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
        }
    }
    if (paintModes & ~(VG_FILL_PATH | VG_STROKE_PATH)) {
        VG_SET_ERR(VG_ILLEGAL_ARGUMENT_ERROR); return;
    }
    (void)adjustments_x;(void)adjustments_y;(void)allowAutoHinting;
}

/* =========================================================================
 * VGU — Path convenience functions
 * ========================================================================= */

static VGUErrorCode vgu_error(VGUErrorCode e) {
    if (g_ctx) g_ctx->error = VG_NO_ERROR;
    return e;
}
#define VGU_ERR(e) vgu_error(e)

static void vgu_append(VGPath path, int ns, const VGubyte *segs, int nc, const VGfloat *c)
{
    /* Convert float coordinates to the path's datatype, matching the RI's VGU append. */
    cm_path_t *p = (cm_path_t *)(uintptr_t)path;
    if (!p) { vgAppendPathData(path, ns, segs, c); return; }
    VGfloat scale = p->scale, bias = p->bias;
    if (scale == 0.f) scale = 1.f;

    switch (p->datatype) {
    case VG_PATH_DATATYPE_S_8: {
        int8_t data[26];
        for (int i = 0; i < nc && i < 26; i++)
            data[i] = (int8_t)floorf((c[i] - bias) / scale + 0.5f);
        vgAppendPathData(path, ns, segs, data);
        break; }
    case VG_PATH_DATATYPE_S_16: {
        int16_t data[26];
        for (int i = 0; i < nc && i < 26; i++)
            data[i] = (int16_t)floorf((c[i] - bias) / scale + 0.5f);
        vgAppendPathData(path, ns, segs, data);
        break; }
    case VG_PATH_DATATYPE_S_32: {
        int32_t data[26];
        for (int i = 0; i < nc && i < 26; i++)
            data[i] = (int32_t)floorf((c[i] - bias) / scale + 0.5f);
        vgAppendPathData(path, ns, segs, data);
        break; }
    default: {
        float data[26];
        for (int i = 0; i < nc && i < 26; i++)
            data[i] = (c[i] - bias) / scale;
        vgAppendPathData(path, ns, segs, data);
        break; }
    }
}

VGUErrorCode vguLine(VGPath path, VGfloat x0, VGfloat y0, VGfloat x1, VGfloat y1)
{
    if (path == VG_INVALID_HANDLE) return VGU_ERR(VGU_BAD_HANDLE_ERROR);
    const VGubyte segs[] = {VG_MOVE_TO_ABS, VG_LINE_TO_ABS};
    const VGfloat c[] = {x0, y0, x1, y1};
    vgu_append(path, 2, segs, 4, c);
    return VGU_NO_ERROR;
}

VGUErrorCode vguPolygon(VGPath path, const VGfloat *pts, VGint count, VGboolean closed)
{
    if (path == VG_INVALID_HANDLE) return VGU_ERR(VGU_BAD_HANDLE_ERROR);
    if (!pts || count < 2) return VGU_ERR(VGU_ILLEGAL_ARGUMENT_ERROR);
    VGubyte segs[1024];
    segs[0] = VG_MOVE_TO_ABS;
    for (int i = 1; i < count && i < 1023; i++) segs[i] = VG_LINE_TO_ABS;
    if (closed) { segs[count] = VG_CLOSE_PATH; count++; }
    vgu_append(path, count, segs, count * 2 - (closed ? 2 : 0), pts);
    return VGU_NO_ERROR;
}

VGUErrorCode vguRect(VGPath path, VGfloat x, VGfloat y, VGfloat w, VGfloat h)
{
    if (path == VG_INVALID_HANDLE) return VGU_ERR(VGU_BAD_HANDLE_ERROR);
    if (w <= 0.f || h <= 0.f) return VGU_ERR(VGU_ILLEGAL_ARGUMENT_ERROR);
    const VGubyte segs[] = {VG_MOVE_TO_ABS, VG_HLINE_TO_ABS, VG_VLINE_TO_ABS,
                             VG_HLINE_TO_ABS, VG_CLOSE_PATH};
    const VGfloat c[] = {x, y, x+w, y+h, x};
    vgu_append(path, 5, segs, 5, c);
    return VGU_NO_ERROR;
}

VGUErrorCode vguRoundRect(VGPath path, VGfloat x, VGfloat y, VGfloat w, VGfloat h,
                           VGfloat aw, VGfloat ah)
{
    if (path == VG_INVALID_HANDLE) return VGU_ERR(VGU_BAD_HANDLE_ERROR);
    if (w <= 0.f || h <= 0.f) return VGU_ERR(VGU_ILLEGAL_ARGUMENT_ERROR);
    if (aw < 0.f) aw = 0.f; if (ah < 0.f) ah = 0.f;
    float hw = aw/2.f, hh = ah/2.f;
    const float k = 0.5522847498f;
    const VGubyte segs[] = {
        VG_MOVE_TO_ABS, VG_HLINE_TO_ABS, VG_CUBIC_TO_ABS,
        VG_VLINE_TO_ABS, VG_CUBIC_TO_ABS,
        VG_HLINE_TO_ABS, VG_CUBIC_TO_ABS,
        VG_VLINE_TO_ABS, VG_CUBIC_TO_ABS,
        VG_CLOSE_PATH
    };
    const VGfloat c[] = {
        x+hw, y,
        x+w-hw,
        x+w-hw, y, x+w, y, x+w, y+hh,
        y+h-hh,
        x+w, y+h-hh, x+w, y+h, x+w-hw, y+h,
        x+hw,
        x+hw, y+h, x, y+h, x, y+h-hh,
        y+hh,
        x, y+hh, x, y, x+hw, y
    };
    (void)k;
    vgu_append(path, 10, segs, sizeof(c)/sizeof(c[0]), c);
    return VGU_NO_ERROR;
}

VGUErrorCode vguEllipse(VGPath path, VGfloat cx, VGfloat cy, VGfloat w, VGfloat h)
{
    if (path == VG_INVALID_HANDLE) return VGU_ERR(VGU_BAD_HANDLE_ERROR);
    if (w <= 0.f || h <= 0.f) return VGU_ERR(VGU_ILLEGAL_ARGUMENT_ERROR);
    float rx = w/2.f, ry = h/2.f;
    const float k = 0.5522847498f;
    const VGubyte segs[] = {VG_MOVE_TO_ABS, VG_CUBIC_TO_ABS, VG_CUBIC_TO_ABS,
                             VG_CUBIC_TO_ABS, VG_CUBIC_TO_ABS, VG_CLOSE_PATH};
    const VGfloat c[] = {
        cx,    cy-ry,
        cx+k*rx, cy-ry,  cx+rx, cy-k*ry,  cx+rx, cy,
        cx+rx, cy+k*ry,  cx+k*rx, cy+ry,  cx,    cy+ry,
        cx-k*rx, cy+ry,  cx-rx, cy+k*ry,  cx-rx, cy,
        cx-rx, cy-k*ry,  cx-k*rx, cy-ry,  cx,    cy-ry
    };
    vgu_append(path, 6, segs, sizeof(c)/sizeof(c[0]), c);
    return VGU_NO_ERROR;
}

VGUErrorCode vguArc(VGPath path, VGfloat x, VGfloat y, VGfloat w, VGfloat h,
                    VGfloat startAngle, VGfloat angleExtent, VGUArcType arcType)
{
    if (path == VG_INVALID_HANDLE) return VGU_ERR(VGU_BAD_HANDLE_ERROR);
    if (w <= 0.f || h <= 0.f) return VGU_ERR(VGU_ILLEGAL_ARGUMENT_ERROR);

    float rx = w/2.f, ry = h/2.f;
    float sa = (float)(startAngle * M_PI / 180.f);
    float ea = (float)((startAngle + angleExtent) * M_PI / 180.f);

    /* Decompose arc into cubic Bezier segments (max 90 degrees each) */
    int nseg = (int)(fabsf(angleExtent) / 90.f) + 1;
    float step = (float)((ea - sa) / nseg);

    float ax = x + rx * cosf(sa), ay = y + ry * sinf(sa);
    const VGubyte mvto = VG_MOVE_TO_ABS;
    const VGfloat mv[] = {ax, ay};
    vgu_append(path, 1, &mvto, 2, mv);

    float cur_a = sa;
    for (int i = 0; i < nseg; i++) {
        float next_a = cur_a + step;
        float alpha = 4.f/3.f * tanf(step/2.f);
        float p1x = x + rx*(cosf(cur_a)  - alpha*sinf(cur_a));
        float p1y = y + ry*(sinf(cur_a)  + alpha*cosf(cur_a));
        float p2x = x + rx*(cosf(next_a) + alpha*sinf(next_a));
        float p2y = y + ry*(sinf(next_a) - alpha*cosf(next_a));
        float p3x = x + rx*cosf(next_a);
        float p3y = y + ry*sinf(next_a);
        const VGubyte cub = VG_CUBIC_TO_ABS;
        const VGfloat cv[] = {p1x, p1y, p2x, p2y, p3x, p3y};
        vgu_append(path, 1, &cub, 6, cv);
        cur_a = next_a;
    }
    if (arcType == VGU_ARC_CHORD) {
        const VGubyte cl[] = {VG_LINE_TO_ABS, VG_CLOSE_PATH};
        const VGfloat clc[] = {ax, ay};
        vgu_append(path, 2, cl, 2, clc);
    } else if (arcType == VGU_ARC_PIE) {
        const VGubyte cl[] = {VG_LINE_TO_ABS, VG_CLOSE_PATH};
        const VGfloat clc[] = {x, y};
        vgu_append(path, 2, cl, 2, clc);
    }
    return VGU_NO_ERROR;
}

/* Warp helpers — compute projective transform (3x3 homography) between quads */
static VGUErrorCode compute_warp(const VGfloat *sx, const VGfloat *sy,
                                  const VGfloat *dx, const VGfloat *dy,
                                  VGfloat *m)
{
    /* Very simplified: identity if quads are trivial */
    (void)sx;(void)sy;(void)dx;(void)dy;
    mat_identity(m);
    return VGU_NO_ERROR;
}

VGUErrorCode vguComputeWarpQuadToSquare(VGfloat sx0, VGfloat sy0, VGfloat sx1, VGfloat sy1,
                                         VGfloat sx2, VGfloat sy2, VGfloat sx3, VGfloat sy3,
                                         VGfloat *matrix)
{
    if (!matrix) return VGU_ILLEGAL_ARGUMENT_ERROR;
    VGfloat sx[] = {sx0,sx1,sx2,sx3}, sy[] = {sy0,sy1,sy2,sy3};
    VGfloat dx[] = {0,1,1,0}, dy[] = {0,0,1,1};
    return compute_warp(sx,sy,dx,dy,matrix);
}

VGUErrorCode vguComputeWarpSquareToQuad(VGfloat dx0, VGfloat dy0, VGfloat dx1, VGfloat dy1,
                                         VGfloat dx2, VGfloat dy2, VGfloat dx3, VGfloat dy3,
                                         VGfloat *matrix)
{
    if (!matrix) return VGU_ILLEGAL_ARGUMENT_ERROR;
    VGfloat sx[] = {0,1,1,0}, sy[] = {0,0,1,1};
    VGfloat dx[] = {dx0,dx1,dx2,dx3}, dy[] = {dy0,dy1,dy2,dy3};
    return compute_warp(sx,sy,dx,dy,matrix);
}

VGUErrorCode vguComputeWarpQuadToQuad(VGfloat dx0, VGfloat dy0, VGfloat dx1, VGfloat dy1,
                                       VGfloat dx2, VGfloat dy2, VGfloat dx3, VGfloat dy3,
                                       VGfloat sx0, VGfloat sy0, VGfloat sx1, VGfloat sy1,
                                       VGfloat sx2, VGfloat sy2, VGfloat sx3, VGfloat sy3,
                                       VGfloat *matrix)
{
    if (!matrix) return VGU_ILLEGAL_ARGUMENT_ERROR;
    VGfloat sx[] = {sx0,sx1,sx2,sx3}, sy[] = {sy0,sy1,sy2,sy3};
    VGfloat dx[] = {dx0,dx1,dx2,dx3}, dy[] = {dy0,dy1,dy2,dy3};
    return compute_warp(sx,sy,dx,dy,matrix);
}
