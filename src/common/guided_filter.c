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


    Implementation of the guided image filter as described in

    "Guided Image Filtering" by Kaiming He, Jian Sun, and Xiaoou Tang in
    K. Daniilidis, P. Maragos, N. Paragios (Eds.): ECCV 2010, Part I,
    LNCS 6311, pp. 1-14, 2010. Springer-Verlag Berlin Heidelberg 2010

    "Guided Image Filtering" by Kaiming He, Jian Sun, and Xiaoou Tang in
    IEEE Transactions on Pattern Analysis and Machine Intelligence, vol. 35,
    no. 6, June 2013, 1397-1409

*/

#include "common/guided_filter.h"
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/darktable.h"
#include "control/conf.h"
                        
#include <assert.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

// processing is split into tiles of this size (or three times the filter
// width, if greater) to keep memory use under control.
#define GF_TILE_SIZE 512

// some shorthand to make code more legible
// if we have OpenMP simd enabled, declare a vectorizable for loop;
// otherwise, just leave it a plain for()

// the filter does internal tiling to keep memory requirements reasonable, so this structure
// defines the position of the tile being processed
typedef struct tile
{
  int left, right, lower, upper;
} tile;

typedef struct color_image
{
  float *data;
  int width, height, stride;
} color_image;

// allocate space for n-component image of size width x height
static inline color_image new_color_image(int width, int height, int ch)
{
  return (color_image){ dt_alloc_align(64, sizeof(float) * width * height * ch), width, height, ch };
}

// free space for n-component image
static inline void free_color_image(color_image *img_p)
{
  dt_free_align(img_p->data);
  img_p->data = NULL;
}

// get a pointer to pixel number 'i' within the image
static inline float *get_color_pixel(color_image img, size_t i)
{
  return img.data + i * img.stride;
}

// calculate the one-dimensional moving average over a window of size 2*w+1
// input array x has stride 1, output array y has stride stride_y
static inline void box_mean_1d(int N, const float *x, float *y, size_t stride_y, int w)
{
  float m = 0.f, n_box = 0.f, c = 0.f;
  if(N > 2 * w)
  {
    for(int i = 0, i_end = w + 1; i < i_end; i++)
    {
      m = Kahan_sum(m, &c, x[i]);
      n_box++;
    }
    for(int i = 0, i_end = w; i < i_end; i++)
    {
      y[i * stride_y] = m / n_box;
      m = Kahan_sum(m, &c, x[i + w + 1]);
      n_box++;
    }
    for(int i = w, i_end = N - w - 1; i < i_end; i++)
    {
      y[i * stride_y] = m / n_box;
      m = Kahan_sum(m, &c, x[i + w + 1]),
      m = Kahan_sum(m, &c, -x[i - w]);
    }
    for(int i = N - w - 1, i_end = N; i < i_end; i++)
    {
      y[i * stride_y] = m / n_box;
      m = Kahan_sum(m, &c, -x[i - w]);
      n_box--;
    }
  }
  else
  {
    for(int i = 0, i_end = min_i(w + 1, N); i < i_end; i++)
    {
      m = Kahan_sum(m, &c, x[i]);
      n_box++;
    }
    for(int i = 0; i < N; i++)
    {
      y[i * stride_y] = m / n_box;
      if(i - w >= 0)
      {
        m = Kahan_sum(m, &c, -x[i - w]);
        n_box--;
      }
      if(i + w + 1 < N)
      {
        m = Kahan_sum(m, &c, x[i + w + 1]);
        n_box++;
      }
    }
  }
}

// calculate the one-dimensional moving average over a window of size 2*w+1 independently for each of four channels
// input array x has stride 4, output array y has stride stride_y
static inline void box_mean_1d_4ch(int N, const float *x, float *y, size_t stride_y, int w)
{
  float n_box = 0.f, m[4] = { 0.f, 0.f, 0.f, 0.f }, c[4] = { 0.f, 0.f, 0.f, 0.f };
  if(N > 2 * w)
  {
    for(int i = 0, i_end = w + 1; i < i_end; i++)
    {
      for(int k = 0; k < 4; k++)
        m[k] = Kahan_sum(m[k], &c[k], x[4*i+k]);
      n_box++;
    }
    for(int i = 0, i_end = w; i < i_end; i++)
    {
      for(int k = 0; k < 4; k++)
      {
        y[i * stride_y + k] = m[k] / n_box;
        m[k] = Kahan_sum(m[k], &c[k], x[4*(i + w + 1) + k]);
      }
      n_box++;
    }
    for(int i = w, i_end = N - w - 1; i < i_end; i++)
    {
      for(int k = 0; k < 4; k++)
      {
        y[i * stride_y + k] = m[k] / n_box;
        m[k] = Kahan_sum(m[k], &c[k], x[4*(i + w + 1) + k]);
        m[k] = Kahan_sum(m[k], &c[k], -x[4*(i - w) + k]);
      }
    }
    for(int i = N - w - 1, i_end = N; i < i_end; i++)
    {
      for(int k = 0; k < 4; k++)
      {
        y[i * stride_y + k] = m[k] / n_box;
        m[k] = Kahan_sum(m[k], &c[k], -x[4*(i - w) + k]);
      }
      n_box--;
    }
  }
  else
  {
    for(int i = 0, i_end = min_i(w + 1, N); i < i_end; i++)
    {
      for(int k = 0; k < 4; k++)
      {
        m[k] = Kahan_sum(m[k], &c[k], x[4*i + k]);
      }
      n_box++;
    }
    for(int i = 0; i < N; i++)
    {
      for(int k = 0; k < 4; k++)
        y[i * stride_y + k] = m[k] / n_box;
      if(i - w >= 0)
      {
        for(int k = 0; k < 4; k++)
          m[k] = Kahan_sum(m[k], &c[k], -x[4*(i - w)+k]);
        n_box--;
      }
      if(i + w + 1 < N)
      {
        for(int k = 0; k < 4; k++)
          m[k] = Kahan_sum(m[k], &c[k], x[4*(i + w + 1)+k]);
        n_box++;
      }
    }
  }
}

// in-place calculate the two-dimensional moving average over a box of size (2*w+1) x (2*w+1)
// this function is always called from an OpenMP thread, thus no parallelization
static void box_mean(gray_image img, int w)
{
  gray_image img_bak = new_gray_image(max_i(img.width, img.height), 1);
  for(int i1 = 0; i1 < img.height; i1++)
  {
    memcpy(img_bak.data, img.data + (size_t)i1 * img.width, sizeof(float) * img.width);
    box_mean_1d(img.width, img_bak.data, img.data + (size_t)i1 * img.width, 1, w);
  }
  for(int i0 = 0; i0 < img.width; i0++)
  {
    for(int i1 = 0; i1 < img.height; i1++) img_bak.data[i1] = img.data[i0 + (size_t)i1 * img.width];
    box_mean_1d(img.height, img_bak.data, img.data + i0, img.width, w);
  }
  free_gray_image(&img_bak);
}

// in-place calculate the two-dimensional moving average of a four-channel image over a box of size (2*w+1) x (2*w+1)
// this function is always called from an OpenMP thread, thus no parallelization
static void box_mean_4ch(color_image img, int w)
{
  color_image img_bak = new_color_image(max_i(img.width, img.height), 1, 4);
  const size_t width = 4 * img.width;
  for(int i1 = 0; i1 < img.height; i1++)
  {
    memcpy(img_bak.data, img.data + (size_t)i1 * width, sizeof(float) * width);
    box_mean_1d_4ch(img.width, img_bak.data, img.data + (size_t)i1 * width, 4, w);
  }
  for(int i0 = 0; i0 < img.width; i0++)
  {
    for(int i1 = 0; i1 < img.height; i1++)
      for(int k = 0; k < 4; k++)
        img_bak.data[4*i1+k] = img.data[4*(i0 + (size_t)i1 * img.width)+k];
    box_mean_1d_4ch(img.height, img_bak.data, img.data + 4*i0, width, w);
  }
  free_color_image(&img_bak);
}

// apply guided filter to single-component image img using the 3-components image imgg as a guide
// the filtering applies a monochrome box filter to a total of 13 image channels:
//    1 monochrome input image
//    3 color guide image
//    3 covariance (R, G, B)
//    6 variance (R-R, R-G, R-B, G-G, G-B, B-B)
// for computational efficiency, we'll pack them into three four-channel images and one mono image instead
// of running 13 separate box filters: guide+input, R/G/B/R-R, R-G/R-B/G-G/G-B, and B-B.
static void guided_filter_tiling(color_image imgg, gray_image img, gray_image img_out, tile target, const int w,
                                 const float eps, const float guide_weight, const float min, const float max)
{
  const tile source = { max_i(target.left - 2 * w, 0), min_i(target.right + 2 * w, imgg.width),
                        max_i(target.lower - 2 * w, 0), min_i(target.upper + 2 * w, imgg.height) };
  const int width = source.right - source.left;
  const int height = source.upper - source.lower;
  size_t size = (size_t)width * (size_t)height;
// since we're packing multiple monochrome planes into a color image, define symbolic constants so that
// we can keep track of which values we're actually using
#define INP_MEAN 0
#define GUIDE_MEAN_R 1
#define GUIDE_MEAN_G 2
#define GUIDE_MEAN_B 3
#define COV_R 0
#define COV_G 1
#define COV_B 2
#define VAR_RR 3  // packed into the covar image
#define VAR_RG 0
#define VAR_RB 1
#define VAR_GG 2
#define VAR_GB 3
#define VAR_BB 0 // not actually packed, it's in a separate gray_image
  color_image mean = new_color_image(width, height, 4);
  color_image covar = new_color_image(width, height, 4);
  color_image variance = new_color_image(width, height, 4);
  gray_image var_imgg_bb = new_gray_image(width, height);
  for(int j_imgg = source.lower; j_imgg < source.upper; j_imgg++)
  {
    int j = j_imgg - source.lower;
    for(int i_imgg = source.left; i_imgg < source.right; i_imgg++)
    {
      int i = i_imgg - source.left;
      const float *pixel_ = get_color_pixel(imgg, i_imgg + (size_t)j_imgg * imgg.width);
      float pixel[3] = { pixel_[0] * guide_weight, pixel_[1] * guide_weight, pixel_[2] * guide_weight };
      const size_t k = i + (size_t)j * width;
      float *const meanpx = get_color_pixel(mean, k);
      const float input = img.data[i_imgg + (size_t)j_imgg * img.width];
      meanpx[INP_MEAN] = input;
      meanpx[GUIDE_MEAN_R] = pixel[0];
      meanpx[GUIDE_MEAN_G] = pixel[1];
      meanpx[GUIDE_MEAN_B] = pixel[2];
      float *const covpx = get_color_pixel(covar, k);
      covpx[COV_R] = pixel[0] * input;
      covpx[COV_G] = pixel[1] * input;
      covpx[COV_B] = pixel[2] * input;
      covpx[VAR_RR] = pixel[0] * pixel[0];
      float *const varpx = get_color_pixel(variance, k);
      varpx[VAR_RG] = pixel[0] * pixel[1];
      varpx[VAR_RB] = pixel[0] * pixel[2];
      varpx[VAR_GG] = pixel[1] * pixel[1];
      varpx[VAR_GB] = pixel[1] * pixel[2];
      var_imgg_bb.data[k] = pixel[2] * pixel[2];
    }
  }
  box_mean_4ch(mean, w);
  box_mean_4ch(covar, w);
  box_mean_4ch(variance, w);
  box_mean(var_imgg_bb, w);
  for(size_t i = 0; i < size; i++)
  {
    const float *meanpx = get_color_pixel(mean, i);
    const float inp_mean = meanpx[INP_MEAN];
    const float guide_r = meanpx[GUIDE_MEAN_R];
    const float guide_g = meanpx[GUIDE_MEAN_G];
    const float guide_b = meanpx[GUIDE_MEAN_B];
    float *const covpx = get_color_pixel(covar, i);
    covpx[COV_R] -= guide_r * inp_mean;
    covpx[COV_G] -= guide_g * inp_mean;
    covpx[COV_B] -= guide_b * inp_mean;
    covpx[VAR_RR] -= guide_r * guide_r;
    covpx[VAR_RR] += eps;
    float *const varpx = get_color_pixel(variance, i);
    varpx[VAR_RG] -= guide_r * guide_g;
    varpx[VAR_RB] -= guide_r * guide_b;
    varpx[VAR_GG] -= guide_g * guide_g;
    varpx[VAR_GG] += eps;
    varpx[VAR_GB] -= guide_g * guide_b;
    var_imgg_bb.data[i] -= guide_b * guide_b;
    var_imgg_bb.data[i] += eps;
  }
  // we will recycle memory of the arrays imgg_mean_? and img_mean for the new coefficient arrays a_? and b to
  // reduce memory foot print
  color_image a_b = mean;
  #define A_RED 0
  #define A_GREEN 1
  #define A_BLUE 2
  #define B 3
  for(int i = 0; i < height*width; i++)
  {
    // solve linear system of equations of size 3x3 via Cramer's rule
    // symmetric coefficient matrix
    const float *covpx = get_color_pixel(covar, i);
    const float *varpx = get_color_pixel(variance, i);
    const float Sigma_0_0 = covpx[VAR_RR];
    const float Sigma_0_1 = varpx[VAR_RG];
    const float Sigma_0_2 = varpx[VAR_RB];
    const float Sigma_1_1 = varpx[VAR_GG];
    const float Sigma_1_2 = varpx[VAR_GB];
    const float Sigma_2_2 = var_imgg_bb.data[i];
    const float cov_imgg_img[3] = { covpx[COV_R], covpx[COV_G], covpx[COV_B] };
    const float det0 = Sigma_0_0 * (Sigma_1_1 * Sigma_2_2 - Sigma_1_2 * Sigma_1_2)
      - Sigma_0_1 * (Sigma_0_1 * Sigma_2_2 - Sigma_0_2 * Sigma_1_2)
      + Sigma_0_2 * (Sigma_0_1 * Sigma_1_2 - Sigma_0_2 * Sigma_1_1);
    float a_r_, a_g_, a_b_, b_;
    if(fabsf(det0) > 4.f * FLT_EPSILON)
    {
      const float det1 = cov_imgg_img[0] * (Sigma_1_1 * Sigma_2_2 - Sigma_1_2 * Sigma_1_2)
        - Sigma_0_1 * (cov_imgg_img[1] * Sigma_2_2 - cov_imgg_img[2] * Sigma_1_2)
        + Sigma_0_2 * (cov_imgg_img[1] * Sigma_1_2 - cov_imgg_img[2] * Sigma_1_1);
      const float det2 = Sigma_0_0 * (cov_imgg_img[1] * Sigma_2_2 - cov_imgg_img[2] * Sigma_1_2)
        - cov_imgg_img[0] * (Sigma_0_1 * Sigma_2_2 - Sigma_0_2 * Sigma_1_2)
        + Sigma_0_2 * (Sigma_0_1 * cov_imgg_img[2] - Sigma_0_2 * cov_imgg_img[1]);
      const float det3 = Sigma_0_0 * (Sigma_1_1 * cov_imgg_img[2] - Sigma_1_2 * cov_imgg_img[1])
        - Sigma_0_1 * (Sigma_0_1 * cov_imgg_img[2] - Sigma_0_2 * cov_imgg_img[1])
        + cov_imgg_img[0] * (Sigma_0_1 * Sigma_1_2 - Sigma_0_2 * Sigma_1_1);
      a_r_ = det1 / det0;
      a_g_ = det2 / det0;
      a_b_ = det3 / det0;
      const float *meanpx = get_color_pixel(mean, i);
      b_ = meanpx[INP_MEAN] - a_r_ * meanpx[GUIDE_MEAN_R] - a_g_ * meanpx[GUIDE_MEAN_G] - a_b_ * meanpx[GUIDE_MEAN_B];
    }
    else
    {
      // linear system is singular
      a_r_ = 0.f;
      a_g_ = 0.f;
      a_b_ = 0.f;
      b_ = get_color_pixel(mean, i)[INP_MEAN];
    }
    // now data of imgg_mean_? is no longer needed, we can safely overwrite aliasing arrays
    a_b.data[4*i+A_RED] = a_r_;
    a_b.data[4*i+A_GREEN] = a_g_;
    a_b.data[4*i+A_BLUE] = a_b_;
    a_b.data[4*i+B] = b_;
  }
  box_mean_4ch(a_b, w);
  for(int j_imgg = target.lower; j_imgg < target.upper; j_imgg++)
  {
    // index of the left most target pixel in the current row
    size_t l = target.left + (size_t)j_imgg * imgg.width;
    // index of the left most source pixel in the current row of the
    // smaller auxiliary gray-scale images a_r, a_g, a_b, and b
    // excluding boundary data from neighboring tiles
    size_t k = (target.left - source.left) + (size_t)(j_imgg - source.lower) * width;
    for(int i_imgg = target.left; i_imgg < target.right; i_imgg++, k++, l++)
    {
      const float *pixel = get_color_pixel(imgg, l);
      const float *px_ab = get_color_pixel(a_b, k);
      float res = guide_weight * (px_ab[A_RED] * pixel[0] + px_ab[A_GREEN] * pixel[1] + px_ab[A_BLUE] * pixel[2]);
      res += px_ab[B];
      img_out.data[i_imgg + (size_t)j_imgg * imgg.width] = CLAMP(res, min, max);
    }
  }
  free_color_image(&mean);
  free_color_image(&covar);
  free_color_image(&variance);
  free_gray_image(&var_imgg_bb);
}

static int compute_tile_height(const int height, const int w)
{
  int tile_h = max_i(3 * w, GF_TILE_SIZE);
#if 0 // enabling the below doesn't make any measureable speed difference, but does cause a handfull of pixels
      // to round off differently (as does changing GF_TILE_SIZE)
  if ((height % tile_h) > 0 && (height % tile_h) < GF_TILE_SIZE/3)
  {
    // if there's just a sliver left over for the last row of tiles, see whether slicing off a few pixels
    // gives us a mostly-full tile
    if (height % (tile_h - 8) >= GF_TILE_SIZE/3)
      tile_h -= 8;
    else  if (height % (tile_h - w/4) >= GF_TILE_SIZE/3)
      tile_h -= (w/4);
    else  if (height % (tile_h - w/2) >= GF_TILE_SIZE/3)
      tile_h -= (w/2);
    // try adding a few pixels
    else if (height % (tile_h + 8) >= GF_TILE_SIZE/3)
      tile_h += 8;
    else if (height % (tile_h + 16) >= GF_TILE_SIZE/3)
      tile_h += 16;
  }
#endif
  return tile_h;
}

static int compute_tile_width(const int width, const int w)
{
  int tile_w = max_i(3 * w, GF_TILE_SIZE);
#if 0 // enabling the below doesn't make any measureable speed difference, but does cause a handfull of pixels
      // to round off differently (as does changing GF_TILE_SIZE)
  if ((width % tile_w) > 0 && (width % tile_w) < GF_TILE_SIZE/2)
  {
    // if there's just a sliver left over for the last column of tiles, see whether slicing off a few pixels
    // gives us a mostly-full tile
    if (width % (tile_w - 8) >= GF_TILE_SIZE/3)
      tile_w -= 8;
    else  if (width % (tile_w - w/4) >= GF_TILE_SIZE/3)
      tile_w -= (w/4);
    else  if (width % (tile_w - w/2) >= GF_TILE_SIZE/3)
      tile_w -= (w/2);
    // try adding a few pixels
    else if (width % (tile_w + 8) >= GF_TILE_SIZE/3)
      tile_w += 8;
    else if (width % (tile_w + 16) >= GF_TILE_SIZE/3)
      tile_w += 16;
  }
#endif
  return tile_w;
}

void guided_filter(const float *const guide, const float *const in, float *const out, const int width,
                   const int height, const int ch,
                   const int w,              // window size
                   const float sqrt_eps,     // regularization parameter
                   const float guide_weight, // to balance the amplitudes in the guiding image and the input image
                   const float min, const float max)
{
  assert(ch >= 3);
  assert(w >= 1);

  color_image img_guide = (color_image){ (float *)guide, width, height, ch };
  gray_image img_in = (gray_image){ (float *)in, width, height };
  gray_image img_out = (gray_image){ out, width, height };
  const int tile_width = compute_tile_width(width,w);
  const int tile_height = compute_tile_height(height,w);
  const float eps = sqrt_eps * sqrt_eps; // this is the regularization parameter of the original papers

#ifdef _OPENMP
#pragma omp parallel for collapse(2) schedule(static)
#endif
  for(int j = 0; j < height; j += tile_height)
  {
    for(int i = 0; i < width; i += tile_width)
    {
      tile target = { i, min_i(i + tile_width, width), j, min_i(j + tile_height, height) };
      guided_filter_tiling(img_guide, img_in, img_out, target, w, eps, guide_weight, min, max);
    }
  }
}
