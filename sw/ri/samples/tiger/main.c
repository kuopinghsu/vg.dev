/*------------------------------------------------------------------------
 *
 * OpenVG 1.0.1 Reference Implementation sample code
 * -------------------------------------------------
 *
 * Copyright (c) 2007 The Khronos Group Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and /or associated documentation files
 * (the "Materials "), to deal in the Materials without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Materials,
 * and to permit persons to whom the Materials are furnished to do so,
 * subject to the following conditions: 
 *
 * The above copyright notice and this permission notice shall be included 
 * in all copies or substantial portions of the Materials. 
 *
 * THE MATERIALS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE MATERIALS OR
 * THE USE OR OTHER DEALINGS IN THE MATERIALS.
 *
 *//**
 * \file
 * \brief	Tiger sample application. Resizing the application window
 *			rerenders the tiger in the new resolution. Pressing 1,2,3
 *			or 4 sets pixel zoom factor, mouse moves inside the zoomed
 *			image (mouse move works on OpenGL >= 1.2).
 * \note	
 *//*-------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>
#define UNREF(X) ((void)(X))

#ifdef HG_FLAT_INCLUDES
#	include "openvg.h"
#	include "vgu.h"
#	include "egl.h"
#else
#	include "VG/openvg.h"
#	include "VG/vgu.h"
#	include "EGL/egl.h"
#endif

#include "tiger.h"

/*--------------------------------------------------------------*/

const float			aspectRatio = 612.0f / 792.0f;
int					renderWidth = 0;
int					renderHeight = 0;
EGLDisplay			egldisplay;
EGLConfig			eglconfig;
EGLSurface			eglsurface;
EGLContext			eglcontext;

/*--------------------------------------------------------------*/

typedef struct
{
	VGFillRule		m_fillRule;
	VGPaintMode		m_paintMode;
	VGCapStyle		m_capStyle;
	VGJoinStyle		m_joinStyle;
	float			m_miterLimit;
	float			m_strokeWidth;
	VGPaint			m_fillPaint;
	VGPaint			m_strokePaint;
	VGPath			m_path;
} PathData;

typedef struct
{
	PathData*			m_paths;
	int					m_numPaths;
} PS;

PS* PS_construct(const char* commands, int commandCount, const float* points, int pointCount)
{
	PS* ps = (PS*)malloc(sizeof(PS));
	int p = 0;
	int c = 0;
	int i = 0;
	int paths = 0;
	int maxElements = 0;
	unsigned char* cmd;
	UNREF(pointCount);

	while(c < commandCount)
	{
		int elements, e;
		c += 4;
		p += 8;
		elements = (int)points[p++];
		assert(elements > 0);
		if(elements > maxElements)
			maxElements = elements;
		for(e=0;e<elements;e++)
		{
			switch(commands[c])
			{
			case 'M': p += 2; break;
			case 'L': p += 2; break;
			case 'C': p += 6; break;
			case 'E': break;
			default:
				assert(0);		//unknown command
			}
			c++;
		}
		paths++;
	}

	ps->m_numPaths = paths;
	ps->m_paths = (PathData*)malloc(paths * sizeof(PathData));
	cmd = (unsigned char*)malloc(maxElements);

	i = 0;
	p = 0;
	c = 0;
	while(c < commandCount)
	{
		int elements, startp, e;
		float color[4];

		//fill type
		int paintMode = 0;
		ps->m_paths[i].m_fillRule = VG_NON_ZERO;
		switch( commands[c] )
		{
		case 'N':
			break;
		case 'F':
			ps->m_paths[i].m_fillRule = VG_NON_ZERO;
			paintMode |= VG_FILL_PATH;
			break;
		case 'E':
			ps->m_paths[i].m_fillRule = VG_EVEN_ODD;
			paintMode |= VG_FILL_PATH;
			break;
		default:
			assert(0);		//unknown command
		}
		c++;

		//stroke
		switch( commands[c] )
		{
		case 'N':
			break;
		case 'S':
			paintMode |= VG_STROKE_PATH;
			break;
		default:
			assert(0);		//unknown command
		}
		ps->m_paths[i].m_paintMode = (VGPaintMode)paintMode;
		c++;

		//line cap
		switch( commands[c] )
		{
		case 'B':
			ps->m_paths[i].m_capStyle = VG_CAP_BUTT;
			break;
		case 'R':
			ps->m_paths[i].m_capStyle = VG_CAP_ROUND;
			break;
		case 'S':
			ps->m_paths[i].m_capStyle = VG_CAP_SQUARE;
			break;
		default:
			assert(0);		//unknown command
		}
		c++;

		//line join
		switch( commands[c] )
		{
		case 'M':
			ps->m_paths[i].m_joinStyle = VG_JOIN_MITER;
			break;
		case 'R':
			ps->m_paths[i].m_joinStyle = VG_JOIN_ROUND;
			break;
		case 'B':
			ps->m_paths[i].m_joinStyle = VG_JOIN_BEVEL;
			break;
		default:
			assert(0);		//unknown command
		}
		c++;

		//the rest of stroke attributes
		ps->m_paths[i].m_miterLimit = points[p++];
		ps->m_paths[i].m_strokeWidth = points[p++];

		//paints
		color[0] = points[p++];
		color[1] = points[p++];
		color[2] = points[p++];
		color[3] = 1.0f;
		ps->m_paths[i].m_strokePaint = vgCreatePaint();
		vgSetParameteri(ps->m_paths[i].m_strokePaint, VG_PAINT_TYPE, VG_PAINT_TYPE_COLOR);
		vgSetParameterfv(ps->m_paths[i].m_strokePaint, VG_PAINT_COLOR, 4, color);

		color[0] = points[p++];
		color[1] = points[p++];
		color[2] = points[p++];
		color[3] = 1.0f;
		ps->m_paths[i].m_fillPaint = vgCreatePaint();
		vgSetParameteri(ps->m_paths[i].m_fillPaint, VG_PAINT_TYPE, VG_PAINT_TYPE_COLOR);
		vgSetParameterfv(ps->m_paths[i].m_fillPaint, VG_PAINT_COLOR, 4, color);

		//read number of elements

		elements = (int)points[p++];
		assert(elements > 0);
		startp = p;
		for(e=0;e<elements;e++)
		{
			switch( commands[c] )
			{
			case 'M':
				cmd[e] = VG_MOVE_TO | VG_ABSOLUTE;
				p += 2;
				break;
			case 'L':
				cmd[e] = VG_LINE_TO | VG_ABSOLUTE;
				p += 2;
				break;
			case 'C':
				cmd[e] = VG_CUBIC_TO | VG_ABSOLUTE;
				p += 6;
				break;
			case 'E':
				cmd[e] = VG_CLOSE_PATH;
				break;
			default:
				assert(0);		//unknown command
			}
			c++;
		}

		ps->m_paths[i].m_path = vgCreatePath(VG_PATH_FORMAT_STANDARD, VG_PATH_DATATYPE_F, 1.0f, 0.0f, 0, 0, (unsigned int)VG_PATH_CAPABILITY_ALL);
		vgAppendPathData(ps->m_paths[i].m_path, elements, cmd, points + startp);
		i++;
	}
	free(cmd);
	return ps;
}

void PS_destruct(PS* ps)
{
	int i;
	assert(ps);
	for(i=0;i<ps->m_numPaths;i++)
	{
		vgDestroyPaint(ps->m_paths[i].m_fillPaint);
		vgDestroyPaint(ps->m_paths[i].m_strokePaint);
		vgDestroyPath(ps->m_paths[i].m_path);
	}
	free(ps->m_paths);
	free(ps);
}

void PS_render(PS* ps)
{
	int i;
	assert(ps);
	vgSeti(VG_BLEND_MODE, VG_BLEND_SRC_OVER);

	for(i=0;i<ps->m_numPaths;i++)
	{
		vgSeti(VG_FILL_RULE, ps->m_paths[i].m_fillRule);
		vgSetPaint(ps->m_paths[i].m_fillPaint, VG_FILL_PATH);

		if(ps->m_paths[i].m_paintMode & VG_STROKE_PATH)
		{
			vgSetf(VG_STROKE_LINE_WIDTH, ps->m_paths[i].m_strokeWidth);
			vgSeti(VG_STROKE_CAP_STYLE, ps->m_paths[i].m_capStyle);
			vgSeti(VG_STROKE_JOIN_STYLE, ps->m_paths[i].m_joinStyle);
			vgSetf(VG_STROKE_MITER_LIMIT, ps->m_paths[i].m_miterLimit);
			vgSetPaint(ps->m_paths[i].m_strokePaint, VG_STROKE_PATH);
		}

		vgDrawPath(ps->m_paths[i].m_path, ps->m_paths[i].m_paintMode);
	}
	assert(vgGetError() == VG_NO_ERROR);
}

PS* tiger = NULL;

/*--------------------------------------------------------------*/

void render(int w, int h)
{
	float clearColor[4] = {1,1,1,1};
	float scale = w / (tigerMaxX - tigerMinX);

	vgSetfv(VG_CLEAR_COLOR, 4, clearColor);
	vgClear(0, 0, w, h);

	vgLoadIdentity();
	vgScale(scale, scale);
	vgTranslate(-tigerMinX, -tigerMinY + 0.5f * (h / scale - (tigerMaxY - tigerMinY)));

	PS_render(tiger);
	assert(vgGetError() == VG_NO_ERROR);

	renderWidth = w;
	renderHeight = h;
}

/*--------------------------------------------------------------*/

void init(int width, int height)
{
	static const EGLint s_configAttribs[] =
	{
		EGL_RED_SIZE,		8,
		EGL_GREEN_SIZE,		8,
		EGL_BLUE_SIZE,		8,
		EGL_ALPHA_SIZE,		8,
		EGL_LUMINANCE_SIZE,	EGL_DONT_CARE,
		EGL_SURFACE_TYPE,	EGL_PBUFFER_BIT,
		EGL_SAMPLES,		1,
		EGL_NONE
	};
	EGLint pbufferAttribs[] =
	{
		EGL_WIDTH,	width,
		EGL_HEIGHT,	height,
		EGL_NONE
	};
	EGLint numconfigs;

	egldisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	eglInitialize(egldisplay, NULL, NULL);
	assert(eglGetError() == EGL_SUCCESS);
	eglBindAPI(EGL_OPENVG_API);

	eglChooseConfig(egldisplay, s_configAttribs, &eglconfig, 1, &numconfigs);
	assert(eglGetError() == EGL_SUCCESS);
	assert(numconfigs == 1);

	eglsurface = eglCreatePbufferSurface(egldisplay, eglconfig, pbufferAttribs);
	assert(eglGetError() == EGL_SUCCESS);
	eglcontext = eglCreateContext(egldisplay, eglconfig, NULL, NULL);
	assert(eglGetError() == EGL_SUCCESS);
	eglMakeCurrent(egldisplay, eglsurface, eglsurface, eglcontext);
	assert(eglGetError() == EGL_SUCCESS);

	tiger = PS_construct(tigerCommands, tigerCommandCount, tigerPoints, tigerPointCount);
}

/*--------------------------------------------------------------*/

void deinit(void)
{
	PS_destruct(tiger);
	eglMakeCurrent(egldisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	assert(eglGetError() == EGL_SUCCESS);
	eglTerminate(egldisplay);
	assert(eglGetError() == EGL_SUCCESS);
	eglReleaseThread();
}

/*--------------------------------------------------------------*/

/*--------------------------------------------------------------*/
/* POSIX headless renderer: renders to an EGL pbuffer and       */
/* writes the result to a PPM file.                             */
/*--------------------------------------------------------------*/

static void save_ppm(const char *filename, int w, int h)
{
	unsigned char *pixels;
	FILE *fp;
	int row;

	pixels = (unsigned char *)malloc((size_t)(w * h * 4));
	if (!pixels)
	{
		fprintf(stderr, "Out of memory reading pixels\n");
		return;
	}

	/* VG_sABGR_8888: on little-endian the 4 in-memory bytes are R,G,B,A */
	vgReadPixels(pixels, w * 4, VG_sABGR_8888, 0, 0, w, h);
	assert(vgGetError() == VG_NO_ERROR);

	fp = fopen(filename, "wb");
	if (!fp)
	{
		fprintf(stderr, "Cannot open output file: %s\n", filename);
		free(pixels);
		return;
	}

	fprintf(fp, "P6\n%d %d\n255\n", w, h);

	/* Flip vertically: VG origin is bottom-left, PPM is top-left */
	for (row = h - 1; row >= 0; row--)
	{
		int col;
		for (col = 0; col < w; col++)
		{
			unsigned char *px = pixels + (row * w + col) * 4;
			fwrite(px, 1, 3, fp); /* write R, G, B (skip A) */
		}
	}

	fclose(fp);
	free(pixels);
	printf("Saved %s (%dx%d)\n", filename, w, h);
}

/*--------------------------------------------------------------*/

int main(int argc, char *argv[])
{
	const char *outfile = (argc >= 2) ? argv[1] : "tiger.ppm";
	const int   width   = (argc >= 3) ? atoi(argv[2]) : 512;
	const int   height  = (int)(width / aspectRatio + 0.5f);

	init(width, height);
	render(width, height);
	save_ppm(outfile, width, height);
	deinit();

	return 0;
}

