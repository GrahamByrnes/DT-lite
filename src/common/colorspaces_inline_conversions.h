/*
 *    This file is part of darktable,
 *    Copyright (C) 2017-2020 darktable developers.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/math.h"
#include "common/darktable.h"

static inline float cbrt_5f(float f)
{
  uint32_t *p = (uint32_t *)&f;
  *p = *p / 3 + 709921077;
  return f;
}

static inline float cbrta_halleyf(const float a, const float R)
{
  const float a3 = a * a * a;
  const float b = a * (a3 + R + R) / (a3 + a3 + R);
  return b;
}

static inline float lab_f(const float x)
{
  const float epsilon = 216.0f / 24389.0f;
  const float kappa = 24389.0f / 27.0f;
  return (x > epsilon) ? cbrta_halleyf(cbrt_5f(x), x) : (kappa * x + 16.0f) / 116.0f;
}

/** uses D50 white point. */
static inline void dt_XYZ_to_Lab(const float XYZ[3], float Lab[3])
{
  const float d50[3] = { 0.9642f, 1.0f, 0.8249f };
  float f[3] = { 0.0f };

  for(int i = 0; i < 3; i++)
    f[i] = lab_f(XYZ[i] / d50[i]);

  Lab[0] = 116.0f * f[1] - 16.0f;
  Lab[1] = 500.0f * (f[0] - f[1]);
  Lab[2] = 200.0f * (f[1] - f[2]);
}

static inline void dt_XYZ_to_Lab_mono(const float Y, float *Lab)
{
  *Lab = 116.0f * lab_f(Y) - 16.0f;
}

static inline float lab_f_inv(const float x)
{
  const float epsilon = 0.20689655172413796f; // cbrtf(216.0f/24389.0f);
  const float kappa = 24389.0f / 27.0f;
  return (x > epsilon) ? x * x * x : (116.0f * x - 16.0f) / kappa;
}

/** uses D50 white point. */
static inline void dt_Lab_to_XYZ(const float Lab[3], float XYZ[3])
{
  const float d50[3] = { 0.9642f, 1.0f, 0.8249f };
  const float fy = (Lab[0] + 16.0f) / 116.0f;
  const float fx = Lab[1] / 500.0f + fy;
  const float fz = fy - Lab[2] / 200.0f;
  const float f[3] = { fx, fy, fz };
  for(int i = 0; i < 3; i++)
    XYZ[i] = d50[i] * lab_f_inv(f[i]);
}

static inline void dt_Lab_to_XYZ_mono(const float Lab, float *Y)
{
  *Y = lab_f_inv((Lab + 16.0f) / 116.0f);
}

/** uses D50 white point. */
static inline void dt_XYZ_to_sRGB(const float *const XYZ, float *const sRGB)
{
  const float xyz_to_srgb_matrix[3][3] = { { 3.1338561, -1.6168667, -0.4906146 },
                                           { -0.9787684, 1.9161415, 0.0334540 },
                                           { 0.0719453, -0.2289914, 1.4052427 } };

  // XYZ -> sRGB
  float rgb[3] = { 0, 0, 0 };
  for(int r = 0; r < 3; r++)
    for(int c = 0; c < 3; c++) rgb[r] += xyz_to_srgb_matrix[r][c] * XYZ[c];
  // linear sRGB -> gamma corrected sRGB
  for(int c = 0; c < 3; c++)
    sRGB[c] = rgb[c] <= 0.0031308 ? 12.92 * rgb[c] : (1.0 + 0.055) * powf(rgb[c], 1.0 / 2.4) - 0.055;
}

/** uses D50 white point and clips the output to [0..1]. */
static inline void dt_XYZ_to_sRGB_clipped(const float *const XYZ, float *const sRGB)
{
  dt_XYZ_to_sRGB(XYZ, sRGB);
#define CLIP(a) ((a) < 0 ? 0 : (a) > 1 ? 1 : (a))
  for(int i = 0; i < 3; i++)
    sRGB[i] = CLIP(sRGB[i]);
#undef CLIP
}

static inline void dt_sRGB_to_XYZ(const float *const sRGB, float *const XYZ)
{
  const float srgb_to_xyz[3][3] = { { 0.4360747, 0.3850649, 0.1430804 },
                                    { 0.2225045, 0.7168786, 0.0606169 },
                                    { 0.0139322, 0.0971045, 0.7141733 } };

  // sRGB -> XYZ
  XYZ[0] = XYZ[1] = XYZ[2] = 0.0;
  float rgb[3] = { 0 };
  // gamma corrected sRGB -> linear sRGB
  for(int c = 0; c < 3; c++)
    rgb[c] = sRGB[c] <= 0.04045 ? sRGB[c] / 12.92 : powf((sRGB[c] + 0.055) / (1 + 0.055), 2.4);
  for(int r = 0; r < 3; r++)
    for(int c = 0; c < 3; c++) XYZ[r] += srgb_to_xyz[r][c] * rgb[c];
}

static inline void dt_XYZ_to_prophotorgb(const float *const XYZ, float *const rgb)
{
  const float xyz_to_rgb[3][3] = {
    // prophoto rgb d50
    { 1.3459433f, -0.2556075f, -0.0511118f},
    {-0.5445989f,  1.5081673f,  0.0205351f},
    { 0.0000000f,  0.0000000f,  1.2118128f},
  };
  rgb[0] = rgb[1] = rgb[2] = 0.0f;
  for(int r = 0; r < 3; r++)
    for(int c = 0; c < 3; c++) rgb[r] += xyz_to_rgb[r][c] * XYZ[c];
}

static inline void dt_prophotorgb_to_XYZ(const float *const rgb, float *const XYZ)
{
  const float rgb_to_xyz[3][3] = {
    // prophoto rgb
    {0.7976749f, 0.1351917f, 0.0313534f},
    {0.2880402f, 0.7118741f, 0.0000857f},
    {0.0000000f, 0.0000000f, 0.8252100f},
  };
  XYZ[0] = XYZ[1] = XYZ[2] = 0.0f;
  for(int r = 0; r < 3; r++)
    for(int c = 0; c < 3; c++) XYZ[r] += rgb_to_xyz[r][c] * rgb[c];
}

static inline void dt_Lab_to_prophotorgb(const float *const Lab, float *const rgb)
{
  float XYZ[3] = { 0.0f };
  dt_Lab_to_XYZ(Lab, XYZ);
  dt_XYZ_to_prophotorgb(XYZ, rgb);
}

static inline void dt_prophotorgb_to_Lab(const float *const rgb, float *const Lab)
{
  float XYZ[3] = { 0.0f };
  dt_prophotorgb_to_XYZ(rgb, XYZ);
  dt_XYZ_to_Lab(XYZ, Lab);
}

static inline void dt_Lab_2_LCH(const float *const Lab, float *const LCH)
{
  float var_H = atan2f(Lab[2], Lab[1]);

  if(var_H > 0.0f)
    var_H = var_H / (2.0f * DT_M_PI_F);
  else
    var_H = 1.0f - fabs(var_H) / (2.0f * DT_M_PI_F);

  LCH[0] = Lab[0];
  LCH[1] = sqrtf(Lab[1] * Lab[1] + Lab[2] * Lab[2]);
  LCH[2] = var_H;
}

static inline void dt_LCH_2_Lab(const float *const LCH, float *const Lab)
{
  Lab[0] = LCH[0];
  Lab[1] = cosf(2.0f * DT_M_PI_F * LCH[2]) * LCH[1];
  Lab[2] = sinf(2.0f * DT_M_PI_F * LCH[2]) * LCH[1];
}

static inline void LCh2rgb(const float *lum, const float *chr, const float *h, float *const rgb)
{
  float XYZ[3] = { 0.0f }, Lab[3] = { 0.0f };
  Lab[0] = *lum;
  Lab[1] = cosf(2.0f * DT_M_PI_F * *h) * *chr;
  Lab[2] = sinf(2.0f * DT_M_PI_F * *h) * *chr;
  dt_Lab_to_XYZ(Lab, XYZ);
  dt_XYZ_to_sRGB(XYZ, rgb);
}

static inline void rgb2LCh(const float *const rgb, float *lum, float *chr, float *h)
{
  float XYZ[3] = { 0.0f }, Lab[3] = { 0.0f }, LCH[3] = { 0.0f };
  dt_sRGB_to_XYZ(rgb, XYZ);
  dt_XYZ_to_Lab(XYZ, Lab);
  dt_Lab_2_LCH(Lab, LCH);
  *lum = LCH[0];
  *chr = LCH[1];
  *h = LCH[2];
}

static inline float dt_camera_rgb_luminance(const float *const rgb)
{
  return (rgb[0] * 0.2225045f + rgb[1] * 0.7168786f + rgb[2] * 0.0606169f);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
