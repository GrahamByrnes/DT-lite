/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#include "blend.h"
#include "common/gaussian.h"
#include "common/guided_filter.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/math.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/tiling.h"
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/darktable.h"
#include "control/conf.h"
#include <stdlib.h>
#include <math.h>


typedef struct _blend_buffer_desc_t
{
  dt_iop_colorspace_type_t cst;
  size_t stride;
  size_t ch;
  size_t bch;
} _blend_buffer_desc_t;

typedef void(_blend_row_func)(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask);

static inline void _CLAMP_XYZ(float *XYZ, const float *min, const float *max)
{
  XYZ[0] =clamp_range_f(XYZ[0], min[0], max[0]);
  XYZ[1] =clamp_range_f(XYZ[1], min[1], max[1]);
  XYZ[2] =clamp_range_f(XYZ[2], min[2], max[2]);
}

static inline void _PX_COPY(const float *src, float *dst)
{
  dst[0] = src[0];
  dst[1] = src[1];
  dst[2] = src[2];
}

static inline float _blendif_factor(dt_iop_colorspace_type_t cst, const float *input, const float *output,
                                    const unsigned int blendif, const float *parameters,
                                    const unsigned int mask_mode, const unsigned int mask_combine,
                                    const dt_iop_order_iccprofile_info_t *work_profile)
{
  float result = 1.0f;
  float scaled[DEVELOP_BLENDIF_SIZE] = { 0.5f };
  unsigned int channel_mask = 0;

  if(!(mask_mode & DEVELOP_MASK_CONDITIONAL)) return (mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;

  switch(cst)
  {
    case iop_cs_Lab:
      scaled[DEVELOP_BLENDIF_L_in] = clamp_range_f(input[0] / 100.0f, 0.0f, 1.0f); // L scaled to 0..1
      scaled[DEVELOP_BLENDIF_A_in]
          = clamp_range_f((input[1] + 128.0f) / 256.0f, 0.0f, 1.0f); // a scaled to 0..1
      scaled[DEVELOP_BLENDIF_B_in]
          = clamp_range_f((input[2] + 128.0f) / 256.0f, 0.0f, 1.0f); // b scaled to 0..1
      scaled[DEVELOP_BLENDIF_L_out] = clamp_range_f(output[0] / 100.0f, 0.0f, 1.0f); // L scaled to 0..1
      scaled[DEVELOP_BLENDIF_A_out]
          = clamp_range_f((output[1] + 128.0f) / 256.0f, 0.0f, 1.0f); // a scaled to 0..1
      scaled[DEVELOP_BLENDIF_B_out]
          = clamp_range_f((output[2] + 128.0f)/256.0f, 0.0f, 1.0f); // b scaled to 0..1
      channel_mask = DEVELOP_BLENDIF_Lab_MASK;
      break;
    case iop_cs_rgb:
      if(work_profile == NULL)
        scaled[DEVELOP_BLENDIF_GRAY_in] = clamp_range_f(0.3f * input[0] + 0.59f * input[1] 
                                                        + 0.11f * input[2], 0.0f, 1.0f);
      else
        scaled[DEVELOP_BLENDIF_GRAY_in] = clamp_range_f(dt_ioppr_get_rgb_matrix_luminance(input,
                                                            work_profile->matrix_in), 0.0f, 1.0f);

      scaled[DEVELOP_BLENDIF_RED_in] = clamp_range_f(input[0], 0.0f, 1.0f);   // Red
      scaled[DEVELOP_BLENDIF_GREEN_in] = clamp_range_f(input[1], 0.0f, 1.0f); // Green
      scaled[DEVELOP_BLENDIF_BLUE_in] = clamp_range_f(input[2], 0.0f, 1.0f);  // Blue

      if(work_profile == NULL)
        scaled[DEVELOP_BLENDIF_GRAY_out] = clamp_range_f(0.3f * output[0] + 0.59f * output[1]
                                                         + 0.11f * output[2], 0.0f, 1.0f);
      else
        scaled[DEVELOP_BLENDIF_GRAY_out] = clamp_range_f(dt_ioppr_get_rgb_matrix_luminance(output,
                                                             work_profile->matrix_in), 0.0f, 1.0f);

      scaled[DEVELOP_BLENDIF_RED_out] = clamp_range_f(output[0], 0.0f, 1.0f);   // Red
      scaled[DEVELOP_BLENDIF_GREEN_out] = clamp_range_f(output[1], 0.0f, 1.0f); // Green
      scaled[DEVELOP_BLENDIF_BLUE_out] = clamp_range_f(output[2], 0.0f, 1.0f);  // Blue
      channel_mask = DEVELOP_BLENDIF_RGB_MASK;

      break;
    default:
      return (mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f; // not implemented for other color spaces
  }

  for(int ch = 0; ch <= DEVELOP_BLENDIF_MAX; ch++)
  {
    if((channel_mask & (1 << ch)) == 0) continue; // skip blendif channels not used in this color space

    if((blendif & (1 << ch)) == 0) // deal with channels where sliders span the whole range
    {
      result *= !(blendif & (1 << (ch + 16))) == !(mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f : 0.0f;
      continue;
    }

    if(result <= 0.000001f) break; // no need to continue if we are already at or close to zero

    float factor;

    if(scaled[ch] >= parameters[4 * ch + 1] && scaled[ch] <= parameters[4 * ch + 2])
      factor = 1.0f;
    else if(scaled[ch] > parameters[4 * ch + 0] && scaled[ch] < parameters[4 * ch + 1]) 
      factor = (scaled[ch] - parameters[4 * ch + 0]) 
                      / fmaxf(0.01f, parameters[4 * ch + 1] - parameters[4 * ch + 0]); 
    else if(scaled[ch] > parameters[4 * ch + 2] && scaled[ch] < parameters[4 * ch + 3]) 
      factor = 1.0f - (scaled[ch] - parameters[4 * ch + 2])
                      / fmaxf(0.01f, parameters[4 * ch + 3] - parameters[4 * ch + 2]); 
    else
      factor = 0.0f;

    if((blendif & (1 << (ch + 16))) != 0)
      factor = 1.0f - factor; // inverted channel?

    result *= ((mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - factor : factor);
  }

  return (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - result : result;
}

static inline void _blend_colorspace_channel_range(dt_iop_colorspace_type_t cst, float *min, float *max)
{
  switch(cst)
  {
    case iop_cs_Lab: // after scaling !!!
      min[0] = 0.0f;
      max[0] = 1.0f;
      min[1] = -1.0f;
      max[1] = 1.0f;
      min[2] = -1.0f;
      max[2] = 1.0f;
      min[3] = 0.0f;
      max[3] = 1.0f;
      break;
    default:
      min[0] = 0.0f;
      max[0] = 1.0f;
      min[1] = 0.0f;
      max[1] = 1.0f;
      min[2] = 0.0f;
      max[2] = 1.0f;
      min[3] = 0.0f;
      max[3] = 1.0f;
      break;
  }
}

static inline void _blend_Lab_scale(const float *i, float *o)
{
  o[0] = i[0] / 100.0f;
  o[1] = i[1] / 128.0f;
  o[2] = i[2] / 128.0f;
}

static inline void _blend_Lab_rescale(const float *i, float *o)
{
  o[0] = i[0] * 100.0f;
  o[1] = i[1] * 128.0f;
  o[2] = i[2] * 128.0f;
}


static inline void _blend_noop(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask,
                               const float *min, const float *max)
{
  for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
  {
    for(int k = 0; k < bd->bch; k++) b[j + k] =clamp_range_f(a[j+k], min ? min[k] : -INFINITY, max ? max[k] : INFINITY);
    if(bd->cst != iop_cs_RAW) b[j + 3] = mask[i];
  }
}


/* generate blend mask */
static void _blend_make_mask(const _blend_buffer_desc_t *bd, const unsigned int blendif,
                             const float *blendif_parameters, const unsigned int mask_mode,
                             const unsigned int mask_combine, const float gopacity, const float *a, const float *b,
                             float *mask, const dt_iop_order_iccprofile_info_t *const work_profile)
{
  for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
  {
    float form = mask[i];
    float conditional = _blendif_factor(bd->cst, &a[j], &b[j], blendif, blendif_parameters, mask_mode,
                                        mask_combine, work_profile);
    float opacity = (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - (1.0f - form) * (1.0f - conditional)
                                                          : form * conditional;
    opacity = (mask_combine & DEVELOP_COMBINE_INV) ? 1.0f - opacity : opacity;
    mask[i] = opacity * gopacity;
  }
}

/* normal blend without any clamping */
static void _blend_normal_unbounded(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(bd->cst, min, max);

  if(bd->cst == iop_cs_Lab)
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      float ta[3], tb[3];
      _blend_Lab_scale(&a[j], ta);
      _blend_Lab_scale(&b[j], tb);

      tb[0] = ta[0] * (1.0f - local_opacity) + tb[0] * local_opacity;
      tb[1] = ta[1] * (1.0f - local_opacity) + tb[1] * local_opacity;
      tb[2] = ta[2] * (1.0f - local_opacity) + tb[2] * local_opacity;

      _blend_Lab_rescale(tb, &b[j]);
      b[j + 3] = local_opacity;
    }
  else if(bd->cst == iop_cs_rgb)
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];

      for(int k = 0; k < bd->bch; k++)
        b[j + k] = a[j + k] * (1.0f - local_opacity) + b[j + k] * local_opacity;

      b[j + 3] = local_opacity;
    }
  else
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];

      for(int k = 0; k < bd->bch; k++)
        b[j + k] = a[j + k] * (1.0f - local_opacity) + b[j + k] * local_opacity;
    }
}

/* multiply */
static void _blend_multiply(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(bd->cst, min, max);

  if(bd->cst == iop_cs_Lab)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      float ta[3], tb[3];
      float lmin = 0.0f, lmax, la, lb;

      _blend_Lab_scale(&a[j], ta);
      _blend_Lab_scale(&b[j], tb);
      lmax = max[0] + fabsf(min[0]);
      la = clamp_range_f(ta[0] + fabsf(min[0]), lmin, lmax);
      lb = clamp_range_f(tb[0] + fabsf(min[0]), lmin, lmax);

      tb[0] = clamp_range_f((la * (1.0f - local_opacity)) + ((la * lb) * local_opacity), min[0], max[0])
              - fabsf(min[0]);

      if(ta[0] > 0.01f)
      {
        tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity) + (ta[1] + tb[1]) * tb[0] / ta[0] * local_opacity,
                              min[1], max[1]);
        tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity) + (ta[2] + tb[2]) * tb[0] / ta[0] * local_opacity,
                              min[2], max[2]);
      }
      else
      {
        tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity) + (ta[1] + tb[1]) * tb[0] / 0.01f * local_opacity,
                              min[1], max[1]);
        tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity) + (ta[2] + tb[2]) * tb[0] / 0.01f * local_opacity,
                              min[2], max[2]);
      }

      _blend_Lab_rescale(tb, &b[j]);
      b[j + 3] = local_opacity;
    }
  }
  else if(bd->cst == iop_cs_rgb)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      for(int k = 0; k < bd->bch; k++)
        b[j + k] = clamp_range_f(
            a[j + k] * (1.0f - local_opacity) + (a[j + k] * b[j + k]) * local_opacity, min[k], max[k]);

      b[j + 3] = local_opacity;
    }
  }
  else
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      for(int k = 0; k < bd->bch; k++)
        b[j + k] = clamp_range_f(
            a[j + k] * (1.0f - local_opacity) + (a[j + k] * b[j + k]) * local_opacity, min[k], max[k]);
    }
  }
}

/* average */
static void _blend_average(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(bd->cst, min, max);

  if(bd->cst == iop_cs_Lab)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      float ta[3], tb[3];
      _blend_Lab_scale(&a[j], ta);
      _blend_Lab_scale(&b[j], tb);

      tb[0] = clamp_range_f(ta[0] * (1.0f - local_opacity) + (ta[0] + tb[0]) / 2.0f * local_opacity, min[0], max[0]);
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity) + (ta[1] + tb[1]) / 2.0f * local_opacity, min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity) + (ta[2] + tb[2]) / 2.0f * local_opacity, min[2], max[2]);

      _blend_Lab_rescale(tb, &b[j]);
      b[j + 3] = local_opacity;
    }
  }
  else if(bd->cst == iop_cs_rgb)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      for(int k = 0; k < bd->bch; k++)
        b[j + k] =clamp_range_f(
            a[j + k] * (1.0f - local_opacity) + (a[j + k] + b[j + k]) / 2.0f * local_opacity, min[k], max[k]);

      b[j + 3] = local_opacity;
    }
  }
  else
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      for(int k = 0; k < bd->bch; k++)
        b[j + k] =clamp_range_f(
            a[j + k] * (1.0f - local_opacity) + (a[j + k] + b[j + k]) / 2.0f * local_opacity, min[k], max[k]);
    }
  }
}

/* add */
static void _blend_add(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(bd->cst, min, max);

  if(bd->cst == iop_cs_Lab)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      float ta[3], tb[3];
      _blend_Lab_scale(&a[j], ta);
      _blend_Lab_scale(&b[j], tb);

      tb[0] = clamp_range_f(ta[0] * (1.0f - local_opacity) + (ta[0] + tb[0]) * local_opacity, min[0], max[0]);
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity) + (ta[1] + tb[1]) * local_opacity, min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity) + (ta[2] + tb[2]) * local_opacity, min[2], max[2]);

      _blend_Lab_rescale(tb, &b[j]);
      b[j + 3] = local_opacity;
    }
  }
  else if(bd->cst == iop_cs_rgb)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      for(int k = 0; k < bd->bch; k++)
        b[j + k] =clamp_range_f(
            a[j + k] * (1.0f - local_opacity) + (a[j + k] + b[j + k]) * local_opacity, min[k], max[k]);
      b[j + 3] = local_opacity;
    }
  }
  else /* if(bd->cst == iop_cs_RAW) */
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      for(int k = 0; k < bd->bch; k++)
        b[j + k] =clamp_range_f(
            a[j + k] * (1.0f - local_opacity) + (a[j + k] + b[j + k]) * local_opacity, min[k], max[k]);
    }
  }
}

/* substract */
static void _blend_substract(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(bd->cst, min, max);

  if(bd->cst == iop_cs_Lab)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      float ta[3], tb[3];
      _blend_Lab_scale(&a[j], ta);
      _blend_Lab_scale(&b[j], tb);
      for(int k = 0; k < bd->bch; k++)
        tb[k] = clamp_range_f(ta[k] * (1.0f - local_opacity)
                                + ((tb[k] + ta[k]) - (fabsf(min[k] + max[k]))) * local_opacity,  min[k], max[k]);

      _blend_Lab_rescale(tb, &b[j]);
      b[j + 3] = local_opacity;
    }
  }
  else if(bd->cst == iop_cs_rgb)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      for(int k = 0; k < bd->bch; k++)
        b[j + k] =clamp_range_f(a[j + k] * (1.0f - local_opacity)
                                +((b[j + k]+a[j + k]) - (fabsf(min[k] + max[k]))) * local_opacity, min[k], max[k]);
      b[j + 3] = local_opacity;
    }
  }
  else /* if(bd->cst == iop_cs_RAW) */
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      for(int k = 0; k < bd->bch; k++)
        b[j + k] = clamp_range_f(a[j + k] * (1.0f - local_opacity)
                                +((b[j + k] + a[j + k]) - (fabsf(min[k] + max[k]))) * local_opacity, min[k], max[k]);
    }
  }
}

/* difference 2 (new) */
static void _blend_difference2(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(bd->cst, min, max);

  if(bd->cst == iop_cs_Lab)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      float ta[3], tb[3];
      _blend_Lab_scale(&a[j], ta);
      _blend_Lab_scale(&b[j], tb);

      tb[0] = fabsf(ta[0] - tb[0]) / fabsf(max[0] - min[0]);
      tb[1] = fabsf(ta[1] - tb[1]) / fabsf(max[1] - min[1]);
      tb[2] = fabsf(ta[2] - tb[2]) / fabsf(max[2] - min[2]);
      tb[0] = fmaxf(tb[0], fmaxf(tb[1], tb[2]));

      tb[0] = clamp_range_f(ta[0] * (1.0f - local_opacity) + tb[0] * local_opacity, min[0], max[0]);
      tb[1] = 0.0f;
      tb[2] = 0.0f;

      _blend_Lab_rescale(tb, &b[j]);
      b[j + 3] = local_opacity;
    }
  }
  else if(bd->cst == iop_cs_rgb)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      float lmin = 0.0f, lmax, la, lb;
      for(int k = 0; k < bd->bch; k++)
      {
        lmax = max[k] + fabsf(min[k]);
        la = a[j + k] + fabsf(min[k]);
        lb = b[j + k] + fabsf(min[k]);

        b[j + k] = clamp_range_f(la * (1.0f - local_opacity) + fabsf(la - lb) * local_opacity, lmin, lmax)
                   - fabsf(min[k]);
      }

      b[j + 3] = local_opacity;
    }
  }
  else
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      float lmin = 0.0f, lmax, la, lb;
      for(int k = 0; k < bd->bch; k++)
      {
        lmax = max[k] + fabsf(min[k]);
        la = a[j + k] + fabsf(min[k]);
        lb = b[j + k] + fabsf(min[k]);

        b[j + k] = clamp_range_f(la * (1.0f - local_opacity) + fabsf(la - lb) * local_opacity, lmin, lmax)
                   - fabsf(min[k]);
      }
    }
  }
}

/* blend only lightness in Lab color space without any clamping (a noop for
 * other color spaces) */
static void _blend_Lab_lightness(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask)
{
  if(bd->cst == iop_cs_Lab)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      float ta[3], tb[3];
      _blend_Lab_scale(&a[j], ta);
      _blend_Lab_scale(&b[j], tb);

      tb[0] = ta[0] * (1.0f - local_opacity) + tb[0] * local_opacity;
      tb[1] = ta[1];
      tb[2] = ta[2];

      _blend_Lab_rescale(tb, &b[j]);
      b[j + 3] = local_opacity;
    }
  }
  else
    _blend_noop(bd, a, b, mask, NULL, NULL); // Noop for RGB and Raw (unclamped)
}

/* blend only a-channel in Lab color space without any clamping (a noop for
 * other color spaces) */
static void _blend_Lab_a(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask)
{
  if(bd->cst == iop_cs_Lab)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      float ta[3], tb[3];
      _blend_Lab_scale(&a[j], ta);
      _blend_Lab_scale(&b[j], tb);

      tb[0] = ta[0];
      tb[1] = ta[1] * (1.0f - local_opacity) + tb[1] * local_opacity;
      tb[2] = ta[2];

      _blend_Lab_rescale(tb, &b[j]);
      b[j + 3] = local_opacity;
    }
  }
  else
    _blend_noop(bd, a, b, mask, NULL, NULL); // Noop for RGB and Raw (unclamped)
}

/* blend only b-channel in Lab color space without any clamping (a noop for
 * other color spaces) */
static void _blend_Lab_b(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask)
{
  if(bd->cst == iop_cs_Lab)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      float ta[3], tb[3];
      _blend_Lab_scale(&a[j], ta);
      _blend_Lab_scale(&b[j], tb);

      tb[0] = ta[0];
      tb[1] = ta[1];
      tb[2] = ta[2] * (1.0f - local_opacity) + tb[2] * local_opacity;

      _blend_Lab_rescale(tb, &b[j]);
      b[j + 3] = local_opacity;
    }
  }
  else
    _blend_noop(bd, a, b, mask, NULL, NULL); // Noop for RGB and Raw (unclamped)
}

/* blend only color in Lab color space without any clamping (a noop for other
 * color spaces) */
static void _blend_Lab_color(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask)
{
  if(bd->cst == iop_cs_Lab)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];
      float ta[3], tb[3];
      _blend_Lab_scale(&a[j], ta);
      _blend_Lab_scale(&b[j], tb);

      tb[0] = ta[0];
      tb[1] = ta[1] * (1.0f - local_opacity) + tb[1] * local_opacity;
      tb[2] = ta[2] * (1.0f - local_opacity) + tb[2] * local_opacity;

      _blend_Lab_rescale(tb, &b[j]);
      b[j + 3] = local_opacity;
    }
  }
  else
    _blend_noop(bd, a, b, mask, NULL, NULL); // Noop for RGB and Raw (unclamped)
}

/* blend only R-channel in RGB color space without any clamping (a noop for
 * other color spaces) */
static void _blend_RGB_R(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask)
{
  if(bd->cst == iop_cs_rgb)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];

      b[j + 0] = a[j + 0] * (1.0f - local_opacity) + b[j + 0] * local_opacity;
      b[j + 1] = a[j + 1];
      b[j + 2] = a[j + 2];
      b[j + 3] = local_opacity;
    }
  }
  else
    _blend_noop(bd, a, b, mask, NULL, NULL); // Noop for Lab and Raw (unclamped)
}

/* blend only R-channel in RGB color space without any clamping (a noop for
 * other color spaces) */
static void _blend_RGB_G(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask)
{
  if(bd->cst == iop_cs_rgb)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];

      b[j + 0] = a[j + 0];
      b[j + 1] = a[j + 1] * (1.0f - local_opacity) + b[j + 1] * local_opacity;
      b[j + 2] = a[j + 2];
      b[j + 3] = local_opacity;
    }
  }
  else
    _blend_noop(bd, a, b, mask, NULL, NULL); // Noop for Lab and Raw (unclamped)
}

/* blend only R-channel in RGB color space without any clamping (a noop for
 * other color spaces) */
static void _blend_RGB_B(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask)
{
  if(bd->cst == iop_cs_rgb)
  {
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
    {
      float local_opacity = mask[i];

      b[j + 0] = a[j + 0];
      b[j + 1] = a[j + 1];
      b[j + 2] = a[j + 2] * (1.0f - local_opacity) + b[j + 2] * local_opacity;
      b[j + 3] = local_opacity;
    }
  }
  else
    _blend_noop(bd, a, b, mask, NULL, NULL); // Noop for Lab and Raw (unclamped)
}

static void display_channel(const _blend_buffer_desc_t *bd, const float *a, float *b, const float *mask,
                            dt_dev_pixelpipe_display_mask_t channel,
                            const dt_iop_order_iccprofile_info_t *work_profile)
{

  switch(channel & DT_DEV_PIXELPIPE_DISPLAY_ANY)
  {
    case DT_DEV_PIXELPIPE_DISPLAY_L:
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        const float c =clamp_range_f(a[j]/100.0f, 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_L | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        const float c =clamp_range_f(b[j]/100.0f, 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_a:
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        const float c =clamp_range_f((a[j+1]+128.0f)/256.0f, 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_a | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        const float c =clamp_range_f((b[j+1]+128.0f)/256.0f, 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_b:
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        const float c =clamp_range_f((a[j+2]+128.0f)/256.0f, 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_b | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        const float c =clamp_range_f((b[j+2]+128.0f)/256.0f, 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_R:
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        const float c =clamp_range_f(a[j], 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_R | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        const float c =clamp_range_f(b[j], 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_G:
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        const float c =clamp_range_f(a[j+1], 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_G | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        const float c =clamp_range_f(b[j+1], 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_B:
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        const float c =clamp_range_f(a[j+2], 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_B | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        const float c =clamp_range_f(b[j+2], 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_GRAY:
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        const float c = (work_profile == NULL)
                          ? clamp_range_f(0.3f * a[j] + 0.59f * a[j + 1] + 0.11f * a[j + 2], 0.0f, 1.0f)
                          : clamp_range_f(dt_ioppr_get_rgb_matrix_luminance(a + j, work_profile->matrix_in), 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_GRAY | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        const float c = (work_profile == NULL)
                          ? clamp_range_f(0.3f * a[j] + 0.59f * a[j + 1] + 0.11f * a[j + 2], 0.0f, 1.0f)
                          : clamp_range_f(dt_ioppr_get_rgb_matrix_luminance(b + j, work_profile->matrix_in), 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_C:
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        float LCH[3];
        dt_Lab_2_LCH(a + j, LCH);
        const float c =clamp_range_f(LCH[1]/(128.0f*sqrtf(2.0f)), 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_LCH_C | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        float LCH[3];
        dt_Lab_2_LCH(b + j, LCH);
        const float c =clamp_range_f(LCH[1] / (128.0f * sqrtf(2.0f)), 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_h:
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        float LCH[3];
        dt_Lab_2_LCH(a + j, LCH);
        const float c =clamp_range_f(LCH[2], 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_LCH_h | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        float LCH[3];
        dt_Lab_2_LCH(b + j, LCH);
        const float c =clamp_range_f(LCH[2], 0.0f, 1.0f);
        for(int k = 0; k < bd->bch; k++) b[j + k] = c;
      }
      break;
    default:
      for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      {
        for(int k = 0; k < bd->bch; k++) b[j + k] = 0.0f;
      }
      break;
  }

  if(bd->cst != iop_cs_rgb)
    for(size_t i = 0, j = 0; j < bd->stride; i++, j += bd->ch)
      b[j + 3] = mask[i];
}

_blend_row_func *dt_develop_choose_blend_func(const unsigned int blend_mode)
{
  _blend_row_func *blend = NULL;
  /* select the blend operator */
  switch(blend_mode)
  {
    case DEVELOP_BLEND_MULTIPLY:
      blend = _blend_multiply;
      break;
    case DEVELOP_BLEND_AVERAGE:
      blend = _blend_average;
      break;
    case DEVELOP_BLEND_ADD:
      blend = _blend_add;
      break;
    case DEVELOP_BLEND_SUBSTRACT:
      blend = _blend_substract;
      break;
    case DEVELOP_BLEND_DIFFERENCE2:
      blend = _blend_difference2;
      break;
    case DEVELOP_BLEND_LAB_LIGHTNESS:
    case DEVELOP_BLEND_LAB_L:
      blend = _blend_Lab_lightness;
      break;
    case DEVELOP_BLEND_LAB_A:
      blend = _blend_Lab_a;
      break;
    case DEVELOP_BLEND_LAB_B:
      blend = _blend_Lab_b;
      break;
    case DEVELOP_BLEND_LAB_COLOR:
      blend = _blend_Lab_color;
      break;
    case DEVELOP_BLEND_RGB_R:
      blend = _blend_RGB_R;
      break;
    case DEVELOP_BLEND_RGB_G:
      blend = _blend_RGB_G;
      break;
    case DEVELOP_BLEND_RGB_B:
      blend = _blend_RGB_B;
      break;
    /* fallback to normal blend */
    case DEVELOP_BLEND_NORMAL2:
    default:
      blend = _blend_normal_unbounded;
      break;
  }

  return blend;
}

void dt_develop_blend_process(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                              const void *const ivoid, void *const ovoid, const struct dt_iop_roi_t *const roi_in,
                              const struct dt_iop_roi_t *const roi_out)
{
  if(piece->pipe->bypass_blendif && self->dev->gui_attached && (self == self->dev->gui_module))
    return;

  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *const)piece->blendop_data;

  if(!d) return;

  const unsigned int mask_mode = d->mask_mode;
  // check if blend is disabled
  if(!(mask_mode & DEVELOP_MASK_ENABLED))
    return;

  const int ch = piece->colors;           // the number of channels in the buffer
  const int bch = (ch < 4) ? ch : ch - 1; // the number of channels to blend (all but alpha)
  const int xoffs = roi_out->x - roi_in->x;
  const int yoffs = roi_out->y - roi_in->y;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const size_t buffsize = (size_t)owidth * oheight;
  const float iscale = roi_in->scale;
  const float oscale = roi_out->scale;
  const _Bool rois_equal = (iwidth == owidth || iheight == oheight || xoffs == 0 || yoffs == 0);
  // In most cases of blending-enabled modules input and output of the module have
  // the exact same dimensions. Only in very special cases we allow a module's input
  // to exceed its output. This is namely the case for the spot removal module where
  // the source of a patch might lie outside the roi of the output image. Therefore:
  // We can only handle blending if roi_out and roi_in have the same scale and
  // if roi_out fits into the area given by roi_in. xoffs and yoffs describe the relative
  // offset of the input image to the output image.
  if(oscale != iscale || xoffs < 0 || yoffs < 0
     || ((xoffs > 0 || yoffs > 0) && (owidth + xoffs > iwidth || oheight + yoffs > iheight)))
  {
    dt_control_log(_("skipped blending in module '%s': roi's do not match"), self->op);
    return;
  }
  // only non-zero if mask_display was set by an _earlier_ module
  const dt_dev_pixelpipe_display_mask_t mask_display = piece->pipe->mask_display;
  // does user want us to display a specific channel?
  const dt_dev_pixelpipe_display_mask_t request_mask_display =
    (self->dev->gui_attached && (self == self->dev->gui_module) && (piece->pipe == self->dev->pipe)
     && (mask_mode & DEVELOP_MASK_MASK_CONDITIONAL))
        ? self->request_mask_display : DT_DEV_PIXELPIPE_DISPLAY_NONE;
  // get channel max values depending on colorspace
  const dt_iop_colorspace_type_t cst = self->blend_colorspace(self, piece->pipe, piece);
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  // check if mask should be suppressed temporarily (i.e. just set to global opacity value)
  const _Bool suppress_mask = self->suppress_mask && self->dev->gui_attached && (self == self->dev->gui_module)
                              && (piece->pipe == self->dev->pipe) && (mask_mode & DEVELOP_MASK_MASK_CONDITIONAL);
  const _Bool mask_feather = d->feathering_radius > 0.1f;
  const _Bool mask_blur = d->blur_radius > 0.1f;
  const _Bool mask_tone_curve = fabsf(d->contrast) >= 0.01f || fabsf(d->brightness) >= 0.01f;
  // get the clipped opacity value  0 - 1
  const float opacity = fminf(fmaxf(0.0f, (d->opacity / 100.0f)), 1.0f);
  // allocate space for blend mask
  float *_mask = dt_alloc_align(64, buffsize * sizeof(float));

  if(!_mask)
  {
    dt_control_log(_("could not allocate buffer for blending"));
    return;
  }

  float *const mask = _mask;

  if(mask_mode == DEVELOP_MASK_ENABLED || suppress_mask)
  {
    // blend uniformly (no drawn or parametric mask)

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(buffsize, mask, opacity)
#endif
    for(size_t i = 0; i < buffsize; i++)
      mask[i] = opacity;
  }
  else if(mask_mode & DEVELOP_MASK_RASTER)
  {
    /* use a raster mask from another module earlier in the pipe */
    gboolean free_mask = FALSE; // if no transformations were applied we get the cached original back
    float *raster_mask = dt_dev_get_raster_mask(piece->pipe, self->raster_mask.sink.source, self->raster_mask.sink.id,
                                                self, &free_mask);

    if(raster_mask)
    {
      // invert if required
      if(d->raster_mask_invert)
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
        dt_omp_firstprivate(buffsize, mask, opacity) \
        shared(raster_mask)
#endif
        for(size_t i = 0; i < buffsize; i++)
          mask[i] = (1.0 - raster_mask[i]) * opacity;
      else
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
        dt_omp_firstprivate(buffsize, mask, opacity) \
        shared(raster_mask)
#endif
        for(size_t i = 0; i < buffsize; i++)
          mask[i] = raster_mask[i] * opacity;
        
      if(free_mask)
        dt_free_align(raster_mask);
    }
    else
    {
      // fallback for when the raster mask couldn't be applied
      const float value = d->raster_mask_invert ? 0.0 : 1.0;
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
      dt_omp_firstprivate(buffsize, mask, value)
#endif
      for(size_t i = 0; i < buffsize; i++)
        mask[i] = value;
    }
  }
  else
  {
    // we blend with a drawn and/or parametric mask
    // get the drawn mask if there is one
    dt_masks_form_t *form = dt_masks_get_from_id_ext(piece->pipe->forms, d->mask_id);

    if(form && (!(self->flags() & IOP_FLAGS_NO_MASKS)) && (d->mask_mode & DEVELOP_MASK_MASK))
    {
      dt_masks_group_render_roi(self, piece, form, roi_out, mask);

      if(d->mask_combine & DEVELOP_COMBINE_MASKS_POS)
      {
        // if we have a mask and this flag is set -> invert the mask
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(buffsize, mask)
#endif
        for(size_t i = 0; i < buffsize; i++)
          mask[i] = 1.0f - mask[i];
      }
    }
    else if((!(self->flags() & IOP_FLAGS_NO_MASKS)) && (d->mask_mode & DEVELOP_MASK_MASK))
    {
      // no form defined but drawn mask active
      // we fill the buffer with 1.0f or 0.0f depending on mask_combine
      const float fill = (d->mask_combine & DEVELOP_COMBINE_MASKS_POS) ? 0.0f : 1.0f;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(buffsize, mask, fill)
#endif
      for(size_t i = 0; i < buffsize; i++)
        mask[i] = fill;
    }
    else
    {
      // we fill the buffer with 1.0f or 0.0f depending on mask_combine
      const float fill = (d->mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(buffsize, mask, fill)
#endif
      for(size_t i = 0; i < buffsize; i++)
        mask[i] = fill;
    }
    // get parametric mask (if any) and apply global opacity
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(bch, ch, cst, d, oheight, opacity, ivoid, iwidth, \
                        mask, owidth, ovoid, work_profile, xoffs, yoffs)
#endif
    for(size_t y = 0; y < oheight; y++)
    {
      size_t iindex = ((y + yoffs) * iwidth + xoffs) * ch;
      size_t oindex = y * owidth * ch;
      _blend_buffer_desc_t bd = { .cst = cst, .stride = (size_t)owidth * ch, .ch = ch, .bch = bch };
      float *in = (float *)ivoid + iindex;
      float *out = (float *)ovoid + oindex;
      float *m = mask + y * owidth;
      _blend_make_mask(&bd, d->blendif, d->blendif_parameters, d->mask_mode, d->mask_combine, opacity, in, out, m,
                       work_profile);
    }

    if(mask_feather)
    {
      int w = (int)(2 * d->feathering_radius * roi_out->scale / piece->iscale + 0.5f);

      if(w < 1)
        w = 1;

      float sqrt_eps = 1.f;
      float guide_weight = 1.f;

      switch(cst)
      {
        case iop_cs_rgb:
          guide_weight = 100.f;
          break;
        case iop_cs_Lab:
          guide_weight = 1.f;
          break;
        case iop_cs_RAW:
        default:
          assert(0);
      }

      float *mask_bak = dt_alloc_align(64, sizeof(*mask_bak) * buffsize);
      memcpy(mask_bak, mask, sizeof(*mask_bak) * buffsize);
      float *guide = d->feathering_guide == DEVELOP_MASK_GUIDE_IN ? (float *)ivoid : (float *)ovoid;

      if(!rois_equal && d->feathering_guide == DEVELOP_MASK_GUIDE_IN)
      {
        float *const guide_tmp = dt_alloc_align(64, sizeof(*guide_tmp) * buffsize * ch);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(ch, guide_tmp, ivoid, iwidth, oheight, owidth, xoffs, yoffs)
#endif
        for(size_t y = 0; y < oheight; y++)
        {
          size_t iindex = ((size_t)(y + yoffs) * iwidth + xoffs) * ch;
          size_t oindex = (size_t)y * owidth * ch;
          memcpy(guide_tmp + oindex, (float *)ivoid + iindex, sizeof(*guide_tmp) * owidth * ch);
        }

        guide = guide_tmp;
      }
///////////////////////
      guided_filter(guide, mask_bak, mask, owidth, oheight, ch, w, sqrt_eps, guide_weight, 0.f, 1.f);

      if(!rois_equal && d->feathering_guide == DEVELOP_MASK_GUIDE_IN)
        dt_free_align(guide);

      dt_free_align(mask_bak);
    }

    if(mask_blur)
    {
      const float sigma = d->blur_radius * roi_out->scale / piece->iscale;
      const float mmax[] = { 1.0f };
      const float mmin[] = { 0.0f };
      dt_gaussian_t *g = dt_gaussian_init(owidth, oheight, 1, mmax, mmin, sigma, 0);

      if(g)
      {
        dt_gaussian_blur(g, mask, mask);
        dt_gaussian_free(g);
      }
    }

    if(mask_tone_curve && opacity > 1e-4f)
    {
      const float mask_epsilon = 16 * FLT_EPSILON;  // empirical mask threshold for fully transparent masks
      const float e = expf(3.f * d->contrast);
      const float brightness = d->brightness;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(brightness, buffsize, e, mask, mask_epsilon, opacity)
#endif
      for(size_t k = 0; k < buffsize; k++)
      {
        float x = mask[k] / opacity;
        x = 2.f * x - 1.f;
        if (1.f - brightness <= 0.f)
          x = mask[k] <= mask_epsilon ? -1.f : 1.f;
        else if (1.f + brightness <= 0.f)
          x = mask[k] >= 1.f - mask_epsilon ? 1.f : -1.f;
        else if (brightness > 0.f)
        {
          x = (x + brightness) / (1.f - brightness);
          x = fminf(x, 1.f);
        }
        else
        {
          x = (x + brightness) / (1.f + brightness);
          x = fmaxf(x, -1.f);
        }
        mask[k] = ((x * e / (1.f + (e - 1.f) * fabsf(x))) / 2.f + 0.5f) * opacity;
      }
    }
  }
  // now apply blending with per-pixel opacity value as defined in mask
  // select the blend operator
  _blend_row_func *const blend = dt_develop_choose_blend_func(d->blend_mode);
  _blend_buffer_desc_t bd = { .cst = cst, .stride = (size_t)owidth * 4, .ch = (size_t)4, .bch = bch };
#ifdef _OPENMP
#pragma omp parallel for default(none)                                                                            \
  dt_omp_firstprivate(bch, blend, cst, ivoid, iwidth, mask, \
                      mask_display, oheight, ovoid, owidth, \
                      request_mask_display, work_profile, xoffs, yoffs, bd)
#endif
  for(size_t y = 0; y < oheight; y++)
  {
    size_t iindex = ((y + yoffs) * iwidth + xoffs) * 4;
    size_t oindex = y * owidth * 4;
    float *in = (float *)ivoid + iindex;
    float *out = (float *)ovoid + oindex;
    float *m = mask + y * owidth;

    if(request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY)
      display_channel(&bd, in, out, m, request_mask_display, work_profile);
    else
      blend(&bd, in, out, m);

    if((mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) && cst != iop_cs_RAW)
      for(size_t j = 0; j < (size_t)owidth * 4; j += 4)
        out[j + 3] = in[j + 3];
  }
  // register if _this_ module should expose mask or display channel
  if(request_mask_display & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL))
    piece->pipe->mask_display = request_mask_display;
  // check if we should store the mask for export or use in subsequent modules
  // TODO: should we skip raster masks?
  if(piece->pipe->store_all_raster_masks || dt_iop_is_raster_mask_used(self, 0))
    g_hash_table_replace(piece->raster_masks, GINT_TO_POINTER(0), _mask);
  else
  {
    g_hash_table_remove(piece->raster_masks, GINT_TO_POINTER(0));
    dt_free_align(_mask);
  }
}

int dt_develop_blend_version(void)
{
  return DEVELOP_BLEND_VERSION;
}

gboolean _develop_blend_params_is_all_zero(const void *params, size_t length)
{
  const char *data = (const char *)params;

  for(size_t k = 0; k < length; k++)
    if(data[k])
      return FALSE;

  return TRUE;
}

int dt_develop_blend_legacy_params(dt_iop_module_t *module, const void *const old_params,
                                   const int old_version, void *new_params, const int new_version,
                                   const int length)
{
  if(_develop_blend_params_is_all_zero(old_params, length))
  {
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;
    dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)module->default_blendop_params;
    *n = *d;
    return 0;
  }

  return 1;
}

int dt_develop_blend_legacy_params_from_so(dt_iop_module_so_t *module_so, const void *const old_params,
                                           const int old_version, void *new_params, const int new_version,
                                           const int length)
{
  // we need a dt_iop_module_t for dt_develop_blend_legacy_params()
  dt_iop_module_t *module;
  module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));

  if(dt_iop_load_module_by_so(module, module_so, NULL))
  {
    free(module);
    return 1;
  }

  if(module->params_size == 0)
  {
    dt_iop_cleanup_module(module);
    free(module);
    return 1;
  }
  // convert the old blend params to new
  int res = dt_develop_blend_legacy_params(module, old_params, old_version,
                                           new_params, dt_develop_blend_version(), length);
  dt_iop_cleanup_module(module);
  free(module);
  return res;
}

// tools/update_modelines.sh
// remove-trailing-space on;
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
