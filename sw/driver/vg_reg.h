/*
 * OpenVG 1.1 Hardware Accelerator - Register Definitions
 *
 * This header defines the MMIO register map for the OpenVG hardware
 * accelerator. Both the driver and the C-model include this file.
 */
#ifndef VG_REG_H
#define VG_REG_H

#include <stdint.h>

/* =========================================================================
 * Base Address (to be overridden by platform HAL)
 * ========================================================================= */
#ifndef VG_BASE_ADDR
#define VG_BASE_ADDR    0x10000000UL
#endif

/* =========================================================================
 * Register Offsets
 * ========================================================================= */

/* --- Global Control & Status --- */
#define VG_REG_CTRL             0x000   /* Global Control Register           */
#define VG_REG_STATUS           0x004   /* Global Status Register            */
#define VG_REG_IRQ_STATUS       0x008   /* Interrupt Status (write-1-clear)  */
#define VG_REG_IRQ_ENABLE       0x00C   /* Interrupt Enable Mask             */
#define VG_REG_SOFT_RESET       0x010   /* Soft Reset (write 1 to reset)     */
#define VG_REG_VERSION          0x014   /* Hardware Version (read-only)      */

/* --- Surface Configuration --- */
#define VG_REG_SURF_ADDR        0x020   /* Frame Buffer Base Address         */
#define VG_REG_SURF_STRIDE      0x024   /* Frame Buffer Stride (bytes/row)   */
#define VG_REG_SURF_WIDTH       0x028   /* Surface Width  (pixels)           */
#define VG_REG_SURF_HEIGHT      0x02C   /* Surface Height (pixels)           */
#define VG_REG_SURF_FORMAT      0x030   /* Surface Pixel Format              */

/* --- Scissor / Viewport --- */
#define VG_REG_SCISSOR_X        0x040   /* Scissor rect X origin             */
#define VG_REG_SCISSOR_Y        0x044   /* Scissor rect Y origin             */
#define VG_REG_SCISSOR_W        0x048   /* Scissor rect Width                */
#define VG_REG_SCISSOR_H        0x04C   /* Scissor rect Height               */
#define VG_REG_SCISSOR_EN       0x050   /* Scissor Enable (bit 0)            */

/* --- Affine Transformation Matrix (User-to-Surface, 3x3 as 6 floats) ---
 *  [ sx  shx tx ]
 *  [ shy sy  ty ]
 *  [  0   0   1 ]
 */
#define VG_REG_MATRIX_SX        0x060   /* Scale X (IEEE 754 float)          */
#define VG_REG_MATRIX_SHX       0x064   /* Shear X                           */
#define VG_REG_MATRIX_TX        0x068   /* Translate X                       */
#define VG_REG_MATRIX_SHY       0x06C   /* Shear Y                           */
#define VG_REG_MATRIX_SY        0x070   /* Scale Y                           */
#define VG_REG_MATRIX_TY        0x074   /* Translate Y                       */

/* --- Fill Rule & Paint Mode --- */
#define VG_REG_FILL_RULE        0x080   /* 0=NON_ZERO, 1=EVEN_ODD            */
#define VG_REG_PAINT_MODE       0x084   /* Bit 0=Fill, Bit 1=Stroke          */
#define VG_REG_BLEND_MODE       0x088   /* VGBlendMode enum value            */
#define VG_REG_IMAGE_MODE       0x08C   /* VGImageMode enum value            */
#define VG_REG_RENDERING_QUAL   0x090   /* VGRenderingQuality enum value     */

/* --- Flat Color Paint --- */
#define VG_REG_FILL_COLOR       0x0A0   /* RGBA8888 fill colour              */
#define VG_REG_STROKE_COLOR     0x0A4   /* RGBA8888 stroke colour            */

/* --- Stroke Parameters --- */
#define VG_REG_STROKE_WIDTH     0x0B0   /* Stroke line width (float)         */
#define VG_REG_STROKE_MITER     0x0B4   /* Miter limit (float)               */
#define VG_REG_STROKE_CAP       0x0B8   /* VGCapStyle  (0=Butt,1=Round,2=Sq) */
#define VG_REG_STROKE_JOIN      0x0BC   /* VGJoinStyle (0=Miter,1=Round,2=B) */
#define VG_REG_DASH_PHASE       0x0C0   /* Dash phase (float)                */
#define VG_REG_DASH_COUNT       0x0C4   /* Number of dash pattern entries    */
/* Dash pattern: 16 entries x 4 bytes each */
#define VG_REG_DASH_PATTERN(n)  (0x0D0 + ((n) & 0xF) * 4)  /* n: 0..15     */

/* --- Path DMA Descriptor --- */
#define VG_REG_PATH_ADDR        0x100   /* Path command buffer (bus addr)    */
#define VG_REG_PATH_SIZE        0x104   /* Path command buffer size (bytes)  */
#define VG_REG_PATH_SCALE       0x108   /* Path scale factor (float)         */
#define VG_REG_PATH_BIAS        0x10C   /* Path bias (float)                 */
#define VG_REG_PATH_KICK        0x110   /* Write any value to start FE       */

/* --- Gradient Paint --- */
#define VG_REG_GRAD_TYPE        0x120   /* 0=None, 1=Linear, 2=Radial        */
#define VG_REG_GRAD_X0          0x124   /* (x0,y0) start / focal point       */
#define VG_REG_GRAD_Y0          0x128
#define VG_REG_GRAD_X1          0x12C   /* (x1,y1) end point                 */
#define VG_REG_GRAD_Y1          0x130
#define VG_REG_GRAD_R           0x134   /* Radial gradient radius (float)    */
#define VG_REG_GRAD_SPREAD      0x138   /* VGColorRampSpreadMode             */
/* Color ramp: up to 16 stops, each 2 registers: offset(float)+RGBA8888 */
#define VG_REG_CRAMP_OFFSET(n)  (0x140 + ((n) & 0xF) * 8)
#define VG_REG_CRAMP_COLOR(n)   (0x144 + ((n) & 0xF) * 8)
#define VG_REG_CRAMP_COUNT      0x1C0   /* Actual number of colour stops     */

/* --- Texture / Image --- */
#define VG_REG_TEX_ADDR         0x1D0   /* Image/texture base address        */
#define VG_REG_TEX_STRIDE       0x1D4   /* Image stride (bytes/row)          */
#define VG_REG_TEX_WIDTH        0x1D8   /* Image width                       */
#define VG_REG_TEX_HEIGHT       0x1DC   /* Image height                      */
#define VG_REG_TEX_FORMAT       0x1E0   /* VGImageFormat                     */
#define VG_REG_TEX_TILING       0x1E4   /* VGTilingMode                      */

/* --- Mask Layer --- */
#define VG_REG_MASK_ADDR        0x1F0   /* Mask layer base address           */
#define VG_REG_MASK_STRIDE      0x1F4   /* Mask stride (bytes/row)           */
#define VG_REG_MASK_EN          0x1F8   /* Mask enable (bit 0)               */

/* --- Tile Engine --- */
#define VG_REG_TILE_SIZE        0x200   /* 0=32x32, 1=64x64                  */
#define VG_REG_TILE_LIST_ADDR   0x204   /* Global edge buffer base address   */
#define VG_REG_TILE_LIST_SIZE   0x208   /* Global edge buffer size (bytes)   */

/* --- Anti-Aliasing --- */
#define VG_REG_AA_SAMPLES       0x210   /* 0=1x1, 1=4x4, 2=8x8              */

/* --- High-precision solid paint (float RGBA) --- */
#define VG_REG_FILL_COLOR_R_F   0x214   /* Fill color R (float)             */
#define VG_REG_FILL_COLOR_G_F   0x218   /* Fill color G (float)             */
#define VG_REG_FILL_COLOR_B_F   0x21C   /* Fill color B (float)             */
#define VG_REG_FILL_COLOR_A_F   0x220   /* Fill color A (float)             */

/* --- Surface interpretation for path rasterization --- */
#define VG_REG_SURF_LINEAR      0x224   /* 0=sRGB, 1=linear RGB             */
#define VG_REG_SURF_PREMULT     0x228   /* 0=non-premultiplied, 1=premul    */
#define VG_REG_COLOR_XFORM_EN   0x22C   /* Apply post-sample color xform    */
#define VG_REG_COLOR_XFORM_0    0x230   /* scale R                          */
#define VG_REG_COLOR_XFORM_1    0x234   /* scale G                          */
#define VG_REG_COLOR_XFORM_2    0x238   /* scale B                          */
#define VG_REG_COLOR_XFORM_3    0x23C   /* scale A                          */
#define VG_REG_COLOR_XFORM_4    0x240   /* bias R                           */
#define VG_REG_COLOR_XFORM_5    0x244   /* bias G                           */
#define VG_REG_COLOR_XFORM_6    0x248   /* bias B                           */
#define VG_REG_COLOR_XFORM_7    0x24C   /* bias A                           */

/* --- Performance Counters (read-only) --- */
#define VG_REG_PERF_CYCLES      0x300   /* Total clock cycles                */
#define VG_REG_PERF_EDGES       0x304   /* Edges processed                   */
#define VG_REG_PERF_CACHE_HIT   0x308   /* Texture cache hit count           */
#define VG_REG_PERF_CACHE_MISS  0x30C   /* Texture cache miss count          */

/* =========================================================================
 * Register Field Definitions
 * ========================================================================= */

/* VG_REG_CTRL */
#define VG_CTRL_START           (1u << 0)   /* Start rendering                */
#define VG_CTRL_FLUSH           (1u << 1)   /* Flush pipeline                 */
#define VG_CTRL_ABORT           (1u << 2)   /* Abort current operation        */
#define VG_CTRL_FE_EN           (1u << 4)   /* Front-End enable               */
#define VG_CTRL_BE_EN           (1u << 5)   /* Back-End enable                */
#define VG_CTRL_TEX_EN          (1u << 6)   /* Texture cache enable           */
#define VG_CTRL_MASK_EN         (1u << 7)   /* Mask engine enable             */

/* VG_REG_STATUS */
#define VG_STATUS_IDLE          (1u << 0)   /* 1 = accelerator is idle        */
#define VG_STATUS_FE_BUSY       (1u << 1)   /* Front-End busy                 */
#define VG_STATUS_BE_BUSY       (1u << 2)   /* Back-End busy                  */
#define VG_STATUS_ERROR         (1u << 31)  /* Error flag                     */

/* VG_REG_IRQ_STATUS / VG_REG_IRQ_ENABLE */
#define VG_IRQ_DONE             (1u << 0)   /* Frame/path render complete     */
#define VG_IRQ_FE_DONE          (1u << 1)   /* Front-End done                 */
#define VG_IRQ_BE_DONE          (1u << 2)   /* Back-End done                  */
#define VG_IRQ_ERROR            (1u << 31)  /* Error                          */

/* VG_REG_SURF_FORMAT / VG_REG_TEX_FORMAT  (VGImageFormat subset) */
#define VG_FMT_RGBA8888         0x00
#define VG_FMT_RGBX8888         0x01
#define VG_FMT_BGRA8888         0x02
#define VG_FMT_RGB565           0x0E
#define VG_FMT_RGBA4444         0x10
#define VG_FMT_A8               0x14

/* VG_REG_FILL_RULE */
#define VG_REG_FILL_NON_ZERO    0
#define VG_REG_FILL_EVEN_ODD    1

/* VG_REG_BLEND_MODE */
#define VG_REG_BLEND_SRC        0
#define VG_REG_BLEND_SRC_OVER   1
#define VG_REG_BLEND_DST_OVER   2
#define VG_REG_BLEND_SRC_IN     3
#define VG_REG_BLEND_DST_IN     4
#define VG_REG_BLEND_MULTIPLY   5
#define VG_REG_BLEND_SCREEN     6
#define VG_REG_BLEND_DARKEN     7
#define VG_REG_BLEND_LIGHTEN    8
#define VG_REG_BLEND_ADDITIVE   9

/* VG_REG_STROKE_CAP */
#define VG_REG_CAP_BUTT         0
#define VG_REG_CAP_ROUND        1
#define VG_REG_CAP_SQUARE       2

/* VG_REG_STROKE_JOIN */
#define VG_REG_JOIN_MITER       0
#define VG_REG_JOIN_ROUND       1
#define VG_REG_JOIN_BEVEL       2

/* VG_REG_GRAD_TYPE */
#define VG_GRAD_NONE            0
#define VG_GRAD_LINEAR          1
#define VG_GRAD_RADIAL          2
#define VG_GRAD_PATTERN         3   /* Sample VG_REG_TEX_* image as paint */

/* VG_REG_TILE_SIZE */
#define VG_TILE_32              0
#define VG_TILE_64              1

/* VG_REG_AA_SAMPLES */
#define VG_AA_NONE              0   /* 1x1  */
#define VG_AA_4X                1   /* 4x4  */
#define VG_AA_8X                2   /* 8x8  */

/* =========================================================================
 * Path Command Buffer Layout
 * =========================================================================
 * The path command buffer is a packed array written to system memory and
 * pointed to by VG_REG_PATH_ADDR.
 *
 * Each path command entry starts with a 32-bit header word:
 *   [31:8]  reserved
 *   [7:0]   VGPathSegment command code (from openvg.h / spec Table 6)
 *
 * Followed by coordinate data (float32) per segment type:
 *   MOVE_TO_ABS/REL   : x, y          (2 floats)
 *   LINE_TO_ABS/REL   : x, y
 *   HLINE_TO_ABS/REL  : x             (1 float)
 *   VLINE_TO_ABS/REL  : y
 *   QUAD_TO_ABS/REL   : x0,y0, x1,y1 (4 floats)
 *   CUBIC_TO_ABS/REL  : x0,y0, x1,y1, x2,y2 (6 floats)
 *   SCUBIC_TO_ABS/REL : x0,y0, x1,y1 (4 floats)
 *   SCCWARC/SCWARC/LCCWARC/LCWARC : rh,rv,rot,x,y (5 floats)
 *   CLOSE_PATH        : (no coords)
 */

/* Path Segment Codes (matches VGPathSegment) */
#define VG_PATH_CMD_CLOSE_PATH  0x00
#define VG_PATH_CMD_MOVE_TO     0x02
#define VG_PATH_CMD_LINE_TO     0x04
#define VG_PATH_CMD_HLINE_TO    0x06
#define VG_PATH_CMD_VLINE_TO    0x08
#define VG_PATH_CMD_QUAD_TO     0x0A
#define VG_PATH_CMD_CUBIC_TO    0x0C
#define VG_PATH_CMD_SQUAD_TO    0x0E  /* Smooth quadratic (C0 smooth) - VG_SQUAD_TO */
#define VG_PATH_CMD_SCUBIC_TO   0x10  /* Smooth cubic (C1) - VG_SCUBIC_TO */
#define VG_PATH_CMD_SCCWARC_TO  0x12  /* Small counter-clockwise arc */
#define VG_PATH_CMD_SCWARC_TO   0x14  /* Small clockwise arc */
#define VG_PATH_CMD_LCCWARC_TO  0x16  /* Large counter-clockwise arc */
#define VG_PATH_CMD_LCWARC_TO   0x18  /* Large clockwise arc */
/* OR with 1 for REL variant */
#define VG_PATH_REL             0x01

#endif /* VG_REG_H */
