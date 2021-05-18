/*
    This file is part of darktable,
    Copyright (C) 2017-2020 darktable developers.

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

#pragma once
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/darktable.h"
#include <stdlib.h>

struct dt_iop_roi_t;

// buffer to store single-channel image along with its dimensions
typedef struct gray_image
{
  float *data;
  int width, height;
} gray_image;


// allocate space for 1-component image of size width x height
static inline gray_image new_gray_image(int width, int height)
{
  return (gray_image){ dt_alloc_align(64, sizeof(float) * width * height), width, height };
}


// free space for 1-component image
static inline void free_gray_image(gray_image *img_p)
{
  dt_free_align(img_p->data);
  img_p->data = NULL;
}


// copy 1-component image img1 to img2
static inline void copy_gray_image(gray_image img1, gray_image img2)
{
  memcpy(img2.data, img1.data, sizeof(float) * img1.width * img1.height);
}


// minimum of two integers
static inline int min_i(int a, int b)
{
  return a < b ? a : b;
}


// maximum of two integers
static inline int max_i(int a, int b)
{
  return a > b ? a : b;
}

// Kahan summation algorithm
static inline float Kahan_sum(const float m, float *c, const float add)
{
   const float t1 = add - (*c);
   const float t2 = m + t1;
   *c = (t2 - m) - t1;
   return t2;
}

#ifdef __SSE2__
// vectorized Kahan summation algorithm
static inline __m128 Kahan_sum_sse(const __m128 m, __m128 *c, const __m128 add)
{
   const __m128 t1 = add - (*c);
   const __m128 t2 = m + t1;
   *c = (t2 - m) - t1;
   return t2;
}
#endif /* __SSE2__ */

void guided_filter(const float *guide, const float *in, float *out, int width, int height, int ch, int w,
                   float sqrt_eps, float guide_weight, float min, float max);
