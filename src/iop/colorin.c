/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "common/iop_profile.h"
#include "common/colorspaces.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/file_location.h"
#include "common/image_cache.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#ifdef HAVE_OPENJPEG
#include "common/imageio_j2k.h"
#endif
#include "common/imageio_jpeg.h"
#include "common/imageio_png.h"
#include "common/imageio_tiff.h"
#ifdef HAVE_LIBAVIF
#include "common/imageio_avif.h"
#endif
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "iop/iop_api.h"

#include "external/adobe_coeff.c"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <lcms2.h>

// max iccprofile file name length
// must be in synch with dt_colorspaces_color_profile_t
#define DT_IOP_COLOR_ICC_LEN 512

#define LUT_SAMPLES 0x10000

DT_MODULE_INTROSPECTION(6, dt_iop_colorin_params_t)

static void update_profile_list(dt_iop_module_t *self);

typedef enum dt_iop_color_normalize_t
{
  DT_NORMALIZE_OFF,               //$DESCRIPTION: "off"
  DT_NORMALIZE_SRGB,              //$DESCRIPTION: "sRGB"
  DT_NORMALIZE_ADOBE_RGB,         //$DESCRIPTION: "Adobe RGB (compatible)"
  DT_NORMALIZE_LINEAR_REC709_RGB, //$DESCRIPTION: "linear Rec709 RGB"
  DT_NORMALIZE_LINEAR_REC2020_RGB //$DESCRIPTION: "linear Rec2020 RGB"
} dt_iop_color_normalize_t;

typedef struct dt_iop_colorin_params_t
{
  dt_colorspaces_color_profile_type_t type; // $DEFAULT: DT_COLORSPACE_ENHANCED_MATRIX
  char filename[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t intent;       // $DEFAULT: DT_INTENT_PERCEPTUAL
  dt_iop_color_normalize_t normalize; // $DEFAULT: DT_NORMALIZE_OFF $DESCRIPTION: "gamut clipping"
  int blue_mapping;
  // working color profile
  dt_colorspaces_color_profile_type_t type_work; // $DEFAULT: DT_COLORSPACE_LIN_REC2020
  char filename_work[DT_IOP_COLOR_ICC_LEN];
} dt_iop_colorin_params_t;

typedef struct dt_iop_colorin_gui_data_t
{
  GtkWidget *profile_combobox, *clipping_combobox, *work_combobox;
  GList *image_profiles;
  int n_image_profiles;
} dt_iop_colorin_gui_data_t;

typedef struct dt_iop_colorin_global_data_t
{
  int kernel_colorin_unbound;
  int kernel_colorin_clipping;
} dt_iop_colorin_global_data_t;

typedef struct dt_iop_colorin_data_t
{
  int clear_input;
  cmsHPROFILE input;
  cmsHPROFILE nrgb;
  cmsHTRANSFORM *xform_cam_Lab;
  cmsHTRANSFORM *xform_cam_nrgb;
  cmsHTRANSFORM *xform_nrgb_Lab;
  float lut[3][LUT_SAMPLES];
  float cmatrix[9];
  float nmatrix[9];
  float lmatrix[9];
  float unbounded_coeffs[3][3]; // approximation for extrapolation of shaper curves
  int blue_mapping;
  int nonlinearlut;
  dt_colorspaces_color_profile_type_t type;
  dt_colorspaces_color_profile_type_t type_work;
  char filename_work[DT_IOP_COLOR_ICC_LEN];
} dt_iop_colorin_data_t;


const char *name()
{
  return _("input color profile");
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

int input_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                     dt_dev_pixelpipe_iop_t *piece)
{
  if(piece)
  {
    const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
    if(d->type == DT_COLORSPACE_LAB)
      return iop_cs_Lab;
  }
  return iop_cs_rgb;
}

int output_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                      dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_Lab;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  return 0;
}

#if 0
static void intent_changed (GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  p->intent = (dt_iop_color_intent_t)dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
#endif

static void profile_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_request_focus(self);
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  int pos = dt_bauhaus_combobox_get(widget);
  GList *prof;
  if(pos < g->n_image_profiles)
    prof = g->image_profiles;
  else
  {
    prof = darktable.color_profiles->profiles;
    pos -= g->n_image_profiles;
  }
  while(prof)
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->in_pos == pos)
    {
      p->type = pp->type;
      memcpy(p->filename, pp->filename, sizeof(p->filename));
      dt_dev_add_history_item(darktable.develop, self, TRUE);

      dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_INPUT);
      return;
    }
    prof = g_list_next(prof);
  }
  // should really never happen.
  fprintf(stderr, "[colorin] color profile %s seems to have disappeared!\n", dt_colorspaces_get_name(p->type, p->filename));
}

static void workicc_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  if(darktable.gui->reset) return;

  dt_iop_request_focus(self);

  dt_colorspaces_color_profile_type_t type_work = DT_COLORSPACE_NONE;
  char filename_work[DT_IOP_COLOR_ICC_LEN];

  int pos = dt_bauhaus_combobox_get(widget);
  GList *prof = darktable.color_profiles->profiles;
  while(prof)
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->work_pos == pos)
    {
      type_work = pp->type;
      g_strlcpy(filename_work, pp->filename, sizeof(filename_work));
      break;
    }
    prof = g_list_next(prof);
  }

  if(type_work != DT_COLORSPACE_NONE)
  {
    p->type_work = type_work;
    g_strlcpy(p->filename_work, filename_work, sizeof(p->filename_work));

    const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_add_profile_info_to_list(self->dev, p->type_work, p->filename_work, DT_INTENT_PERCEPTUAL);
    if(work_profile == NULL || isnan(work_profile->matrix_in[0]) || isnan(work_profile->matrix_out[0]))
    {
      fprintf(stderr, "[colorin] can't extract matrix from colorspace `%s', it will be replaced by Rec2020 RGB!\n", p->filename_work);
      dt_control_log(_("can't extract matrix from colorspace `%s', it will be replaced by Rec2020 RGB!"), p->filename_work);

    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);

    dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_WORK);

    // we need to rebuild the pipe so the profile take effect
    self->dev->pipe->changed |= DT_DEV_PIPE_REMOVE;
    self->dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
    self->dev->preview2_pipe->changed |= DT_DEV_PIPE_REMOVE;
    self->dev->pipe->cache_obsolete = 1;
    self->dev->preview_pipe->cache_obsolete = 1;
    self->dev->preview2_pipe->cache_obsolete = 1;

    // invalidate buffers and force redraw of darkroom
    dt_dev_invalidate_all(self->dev);
  }
  else
  {
    // should really never happen.
    fprintf(stderr, "[colorin] color profile %s seems to have disappeared!\n", dt_colorspaces_get_name(p->type_work, p->filename_work));
  }
}


static float lerp_lut(const float *const lut, const float v)
{
  // TODO: check if optimization is worthwhile!
  const float ft = CLAMPS(v * (LUT_SAMPLES - 1), 0, LUT_SAMPLES - 1);
  const int t = ft < LUT_SAMPLES - 2 ? ft : LUT_SAMPLES - 2;
  const float f = ft - t;
  const float l1 = lut[t];
  const float l2 = lut[t + 1];
  return l1 * (1.0f - f) + l2 * f;
}

static inline void apply_blue_mapping(const float *const in, float *const out)
{
  out[0] = in[0];
  out[1] = in[1];
  out[2] = in[2];

  const float YY = out[0] + out[1] + out[2];
  if(YY > 0.0f)
  {
    const float zz = out[2] / YY;
    const float bound_z = 0.5f, bound_Y = 0.5f;
    const float amount = 0.11f;
    if(zz > bound_z)
    {
      const float t = (zz - bound_z) / (1.0f - bound_z) * fminf(1.0, YY / bound_Y);
      out[1] += t * amount;
      out[2] -= t * amount;
    }
  }
}

static void process_cmatrix_bm(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                               const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                               const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int clipping = (d->nrgb != NULL);

    // fprintf(stderr, "Using cmatrix codepath\n");
    // only color matrix. use our optimized fast path!
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clipping, d, ivoid, ovoid, roi_out) \
  schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    const float *in = (const float *)ivoid + (size_t)4 * j * roi_out->width;
    float *out = (float *)ovoid + (size_t)4 * j * roi_out->width;
    float cam[3];

    for(int i = 0; i < roi_out->width; i++, in += 4, out += 4)
    {
      // memcpy(cam, buf_in, sizeof(float)*3);
      // avoid calling this for linear profiles (marked with negative entries), assures unbounded
      // color management without extrapolation.
      for(int c = 0; c < 3; c++)
        cam[c] = (d->lut[c][0] >= 0.0f) ? ((in[c] < 1.0f) ? lerp_lut(d->lut[c], in[c])
                                                          : dt_iop_eval_exp(d->unbounded_coeffs[c], in[c]))
                                        : in[c];

      apply_blue_mapping(cam, cam);

      if(!clipping)
      {
        float _xyz[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        for(int c = 0; c < 3; c++)
        {
          _xyz[c] = 0.0f;
          for(int k = 0; k < 3; k++)
            _xyz[c] += d->cmatrix[3 * c + k] * cam[k];

        }

        dt_XYZ_to_Lab(_xyz, out);
      }
      else
      {
        float nRGB[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        
        for(int c = 0; c < 3; c++)
        {
          nRGB[c] = 0.0f;
          for(int k = 0; k < 3; k++)
            nRGB[c] += d->nmatrix[3 * c + k] * cam[k];

        }

        float cRGB[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(int c = 0; c < 3; c++)
          cRGB[c] = CLAMP(nRGB[c], 0.0f, 1.0f);

        float XYZ[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(int c = 0; c < 3; c++)
        {
          XYZ[c] = 0.0f;
          for(int k = 0; k < 3; k++)
            XYZ[c] += d->lmatrix[3 * c + k] * cRGB[k];
        }

        dt_XYZ_to_Lab(XYZ, out);
      }
    }
  }
}

static void process_cmatrix_fastpath_simple(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                            const void *const ivoid, void *const ovoid,
                                            const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;

// fprintf(stderr, "Using cmatrix codepath\n");
// only color matrix. use our optimized fast path!
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(d, ivoid, ovoid, roi_out) \
  schedule(static)
#endif
  for(int k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *in = (float *)ivoid + (size_t)4 * k;
    float *out = (float *)ovoid + (size_t)4 * k;
    float _xyz[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    for(int c = 0; c < 3; c++)
    {
      _xyz[c] = 0.0f;
      for(int i = 0; i < 3; i++)
        _xyz[c] += d->cmatrix[3 * c + i] * in[i];
    }

    dt_XYZ_to_Lab(_xyz, out);
  }
}

static void process_cmatrix_fastpath_clipping(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                              const void *const ivoid, void *const ovoid,
                                              const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;

// fprintf(stderr, "Using cmatrix codepath\n");
// only color matrix. use our optimized fast path!
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(d, ivoid, ovoid, roi_out) \
  schedule(static)
#endif
  for(int k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *in = (float *)ivoid + (size_t)4 * k;
    float *out = (float *)ovoid + (size_t)4 * k;

    float nRGB[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for(int c = 0; c < 3; c++)
    {
      nRGB[c] = 0.0f;
      for(int i = 0; i < 3; i++)
        nRGB[c] += d->nmatrix[3 * c + i] * in[i];
    }

    float cRGB[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for(int c = 0; c < 3; c++)
      cRGB[c] = CLAMP(nRGB[c], 0.0f, 1.0f);

    float XYZ[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for(int c = 0; c < 3; c++)
    {
      XYZ[c] = 0.0f;
      for(int i = 0; i < 3; i++)
        XYZ[c] += d->lmatrix[3 * c + i] * cRGB[i];
    }

    dt_XYZ_to_Lab(XYZ, out);
  }
}

static void process_cmatrix_fastpath(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                     const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                     const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int clipping = (d->nrgb != NULL);

  if(!clipping)
    process_cmatrix_fastpath_simple(self, piece, ivoid, ovoid, roi_in, roi_out);
  else
    process_cmatrix_fastpath_clipping(self, piece, ivoid, ovoid, roi_in, roi_out);
}

static void process_cmatrix_proper(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                   const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                   const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int clipping = (d->nrgb != NULL);
// only color matrix. use our optimized fast path!
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clipping, d, ivoid, ovoid, roi_out) \
  schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    const float *in = (const float *)ivoid + (size_t)4 * j * roi_out->width;
    float *out = (float *)ovoid + (size_t)4 * j * roi_out->width;
    float cam[3];

    for(int i = 0; i < roi_out->width; i++, in += 4, out += 4)
    {
      // memcpy(cam, buf_in, sizeof(float)*3);
      // avoid calling this for linear profiles (marked with negative entries), assures unbounded
      // color management without extrapolation.
      for(int c = 0; c < 3; c++)
        cam[c] = (d->lut[c][0] >= 0.0f) ? ((in[c] < 1.0f)
                 ? lerp_lut(d->lut[c], in[c]) : dt_iop_eval_exp(d->unbounded_coeffs[c], in[c])) : in[c];

      if(!clipping)
      {
        float _xyz[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(int c = 0; c < 3; c++)
        {
          _xyz[c] = 0.0f;
          for(int k = 0; k < 3; k++)
            _xyz[c] += d->cmatrix[3 * c + k] * cam[k];
        }
        dt_XYZ_to_Lab(_xyz, out);
      }
      else
      {
        float nRGB[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(int c = 0; c < 3; c++)
        {
          nRGB[c] = 0.0f;
          for(int k = 0; k < 3; k++)
            nRGB[c] += d->nmatrix[3 * c + k] * cam[k];
        }
        float cRGB[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(int c = 0; c < 3; c++)
          cRGB[c] = CLAMP(nRGB[c], 0.0f, 1.0f);

        float XYZ[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(int c = 0; c < 3; c++)
        {
          XYZ[c] = 0.0f;
          for(int k = 0; k < 3; k++)
            XYZ[c] += d->lmatrix[3 * c + k] * cRGB[k];
        }
        dt_XYZ_to_Lab(XYZ, out);
      }
    }
  }
}

static void process_cmatrix(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                            void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int blue_mapping = d->blue_mapping && dt_image_is_matrix_correction_supported(&piece->pipe->image);

  if(!blue_mapping &&  d->nonlinearlut==0)
    process_cmatrix_fastpath(self, piece, ivoid, ovoid, roi_in, roi_out);
  else if(blue_mapping)
    process_cmatrix_bm(self, piece, ivoid, ovoid, roi_in, roi_out);
  else
    process_cmatrix_proper(self, piece, ivoid, ovoid, roi_in, roi_out);
}

static void process_lcms2_bm(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                             void *const ovoid, const dt_iop_roi_t *const roi_in,
                             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
// use general lcms2 fallback
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(d, ivoid, ovoid, roi_out) \
  schedule(static)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    const float *in = (const float *)ivoid + (size_t)4 * k * roi_out->width;
    float *out = (float *)ovoid + (size_t)4 * k * roi_out->width;
    float *camptr = (float *)out;
    for(int j = 0; j < roi_out->width; j++, in += 4, camptr += 4)
      apply_blue_mapping(in, camptr);
    // convert to (L,a/L,b/L) to be able to change L without changing saturation.
    if(!d->nrgb)
      cmsDoTransform(d->xform_cam_Lab, out, out, roi_out->width);
    else
    {
      cmsDoTransform(d->xform_cam_nrgb, out, out, roi_out->width);
      float *rgbptr = (float *)out;
      for(int j = 0; j < roi_out->width; j++, rgbptr += 4)
        for(int c = 0; c < 3; c++)
          rgbptr[c] = CLAMP(rgbptr[c], 0.0f, 1.0f);

      cmsDoTransform(d->xform_nrgb_Lab, out, out, roi_out->width);
    }
  }
}

static void process_lcms2_proper(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                 const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                 const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;

// use general lcms2 fallback
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(d, ivoid, ovoid, roi_out) \
  schedule(static)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    const float *in = (const float *)ivoid + (size_t)4 * k * roi_out->width;
    float *out = (float *)ovoid + (size_t)4 * k * roi_out->width;
    // convert to (L,a/L,b/L) to be able to change L without changing saturation.
    if(!d->nrgb)
      cmsDoTransform(d->xform_cam_Lab, in, out, roi_out->width);
    else
    {
      cmsDoTransform(d->xform_cam_nrgb, in, out, roi_out->width);
      float *rgbptr = (float *)out;
      for(int j = 0; j < roi_out->width; j++, rgbptr += 4)
        for(int c = 0; c < 3; c++)
          rgbptr[c] = CLAMP(rgbptr[c], 0.0f, 1.0f);

      cmsDoTransform(d->xform_nrgb_Lab, out, out, roi_out->width);
    }
  }
}

static void process_lcms2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                          void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int blue_mapping = d->blue_mapping && dt_image_is_matrix_correction_supported(&piece->pipe->image);
  // use general lcms2 fallback
  if(blue_mapping)
    process_lcms2_bm(self, piece, ivoid, ovoid, roi_in, roi_out);
  else
    process_lcms2_proper(self, piece, ivoid, ovoid, roi_in, roi_out);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  
  if(d->type == DT_COLORSPACE_LAB)
    memcpy(ovoid, ivoid, sizeof(float) * 4 * roi_out->width * roi_out->height);
  else if(!isnan(d->cmatrix[0]))
    process_cmatrix(self, piece, ivoid, ovoid, roi_in, roi_out);
  else
    process_lcms2(self, piece, ivoid, ovoid, roi_in, roi_out);

  dt_ioppr_set_pipe_work_profile_info(self->dev, piece->pipe, d->type_work, d->filename_work, DT_INTENT_PERCEPTUAL);

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) 
      dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

static void mat3mul(float *dst, const float *const m1, const float *const m2)
{
  for(int k = 0; k < 3; k++)
    for(int i = 0; i < 3; i++)
    {
      float x = 0.0f;
      for(int j = 0; j < 3; j++) x += m1[3 * k + j] * m2[3 * j + i];
      dst[3 * k + i] = x;
    }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)p1;
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  d->type = p->type;
  d->type_work = p->type_work;
  g_strlcpy(d->filename_work, p->filename_work, sizeof(d->filename_work));

  const cmsHPROFILE Lab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;
  // only clean up when it's a type that we created here
  if(d->input && d->clear_input)
    dt_colorspaces_cleanup_profile(d->input);

  d->input = NULL;
  d->clear_input = 0;
  d->nrgb = NULL;
  d->blue_mapping = p->blue_mapping;

  switch(p->normalize)
  {
    case DT_NORMALIZE_SRGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_IN)->profile;
      break;
    case DT_NORMALIZE_ADOBE_RGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_ADOBERGB, "", DT_PROFILE_DIRECTION_IN)->profile;
      break;
    case DT_NORMALIZE_LINEAR_REC709_RGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC709, "", DT_PROFILE_DIRECTION_IN)->profile;
      break;
    case DT_NORMALIZE_LINEAR_REC2020_RGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC2020, "", DT_PROFILE_DIRECTION_IN)->profile;
      break;
    case DT_NORMALIZE_OFF:
    default:
      d->nrgb = NULL;
  }

  if(d->xform_cam_Lab)
  {
    cmsDeleteTransform(d->xform_cam_Lab);
    d->xform_cam_Lab = NULL;
  }
  if(d->xform_cam_nrgb)
  {
    cmsDeleteTransform(d->xform_cam_nrgb);
    d->xform_cam_nrgb = NULL;
  }
  if(d->xform_nrgb_Lab)
  {
    cmsDeleteTransform(d->xform_nrgb_Lab);
    d->xform_nrgb_Lab = NULL;
  }

  d->cmatrix[0] = d->nmatrix[0] = d->lmatrix[0] = NAN;
  d->lut[0][0] = -1.0f;
  d->lut[1][0] = -1.0f;
  d->lut[2][0] = -1.0f;
  d->nonlinearlut = 0;
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));

  dt_colorspaces_color_profile_type_t type = p->type;
  if(type == DT_COLORSPACE_LAB)
  {
    piece->enabled = 0;
    return;
  }
  piece->enabled = 1;

  if(type == DT_COLORSPACE_EMBEDDED_ICC)
  {
    // embedded color profile
    const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, pipe->image.id, 'r');
    if(cimg == NULL || cimg->profile == NULL)
      type = DT_COLORSPACE_EMBEDDED_MATRIX;
    else
    {
      d->input = dt_colorspaces_get_rgb_profile_from_mem(cimg->profile, cimg->profile_size);
      d->clear_input = 1;
    }
    dt_image_cache_read_release(darktable.image_cache, cimg);
  }
  if(type == DT_COLORSPACE_EMBEDDED_MATRIX)
  {
    // embedded matrix, hopefully D65
    if(isnan(pipe->image.d65_color_matrix[0]))
      type = DT_COLORSPACE_STANDARD_MATRIX;
    else
    {
      d->input = dt_colorspaces_create_xyzimatrix_profile((float(*)[3])pipe->image.d65_color_matrix);
      d->clear_input = 1;
    }
  }
  if(type == DT_COLORSPACE_STANDARD_MATRIX)
  {
    // color matrix
    float cam_xyz[12];
    cam_xyz[0] = NAN;

    // Use the legacy name if it has been set to honor the partial matching matrices of low-end Canons
    if (pipe->image.camera_legacy_makermodel[0])
      dt_dcraw_adobe_coeff(pipe->image.camera_legacy_makermodel, (float(*)[12])cam_xyz);
    else
      dt_dcraw_adobe_coeff(pipe->image.camera_makermodel, (float(*)[12])cam_xyz);

    if(isnan(cam_xyz[0]))
    {
      if(dt_image_is_matrix_correction_supported(&pipe->image))
      {
        fprintf(stderr, "[colorin] `%s' color matrix not found!\n", pipe->image.camera_makermodel);
        dt_control_log(_("`%s' color matrix not found!"), pipe->image.camera_makermodel);
      }
      type = DT_COLORSPACE_LIN_REC709;
    }
    else
    {
      d->input = dt_colorspaces_create_xyzimatrix_profile((float(*)[3])cam_xyz);
      d->clear_input = 1;
    }
  }

  if(!d->input)
  {
    const dt_colorspaces_color_profile_t *profile = dt_colorspaces_get_profile(type, p->filename, DT_PROFILE_DIRECTION_IN);
    if(profile) d->input = profile->profile;
  }

  if(!d->input && type != DT_COLORSPACE_SRGB)
  {
    // use linear_rec709_rgb as fallback for missing non-sRGB profiles:
    d->input = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC709, "", DT_PROFILE_DIRECTION_IN)->profile;
    d->clear_input = 0;
  }
  // final resort: sRGB
  if(!d->input)
  {
    d->input = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_IN)->profile;
    d->clear_input = 0;
  }
  // should never happen, but catch that case to avoid a crash
  if(!d->input)
  {
    fprintf(stderr, "[colorin] input profile could not be generated!\n");
    dt_control_log(_("input profile could not be generated!"));
    piece->enabled = 0;
    return;
  }

  cmsColorSpaceSignature input_color_space = cmsGetColorSpace(d->input);
  cmsUInt32Number input_format;
  switch(input_color_space)
  {
    case cmsSigRgbData:
      input_format = TYPE_RGBA_FLT;
      break;
    case cmsSigXYZData:
      input_format = TYPE_XYZA_FLT;
      break;
    default:
      // fprintf("%.*s", 4, input_color_space) doesn't work, it prints the string backwards :(
      fprintf(stderr, "[colorin] input profile color space `%c%c%c%c' not supported\n",
              (char)(input_color_space>>24),
              (char)(input_color_space>>16),
              (char)(input_color_space>>8),
              (char)(input_color_space));
      input_format = TYPE_RGBA_FLT; // this will fail later, triggering the linear rec709 fallback
  }

  // prepare transformation matrix or lcms2 transforms as fallback
  if(d->nrgb)
  {
    // user wants us to clip to a given RGB profile
    if(dt_colorspaces_get_matrix_from_input_profile(d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2],
                                                    LUT_SAMPLES, p->intent))
    {
      d->cmatrix[0] = NAN;
      d->xform_cam_Lab = cmsCreateTransform(d->input, input_format, Lab, TYPE_LabA_FLT, p->intent, 0);
      d->xform_cam_nrgb = cmsCreateTransform(d->input, input_format, d->nrgb, TYPE_RGBA_FLT, p->intent, 0);
      d->xform_nrgb_Lab = cmsCreateTransform(d->nrgb, TYPE_RGBA_FLT, Lab, TYPE_LabA_FLT, p->intent, 0);
    }
    else
    {
      float lutr[1], lutg[1], lutb[1];
      float omat[9];
      dt_colorspaces_get_matrix_from_output_profile(d->nrgb, omat, lutr, lutg, lutb, 1, p->intent);
      mat3mul(d->nmatrix, omat, d->cmatrix);
      dt_colorspaces_get_matrix_from_input_profile(d->nrgb, d->lmatrix, lutr, lutg, lutb, 1, p->intent);
    }
  }
  else
  {
    // default mode: unbound processing
    if(dt_colorspaces_get_matrix_from_input_profile(d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2],
                                                    LUT_SAMPLES, p->intent))
    {
      d->cmatrix[0] = NAN;
      d->xform_cam_Lab = cmsCreateTransform(d->input, input_format, Lab, TYPE_LabA_FLT, p->intent, 0);
    }
  }

  // we might have failed generating the clipping transformations, check that:
  if(d->nrgb && ((!d->xform_cam_nrgb && isnan(d->nmatrix[0])) || (!d->xform_nrgb_Lab && isnan(d->lmatrix[0]))))
  {
    if(d->xform_cam_nrgb)
    {
      cmsDeleteTransform(d->xform_cam_nrgb);
      d->xform_cam_nrgb = NULL;
    }
    if(d->xform_nrgb_Lab)
    {
      cmsDeleteTransform(d->xform_nrgb_Lab);
      d->xform_nrgb_Lab = NULL;
    }
    d->nrgb = NULL;
  }

  // user selected a non-supported output profile, check that:
  if(!d->xform_cam_Lab && isnan(d->cmatrix[0]))
  {
    if(p->type == DT_COLORSPACE_FILE)
      fprintf(stderr, "[colorin] unsupported input profile `%s' has been replaced by linear Rec709 RGB!\n", p->filename);
    else
      fprintf(stderr, "[colorin] unsupported input profile has been replaced by linear Rec709 RGB!\n");

    dt_control_log(_("unsupported input profile has been replaced by linear Rec709 RGB!"));
    if(d->input && d->clear_input)
      dt_colorspaces_cleanup_profile(d->input);

    d->nrgb = NULL;
    d->input = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC709, "", DT_PROFILE_DIRECTION_IN)->profile;
    d->clear_input = 0;
    if(dt_colorspaces_get_matrix_from_input_profile(d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2],
                                                    LUT_SAMPLES, p->intent))
    {
      d->cmatrix[0] = NAN;
      d->xform_cam_Lab = cmsCreateTransform(d->input, TYPE_RGBA_FLT, Lab, TYPE_LabA_FLT, p->intent, 0);
    }
  }

  d->nonlinearlut = 0;
  // now try to initialize unbounded mode:
  // we do a extrapolation for input values above 1.0f.
  // unfortunately we can only do this if we got the computation
  // in our hands, i.e. for the fast builtin-dt-matrix-profile path.
  for(int k = 0; k < 3; k++)
  {
    // omit luts marked as linear (negative as marker)
    if(d->lut[k][0] >= 0.0f)
    {
      d->nonlinearlut++;

      const float x[4] = { 0.7f, 0.8f, 0.9f, 1.0f };
      const float y[4] = { lerp_lut(d->lut[k], x[0]), lerp_lut(d->lut[k], x[1]),
                           lerp_lut(d->lut[k], x[2]), lerp_lut(d->lut[k], x[3]) };
      dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs[k]);
    }
    else
      d->unbounded_coeffs[k][0] = -1.0f;
  }
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colorin_data_t));
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  d->input = NULL;
  d->nrgb = NULL;
  d->xform_cam_Lab = NULL;
  d->xform_cam_nrgb = NULL;
  d->xform_nrgb_Lab = NULL;
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  if(d->input && d->clear_input) dt_colorspaces_cleanup_profile(d->input);
  if(d->xform_cam_Lab)
  {
    cmsDeleteTransform(d->xform_cam_Lab);
    d->xform_cam_Lab = NULL;
  }
  if(d->xform_cam_nrgb)
  {
    cmsDeleteTransform(d->xform_cam_nrgb);
    d->xform_cam_nrgb = NULL;
  }
  if(d->xform_nrgb_Lab)
  {
    cmsDeleteTransform(d->xform_nrgb_Lab);
    d->xform_nrgb_Lab = NULL;
  }

  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)module->params;
  dt_bauhaus_combobox_set(g->clipping_combobox, p->normalize);

  update_profile_list(self);

  // working profile
  int idx = -1;
  GList *prof = darktable.color_profiles->profiles;
  while(prof)
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->work_pos > -1 && pp->type == p->type_work
       && (pp->type != DT_COLORSPACE_FILE || dt_colorspaces_is_profile_equal(pp->filename, p->filename_work)))
    {
      idx = pp->work_pos;
      break;
    }
    prof = g_list_next(prof);
  }
  if(idx < 0)
  {
    idx = 0;
    fprintf(stderr, "[colorin] could not find requested working profile `%s'!\n",
            dt_colorspaces_get_name(p->type_work, p->filename_work));
  }
  dt_bauhaus_combobox_set(g->work_combobox, idx);

  // TODO: merge this into update_profile_list()
  prof = g->image_profiles;
  while(prof)
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->type == p->type && (pp->type != DT_COLORSPACE_FILE || dt_colorspaces_is_profile_equal(pp->filename, p->filename)))
    {
      dt_bauhaus_combobox_set(g->profile_combobox, pp->in_pos);
      return;
    }
    prof = g_list_next(prof);
  }
  prof = darktable.color_profiles->profiles;
  while(prof)
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->in_pos > -1 &&
       pp->type == p->type && (pp->type != DT_COLORSPACE_FILE || dt_colorspaces_is_profile_equal(pp->filename, p->filename)))
    {
      dt_bauhaus_combobox_set(g->profile_combobox, pp->in_pos + g->n_image_profiles);
      return;
    }
    prof = g_list_next(prof);
  }
  dt_bauhaus_combobox_set(g->profile_combobox, 0);

  if(p->type != DT_COLORSPACE_ENHANCED_MATRIX)
    fprintf(stderr, "[colorin] could not find requested profile `%s'!\n", dt_colorspaces_get_name(p->type, p->filename));
}

// FIXME: update the gui when we add/remove the eprofile or ematrix
void reload_defaults(dt_iop_module_t *module)
{
  module->default_enabled = 1;
  module->hide_enable_button = 1;

  dt_iop_colorin_params_t *d = module->default_params;

  dt_colorspaces_color_profile_type_t color_profile = DT_COLORSPACE_NONE;

  // we might be called from presets update infrastructure => there is no image
  if(!module->dev || module->dev->image_storage.id <= 0)
    goto end;

  gboolean use_eprofile = FALSE;
  // some file formats like jpeg can have an embedded color profile
  // currently we only support jpeg, j2k, tiff and png
  dt_image_t *img = dt_image_cache_get(darktable.image_cache, module->dev->image_storage.id, 'w');
  
  if(!img->profile)
  {
    char filename[PATH_MAX] = { 0 };
    gboolean from_cache = TRUE;
    dt_image_full_path(img->id, filename, sizeof(filename), &from_cache);
    const gchar *cc = filename + strlen(filename);
    for(; *cc != '.' && cc > filename; cc--)
      ;
    gchar *ext = g_ascii_strdown(cc + 1, -1);
    if(!strcmp(ext, "jpg") || !strcmp(ext, "jpeg"))
    {
      dt_imageio_jpeg_t jpg;
      if(!dt_imageio_jpeg_read_header(filename, &jpg))
      {
        img->profile_size = dt_imageio_jpeg_read_profile(&jpg, &img->profile);
        use_eprofile = (img->profile_size > 0);
      }
    }
#ifdef HAVE_OPENJPEG
    else if(!strcmp(ext, "jp2") || !strcmp(ext, "j2k") || !strcmp(ext, "j2c") || !strcmp(ext, "jpc"))
    {
      img->profile_size = dt_imageio_j2k_read_profile(filename, &img->profile);
      use_eprofile = (img->profile_size > 0);
    }
#endif
    // the ldr test just checks for magics in the file header
    else if((!strcmp(ext, "tif") || !strcmp(ext, "tiff")) && dt_imageio_is_ldr(filename))
    {
      img->profile_size = dt_imageio_tiff_read_profile(filename, &img->profile);
      use_eprofile = (img->profile_size > 0);
    }
    else if(!strcmp(ext, "png"))
    {
      img->profile_size = dt_imageio_png_read_profile(filename, &img->profile);
      use_eprofile = (img->profile_size > 0);
    }
#ifdef HAVE_LIBAVIF
    else if(!strcmp(ext, "avif"))
    {
      dt_colorspaces_cicp_t cicp;
      img->profile_size = dt_imageio_avif_read_profile(filename, &img->profile, &cicp);
      /* try the nclx box before falling back to any ICC profile */
      if((color_profile = dt_colorspaces_cicp_to_type(&cicp, filename)) == DT_COLORSPACE_NONE)
        color_profile = (img->profile_size > 0) ? DT_COLORSPACE_EMBEDDED_ICC : DT_COLORSPACE_NONE;
    }
#endif
    g_free(ext);
  }
  else
    use_eprofile = TRUE; // the image has a profile assigned

  if (color_profile != DT_COLORSPACE_NONE)
    d->type = color_profile;
  else if(use_eprofile)
    d->type = DT_COLORSPACE_EMBEDDED_ICC;
  else if(img->flags & DT_IMAGE_4BAYER) // 4Bayer images have been pre-converted to rec2020
    d->type = DT_COLORSPACE_LIN_REC709;
  else if (img->flags & DT_IMAGE_MONOCHROME)
    d->type = DT_COLORSPACE_LIN_REC709;
  else if(module->dev->image_storage.colorspace == DT_IMAGE_COLORSPACE_SRGB)
    d->type = DT_COLORSPACE_SRGB;
  else if(module->dev->image_storage.colorspace == DT_IMAGE_COLORSPACE_ADOBE_RGB)
    d->type = DT_COLORSPACE_ADOBERGB;
  else if(dt_image_is_ldr(&module->dev->image_storage))
    d->type = DT_COLORSPACE_SRGB;
  else if(!isnan(module->dev->image_storage.d65_color_matrix[0]))
    d->type = DT_COLORSPACE_EMBEDDED_MATRIX;

  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);

end:
  memcpy(module->params, module->default_params, sizeof(dt_iop_colorin_params_t));
}

static void update_profile_list(dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;

  // clear and refill the image profile list
  g_list_free_full(g->image_profiles, free);
  g->image_profiles = NULL;
  g->n_image_profiles = 0;

  int pos = -1;
  // some file formats like jpeg can have an embedded color profile
  // currently we only support jpeg, j2k, tiff and png
  const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, self->dev->image_storage.id, 'r');
  if(cimg->profile)
  {
    dt_colorspaces_color_profile_t *prof
        = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
    g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_EMBEDDED_ICC, ""), sizeof(prof->name));
    prof->type = DT_COLORSPACE_EMBEDDED_ICC;
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->in_pos = ++pos;
  }
  dt_image_cache_read_release(darktable.image_cache, cimg);
  // use the matrix embedded in some DNGs and EXRs
  if(!isnan(self->dev->image_storage.d65_color_matrix[0]))
  {
    dt_colorspaces_color_profile_t *prof
        = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
    g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_EMBEDDED_MATRIX, ""), sizeof(prof->name));
    prof->type = DT_COLORSPACE_EMBEDDED_MATRIX;
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->in_pos = ++pos;
  }
  // get color matrix from raw image:
  float cam_xyz[12];
  cam_xyz[0] = NAN;

  // Use the legacy name if it has been set to honor the partial matching matrices of low-end Canons
  if (self->dev->image_storage.camera_legacy_makermodel[0])
    dt_dcraw_adobe_coeff(self->dev->image_storage.camera_legacy_makermodel, (float(*)[12])cam_xyz);
  else
    dt_dcraw_adobe_coeff(self->dev->image_storage.camera_makermodel, (float(*)[12])cam_xyz);

  if(!isnan(cam_xyz[0]) && !(self->dev->image_storage.flags & DT_IMAGE_4BAYER))
  {
    dt_colorspaces_color_profile_t *prof
        = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
    g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_STANDARD_MATRIX, ""), sizeof(prof->name));
    prof->type = DT_COLORSPACE_STANDARD_MATRIX;
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->in_pos = ++pos;
  }

  g->n_image_profiles = pos + 1;
  g->image_profiles = g_list_first(g->image_profiles);

  // update the gui
  dt_bauhaus_combobox_clear(g->profile_combobox);

  for(GList *l = g->image_profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    dt_bauhaus_combobox_add(g->profile_combobox, prof->name);
  }
  for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->in_pos > -1) dt_bauhaus_combobox_add(g->profile_combobox, prof->name);
  }

  // working profile
  dt_bauhaus_combobox_clear(g->work_combobox);

  for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->work_pos > -1) dt_bauhaus_combobox_add(g->work_combobox, prof->name);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  // pthread_mutex_lock(&darktable.plugin_threadsafe);
  self->gui_data = malloc(sizeof(dt_iop_colorin_gui_data_t));
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;

  g->image_profiles = NULL;

  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->profile_combobox = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->profile_combobox, NULL, _("input profile"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->profile_combobox, TRUE, TRUE, 0);

  g->work_combobox = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->work_combobox, NULL, _("working profile"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->work_combobox, TRUE, TRUE, 0);

  // now generate the list of profiles applicable to the current image and update the list
  update_profile_list(self);

  dt_bauhaus_combobox_set(g->profile_combobox, 0);
  {
    char *system_profile_dir = g_build_filename(datadir, "color", "in", NULL);
    char *user_profile_dir = g_build_filename(confdir, "color", "in", NULL);
    char *tooltip = g_strdup_printf(_("ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(g->profile_combobox, tooltip);
    g_free(system_profile_dir);
    g_free(user_profile_dir);
    g_free(tooltip);
  }

  dt_bauhaus_combobox_set(g->work_combobox, 0);
  {
    char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
    char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);
    char *tooltip = g_strdup_printf(_("ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(g->work_combobox, tooltip);
    g_free(system_profile_dir);
    g_free(user_profile_dir);
    g_free(tooltip);
  }

  g_signal_connect(G_OBJECT(g->profile_combobox), "value-changed", G_CALLBACK(profile_changed), (gpointer)self);
  g_signal_connect(G_OBJECT(g->work_combobox), "value-changed", G_CALLBACK(workicc_changed), (gpointer)self);

  g->clipping_combobox = dt_bauhaus_combobox_from_params(self, "normalize");
  gtk_widget_set_tooltip_text(g->clipping_combobox, _("confine Lab values to gamut of RGB color space"));
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  while(g->image_profiles)
  {
    g_free(g->image_profiles->data);
    g->image_profiles = g_list_delete_link(g->image_profiles, g->image_profiles);
  }
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
