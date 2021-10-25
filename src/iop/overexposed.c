/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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

#include <stdlib.h>
#include <cairo.h>

#include "common/iop_profile.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "iop/iop_api.h"

DT_MODULE(3)

typedef enum dt_iop_overexposed_colorscheme_t
{
  DT_IOP_OVEREXPOSED_BLACKWHITE = 0,
  DT_IOP_OVEREXPOSED_REDBLUE = 1,
  DT_IOP_OVEREXPOSED_PURPLEGREEN = 2
} dt_iop_overexposed_colorscheme_t;

static const float dt_iop_overexposed_colors[][2][4]
    = { {
          { 0.0f, 0.0f, 0.0f, 1.0f }, // black
          { 1.0f, 1.0f, 1.0f, 1.0f }  // white
        },
        {
          { 1.0f, 0.0f, 0.0f, 1.0f }, // red
          { 0.0f, 0.0f, 1.0f, 1.0f }  // blue
        },
        {
          { 0.371f, 0.434f, 0.934f, 1.0f }, // purple (#5f6fef)
          { 0.512f, 0.934f, 0.371f, 1.0f }  // green  (#83ef5f)
        } };

typedef struct dt_iop_overexposed_global_data_t
{
  int kernel_overexposed;
} dt_iop_overexposed_global_data_t;

typedef struct dt_iop_overexposed_t
{
  int dummy;
} dt_iop_overexposed_t;

const char *name()
{
  return _("overexposed");
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

// FIXME: this is pretty much a duplicate of dt_ioppr_get_histogram_profile_type() excepting that it doesn't check darktable.color_profiles->mode
static void _get_histogram_profile_type(dt_colorspaces_color_profile_type_t *out_type, const gchar **out_filename)
{
  // if in gamut check use soft proof
  if(darktable.color_profiles->histogram_type == DT_COLORSPACE_SOFTPROOF)
  {
    *out_type = darktable.color_profiles->softproof_type;
    *out_filename = darktable.color_profiles->softproof_filename;
  }
  else if(darktable.color_profiles->histogram_type == DT_COLORSPACE_WORK)
    dt_ioppr_get_work_profile_type(darktable.develop, out_type, out_filename);
  else if(darktable.color_profiles->histogram_type == DT_COLORSPACE_EXPORT)
    dt_ioppr_get_export_profile_type(darktable.develop, out_type, out_filename);
  else
  {
    *out_type = darktable.color_profiles->histogram_type;
    *out_filename = darktable.color_profiles->histogram_filename;
  }
}

static void _transform_image_colorspace(dt_iop_module_t *self, const float *const img_in, float *const img_out,
                                        const dt_iop_roi_t *const roi_in)
{
  dt_colorspaces_color_profile_type_t histogram_type = DT_COLORSPACE_SRGB;
  const gchar *histogram_filename = NULL;

  _get_histogram_profile_type(&histogram_type, &histogram_filename);

  const dt_iop_order_iccprofile_info_t *const profile_info_from
      = dt_ioppr_add_profile_info_to_list(self->dev, darktable.color_profiles->display_type,
                                          darktable.color_profiles->display_filename, INTENT_PERCEPTUAL);
  const dt_iop_order_iccprofile_info_t *const profile_info_to
      = dt_ioppr_add_profile_info_to_list(self->dev, histogram_type, histogram_filename, INTENT_PERCEPTUAL);

  if(profile_info_from && profile_info_to)
    dt_ioppr_transform_image_colorspace_rgb(img_in, img_out, roi_in->width, roi_in->height, profile_info_from,
                                            profile_info_to, self->op);
  else
    fprintf(stderr, "[_transform_image_colorspace] can't create transform profile\n");
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_develop_t *dev = self->dev;

  const int ch = piece->colors;
  const int bch = ch < 4 ? ch : ch - 1;

  float *const img_tmp = dt_alloc_align(64, 4 * roi_out->width * roi_out->height * sizeof(float));
  if(img_tmp == NULL)
  {
    fprintf(stderr, "[overexposed process] can't alloc temp image\n");
    goto cleanup;
  }

  const float lower = MAX(dev->overexposed.lower / 100.0f, 1e-6f);
  const float upper = dev->overexposed.upper / 100.0f;

  const int colorscheme = dev->overexposed.colorscheme;
  const float *const upper_color = dt_iop_overexposed_colors[colorscheme][0];
  const float *const lower_color = dt_iop_overexposed_colors[colorscheme][1];

  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;
  const int npixels = roi_out->height * roi_out->width;

  // display mask using histogram profile as output
  _transform_image_colorspace(self, in, img_tmp, roi_out);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(bch, img_tmp, in, lower, lower_color, out, npixels, \
                      upper, upper_color) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)4 * npixels; k += 4)
  {
    int img_temp_sc = 0;
    for(int c = 0; c < bch; c++)
        img_temp_sc |= img_tmp[k + c] >= upper;
    if(img_temp_sc)
    {
      for(int c = 0; c < bch; c++)
        out[k + c] = upper_color[c];
    }
    else
    {
      for(int c = 0; c < bch; c++)
        img_temp_sc |= img_tmp[k + c] <= lower;
      if(img_temp_sc <= lower)
        for(int c = 0; c < bch; c++)
          out[k + c] = lower_color[c];
      else
        for(int c = 0; c < bch; c++)
        {
          const size_t p = (size_t)k + c;
          out[p] = in[p];
        }
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) 
      dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);

cleanup:
  if(img_tmp) dt_free_align(img_tmp);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  if(pipe->type != DT_DEV_PIXELPIPE_FULL || !self->dev->overexposed.enabled || !self->dev->gui_attached)
    piece->enabled = 0;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = NULL;
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_overexposed_t));
  module->default_params = calloc(1, sizeof(dt_iop_overexposed_t));
  module->hide_enable_button = 1;
  module->default_enabled = 1;
  module->params_size = sizeof(dt_iop_overexposed_t);
  module->gui_data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
