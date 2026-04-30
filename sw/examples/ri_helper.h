/*
 * ri_helper.h – Shared EGL + OpenVG boilerplate for headless RI tests.
 *
 * Include once from a single .c source that defines main().
 * Compile with:
 *   gcc -std=c99 -I<ri/include> -I<ri/include>/VG -I<ri/include>/EGL \
 *       -DOPENVG_STATIC_LIBRARY -DEGL_STATIC_LIBRARY ...
 */
#pragma once

#ifndef OPENVG_STATIC_LIBRARY
#  define OPENVG_STATIC_LIBRARY
#endif
#ifndef EGL_STATIC_LIBRARY
#  define EGL_STATIC_LIBRARY
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "VG/openvg.h"
#include "EGL/egl.h"

static EGLDisplay _ri_dpy;
static EGLConfig  _ri_cfg;
static EGLSurface _ri_surf;
static EGLContext _ri_ctx;
static int        _ri_w, _ri_h;

/* Initialise EGL + OpenVG pbuffer surface of the given size. */
static void ri_init(int w, int h)
{
    static const EGLint cfgAttrs[] = {
        EGL_RED_SIZE,       8,
        EGL_GREEN_SIZE,     8,
        EGL_BLUE_SIZE,      8,
        EGL_ALPHA_SIZE,     8,
        EGL_LUMINANCE_SIZE, EGL_DONT_CARE,
        EGL_SURFACE_TYPE,   EGL_PBUFFER_BIT,
        EGL_SAMPLES,        1,
        EGL_NONE
    };
    EGLint pbAttrs[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLint n;

    _ri_w = w;  _ri_h = h;
    _ri_dpy  = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(_ri_dpy, NULL, NULL);
    eglBindAPI(EGL_OPENVG_API);
    eglChooseConfig(_ri_dpy, cfgAttrs, &_ri_cfg, 1, &n);
    assert(n == 1);
    _ri_surf = eglCreatePbufferSurface(_ri_dpy, _ri_cfg, pbAttrs);
    assert(_ri_surf != EGL_NO_SURFACE);
    _ri_ctx  = eglCreateContext(_ri_dpy, _ri_cfg, NULL, NULL);
    assert(_ri_ctx  != EGL_NO_CONTEXT);
    eglMakeCurrent(_ri_dpy, _ri_surf, _ri_surf, _ri_ctx);
}

/* Tear down EGL. */
static void ri_deinit(void)
{
    eglMakeCurrent(_ri_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(_ri_dpy);
    eglReleaseThread();
}

/* Clear the surface to solid white. */
static void ri_clear_white(void) __attribute__((unused));
static void ri_clear_white(void)
{
    VGfloat cc[4] = { 1.f, 1.f, 1.f, 1.f };
    vgSetfv(VG_CLEAR_COLOR, 4, cc);
    vgClear(0, 0, _ri_w, _ri_h);
}

/*
 * Load a PATH_USER_TO_SURFACE matrix that maps screen-Y-down coordinates
 * (as used in the cmodel) to OpenVG Y-up surface coordinates.
 *
 * Transformation:  openvg_x = x,  openvg_y = H - y
 *
 * Matrix (OpenVG column-major layout [sx,shy,0, shx,sy,0, tx,ty,1]):
 *   [1,  0, 0,  0, -1, 0,  0, H, 1]
 */
static void ri_set_screen_matrix(void) __attribute__((unused));
static void ri_set_screen_matrix(void)
{
    VGfloat m[9] = {
        1.f,  0.f, 0.f,
        0.f, -1.f, 0.f,
        0.f,  (VGfloat)_ri_h, 1.f
    };
    vgSeti(VG_MATRIX_MODE, VG_MATRIX_PATH_USER_TO_SURFACE);
    vgLoadMatrix(m);
}

/*
 * Save the current surface as a PPM file.
 * Flips vertically so PPM row 0 = top of image (OpenVG Y-down).
 */
static void ri_save_ppm(const char *path)
{
    int w = _ri_w, h = _ri_h;
    unsigned char *pixels = (unsigned char *)malloc((size_t)(w * h * 4));
    assert(pixels);
    vgReadPixels(pixels, w * 4, VG_sABGR_8888, 0, 0, w, h);
    assert(vgGetError() == VG_NO_ERROR);

    FILE *fp = fopen(path, "wb");
    assert(fp);
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (int row = h - 1; row >= 0; row--)
        for (int col = 0; col < w; col++) {
            unsigned char *px = pixels + (row * w + col) * 4;
            fwrite(px, 1, 3, fp);
        }
    fclose(fp);
    free(pixels);
    printf("Saved %s (%dx%d)\n", path, w, h);
}
