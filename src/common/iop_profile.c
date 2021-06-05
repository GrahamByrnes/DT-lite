/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/darktable.h"
#include "common/iop_profile.h"
#include "common/debug.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Note :
 * we do not use finite-math-only and fast-math because divisions by zero are not manually avoided in the code
 * fp-contract=fast enables hardware-accelerated Fused Multiply-Add
 * the rest is loop reorganization and vectorization optimization
 **/
#if defined(__GNUC__)
#pragma GCC optimize ("unroll-loops", "tree-loop-if-convert", \
                      "tree-loop-distribution", "no-strict-aliasing", \
                      "loop-interchange", "loop-nest-optimize", "tree-loop-im", \
                      "unswitch-loops", "tree-loop-ivcanon", "ira-loop-pressure", \
                      "split-ivs-in-unroller", "variable-expansion-in-unroller", \
                      "split-loops", "ivopts", "predictive-commoning",\
                      "tree-loop-linear", "loop-block", "loop-strip-mine", \
                      "fp-contract=fast", \
                      "tree-vectorize")
#endif

static void _transform_from_to_rgb_lab_lcms2(const float *const image_in, float *const image_out, const int width,
                                             const int height, const dt_colorspaces_color_profile_type_t type,
                                             const char *filename, const int intent, const int direction)
{
  cmsHTRANSFORM *xform = NULL;
  cmsHPROFILE *rgb_profile = NULL;
  cmsHPROFILE *lab_profile = NULL;

  if(type != DT_COLORSPACE_NONE)
  {
    const dt_colorspaces_color_profile_t *profile = dt_colorspaces_get_profile(type, filename, DT_PROFILE_DIRECTION_WORK);
    if(profile) rgb_profile = profile->profile;
  }
  else
    rgb_profile = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC2020, "", DT_PROFILE_DIRECTION_WORK)->profile;

  if(rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(rgb_profile);

    if(rgb_color_space != cmsSigRgbData)
    {
      fprintf(stderr, "working profile color space `%c%c%c%c' not supported\n",
                (char)(rgb_color_space>>24),
                (char)(rgb_color_space>>16),
                (char)(rgb_color_space>>8),
                (char)(rgb_color_space));
      rgb_profile = NULL;
    }
  }

  if(rgb_profile == NULL)
  {
    rgb_profile = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC2020, "", DT_PROFILE_DIRECTION_WORK)->profile;
    fprintf(stderr, _("unsupported working profile %s has been replaced by Rec2020 RGB!\n"), filename);
  }

  lab_profile = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;
  cmsHPROFILE *input_profile = NULL;
  cmsHPROFILE *output_profile = NULL;
  cmsUInt32Number input_format = TYPE_RGBA_FLT;
  cmsUInt32Number output_format = TYPE_LabA_FLT;

  if(direction == 1) // rgb --> lab
  {
    input_profile = rgb_profile;
    input_format = TYPE_RGBA_FLT;
    output_profile = lab_profile;
    output_format = TYPE_LabA_FLT;
  }
  else // lab -->rgb
  {
    input_profile = lab_profile;
    input_format = TYPE_LabA_FLT;
    output_profile = rgb_profile;
    output_format = TYPE_RGBA_FLT;
  }

  xform = cmsCreateTransform(input_profile, input_format, output_profile, output_format, intent, 0);

  if(xform)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(image_in, image_out, width, height) \
    shared(xform) \
    schedule(static)
#endif
    for(int y = 0; y < height; y++)
    {
      const float *const in = image_in + y * width * 4;
      float *const out = image_out + y * width * 4;
      cmsDoTransform(xform, in, out, width);
    }
  }
  else
    fprintf(stderr, "[_transform_from_to_rgb_lab_lcms2] cannot create transform\n");

  if(xform)
    cmsDeleteTransform(xform);
}

static void _transform_rgb_to_rgb_lcms2(const float *const image_in, float *const image_out, const int width,
                                        const int height, const dt_colorspaces_color_profile_type_t type_from,
                                        const char *filename_from,
                                        const dt_colorspaces_color_profile_type_t type_to, const char *filename_to,
                                        const int intent)
{
  cmsHTRANSFORM *xform = NULL;
  cmsHPROFILE *from_rgb_profile = NULL;
  cmsHPROFILE *to_rgb_profile = NULL;

  if(type_from == DT_COLORSPACE_DISPLAY || type_to == DT_COLORSPACE_DISPLAY
     || type_from == DT_COLORSPACE_DISPLAY2 || type_to == DT_COLORSPACE_DISPLAY2)
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

  if(type_from != DT_COLORSPACE_NONE)
  {
    const dt_colorspaces_color_profile_t *profile_from
        = dt_colorspaces_get_profile(type_from, filename_from, DT_PROFILE_DIRECTION_ANY);
        
    if(profile_from)
      from_rgb_profile = profile_from->profile;
  }
  else
    fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] invalid from profile\n");

  if(type_to != DT_COLORSPACE_NONE)
  {
    const dt_colorspaces_color_profile_t *profile_to
        = dt_colorspaces_get_profile(type_to, filename_to, DT_PROFILE_DIRECTION_ANY);
    if(profile_to)
      to_rgb_profile = profile_to->profile;
  }
  else
    fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] invalid to profile\n");

  if(from_rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(from_rgb_profile);

    if(rgb_color_space != cmsSigRgbData)
    {
      fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] profile color space `%c%c%c%c' not supported\n",
              (char)(rgb_color_space >> 24), (char)(rgb_color_space >> 16), (char)(rgb_color_space >> 8),
              (char)(rgb_color_space));
      from_rgb_profile = NULL;
    }
  }

  if(to_rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(to_rgb_profile);

    if(rgb_color_space != cmsSigRgbData)
    {
      fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] profile color space `%c%c%c%c' not supported\n",
              (char)(rgb_color_space >> 24), (char)(rgb_color_space >> 16), (char)(rgb_color_space >> 8),
              (char)(rgb_color_space));
      to_rgb_profile = NULL;
    }
  }

  cmsHPROFILE *input_profile = NULL;
  cmsHPROFILE *output_profile = NULL;
  cmsUInt32Number input_format = TYPE_RGBA_FLT;
  cmsUInt32Number output_format = TYPE_RGBA_FLT;

  input_profile = from_rgb_profile;
  input_format = TYPE_RGBA_FLT;
  output_profile = to_rgb_profile;
  output_format = TYPE_RGBA_FLT;

  if(input_profile && output_profile)
    xform = cmsCreateTransform(input_profile, input_format, output_profile, output_format, intent, 0);

  if(type_from == DT_COLORSPACE_DISPLAY || type_to == DT_COLORSPACE_DISPLAY || type_from == DT_COLORSPACE_DISPLAY2
     || type_to == DT_COLORSPACE_DISPLAY2)
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

  if(xform)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(image_in, image_out, width, height) \
    shared(xform) \
    schedule(static)
#endif
    for(int y = 0; y < height; y++)
    {
      const float *const in = image_in + y * width * 4;
      float *const out = image_out + y * width * 4;
      cmsDoTransform(xform, in, out, width);
    }
  }
  else
    fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] cannot create transform\n");

  if(xform) cmsDeleteTransform(xform);
}

static void _transform_lcms2(struct dt_iop_module_t *self, const float *const image_in, float *const image_out,
                             const int width, const int height, const int cst_from, const int cst_to,
                             int *converted_cst, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return;
  }

  *converted_cst = cst_to;

  if(cst_from == iop_cs_rgb && cst_to == iop_cs_Lab)
  {
    printf("[_transform_lcms2] transfoming from RGB to Lab (%s %s)\n", self->op, self->multi_name);
    _transform_from_to_rgb_lab_lcms2(image_in, image_out, width, height, profile_info->type,
                                     profile_info->filename, profile_info->intent, 1);
  }
  else if(cst_from == iop_cs_Lab && cst_to == iop_cs_rgb)
  {
    printf("[_transform_lcms2] transfoming from Lab to RGB (%s %s)\n", self->op, self->multi_name);
    _transform_from_to_rgb_lab_lcms2(image_in, image_out, width, height, profile_info->type,
                                     profile_info->filename, profile_info->intent, -1);
  }
  else
  {
    *converted_cst = cst_from;
    fprintf(stderr, "[_transform_lcms2] invalid conversion from %i to %i\n", cst_from, cst_to);
  }
}

static inline void _transform_lcms2_rgb(const float *const image_in, float *const image_out,
                                        const int width, const int height,
                                        const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                        const dt_iop_order_iccprofile_info_t *const profile_info_to)
{
  _transform_rgb_to_rgb_lcms2(image_in, image_out, width, height, profile_info_from->type,
                              profile_info_from->filename, profile_info_to->type, profile_info_to->filename,
                              profile_info_to->intent);
}

static inline void _transform_rgb_to_lab_matrix(const float *const restrict image_in, float *const restrict image_out,
                                                const int width, const int height,
                                                const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const size_t npixels = (size_t)width * height * 4;
  const float *const restrict matrix = profile_info->matrix_in;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(image_in, image_out, profile_info, npixels, matrix) \
    schedule(static) aligned(image_in, image_out:64) aligned(matrix:16)
#endif
  for(size_t y = 0; y < npixels; y += 4)
  {
    const float *const in = image_in + y ;
    float *const out = image_out + y;
    float xyz[3] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f };

    _ioppr_linear_rgb_matrix_to_xyz(in, xyz, matrix);
    dt_XYZ_to_Lab(xyz, out);
  }
}

static inline void _transform_lab_to_rgb_matrix(const float *const restrict image_in, float *const restrict image_out, const int width,
                                         const int height,
                                         const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const size_t stride = (size_t)width * height * 4;
  const float *const restrict matrix = profile_info->matrix_out;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(image_in, image_out, stride, profile_info, matrix) \
  schedule(static) aligned(image_in, image_out:64) aligned(matrix:16)
#endif
  for(size_t y = 0; y < stride; y += 4)
  {
    const float *const in = image_in + y;
    float *const out = image_out + y;
    float xyz[3] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f };
    dt_Lab_to_XYZ(in, xyz);
    _ioppr_xyz_to_linear_rgb_matrix(xyz, out, matrix);
  }
}

static inline void _transform_matrix_rgb(const float *const restrict image_in, float *const restrict image_out, const int width,
                                  const int height, const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                  const dt_iop_order_iccprofile_info_t *const profile_info_to)
{
  const size_t stride = (size_t)width * height * 4;
  const float *const restrict matrix_in = profile_info_from->matrix_in;
  const float *const restrict matrix_out = profile_info_to->matrix_out;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(stride, image_in, image_out, profile_info_from, profile_info_to, matrix_in, matrix_out) \
    schedule(static) aligned(image_in, image_out:64) aligned(matrix_in, matrix_out:16)
#endif
  for(size_t y = 0; y < stride; y += 4)
  {
    const float *const in = image_in + y;
    float *const out = image_out + y;
    float xyz[3] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f };

    _ioppr_linear_rgb_matrix_to_xyz(in, xyz, matrix_in);
    _ioppr_xyz_to_linear_rgb_matrix(xyz, out, matrix_out);
  }
}

static inline void _transform_matrix(struct dt_iop_module_t *self, const float *const restrict image_in, float *const restrict image_out,
                              const int width, const int height, const int cst_from, const int cst_to,
                              int *converted_cst, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  *converted_cst = cst_to;

  if(cst_from == iop_cs_rgb && cst_to == iop_cs_Lab)
    _transform_rgb_to_lab_matrix(image_in, image_out, width, height, profile_info);
  else if(cst_from == iop_cs_Lab && cst_to == iop_cs_rgb)
    _transform_lab_to_rgb_matrix(image_in, image_out, width, height, profile_info);
  else
  {
    *converted_cst = cst_from;
    fprintf(stderr, "[_transform_matrix] invalid conversion from %i to %i\n", cst_from, cst_to);
  }
}

static inline void _transform_rgb_to_lab_mono(const float *const restrict image_in, float *const restrict image_out,
                                              const int width, const int height,
                                              const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const size_t npixels = (size_t)width * height * 4;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(image_in, image_out, profile_info, npixels) \
    schedule(static)
#endif
  for(size_t y = 0; y < npixels; y += 4)
  {
    const float *const in = image_in + y ;
    float *const out = image_out + y;
    dt_XYZ_to_Lab_mono(*in, out);
  }
}

static inline void _transform_lab_to_rgb_mono(const float *const restrict image_in, float *const restrict image_out,
                                              const int width, const int height,
                                              const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const size_t npixels = (size_t)width * height * 4;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(image_in, image_out, npixels, profile_info) \
  schedule(static)
#endif
  for(size_t y = 0; y < npixels; y += 4)
  {
    const float *const in = image_in + y;
    float *const out = image_out + y;
    dt_Lab_to_XYZ_mono(*in, out);
  }
}

static inline void _transform_mono(struct dt_iop_module_t *self, const float *const restrict image_in, float *const restrict image_out,
                              const int width, const int height, const int cst_from, const int cst_to,
                              int *converted_cst, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  *converted_cst = cst_to;
  
  if(cst_from == iop_cs_rgb && cst_to == iop_cs_Lab)
    _transform_rgb_to_lab_mono(image_in, image_out, width, height, profile_info);
  else if(cst_from == iop_cs_Lab && cst_to == iop_cs_rgb)
    _transform_lab_to_rgb_mono(image_in, image_out, width, height, profile_info);
  else
  {
    *converted_cst = cst_from;
    fprintf(stderr, "[_transform_mono] invalid conversion from %i to %i\n", cst_from, cst_to);
  }
}

#define DT_IOPPR_LUT_SAMPLES 0x10000

void dt_ioppr_init_profile_info(dt_iop_order_iccprofile_info_t *profile_info, const int lutsize)
{
  profile_info->type = DT_COLORSPACE_NONE;
  profile_info->filename[0] = '\0';
  profile_info->intent = DT_INTENT_PERCEPTUAL;
  profile_info->matrix_in[0] = NAN;
  profile_info->matrix_out[0] = NAN;
  profile_info->unbounded_coeffs_in[0][0] = profile_info->unbounded_coeffs_in[1][0] = profile_info->unbounded_coeffs_in[2][0] = -1.0f;
  profile_info->unbounded_coeffs_out[0][0] = profile_info->unbounded_coeffs_out[1][0] = profile_info->unbounded_coeffs_out[2][0] = -1.0f;
  profile_info->nonlinearlut = 0;
  profile_info->grey = 0.f;
  profile_info->lutsize = (lutsize > 0) ? lutsize: DT_IOPPR_LUT_SAMPLES;
  for(int i = 0; i < 3; i++)
  {
    profile_info->lut_in[i] = dt_alloc_sse_ps(profile_info->lutsize);
    profile_info->lut_in[i][0] = -1.0f;
    profile_info->lut_out[i] = dt_alloc_sse_ps(profile_info->lutsize);
    profile_info->lut_out[i][0] = -1.0f;
  }
}

#undef DT_IOPPR_LUT_SAMPLES

void dt_ioppr_cleanup_profile_info(dt_iop_order_iccprofile_info_t *profile_info)
{
  for(int i = 0; i < 3; i++)
  {
    if(profile_info->lut_in[i]) dt_free_align(profile_info->lut_in[i]);
    if(profile_info->lut_out[i]) dt_free_align(profile_info->lut_out[i]);
  }
}

/** generate the info for the profile (type, filename) if matrix can be retrieved from lcms2
 * it can be called multiple time between init and cleanup
 * return 0 if OK, non zero otherwise
 */
static int dt_ioppr_generate_profile_info(dt_iop_order_iccprofile_info_t *profile_info, const int type, const char *filename, const int intent)
{
  int err_code = 0;
  cmsHPROFILE *rgb_profile = NULL;

  profile_info->matrix_in[0] = NAN;
  profile_info->matrix_out[0] = NAN;
  for(int i = 0; i < 3; i++)
  {
    profile_info->lut_in[i][0] = -1.0f;
    profile_info->lut_out[i][0] = -1.0f;
  }

  profile_info->nonlinearlut = 0;
  profile_info->grey = 0.1842f;
  profile_info->type = type;
  g_strlcpy(profile_info->filename, filename, sizeof(profile_info->filename));
  profile_info->intent = intent;

  if(type == DT_COLORSPACE_DISPLAY || type == DT_COLORSPACE_DISPLAY2)
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

  const dt_colorspaces_color_profile_t *profile
      = dt_colorspaces_get_profile(type, filename, DT_PROFILE_DIRECTION_ANY);

  if(profile)
    rgb_profile = profile->profile;

  if(type == DT_COLORSPACE_DISPLAY || type == DT_COLORSPACE_DISPLAY2)
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

  // we only allow rgb profiles
  if(rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(rgb_profile);

    if(rgb_color_space != cmsSigRgbData)
    {
      fprintf(stderr, "working profile color space `%c%c%c%c' not supported\n",
              (char)(rgb_color_space>>24),
              (char)(rgb_color_space>>16),
              (char)(rgb_color_space>>8),
              (char)(rgb_color_space));
      rgb_profile = NULL;
    }
  }
  // get the matrix
  if(rgb_profile)
  {
    if(dt_colorspaces_get_matrix_from_input_profile(rgb_profile, profile_info->matrix_in,
        profile_info->lut_in[0], profile_info->lut_in[1], profile_info->lut_in[2],
        profile_info->lutsize, profile_info->intent) ||
        dt_colorspaces_get_matrix_from_output_profile(rgb_profile, profile_info->matrix_out,
            profile_info->lut_out[0], profile_info->lut_out[1], profile_info->lut_out[2],
            profile_info->lutsize, profile_info->intent))
    {
      profile_info->matrix_in[0] = NAN;
      profile_info->matrix_out[0] = NAN;
      for(int i = 0; i < 3; i++)
      {
        profile_info->lut_in[i][0] = -1.0f;
        profile_info->lut_out[i][0] = -1.0f;
      }
    }
    else if(isnan(profile_info->matrix_in[0]) || isnan(profile_info->matrix_out[0]))
    {
      profile_info->matrix_in[0] = NAN;
      profile_info->matrix_out[0] = NAN;
      for(int i = 0; i < 3; i++)
      {
        profile_info->lut_in[i][0] = -1.0f;
        profile_info->lut_out[i][0] = -1.0f;
      }
    }
  }

  return err_code;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_profile_info_from_list(struct dt_develop_t *dev,
                                                                    const int profile_type, const char *profile_filename)
{
  dt_iop_order_iccprofile_info_t *profile_info = NULL;
  GList *profiles = g_list_first(dev->allprofile_info);

  while(profiles)
  {
    dt_iop_order_iccprofile_info_t *prof = (dt_iop_order_iccprofile_info_t *)(profiles->data);
    if(prof->type == profile_type && strcmp(prof->filename, profile_filename) == 0)
    {
      profile_info = prof;
      break;
    }
    profiles = g_list_next(profiles);
  }

  return profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_add_profile_info_to_list(struct dt_develop_t *dev, const int profile_type,
                                                                  const char *profile_filename, const int intent)
{
  dt_iop_order_iccprofile_info_t *profile_info = dt_ioppr_get_profile_info_from_list(dev, profile_type, profile_filename);

  if(profile_info == NULL)
  {
    profile_info = malloc(sizeof(dt_iop_order_iccprofile_info_t));
    dt_ioppr_init_profile_info(profile_info, 0);
    const int err = dt_ioppr_generate_profile_info(profile_info, profile_type, profile_filename, intent);
    if(err == 0)
      dev->allprofile_info = g_list_append(dev->allprofile_info, profile_info);
    else
    {
      free(profile_info);
      profile_info = NULL;
    }
  }
  return profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_iop_work_profile_info(struct dt_iop_module_t *module, GList *iop_list)
{
  dt_iop_order_iccprofile_info_t *profile = NULL;
  // first check if the module is between colorin and colorout
  gboolean in_between = FALSE;
  GList *modules = g_list_first(iop_list);

  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);
    // we reach the module, that's it
    if(strcmp(mod->op, module->op) == 0)
      break;
    // if we reach colorout means that the module is after it
    if(strcmp(mod->op, "colorout") == 0)
    {
      in_between = FALSE;
      break;
    }
    // we reach colorin, so far we're good
    if(strcmp(mod->op, "colorin") == 0)
    {
      in_between = TRUE;
      break;
    }

    modules = g_list_next(modules);
  }

  if(in_between)
  {
    dt_colorspaces_color_profile_type_t type = DT_COLORSPACE_NONE;
    const char *filename = NULL;
    dt_develop_t *dev = module->dev;
    dt_ioppr_get_work_profile_type(dev, &type, &filename);

    if(filename)
      profile = dt_ioppr_add_profile_info_to_list(dev, type, filename, DT_INTENT_PERCEPTUAL);
  }

  return profile;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_set_pipe_work_profile_info(struct dt_develop_t *dev,
                      struct dt_dev_pixelpipe_t *pipe, const int type, const char *filename, const int intent)
{
  dt_iop_order_iccprofile_info_t *profile_info = dt_ioppr_add_profile_info_to_list(dev, type, filename, intent);

  if(profile_info == NULL || isnan(profile_info->matrix_in[0]) || isnan(profile_info->matrix_out[0]))
  {
    fprintf(stderr, 
      "[dt_ioppr_set_pipe_work_profile_info] unsupported working profile %i %s, it will be replaced with linear rec2020\n", 
      type, filename);
    profile_info = dt_ioppr_add_profile_info_to_list(dev, DT_COLORSPACE_LIN_REC2020, "", intent);
  }
  
  pipe->dsc.work_profile_info = profile_info;
  return profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_histogram_profile_info(struct dt_develop_t *dev)
{
  dt_colorspaces_color_profile_type_t histogram_profile_type;
  const char *histogram_profile_filename;
  dt_ioppr_get_histogram_profile_type(&histogram_profile_type, &histogram_profile_filename);

  return dt_ioppr_add_profile_info_to_list(dev, histogram_profile_type, histogram_profile_filename,
                                           DT_INTENT_PERCEPTUAL);
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_work_profile_info(struct dt_dev_pixelpipe_t *pipe)
{
  return pipe->dsc.work_profile_info;
}

// returns a pointer to the filename of the work profile instead of the actual string data
// pointer must not be stored
void dt_ioppr_get_work_profile_type(struct dt_develop_t *dev, int *profile_type, const char **profile_filename)
{
  *profile_type = DT_COLORSPACE_NONE;
  *profile_filename = NULL;
  // use introspection to get the params values
  dt_iop_module_so_t *colorin_so = NULL;
  dt_iop_module_t *colorin = NULL;
  GList *modules = g_list_first(darktable.iop);

  while(modules)
  {
    dt_iop_module_so_t *module_so = (dt_iop_module_so_t *)(modules->data);

    if(!strcmp(module_so->op, "colorin"))
    {
      colorin_so = module_so;
      break;
    }
    modules = g_list_next(modules);
  }

  if(colorin_so && colorin_so->get_p)
  {
    modules = g_list_first(dev->iop);

    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

      if(!strcmp(module->op, "colorin"))
      {
        colorin = module;
        break;
      }
      modules = g_list_next(modules);
    }
  }

  if(colorin)
  {
    dt_colorspaces_color_profile_type_t *_type = colorin_so->get_p(colorin->params, "type_work");
    char *_filename = colorin_so->get_p(colorin->params, "filename_work");
    if(_type && _filename)
    {
      *profile_type = *_type;
      *profile_filename = _filename;
    }
    else
      fprintf(stderr, "[dt_ioppr_get_work_profile_type] can't get colorin parameters\n");
  }
  else
    fprintf(stderr, "[dt_ioppr_get_work_profile_type] can't find colorin iop\n");
}

void dt_ioppr_get_export_profile_type(struct dt_develop_t *dev, int *profile_type, const char **profile_filename)
{
  *profile_type = DT_COLORSPACE_NONE;
  *profile_filename = NULL;
  // use introspection to get the params values
  dt_iop_module_so_t *colorout_so = NULL;
  dt_iop_module_t *colorout = NULL;
  GList *modules = g_list_last(darktable.iop);

  while(modules)
  {
    dt_iop_module_so_t *module_so = (dt_iop_module_so_t *)(modules->data);

    if(!strcmp(module_so->op, "colorout"))
    {
      colorout_so = module_so;
      break;
    }
    modules = g_list_previous(modules);
  }

  if(colorout_so && colorout_so->get_p)
  {
    modules = g_list_last(dev->iop);

    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

      if(!strcmp(module->op, "colorout"))
      {
        colorout = module;
        break;
      }
      modules = g_list_previous(modules);
    }
  }

  if(colorout)
  {
    dt_colorspaces_color_profile_type_t *_type = colorout_so->get_p(colorout->params, "type");
    char *_filename = colorout_so->get_p(colorout->params, "filename");

    if(_type && _filename)
    {
      *profile_type = *_type;
      *profile_filename = _filename;
    }
    else
      fprintf(stderr, "[dt_ioppr_get_export_profile_type] can't get colorout parameters\n");
  }
  else
    fprintf(stderr, "[dt_ioppr_get_export_profile_type] can't find colorout iop\n");
}

void dt_ioppr_get_histogram_profile_type(int *profile_type, const char **profile_filename)
{
  const dt_colorspaces_color_mode_t mode = darktable.color_profiles->mode;
  // if in gamut check use soft proof
  if(mode != DT_PROFILE_NORMAL || darktable.color_profiles->histogram_type == DT_COLORSPACE_SOFTPROOF)
  {
    *profile_type = darktable.color_profiles->softproof_type;
    *profile_filename = darktable.color_profiles->softproof_filename;
  }
  else if(darktable.color_profiles->histogram_type == DT_COLORSPACE_WORK)
    dt_ioppr_get_work_profile_type(darktable.develop, profile_type, profile_filename);
  else if(darktable.color_profiles->histogram_type == DT_COLORSPACE_EXPORT)
    dt_ioppr_get_export_profile_type(darktable.develop, profile_type, profile_filename);
  else
  {
    *profile_type = darktable.color_profiles->histogram_type;
    *profile_filename = darktable.color_profiles->histogram_filename;
  }
}

void dt_ioppr_transform_image_colorspace(struct dt_iop_module_t *self, const float *const image_in,
                                         float *const image_out, const int width, const int height,
                                         const int cst_from, const int cst_to, int *converted_cst,
                                         const int chan_in, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return;
  }

  if(profile_info == NULL || profile_info->type == DT_COLORSPACE_NONE)
  {
    *converted_cst = cst_from;
    return;
  }

  if(chan_in == 4)
  {
    if(!isnan(profile_info->matrix_in[0]) && !isnan(profile_info->matrix_out[0]))
      _transform_matrix(self, image_in, image_out, width, height, cst_from, cst_to, converted_cst, profile_info);
    else
      _transform_lcms2(self, image_in, image_out, width, height, cst_from, cst_to, converted_cst, profile_info);
  }
  else if(chan_in == 1)
    _transform_mono(self, image_in, image_out, width, height, cst_from, cst_to, converted_cst, profile_info);

  if(*converted_cst == cst_from)
    fprintf(stderr, "[dt_ioppr_transform_image_colorspace] invalid conversion from %i to %i\n", cst_from, cst_to);

}

void dt_ioppr_transform_image_colorspace_rgb(const float *const restrict image_in, float *const restrict image_out,
                                             const int width, const int height,
                                             const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                             const dt_iop_order_iccprofile_info_t *const profile_info_to,
                                             const char *message)
{
  if(profile_info_from->type == DT_COLORSPACE_NONE || profile_info_to->type == DT_COLORSPACE_NONE)
    return;
    
  if(profile_info_from->type == profile_info_to->type
     && strcmp(profile_info_from->filename, profile_info_to->filename) == 0)
  {
    if(image_in != image_out)
      memcpy(image_out, image_in, width * height * 4 * sizeof(float));

    return;
  }

  if(!isnan(profile_info_from->matrix_in[0]) && !isnan(profile_info_from->matrix_out[0])
     && !isnan(profile_info_to->matrix_in[0]) && !isnan(profile_info_to->matrix_out[0]))

    _transform_lcms2_rgb(image_in, image_out, width, height, profile_info_from, profile_info_to);
}

#undef DT_IOP_ORDER_PROFILE