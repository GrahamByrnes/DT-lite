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
#include "common/colorspaces.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/file_location.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_math.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// max iccprofile file name length
// must be in synch with dt_colorspaces_color_profile_t
#define DT_IOP_COLOR_ICC_LEN 512
#define LUT_SAMPLES 0x10000

DT_MODULE_INTROSPECTION(5, dt_iop_colorout_params_t)

typedef struct dt_iop_colorout_data_t
{
  dt_colorspaces_color_profile_type_t type;
  dt_colorspaces_color_mode_t mode;
  float lut[3][LUT_SAMPLES];
  float cmatrix[9];
  cmsHTRANSFORM *xform;
  float unbounded_coeffs[3][3]; // for extrapolation of shaper curves
} dt_iop_colorout_data_t;

typedef struct dt_iop_colorout_global_data_t
{
  int kernel_colorout;
} dt_iop_colorout_global_data_t;

typedef struct dt_iop_colorout_params_t
{
  dt_colorspaces_color_profile_type_t type;
  char filename[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t intent;
} dt_iop_colorout_params_t;

typedef struct dt_iop_colorout_gui_data_t
{
  GtkWidget *output_intent, *output_profile;
} dt_iop_colorout_gui_data_t;


const char *name()
{
  return _("output color profile");
}

int default_group()
{
  return IOP_GROUP_COLOR;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_Lab;
}

int input_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                     dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_Lab;
}

int output_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                      dt_dev_pixelpipe_iop_t *piece)
{
  int cst = iop_cs_rgb;
  if(piece)
  {
    const dt_iop_colorout_data_t *const d = (dt_iop_colorout_data_t *)piece->data;
    if(d->type == DT_COLORSPACE_LAB)
      cst = iop_cs_Lab;
  }
  else
  {
    dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
    if(p->type == DT_COLORSPACE_LAB)
      cst = iop_cs_Lab;
  }
  return cst;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  return 1;
}

static void intent_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
  p->intent = (dt_iop_color_intent_t)dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void output_profile_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
  int pos = dt_bauhaus_combobox_get(widget);

  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
    if(pp->out_pos == pos)
    {
      p->type = pp->type;
      g_strlcpy(p->filename, pp->filename, sizeof(p->filename));
      dt_dev_add_history_item(darktable.develop, self, TRUE);

      dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_EXPORT);
      return;
    }
  }
  fprintf(stderr, "[colorout] color profile %s seems to have disappeared!\n", dt_colorspaces_get_name(p->type, p->filename));
}

static void _signal_profile_changed(gpointer instance, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  if(!dev->gui_attached || dev->gui_leaving) return;
  dt_dev_reprocess_center(dev);
}

#if 1
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
#endif

static void process_fastpath_apply_tonecurves(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                              void *const ovoid, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorout_data_t *const d = piece->data;
  const int ch = piece->colors;
  const int bch = ch < 4 ? ch : ch -1;

  if(!isnan(d->cmatrix[0]))
  {
    if((d->lut[0][0] >= 0.0f) || (d->lut[1][0] >= 0.0f) || (d->lut[2][0] >= 0.0f))
    { // apply profile
      //fprintf(stderr,"fastpath, ch=%d\n", ch);
      const size_t npixels = roi_out->width * roi_out->height;
      float *const out = (float *const)ovoid;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(bch, d, out, npixels) \
      schedule(static)
#endif
      for(size_t k = 0; k < (size_t)4 * npixels; k += 4)
      {
        for(int c = 0; c < bch; c++)
          if(d->lut[c][0] >= 0.0f)
            out[k + c] = (out[k + c] < 1.0f) ? lerp_lut(d->lut[c], out[k + c])
                               : dt_iop_eval_exp(d->unbounded_coeffs[c], out[k + c]);

        for(int c = bch; c < 3; c++)
          out[k + c] = out[k];
      }
    }
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorout_data_t *const d = piece->data;
  const int ch = piece->colors;
  const int gamutcheck = (d->mode == DT_PROFILE_GAMUTCHECK);
  const int width = roi_out->width;
  const int height = roi_out->height;
  const int npixels = roi_out->width * roi_out->height;

  if(d->type == DT_COLORSPACE_LAB)
  {
    fprintf(stderr,"colorout using lab direct\n");
    memcpy(ovoid, ivoid, sizeof(float) * 4 * npixels);
  }
  else if(!isnan(d->cmatrix[0]) && ch == 4)
  {
  //fprintf(stderr,"colorout using matrix, ch=4\n");
  // convert to rgb using matrix
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(d, ivoid, ovoid, npixels) \
    schedule(static)
#endif
    for(size_t k = 0; k < (size_t)4 * npixels; k += 4)
    {
      const float *const in = (const float *const)ivoid + (size_t)k;
      float *out = (float *)ovoid + (size_t)k;
      float xyz[3];
      dt_Lab_to_XYZ(in, xyz);
      for(int c = 0; c < 3; c++)
      {
        out[c] = 0.0f;
        for(int i = 0; i < 3; i++)
          out[c] += d->cmatrix[3 * c + i] * xyz[i];
      }
      out[3] = in[3];
    }
    process_fastpath_apply_tonecurves(self, piece, ovoid, roi_out);
  }
  else if(ch == 4)
  {
    //fprintf(stderr,"colorout using xform codepath, ch=4\n");
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(d, gamutcheck, ivoid, ovoid, width, height) \
    schedule(static)
#endif
    for(int k = 0; k < height; k++)
    {
      const float *in = ((float *)ivoid) + (size_t)4 * k * width;
      float *out = ((float *)ovoid) + (size_t)4 * k * width;
      cmsDoTransform(d->xform, in, out, width);
      for(int j = 0; j < width; j++, out += 4, in += 4)
      {
        out[3] = in[3];
        
        if(gamutcheck && (out[0] < 0.0f || out[1] < 0.0f || out[2] < 0.0f))
          out[0] = 0.0f, out[1] = out[2] = 1.0f;
      }
    }
  }
  else if(ch == 1)
  {
    //fprintf(stderr,"colorout using matrix, ch=1\n");
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(gamutcheck, ivoid, ovoid, npixels) \
    schedule(static)
#endif
    for(int k = 0; k < 4 * npixels; k += 4)
    {
      const float *in = ((float *)ivoid) + (size_t)k;
      float *out = ((float *)ovoid) + (size_t)k;
      dt_Lab_to_XYZ_mono(*in, out);
      out[2] = out[1] = out[0];//////////////////
      out[3] = in[3];

      if(gamutcheck && (out[0] < 0.0f))
        out[0] = 0.0f, out[1] = out[2] = 1.0f;
    }
    process_fastpath_apply_tonecurves(self, piece, ovoid, roi_out);
  }
  else
    dt_unreachable_codepath();
  // we no longer use the working profile
  piece->pipe->dsc.work_profile_info = NULL;
}

static cmsHPROFILE _make_clipping_profile(cmsHPROFILE profile)
{
  cmsUInt32Number size;
  cmsHPROFILE old_profile = profile;
  profile = NULL;

  if(old_profile && cmsSaveProfileToMem(old_profile, NULL, &size))
  {
    char *data = malloc(size);

    if(cmsSaveProfileToMem(old_profile, data, &size))
      profile = cmsOpenProfileFromMem(data, size);

    free(data);
  }

  return profile;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)p1;
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  d->type = p->type;

  const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");

  dt_colorspaces_color_profile_type_t out_type = DT_COLORSPACE_SRGB;
  gchar *out_filename = NULL;
  dt_iop_color_intent_t out_intent = DT_INTENT_PERCEPTUAL;

  const cmsHPROFILE Lab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;

  cmsHPROFILE output = NULL;
  cmsHPROFILE softproof = NULL;
  cmsUInt32Number output_format = TYPE_RGBA_FLT;

  d->mode = (pipe->type & DT_DEV_PIXELPIPE_FULL) == DT_DEV_PIXELPIPE_FULL ? darktable.color_profiles->mode : DT_PROFILE_NORMAL;

  if(d->xform)
  {
    cmsDeleteTransform(d->xform);
    d->xform = NULL;
  }
  d->cmatrix[0] = NAN;
  d->lut[0][0] = -1.0f;
  d->lut[1][0] = -1.0f;
  d->lut[2][0] = -1.0f;

  /* if we are exporting then check and set usage of override profile */
  if((pipe->type & DT_DEV_PIXELPIPE_EXPORT) == DT_DEV_PIXELPIPE_EXPORT)
  {
    if(pipe->icc_type != DT_COLORSPACE_NONE)
    {
      p->type = pipe->icc_type;
      g_strlcpy(p->filename, pipe->icc_filename, sizeof(p->filename));
    }
    if((unsigned int)pipe->icc_intent < DT_INTENT_LAST) p->intent = pipe->icc_intent;

    out_type = p->type;
    out_filename = p->filename;
    out_intent = p->intent;
  }
  else if((pipe->type & DT_DEV_PIXELPIPE_THUMBNAIL) == DT_DEV_PIXELPIPE_THUMBNAIL)
  {
    out_type = dt_mipmap_cache_get_colorspace();
    out_filename = (out_type == DT_COLORSPACE_DISPLAY ? darktable.color_profiles->display_filename : "");
    out_intent = darktable.color_profiles->display_intent;
  }
  else if((pipe->type & DT_DEV_PIXELPIPE_PREVIEW2) == DT_DEV_PIXELPIPE_PREVIEW2)
  {
    /* preview2 is only used in second darkroom window, using display2 profile as output */
    out_type = darktable.color_profiles->display2_type;
    out_filename = darktable.color_profiles->display2_filename;
    out_intent = darktable.color_profiles->display2_intent;
  }
  else
  {
    /* we are not exporting, using display profile as output */
    out_type = darktable.color_profiles->display_type;
    out_filename = darktable.color_profiles->display_filename;
    out_intent = darktable.color_profiles->display_intent;
  }

  // when the output type is Lab then process is a nop, so we can avoid creating a transform
  // and the subsequent error messages
  d->type = out_type;
  if(out_type == DT_COLORSPACE_LAB)
    return;

  /*
   * Setup transform flags
   */
  uint32_t transformFlags = 0;

  /* creating output profile */
  if(out_type == DT_COLORSPACE_DISPLAY || out_type == DT_COLORSPACE_DISPLAY2)
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

  const dt_colorspaces_color_profile_t *out_profile
      = dt_colorspaces_get_profile(out_type, out_filename, DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY
                                                               | DT_PROFILE_DIRECTION_DISPLAY2);
  if(out_profile)
  {
    output = out_profile->profile;
    if(out_type == DT_COLORSPACE_XYZ) output_format = TYPE_XYZA_FLT;
  }
  else
  {
    output = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_OUT
                                        | DT_PROFILE_DIRECTION_DISPLAY | DT_PROFILE_DIRECTION_DISPLAY2)
                                         ->profile;
    dt_control_log(_("missing output profile has been replaced by sRGB!"));
    fprintf(stderr, "missing output profile `%s' has been replaced by sRGB!\n",
            dt_colorspaces_get_name(out_type, out_filename));
  }

  /* creating softproof profile if softproof is enabled */
  if(d->mode != DT_PROFILE_NORMAL && (pipe->type & DT_DEV_PIXELPIPE_FULL) == DT_DEV_PIXELPIPE_FULL)
  {
    const dt_colorspaces_color_profile_t *prof = dt_colorspaces_get_profile(
        darktable.color_profiles->softproof_type, darktable.color_profiles->softproof_filename,
        DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY | DT_PROFILE_DIRECTION_DISPLAY2);
    if(prof)
      softproof = prof->profile;
    else
    {
      softproof = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "",
                                             DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY
                                             | DT_PROFILE_DIRECTION_DISPLAY2)->profile;
      dt_control_log(_("missing softproof profile has been replaced by sRGB!"));
      fprintf(stderr, "missing softproof profile `%s' has been replaced by sRGB!\n",
              dt_colorspaces_get_name(darktable.color_profiles->softproof_type,
                                      darktable.color_profiles->softproof_filename));
    }
    // some of our internal profiles are what lcms considers ideal profiles as they have a parametric TRC so
    // taking a roundtrip through those profiles during softproofing has no effect. as a workaround we have to
    // make lcms quantisize those gamma tables to get the desired effect.
    // in case that fails we don't enable softproofing.
    softproof = _make_clipping_profile(softproof);
    if(softproof)
    {
      // TODO: the use of bpc should be userconfigurable either from module or preference pane
      // softproof flag and black point compensation
      transformFlags |= cmsFLAGS_SOFTPROOFING | cmsFLAGS_NOCACHE | cmsFLAGS_BLACKPOINTCOMPENSATION;

      if(d->mode == DT_PROFILE_GAMUTCHECK) transformFlags |= cmsFLAGS_GAMUTCHECK;
    }
  }
  /*
   * NOTE: theoretically, we should be passing
   * UsedDirection = LCMS_USED_AS_PROOF  into
   * dt_colorspaces_get_matrix_from_output_profile() so that
   * dt_colorspaces_get_matrix_from_profile() knows it, but since we do not try
   * to use our matrix codepath when softproof is enabled, this seemed redundant.
   */
  // get matrix from profile, if softproofing or high quality exporting always go xform codepath
  if(d->mode != DT_PROFILE_NORMAL || force_lcms2
     || dt_colorspaces_get_matrix_from_output_profile(output, d->cmatrix, d->lut[0], d->lut[1], d->lut[2],
                                                      LUT_SAMPLES, out_intent))
  {
    d->cmatrix[0] = NAN;
    d->xform = cmsCreateProofingTransform(Lab, TYPE_LabA_FLT, output, output_format, softproof,
                                          out_intent, INTENT_RELATIVE_COLORIMETRIC, transformFlags);
  }

  // user selected a non-supported output profile, check that:
  if(!d->xform && isnan(d->cmatrix[0]))
  {
    dt_control_log(_("unsupported output profile has been replaced by sRGB!"));
    fprintf(stderr, "unsupported output profile `%s' has been replaced by sRGB!\n", out_profile->name);
    output = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_OUT)->profile;
    if(d->mode != DT_PROFILE_NORMAL
       || dt_colorspaces_get_matrix_from_output_profile(output, d->cmatrix, d->lut[0], d->lut[1],
                                                        d->lut[2], LUT_SAMPLES, out_intent))
    {
      d->cmatrix[0] = NAN;
      d->xform = cmsCreateProofingTransform(Lab, TYPE_LabA_FLT, output, output_format, softproof,
                                            out_intent, INTENT_RELATIVE_COLORIMETRIC, transformFlags);
    }
  }

  if(out_type == DT_COLORSPACE_DISPLAY || out_type == DT_COLORSPACE_DISPLAY2)
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

  // now try to initialize unbounded mode:
  // we do extrapolation for input values above 1.0f.
  // unfortunately we can only do this if we got the computation
  // in our hands, i.e. for the fast builtin-dt-matrix-profile path.
  for(int k = 0; k < 3; k++)
  {
    // omit luts marked as linear (negative as marker)
    if(d->lut[k][0] >= 0.0f)
    {
      const float x[4] = { 0.7f, 0.8f, 0.9f, 1.0f };
      const float y[4] = { lerp_lut(d->lut[k], x[0]), lerp_lut(d->lut[k], x[1]), lerp_lut(d->lut[k], x[2]),
                           lerp_lut(d->lut[k], x[3]) };
      dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs[k]);
    }
    else
      d->unbounded_coeffs[k][0] = -1.0f;
  }

  // softproof is never the original but always a copy that went through _make_clipping_profile()
  dt_colorspaces_cleanup_profile(softproof);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_colorout_data_t));
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  d->xform = NULL;
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  if(d->xform)
  {
    cmsDeleteTransform(d->xform);
    d->xform = NULL;
  }

  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)module->params;
  dt_bauhaus_combobox_set(g->output_intent, (int)p->intent);

  for(GList *iter = darktable.color_profiles->profiles; iter; iter = g_list_next(iter))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)iter->data;
    if(pp->out_pos > -1 &&
       p->type == pp->type && (p->type != DT_COLORSPACE_FILE || !strcmp(p->filename, pp->filename)))
    {
      dt_bauhaus_combobox_set(g->output_profile, pp->out_pos);
      return;
    }
  }

  dt_bauhaus_combobox_set(g->output_profile, 0);
  fprintf(stderr, "[colorout] could not find requested profile `%s'!\n", dt_colorspaces_get_name(p->type, p->filename));
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_colorout_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_colorout_params_t));
  module->params_size = sizeof(dt_iop_colorout_params_t);
  module->gui_data = NULL;
  module->hide_enable_button = 1;
  module->default_enabled = 1;
  dt_iop_colorout_params_t tmp = (dt_iop_colorout_params_t){ DT_COLORSPACE_SRGB, "", DT_INTENT_PERCEPTUAL};
  memcpy(module->params, &tmp, sizeof(dt_iop_colorout_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorout_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}

static void _preference_changed(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;

  const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");
  if(force_lcms2)
  {
    gtk_widget_set_no_show_all(g->output_intent, FALSE);
    gtk_widget_set_visible(g->output_intent, TRUE);
  }
  else
  {
    gtk_widget_set_no_show_all(g->output_intent, TRUE);
    gtk_widget_set_visible(g->output_intent, FALSE);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");
  self->gui_data = calloc(1, sizeof(dt_iop_colorout_gui_data_t));
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;

  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->output_intent = dt_bauhaus_combobox_new(self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->output_intent, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->output_intent, NULL, _("output intent"));
  dt_bauhaus_combobox_add(g->output_intent, _("perceptual"));
  dt_bauhaus_combobox_add(g->output_intent, _("relative colorimetric"));
  dt_bauhaus_combobox_add(g->output_intent, C_("rendering intent", "saturation"));
  dt_bauhaus_combobox_add(g->output_intent, _("absolute colorimetric"));

  if(!force_lcms2)
  {
    gtk_widget_set_no_show_all(g->output_intent, TRUE);
    gtk_widget_set_visible(g->output_intent, FALSE);
  }

  g->output_profile = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->output_profile, NULL, _("export profile"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->output_profile, TRUE, TRUE, 0);
  for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->out_pos > -1) dt_bauhaus_combobox_add(g->output_profile, prof->name);
  }

  gtk_widget_set_tooltip_text(g->output_intent, _("rendering intent"));
  char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
  char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);
  char *tooltip = g_strdup_printf(_("ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
  gtk_widget_set_tooltip_text(g->output_profile, tooltip);
  g_free(system_profile_dir);
  g_free(user_profile_dir);
  g_free(tooltip);

  g_signal_connect(G_OBJECT(g->output_intent), "value-changed", G_CALLBACK(intent_changed), (gpointer)self);
  g_signal_connect(G_OBJECT(g->output_profile), "value-changed", G_CALLBACK(output_profile_changed), (gpointer)self);

  // reload the profiles when the display or softproof profile changed!
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_CHANGED,
                            G_CALLBACK(_signal_profile_changed), self->dev);
  // update the gui when the preferences changed (i.e. show intent when using lcms2)
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE,
                            G_CALLBACK(_preference_changed), (gpointer)self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_signal_profile_changed), self->dev);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_preference_changed), self);

  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
