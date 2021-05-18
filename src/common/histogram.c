/*
    This file is part of darktable,
    Copyright (C) 2014-2020 darktable developers.

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
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include <assert.h>
#include <stdlib.h>

#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/histogram.h"
#include "develop/imageop.h"

#define S(V, params) ((params->mul) * ((float)V))
#define P(V, params) (CLAMP((V), 0, (params->bins_count - 1)))
#define PU(V, params) (MIN((V), (params->bins_count - 1)))
#define PS(V, params) (P(S(V, params), params))

//------------------------------------------------------------------------------

inline static void histogram_helper_cs_RAW(const dt_dev_histogram_collection_params_t *const histogram_params,
                                           const void *pixel, uint32_t *histogram, int j,
                                           const dt_iop_order_iccprofile_info_t *const profile_info, const int ch)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  const float *input = (float *)pixel + roi->width * j + roi->crop_x;
  fprintf(stderr, "in common/histogram, L45, cs_RAW\n");
  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++, input++)
  {
    const uint32_t p = PS(*input, histogram_params);
    histogram[p]++;
  }
}

inline void dt_histogram_helper_cs_RAW_uint16(const dt_dev_histogram_collection_params_t *const histogram_params,
                                              const void *pixel, uint32_t *histogram, int j,
                                              const dt_iop_order_iccprofile_info_t *const profile_info, const int ch)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  uint16_t *in = (uint16_t *)pixel + roi->width * j + roi->crop_x;
  fprintf(stderr, "in common/histogram, L58, cs_RAW_uint16\n");
  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++, in++)
  {
    const uint16_t p = PU(*in, histogram_params);
    histogram[p]++;
  }
}

//------------------------------------------------------------------------------

inline static void histogram_helper_cs_rgb_helper_process_pixel_float(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel,
    uint32_t *histogram, const int ch)
{
  const uint32_t R = PS(pixel[0], histogram_params);
  histogram[4 * R]++;
  if(ch > 1)
  {
    const uint32_t G = PS(pixel[1], histogram_params);
    const uint32_t B = PS(pixel[2], histogram_params);
    histogram[4 * G + 1]++;
    histogram[4 * B + 2]++;
  }
  else
    histogram[4 * R + 1] = histogram[4 * R + 2] = histogram[4 * R];///////////////////////////
}

inline static void histogram_helper_cs_rgb(const dt_dev_histogram_collection_params_t *const histogram_params,
                                           const void *pixel, uint32_t *histogram, int j,
                                           const dt_iop_order_iccprofile_info_t *const profile_info, const int ch)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  float *in = (float *)pixel + 4 * (roi->width * j + roi->crop_x);

  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++, in += 4)
    histogram_helper_cs_rgb_helper_process_pixel_float(histogram_params, in, histogram, ch);
}

//------------------------------------------------------------------------------

inline static void histogram_helper_cs_Lab_helper_process_pixel_float(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel,
    uint32_t *histogram, const int ch)
{
  const float Lv = pixel[0];
  const float max = histogram_params->bins_count - 1;
  const uint32_t L = CLAMP(histogram_params->mul / 100.0f * (Lv), 0, max);
  histogram[4 * L]++;
  if(ch > 1)
  {
    const float av = pixel[1];
    const float bv = pixel[2];
    const uint32_t a = CLAMP(histogram_params->mul / 256.0f * (av + 128.0f), 0, max);
    const uint32_t b = CLAMP(histogram_params->mul / 256.0f * (bv + 128.0f), 0, max);
    histogram[4 * a + 1]++;
    histogram[4 * b + 2]++;
  }
}

inline static void histogram_helper_cs_Lab(const dt_dev_histogram_collection_params_t *const histogram_params,
                                           const void *pixel, uint32_t *histogram, int j,
                                           const dt_iop_order_iccprofile_info_t *const profile_info, const int ch)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  float *in = (float *)pixel + 4 * (roi->width * j + roi->crop_x);

  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++, in += 4)
    histogram_helper_cs_Lab_helper_process_pixel_float(histogram_params, in, histogram, ch);
}

inline static void histogram_helper_cs_Lab_LCh_helper_process_pixel_float(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel, uint32_t *histogram, const int ch)
{
  float LCh[3];
  dt_Lab_2_LCH(pixel, LCh);
  const uint32_t L = PS((LCh[0] / 100.f), histogram_params);
  histogram[4 * L]++;
  if(ch > 1)
  {
    const uint32_t C = PS((LCh[1] / (128.0f * sqrtf(2.0f))), histogram_params);
    const uint32_t h = PS(LCh[2], histogram_params);
    histogram[4 * C + 1]++;
    histogram[4 * h + 2]++;
  }
}

inline static void histogram_helper_cs_Lab_LCh(const dt_dev_histogram_collection_params_t *const histogram_params,
                                               const void *pixel, uint32_t *histogram, int j,
                                               const dt_iop_order_iccprofile_info_t *const profile_info, const int ch)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  float *in = (float *)pixel + 4 * (roi->width * j + roi->crop_x);
  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++, in += 4)
    histogram_helper_cs_Lab_LCh_helper_process_pixel_float(histogram_params, in, histogram, ch);
}

//==============================================================================

void dt_histogram_worker(dt_dev_histogram_collection_params_t *const histogram_params,
                         dt_dev_histogram_stats_t *histogram_stats, const void *const pixel,
                         uint32_t **histogram, const dt_worker Worker,
                         const dt_iop_order_iccprofile_info_t *const profile_info, const int ch)
{
  const int nthreads = omp_get_max_threads();
  const size_t bins_total = (size_t)4 * histogram_params->bins_count;
  const size_t buf_size = bins_total * sizeof(uint32_t);
  void *partial_hists = calloc(nthreads, buf_size);

  if(histogram_params->mul == 0)
    histogram_params->mul = (double)(histogram_params->bins_count - 1);

  const dt_histogram_roi_t *const roi = histogram_params->roi;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(histogram_params, pixel, Worker, profile_info, bins_total, roi, ch) \
  shared(partial_hists) \
  schedule(static)
#endif
  for(int j = roi->crop_y; j < roi->height - roi->crop_height; j++)
  {
    uint32_t *thread_hist = (uint32_t *)partial_hists + bins_total * omp_get_thread_num();
    Worker(histogram_params, pixel, thread_hist, j, profile_info, ch);
  }

#ifdef _OPENMP
  *histogram = realloc(*histogram, buf_size);
  memset(*histogram, 0, buf_size);
  uint32_t *hist = *histogram;

#pragma omp parallel for default(none) \
  dt_omp_firstprivate(nthreads, bins_total) \
  shared(hist, partial_hists) \
  schedule(static)
  for(size_t k = 0; k < bins_total; k++)
    for(size_t n = 0; n < nthreads; n++)
    {
      const uint32_t *thread_hist = (uint32_t *)partial_hists + bins_total * n;
      hist[k] += thread_hist[k];
    }
#else
  *histogram = realloc(*histogram, buf_size);
  memmove(*histogram, partial_hists, buf_size);
#endif
  free(partial_hists);

  histogram_stats->bins_count = histogram_params->bins_count;
  histogram_stats->pixels = (roi->width - roi->crop_width - roi->crop_x)
                            * (roi->height - roi->crop_height - roi->crop_y);
}

//------------------------------------------------------------------------------

void dt_histogram_helper(dt_dev_histogram_collection_params_t *histogram_params,
    dt_dev_histogram_stats_t *histogram_stats, const dt_iop_colorspace_type_t cst,
    const dt_iop_colorspace_type_t cst_to, const void *pixel, uint32_t **histogram,
    const int compensate_middle_grey, const dt_iop_order_iccprofile_info_t *const profile_info, const int ch_in)
{
  switch(cst)
  {
    case iop_cs_RAW:
      dt_histogram_worker(histogram_params, histogram_stats, pixel, histogram, histogram_helper_cs_RAW,
                          profile_info, ch_in);
      histogram_stats->ch = 1u;
      break;

    case iop_cs_rgb:
      dt_histogram_worker(histogram_params, histogram_stats, pixel, histogram, histogram_helper_cs_rgb,
                            profile_info, ch_in);
      histogram_stats->ch = 3u;
      break;

    case iop_cs_Lab:
    default:
      if(cst_to != iop_cs_LCh)
        dt_histogram_worker(histogram_params, histogram_stats, pixel, histogram, histogram_helper_cs_Lab,
                            profile_info, ch_in);
      else
        dt_histogram_worker(histogram_params, histogram_stats, pixel, histogram, histogram_helper_cs_Lab_LCh,
                            profile_info, ch_in);
    
      histogram_stats->ch = 3u;
      break;
  }
}

void dt_histogram_max_helper(const dt_dev_histogram_stats_t *const histogram_stats,
                             const dt_iop_colorspace_type_t cst, const dt_iop_colorspace_type_t cst_to,
                             uint32_t **histogram, uint32_t *histogram_max)
{
  if(*histogram == NULL) return;
  histogram_max[0] = histogram_max[1] = histogram_max[2] = histogram_max[3] = 0;
  uint32_t *hist = *histogram;
  switch(cst)
  {
    case iop_cs_RAW:
      for(int k = 0; k < histogram_stats->bins_count; k += 4)   // removed mult by 4 (replaced)
        histogram_max[0] = histogram_max[0] > hist[k] ? histogram_max[0] : hist[k];
      break;

    case iop_cs_rgb:
      // don't count <= 0 pixels
      for(int j = 0; j< 4; j++)
        for(int k = 4 + j; k < 4 * histogram_stats->bins_count; k += 4)
          histogram_max[j] = histogram_max[j] > hist[k] ? histogram_max[j] : hist[k];
      break;

    case iop_cs_Lab:
    default:
      if(cst_to == iop_cs_LCh)
      {
        // don't count <= 0 pixels
        for(int j = 0; j< 4; j++)
          for(int k = 4 + j; k < 4 * histogram_stats->bins_count; k += 4)
            histogram_max[j] = histogram_max[j] > hist[k] ? histogram_max[j] : hist[k];
      }
      else
      {
        // don't count <= 0 pixels in L
        for(int k = 4; k < 4 * histogram_stats->bins_count; k += 4)
          histogram_max[0] = histogram_max[0] > hist[k] ? histogram_max[0] : hist[k];
        // don't count <= -128 and >= +128 pixels in a and b
        for(int j = 1; j< 3; j++)
          for(int k = 4 + j; k < 4 * (histogram_stats->bins_count - 1); k += 4)
            histogram_max[j] = histogram_max[j] > hist[k] ? histogram_max[j] : hist[k];
      }
      break;
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
