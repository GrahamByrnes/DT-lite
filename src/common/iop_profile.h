/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef DT_IOP_PROFILE_H
#define DT_IOP_PROFILE_H

#include "common/colorspaces_inline_conversions.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

struct dt_iop_module_t;
struct dt_develop_t;
struct dt_dev_pixelpipe_t;

// must be in synch with filename in dt_colorspaces_color_profile_t in colorspaces.h
#define DT_IOPPR_COLOR_ICC_LEN 512

typedef struct dt_iop_order_iccprofile_info_t
{
  int type; // a dt_colorspaces_color_profile_type_t
  char filename[DT_IOPPR_COLOR_ICC_LEN];
  int intent; // a dt_iop_color_intent_t
  float matrix_in[9] DT_ALIGNED_PIXEL; // don't align on more than 16 bits or OpenCL will fail
  float matrix_out[9] DT_ALIGNED_PIXEL;
  int lutsize;
  float *lut_in[3];
  float *lut_out[3];
  float unbounded_coeffs_in[3][3] DT_ALIGNED_PIXEL;
  float unbounded_coeffs_out[3][3] DT_ALIGNED_PIXEL;
  int nonlinearlut;
  float grey;
} dt_iop_order_iccprofile_info_t;

#undef DT_IOPPR_COLOR_ICC_LEN

/** must be called before using profile_info, default lutsize = 0 */
void dt_ioppr_init_profile_info(dt_iop_order_iccprofile_info_t *profile_info, const int lutsize);
/** must be called when done with profile_info */
void dt_ioppr_cleanup_profile_info(dt_iop_order_iccprofile_info_t *profile_info);

/** returns the profile info from dev profiles info list that matches (profile_type, profile_filename)
 * NULL if not found
 */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_profile_info_from_list(struct dt_develop_t *dev, const int profile_type, const char *profile_filename);
/** adds the profile info from (profile_type, profile_filename) to the dev profiles info list if not already exists
 * returns the generated profile or the existing one
 */
dt_iop_order_iccprofile_info_t *dt_ioppr_add_profile_info_to_list(struct dt_develop_t *dev, const int profile_type, const char *profile_filename, const int intent);

/** returns a reference to the work profile info as set on colorin iop
 * only if module is between colorin and colorout, otherwise returns NULL
 * work profile must not be cleanup()
 */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_iop_work_profile_info(struct dt_iop_module_t *module, GList *iop_list);

/** set the work profile (type, filename) on the pipe, should be called on process*()
 * if matrix cannot be generated it default to linear rec 2020
 * returns the actual profile that has been set
 */
dt_iop_order_iccprofile_info_t *dt_ioppr_set_pipe_work_profile_info(struct dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe,
    const int type, const char *filename, const int intent);
/** returns a reference to the histogram profile info
 * histogram profile must not be cleanup()
 */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_histogram_profile_info(struct dt_develop_t *dev);

/** returns the active work profile on the pipe */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_work_profile_info(struct dt_dev_pixelpipe_t *pipe);

/** returns the current setting of the work profile on colorin iop */
void dt_ioppr_get_work_profile_type(struct dt_develop_t *dev, int *profile_type, const char **profile_filename);
/** returns the current setting of the export profile on colorout iop */
void dt_ioppr_get_export_profile_type(struct dt_develop_t *dev, int *profile_type, const char **profile_filename);
/** returns the current setting of the histogram profile */
void dt_ioppr_get_histogram_profile_type(int *profile_type, const char **profile_filename);

/** transforms image from cst_from to cst_to colorspace using profile_info */
void dt_ioppr_transform_image_colorspace(struct dt_iop_module_t *self, const float *const image_in,
                                         float *const image_out, const int width, const int height,
                                         const int cst_from, const int cst_to, int *converted_cst,
                                         const int chan_in, const dt_iop_order_iccprofile_info_t *const profile_info);

void dt_ioppr_transform_image_colorspace_rgb(const float *const image_in, float *const image_out,
                                             const int width, const int height,
                                             const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                             const dt_iop_order_iccprofile_info_t *const profile_info_to,
                                             const char *message);

#ifdef _OPENMP
#pragma omp declare simd \
  aligned(xyz, rgb, matrix:16) \
  uniform(xyz, rgb, matrix)
#endif
static inline void _ioppr_linear_rgb_matrix_to_xyz(const float rgb[3], float xyz[3],
                                                   const float matrix[9])
{
  for(int c = 0; c < 3; c++) xyz[c] = 0.0f;

  for(int c = 0; c < 3; c++)
    for(int i = 0; i < 3; i++)
      xyz[c] += matrix[3 * c + i] * rgb[i];
}

#ifdef _OPENMP
#pragma omp declare simd \
  aligned(xyz, rgb, matrix:16) \
  uniform(xyz, rgb, matrix)
#endif
static inline void _ioppr_xyz_to_linear_rgb_matrix(const float xyz[3], float rgb[3],
                                                   const float matrix[9])
{
  for(int c = 0; c < 3; c++)
    rgb[c] = 0.0f;

  for(int c = 0; c < 3; c++)
    for(int i = 0; i < 3; i++)
      rgb[c] += matrix[3 * c + i] * xyz[i];
}

#ifdef _OPENMP
#pragma omp declare simd \
  aligned(rgb, matrix_in:16) \
  uniform(rgb, matrix_in)
#endif
static inline float dt_ioppr_get_rgb_matrix_luminance(const float rgb[3], const float matrix_in[9])
{
  return matrix_in[3] * rgb[0] + matrix_in[4] * rgb[1] + matrix_in[5] * rgb[2];
}


#ifdef _OPENMP
#pragma omp declare simd \
  aligned(rgb, xyz, matrix_in:16) \
  uniform(rgb, xyz, matrix_in)
#endif
static inline void dt_ioppr_rgb_matrix_to_xyz(const float rgb[3], float xyz[3],
                                              const float matrix_in[9])
{
  _ioppr_linear_rgb_matrix_to_xyz(rgb, xyz, matrix_in);
}


#ifdef _OPENMP
#pragma omp declare simd \
  aligned(lab, rgb, matrix_out:16) \
  uniform(lab, rgb, matrix_out)
#endif
static inline void dt_ioppr_lab_to_rgb_matrix(const float lab[3], float rgb[3],
                                              const float matrix_out[9])
{
  float xyz[3] DT_ALIGNED_PIXEL;
  dt_Lab_to_XYZ(lab, xyz);
  _ioppr_xyz_to_linear_rgb_matrix(xyz, rgb, matrix_out);
}

#ifdef _OPENMP
#pragma omp declare simd \
  aligned(lab, rgb, matrix_in:16) \
  uniform(rgb, lab, matrix_in)
#endif
static inline void dt_ioppr_rgb_matrix_to_lab(const float rgb[3], float lab[3],
                                              const float matrix_in[9])
{
  float xyz[3] DT_ALIGNED_PIXEL;
  dt_ioppr_rgb_matrix_to_xyz(rgb, xyz, matrix_in);
  dt_XYZ_to_Lab(xyz, lab);
}

static inline float dt_ioppr_get_profile_info_middle_grey(const dt_iop_order_iccprofile_info_t *const profile_info)
{
  return profile_info->grey;
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float dt_ioppr_compensate_middle_grey(const float x, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  // we transform the curve nodes from the image colorspace to lab
  float lab[3] DT_ALIGNED_PIXEL = { 0.0f };
  const float rgb[3] DT_ALIGNED_PIXEL = { x, x, x };
  dt_ioppr_rgb_matrix_to_lab(rgb, lab, profile_info->matrix_in);
  return lab[0] * .01f;
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float dt_ioppr_uncompensate_middle_grey(const float x, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  // we transform the curve nodes from lab to the image colorspace
  const float lab[3] DT_ALIGNED_PIXEL = { x * 100.f, 0.0f, 0.0f };
  float rgb[3] DT_ALIGNED_PIXEL = { 0.0f };

  dt_ioppr_lab_to_rgb_matrix(lab, rgb, profile_info->matrix_out);
  return rgb[0];
}

#endif
