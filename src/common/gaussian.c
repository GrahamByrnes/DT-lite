/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

//#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include <math.h>
#include "common/gaussian.h"
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/darktable.h"
#include "control/conf.h"

#define CLAMPF(a, mn, mx) ((a) < (mn) ? (mn) : ((a) > (mx) ? (mx) : (a)))

#define BLOCKSIZE (1 << 6)

static void compute_gauss_params(const float sigma, dt_gaussian_order_t order, float *a0, float *a1,
                                 float *a2, float *a3, float *b1, float *b2, float *coefp, float *coefn)
{
  const float alpha = 1.695f / sigma;
  const float ema = exp(-alpha);
  const float ema2 = exp(-2.0f * alpha);
  *b1 = -2.0f * ema;
  *b2 = ema2;
  *a0 = 0.0f;
  *a1 = 0.0f;
  *a2 = 0.0f;
  *a3 = 0.0f;
  *coefp = 0.0f;
  *coefn = 0.0f;

  switch(order)
  {
    default:
    case DT_IOP_GAUSSIAN_ZERO:
    {
      const float k = (1.0f - ema) * (1.0f - ema) / (1.0f + (2.0f * alpha * ema) - ema2);
      *a0 = k;
      *a1 = k * (alpha - 1.0f) * ema;
      *a2 = k * (alpha + 1.0f) * ema;
      *a3 = -k * ema2;
    }
    break;

    case DT_IOP_GAUSSIAN_ONE:
    {
      *a0 = (1.0f - ema) * (1.0f - ema);
      *a1 = 0.0f;
      *a2 = -*a0;
      *a3 = 0.0f;
    }
    break;

    case DT_IOP_GAUSSIAN_TWO:
    {
      const float k = -(ema2 - 1.0f) / (2.0f * alpha * ema);
      float kn = -2.0f * (-1.0f + (3.0f * ema) - (3.0f * ema * ema) + (ema * ema * ema));
      kn /= ((3.0f * ema) + 1.0f + (3.0f * ema * ema) + (ema * ema * ema));
      *a0 = kn;
      *a1 = -kn * (1.0f + (k * alpha)) * ema;
      *a2 = kn * (1.0f - (k * alpha)) * ema;
      *a3 = -kn * ema2;
    }
  }

  *coefp = (*a0 + *a1) / (1.0f + *b1 + *b2);
  *coefn = (*a2 + *a3) / (1.0f + *b1 + *b2);
}

size_t dt_gaussian_memory_use(const int width,    // width of input image
                              const int height,   // height of input image
                              const int channels) // channels per pixel
{
  size_t mem_use;
  mem_use = (size_t)width * height * channels * sizeof(float);
  return mem_use;
}

size_t dt_gaussian_singlebuffer_size(const int width,    // width of input image
                                     const int height,   // height of input image
                                     const int channels) // channels per pixel
{
  size_t mem_use;
  mem_use = (size_t)width * height * channels * sizeof(float);
  return mem_use;
}


dt_gaussian_t *dt_gaussian_init(const int width,    // width of input image
                                const int height,   // height of input image
                                const int channels, // channels per pixel
                                const float *max,   // maximum allowed values per channel for clamping
                                const float *min,   // minimum allowed values per channel for clamping
                                const float sigma,  // gaussian sigma
                                const int order)    // order of gaussian blur
{
  dt_gaussian_t *g = (dt_gaussian_t *)malloc(sizeof(dt_gaussian_t));
  if(!g) return NULL;

  g->width = width;
  g->height = height;
  g->channels = channels;
  g->sigma = sigma;
  g->order = order;
  g->buf = NULL;
  g->max = (float *)calloc(channels, sizeof(float));
  g->min = (float *)calloc(channels, sizeof(float));

  if(!g->min || !g->max) goto error;

  for(int k = 0; k < channels; k++)
  {
    g->max[k] = max[k];
    g->min[k] = min[k];
  }

  g->buf = dt_alloc_align(64, (size_t)width * height * channels * sizeof(float));
  if(!g->buf) goto error;

  return g;

error:
  dt_free_align(g->buf);
  free(g->max);
  free(g->min);
  free(g);
  return NULL;
}


void dt_gaussian_blur(dt_gaussian_t *g, const float *const in, float *const out)
{

  const int width = g->width;
  const int height = g->height;
  const int ch = MIN(4, g->channels); // just to appease zealous compiler warnings about stack usage

  float a0, a1, a2, a3, b1, b2, coefp, coefn;

  compute_gauss_params(g->sigma, g->order, &a0, &a1, &a2, &a3, &b1, &b2, &coefp, &coefn);

  float *temp = g->buf;

  float *Labmax = g->max;
  float *Labmin = g->min;

// vertical blur column by column
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, width, height, ch) \
  shared(temp, Labmin, Labmax, a0, a1, a2, a3, b1, b2, coefp, coefn) \
  schedule(static)
#endif
  for(int i = 0; i < width; i++)
  {
    float xp[4] = {0.0f};
    float yb[4] = {0.0f};
    float yp[4] = {0.0f};
    float xc[4] = {0.0f};
    float yc[4] = {0.0f};
    float xn[4] = {0.0f};
    float xa[4] = {0.0f};
    float yn[4] = {0.0f};
    float ya[4] = {0.0f};

    // forward filter
    for(int k = 0; k < ch; k++)
    {
      xp[k] = CLAMPF(in[(size_t)i * ch + k], Labmin[k], Labmax[k]);
      yb[k] = xp[k] * coefp;
      yp[k] = yb[k];
      xc[k] = yc[k] = xn[k] = xa[k] = yn[k] = ya[k] = 0.0f;
    }

    for(int j = 0; j < height; j++)
    {
      size_t offset = ((size_t)j * width + i) * ch;

      for(int k = 0; k < ch; k++)
      {
        xc[k] = CLAMPF(in[offset + k], Labmin[k], Labmax[k]);
        yc[k] = (a0 * xc[k]) + (a1 * xp[k]) - (b1 * yp[k]) - (b2 * yb[k]);

        temp[offset + k] = yc[k];

        xp[k] = xc[k];
        yb[k] = yp[k];
        yp[k] = yc[k];
      }
    }

    // backward filter
    for(int k = 0; k < ch; k++)
    {
      xn[k] = CLAMPF(in[((size_t)(height - 1) * width + i) * ch + k], Labmin[k], Labmax[k]);
      xa[k] = xn[k];
      yn[k] = xn[k] * coefn;
      ya[k] = yn[k];
    }

    for(int j = height - 1; j > -1; j--)
    {
      size_t offset = ((size_t)j * width + i) * ch;

      for(int k = 0; k < ch; k++)
      {
        xc[k] = CLAMPF(in[offset + k], Labmin[k], Labmax[k]);

        yc[k] = (a2 * xn[k]) + (a3 * xa[k]) - (b1 * yn[k]) - (b2 * ya[k]);

        xa[k] = xn[k];
        xn[k] = xc[k];
        ya[k] = yn[k];
        yn[k] = yc[k];

        temp[offset + k] += yc[k];
      }
    }
  }

// horizontal blur line by line
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(out, ch, width, height) \
  shared(temp, Labmin, Labmax, a0, a1, a2, a3, b1, b2, coefp, coefn) \
  schedule(static)
#endif
  for(int j = 0; j < height; j++)
  {
    float xp[4] = {0.0f};
    float yb[4] = {0.0f};
    float yp[4] = {0.0f};
    float xc[4] = {0.0f};
    float yc[4] = {0.0f};
    float xn[4] = {0.0f};
    float xa[4] = {0.0f};
    float yn[4] = {0.0f};
    float ya[4] = {0.0f};

    // forward filter
    for(int k = 0; k < ch; k++)
    {
      xp[k] = CLAMPF(temp[(size_t)j * width * ch + k], Labmin[k], Labmax[k]);
      yb[k] = xp[k] * coefp;
      yp[k] = yb[k];
      xc[k] = yc[k] = xn[k] = xa[k] = yn[k] = ya[k] = 0.0f;
    }

    for(int i = 0; i < width; i++)
    {
      size_t offset = ((size_t)j * width + i) * ch;

      for(int k = 0; k < ch; k++)
      {
        xc[k] = CLAMPF(temp[offset + k], Labmin[k], Labmax[k]);
        yc[k] = (a0 * xc[k]) + (a1 * xp[k]) - (b1 * yp[k]) - (b2 * yb[k]);

        out[offset + k] = yc[k];

        xp[k] = xc[k];
        yb[k] = yp[k];
        yp[k] = yc[k];
      }
    }

    // backward filter
    for(int k = 0; k < ch; k++)
    {
      xn[k] = CLAMPF(temp[((size_t)(j + 1) * width - 1) * ch + k], Labmin[k], Labmax[k]);
      xa[k] = xn[k];
      yn[k] = xn[k] * coefn;
      ya[k] = yn[k];
    }

    for(int i = width - 1; i > -1; i--)
    {
      size_t offset = ((size_t)j * width + i) * ch;

      for(int k = 0; k < ch; k++)
      {
        xc[k] = CLAMPF(temp[offset + k], Labmin[k], Labmax[k]);

        yc[k] = (a2 * xn[k]) + (a3 * xa[k]) - (b1 * yn[k]) - (b2 * ya[k]);

        xa[k] = xn[k];
        xn[k] = xc[k];
        ya[k] = yn[k];
        yn[k] = yc[k];

        out[offset + k] += yc[k];
      }
    }
  }
}

void dt_gaussian_blur_4c(dt_gaussian_t *g, const float *const in, float *const out)
{
  if(darktable.codepath.OPENMP_SIMD) return dt_gaussian_blur(g, in, out);
  else
    dt_unreachable_codepath();
}

void dt_gaussian_free(dt_gaussian_t *g)
{
  if(!g) return;
  free(g->min);
  free(g->max);
  free(g);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
