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
#include <assert.h>
#include <lcms2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "external/wb_presets.c"
#include "gui/gtk.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"

// for Kelvin temperature and bogus WB
#include "common/colorspaces.h"
#include "external/cie_colorimetric_tables.c"

DT_MODULE_INTROSPECTION(3, dt_iop_temperature_params_t)

#define INITIALBLACKBODYTEMPERATURE 4000

#define DT_IOP_LOWEST_TEMPERATURE 1901
#define DT_IOP_HIGHEST_TEMPERATURE 25000

#define DT_IOP_LOWEST_TINT 0.135
#define DT_IOP_HIGHEST_TINT 2.326

#define DT_IOP_NUM_OF_STD_TEMP_PRESETS 4

#define COLORED_SLIDERS 0

//storing the last picked color (if any)
static float old[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

static void gui_sliders_update(struct dt_iop_module_t *self);

typedef struct dt_iop_temperature_params_t
{
  float coeffs[4];
} dt_iop_temperature_params_t;

typedef struct dt_iop_temperature_gui_data_t
{
  GtkWidget *scale_k, *scale_tint, *coeff_widgets, *scale_r, *scale_g, *scale_b, *scale_g2;
  GtkWidget *presets;
  GtkWidget *colorpicker;
  GtkWidget *finetune;
  GtkWidget *box_enabled;
  GtkWidget *label_disabled;
  GtkWidget *stack;
  int preset_cnt;
  int preset_num[50];
  double daylight_wb[4];
  double mod_coeff[4];
  double XYZ_to_CAM[4][3], CAM_to_XYZ[3][4];
} dt_iop_temperature_gui_data_t;

typedef struct dt_iop_temperature_data_t
{
  float coeffs[4];
} dt_iop_temperature_data_t;


typedef struct dt_iop_temperature_global_data_t
{
  int kernel_whitebalance_4f;
  int kernel_whitebalance_1f;
  int kernel_whitebalance_1f_xtrans;
} dt_iop_temperature_global_data_t;

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  return 1;
}

const char *name()
{
  return C_("modulename", "white balance");
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_UNSAFE_COPY;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_RAW;
}

/*
 * Spectral power distribution functions
 * https://en.wikipedia.org/wiki/Spectral_power_distribution
 */
typedef double((*spd)(unsigned long int wavelength, double TempK));

/*
 * Bruce Lindbloom, "Spectral Power Distribution of a Blackbody Radiator"
 * http://www.brucelindbloom.com/Eqn_Blackbody.html
 */
static double spd_blackbody(unsigned long int wavelength, double TempK)
{
  // convert wavelength from nm to m
  const long double lambda = (double)wavelength * 1e-9;

/*
 * these 2 constants were computed using following Sage code:
 *
 * (from http://physics.nist.gov/cgi-bin/cuu/Value?h)
 * h = 6.62606957 * 10^-34 # Planck
 * c= 299792458 # speed of light in vacuum
 * k = 1.3806488 * 10^-23 # Boltzmann
 *
 * c_1 = 2 * pi * h * c^2
 * c_2 = h * c / k
 *
 * print 'c_1 = ', c_1, ' ~= ', RealField(128)(c_1)
 * print 'c_2 = ', c_2, ' ~= ', RealField(128)(c_2)
 */

#define c1 3.7417715246641281639549488324352159753e-16L
#define c2 0.014387769599838156481252937624049081933L

  return (double)(c1 / (powl(lambda, 5) * (expl(c2 / (lambda * TempK)) - 1.0L)));

#undef c2
#undef c1
}

/*
 * Bruce Lindbloom, "Spectral Power Distribution of a CIE D-Illuminant"
 * http://www.brucelindbloom.com/Eqn_DIlluminant.html
 * and https://en.wikipedia.org/wiki/Standard_illuminant#Illuminant_series_D
 */
static double spd_daylight(unsigned long int wavelength, double TempK)
{
  cmsCIExyY WhitePoint = { 0.3127, 0.3290, 1.0 };

  /*
   * Bruce Lindbloom, "TempK to xy"
   * http://www.brucelindbloom.com/Eqn_T_to_xy.html
   */
  cmsWhitePointFromTemp(&WhitePoint, TempK);

  const double M = (0.0241 + 0.2562 * WhitePoint.x - 0.7341 * WhitePoint.y),
               m1 = (-1.3515 - 1.7703 * WhitePoint.x + 5.9114 * WhitePoint.y) / M,
               m2 = (0.0300 - 31.4424 * WhitePoint.x + 30.0717 * WhitePoint.y) / M;

  const unsigned long int j
      = ((wavelength - cie_daylight_components[0].wavelength)
         / (cie_daylight_components[1].wavelength - cie_daylight_components[0].wavelength));

  return (cie_daylight_components[j].S[0] + m1 * cie_daylight_components[j].S[1]
          + m2 * cie_daylight_components[j].S[2]);
}

/*
 * Bruce Lindbloom, "Computing XYZ From Spectral Data (Emissive Case)"
 * http://www.brucelindbloom.com/Eqn_Spect_to_XYZ.html
 */
static cmsCIEXYZ spectrum_to_XYZ(double TempK, spd I)
{
  cmsCIEXYZ Source = {.X = 0.0, .Y = 0.0, .Z = 0.0 };

  /*
   * Color matching functions
   * https://en.wikipedia.org/wiki/CIE_1931_color_space#Color_matching_functions
   */
  for(size_t i = 0; i < cie_1931_std_colorimetric_observer_count; i++)
  {
    const unsigned long int lambda = cie_1931_std_colorimetric_observer[0].wavelength
                                     + (cie_1931_std_colorimetric_observer[1].wavelength
                                        - cie_1931_std_colorimetric_observer[0].wavelength) * i;
    const double P = I(lambda, TempK);
    Source.X += P * cie_1931_std_colorimetric_observer[i].xyz.X;
    Source.Y += P * cie_1931_std_colorimetric_observer[i].xyz.Y;
    Source.Z += P * cie_1931_std_colorimetric_observer[i].xyz.Z;
  }

  // normalize so that each component is in [0.0, 1.0] range
  const double _max = MAX(MAX(Source.X, Source.Y), Source.Z);
  Source.X /= _max;
  Source.Y /= _max;
  Source.Z /= _max;

  return Source;
}

//
static cmsCIEXYZ temperature_to_XYZ(double TempK)
{
  if(TempK < DT_IOP_LOWEST_TEMPERATURE) TempK = DT_IOP_LOWEST_TEMPERATURE;
  if(TempK > DT_IOP_HIGHEST_TEMPERATURE) TempK = DT_IOP_HIGHEST_TEMPERATURE;

  if(TempK < INITIALBLACKBODYTEMPERATURE)
  {
    // if temperature is less than 4000K we use blackbody,
    // because there will be no Daylight reference below 4000K...
    return spectrum_to_XYZ(TempK, spd_blackbody);
  }
  else
  {
    return spectrum_to_XYZ(TempK, spd_daylight);
  }
}

// binary search inversion
static void XYZ_to_temperature(cmsCIEXYZ XYZ, double *TempK, double *tint)
{
  double maxtemp = DT_IOP_HIGHEST_TEMPERATURE, mintemp = DT_IOP_LOWEST_TEMPERATURE;
  cmsCIEXYZ _xyz;

  for(*TempK = (maxtemp + mintemp) / 2.0; (maxtemp - mintemp) > 1.0; *TempK = (maxtemp + mintemp) / 2.0)
  {
    _xyz = temperature_to_XYZ(*TempK);
    if(_xyz.Z / _xyz.X > XYZ.Z / XYZ.X)
      maxtemp = *TempK;
    else
      mintemp = *TempK;
  }

  *tint = (_xyz.Y / _xyz.X) / (XYZ.Y / XYZ.X);

  if(*TempK < DT_IOP_LOWEST_TEMPERATURE) *TempK = DT_IOP_LOWEST_TEMPERATURE;
  if(*TempK > DT_IOP_HIGHEST_TEMPERATURE) *TempK = DT_IOP_HIGHEST_TEMPERATURE;
  if(*tint < DT_IOP_LOWEST_TINT) *tint = DT_IOP_LOWEST_TINT;
  if(*tint > DT_IOP_HIGHEST_TINT) *tint = DT_IOP_HIGHEST_TINT;
}

static void xyz2mul(dt_iop_module_t *self, cmsCIEXYZ xyz, double mul[4])
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  double XYZ[3] = { xyz.X, xyz.Y, xyz.Z };

  double CAM[4];
  for(int k = 0; k < 4; k++)
  {
    CAM[k] = 0.0;
    for(int i = 0; i < 3; i++)
    {
      CAM[k] += g->XYZ_to_CAM[k][i] * XYZ[i];
    }
  }

  for(int k = 0; k < 4; k++) mul[k] = 1.0 / CAM[k];
}

static void temp2mul(dt_iop_module_t *self, double TempK, double tint, double mul[4])
{
  cmsCIEXYZ xyz = temperature_to_XYZ(TempK);

  xyz.Y /= tint;

  xyz2mul(self, xyz, mul);
}

static cmsCIEXYZ mul2xyz(dt_iop_module_t *self, const float coeffs[4])
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  double CAM[4];
  for(int k = 0; k < 4; k++) CAM[k] = coeffs[k] > 0.0f ? 1.0 / coeffs[k] : 0.0f;

  double XYZ[3];
  for(int k = 0; k < 3; k++)
  {
    XYZ[k] = 0.0;
    for(int i = 0; i < 4; i++)
    {
      XYZ[k] += g->CAM_to_XYZ[k][i] * CAM[i];
    }
  }

  return (cmsCIEXYZ){ XYZ[0], XYZ[1], XYZ[2] };
}

static void mul2temp(dt_iop_module_t *self, float coeffs[4], double *TempK, double *tint)
{
  XYZ_to_temperature(mul2xyz(self, coeffs), TempK, tint);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const uint32_t filters = piece->pipe->dsc.filters;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
  const dt_iop_temperature_data_t *const d = (dt_iop_temperature_data_t *)piece->data;

  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  if(filters == 9u)
  { // xtrans float mosaiced
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
    dt_omp_firstprivate(d, in, out, roi_out, xtrans) \
    schedule(static) \
    collapse(2)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      for(int i = 0; i < roi_out->width; i++)
      {
        const size_t p = (size_t)j * roi_out->width + i;
        out[p] = in[p] * d->coeffs[FCxtrans(j, i, roi_out, xtrans)];
      }
    }
  }
  else if(filters)
  { // bayer float mosaiced
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
    dt_omp_firstprivate(d, filters, in, out, roi_out) \
    schedule(static) \
    collapse(2)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      for(int i = 0; i < roi_out->width; i++)
      {
        const size_t p = (size_t)j * roi_out->width + i;
        out[p] = in[p] * d->coeffs[FC(j + roi_out->y, i + roi_out->x, filters)];
      }
    }
  }
  else
  { // non-mosaiced
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
    dt_omp_firstprivate(d, in, out, roi_out) \
    schedule(static) \
    collapse(2)
#endif
    for(size_t k = 0; k < (size_t)4 * roi_out->width * roi_out->height; k += 4)
    {
      for(int c = 0; c < 3; c++)
      {
        const size_t p = (size_t)k + c;
        out[p] = in[p] * d->coeffs[c];
      }
    }

    if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
        dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  }

  piece->pipe->dsc.temperature.enabled = 1;
  for(int k = 0; k < 4; k++)
  {
    piece->pipe->dsc.temperature.coeffs[k] = d->coeffs[k];
    piece->pipe->dsc.processed_maximum[k] = d->coeffs[k] * piece->pipe->dsc.processed_maximum[k];
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)p1;
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;

  if(self->hide_enable_button)
  {
    piece->enabled = 0;
    return;
  }

  for(int k = 0; k < 4; k++) 
    d->coeffs[k] = p->coeffs[k];
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_temperature_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)module->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)module->default_params;

  if(self->hide_enable_button)
  {
    gtk_stack_set_visible_child_name(GTK_STACK(g->stack), "disabled");
    return;
  }
  gtk_stack_set_visible_child_name(GTK_STACK(g->stack), "enabled");

  dt_iop_color_picker_reset(self, TRUE);

  double TempK, tint;
  mul2temp(self, p->coeffs, &TempK, &tint);

  dt_bauhaus_slider_set(g->scale_r, p->coeffs[0]);
  dt_bauhaus_slider_set(g->scale_g, p->coeffs[1]);
  dt_bauhaus_slider_set(g->scale_b, p->coeffs[2]);
  dt_bauhaus_slider_set(g->scale_g2, p->coeffs[3]);
  dt_bauhaus_slider_set(g->scale_k, TempK);
  dt_bauhaus_slider_set(g->scale_tint, tint);

  gui_sliders_update(self);
  for(int k = 0; k < 4; k++) g->mod_coeff[k] = p->coeffs[k];

  dt_bauhaus_combobox_clear(g->presets);
  dt_bauhaus_combobox_add(g->presets, C_("white balance", "camera"));
  dt_bauhaus_combobox_add(g->presets, C_("white balance", "camera neutral"));
  dt_bauhaus_combobox_add(g->presets, C_("white balance", "spot"));
  dt_bauhaus_combobox_add(g->presets, C_("white balance", "user modified"));
  g->preset_cnt = DT_IOP_NUM_OF_STD_TEMP_PRESETS;
  memset(g->preset_num, 0, sizeof(g->preset_num));

  const char *wb_name = NULL;
  if(!dt_image_is_ldr(&self->dev->image_storage))
    for(int i = 0; i < wb_preset_count; i++)
    {
      if(g->preset_cnt >= 50) break;
      if(!strcmp(wb_preset[i].make, self->dev->image_storage.camera_maker)
         && !strcmp(wb_preset[i].model, self->dev->image_storage.camera_model))
      {
        if(!wb_name || strcmp(wb_name, wb_preset[i].name))
        {
          wb_name = wb_preset[i].name;
          dt_bauhaus_combobox_add(g->presets, _(wb_preset[i].name));
          g->preset_num[g->preset_cnt] = i;
          g->preset_cnt++;
        }
      }
    }

  gboolean found = FALSE;
  // is this a camera white balance?
  if(memcmp(p->coeffs, fp->coeffs, 3 * sizeof(float)) == 0)
  {
    dt_bauhaus_combobox_set(g->presets, 0);
    found = TRUE;
  }
  else
    // is this a "camera neutral white balance"?
    if((p->coeffs[0] == (float)g->daylight_wb[0]) && (p->coeffs[1] == (float)g->daylight_wb[1])
       && (p->coeffs[2] == (float)g->daylight_wb[2]))
    {
      dt_bauhaus_combobox_set(g->presets, 1);
      found = TRUE;
    }

  if (!found)
    dt_bauhaus_combobox_set(g->presets, 3);
}

static void prepare_matrices(dt_iop_module_t *module)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)module->gui_data;

  // sRGB D65
  const double RGB_to_XYZ[3][4] = { { 0.4124564, 0.3575761, 0.1804375, 0 },
                                    { 0.2126729, 0.7151522, 0.0721750, 0 },
                                    { 0.0193339, 0.1191920, 0.9503041, 0 } };

  // sRGB D65
  const double XYZ_to_RGB[4][3] = { { 3.2404542, -1.5371385, -0.4985314 },
                                    { -0.9692660, 1.8760108, 0.0415560 },
                                    { 0.0556434, -0.2040259, 1.0572252 },
                                    { 0, 0, 0 } };

  if(!dt_image_is_raw(&module->dev->image_storage))
  {
    // let's just assume for now(TM) that if it is not raw, it is sRGB
    memcpy(g->XYZ_to_CAM, XYZ_to_RGB, sizeof(g->XYZ_to_CAM));
    memcpy(g->CAM_to_XYZ, RGB_to_XYZ, sizeof(g->CAM_to_XYZ));
    return;
  }

  char *camera = module->dev->image_storage.camera_makermodel;
  if (!dt_colorspaces_conversion_matrices_xyz(camera, module->dev->image_storage.d65_color_matrix,
                                                      g->XYZ_to_CAM, g->CAM_to_XYZ))
  {
    fprintf(stderr, "[temperature] `%s' color matrix not found for image\n", camera);
    dt_control_log(_("`%s' color matrix not found for image"), camera);
  }
}

static void find_coeffs(dt_iop_module_t *module, float coeffs[4])
{
  const dt_image_t *img = &module->dev->image_storage;

  // the raw should provide wb coeffs:
  int ok = 1;
  // Only check the first three values, the fourth is usually NAN for RGB
  const int num_coeffs = (img->flags & DT_IMAGE_4BAYER) ? 4 : 3;
  for(int k = 0; ok && k < num_coeffs; k++)
  {
    if(!isnormal(img->wb_coeffs[k]) || img->wb_coeffs[k] == 0.0f) ok = 0;
  }
  if(ok)
  {
    for(int k = 0; k < 4; k++) coeffs[k] = img->wb_coeffs[k];
    return;
  }
  // did not find preset either?
  // final security net: hardcoded default that fits most cams.
  coeffs[0] = 2.0f;
  coeffs[1] = 1.0f;
  coeffs[2] = 1.5f;
  coeffs[3] = 1.0f;
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_temperature_params_t tmp
      = (dt_iop_temperature_params_t){.coeffs = { 1.0, 1.0, 1.0, 1.0 } };

  // we might be called from presets update infrastructure => there is no image
  if(!module->dev || module->dev->image_storage.id == -1) goto end;

  const int is_raw = dt_image_is_matrix_correction_supported(&module->dev->image_storage);

  module->default_enabled = 0;
  module->hide_enable_button = 0;

  // White balance module doesn't need to be enabled for monochrome raws (like
  // for leica monochrom cameras). prepare_matrices is a noop as well, as there
  // isn't a color matrix, so we can skip that as well.
  if(dt_image_is_monochrome(&(module->dev->image_storage)))
    module->hide_enable_button = 1;
  else
  {
    if(module->gui_data)
      prepare_matrices(module);
    // check if file is raw / hdr //
    if(is_raw)
    {
      // raw images need wb:
      module->default_enabled = 1;
      // do best to find starting coeffs
      find_coeffs(module, tmp.coeffs);
      tmp.coeffs[0] /= tmp.coeffs[1];
      tmp.coeffs[2] /= tmp.coeffs[1];
      tmp.coeffs[3] /= tmp.coeffs[1];
      tmp.coeffs[1] = 1.0f;
    }
  }
  // remember daylight wb used for temperature/tint conversion,
  // assuming it corresponds to CIE daylight (D65)
  if(module->gui_data)
  {
    dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)module->gui_data;

    dt_bauhaus_slider_set_default(g->scale_r, tmp.coeffs[0]);
    dt_bauhaus_slider_set_default(g->scale_g, tmp.coeffs[1]);
    dt_bauhaus_slider_set_default(g->scale_b, tmp.coeffs[2]);
    dt_bauhaus_slider_set_default(g->scale_g2, tmp.coeffs[3]);

    // to have at least something and definitely not crash
    for(int c = 0; c < 4; c++)
      g->daylight_wb[c] = tmp.coeffs[c];

    double TempK, tint;
    mul2temp(module, tmp.coeffs, &TempK, &tint);

    dt_bauhaus_slider_set_default(g->scale_k, TempK);
    dt_bauhaus_slider_set_default(g->scale_tint, tint);

#if COLORED_SLIDERS
    const float neutral_stop_tint = (tint - DT_IOP_LOWEST_TINT) / (DT_IOP_HIGHEST_TINT - DT_IOP_LOWEST_TINT);
    dt_bauhaus_slider_clear_stops(g->scale_tint);
    dt_bauhaus_slider_set_stop(g->scale_tint, 0.0, 1.0, 0.0, 1.0);
    dt_bauhaus_slider_set_stop(g->scale_tint, neutral_stop_tint, 1.0, 1.0, 1.0);
    dt_bauhaus_slider_set_stop(g->scale_tint, 1.0, 0.0, 1.0, 0.0);
#endif
  }

end:
  memcpy(module->params, &tmp, sizeof(dt_iop_temperature_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_temperature_params_t));
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_temperature_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_temperature_params_t));
  module->params_size = sizeof(dt_iop_temperature_params_t);
  module->gui_data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}

static void gui_update_from_coeffs(dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  double TempK, tint;
  mul2temp(self, p->coeffs, &TempK, &tint);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->scale_k, TempK);
  dt_bauhaus_slider_set(g->scale_tint, tint);
  dt_bauhaus_slider_set(g->scale_r, p->coeffs[0]);
  dt_bauhaus_slider_set(g->scale_g, p->coeffs[1]);
  dt_bauhaus_slider_set(g->scale_b, p->coeffs[2]);
  dt_bauhaus_slider_set(g->scale_g2, p->coeffs[3]);
  --darktable.gui->reset;
}

static void temp_changed(dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;

  dt_iop_color_picker_reset(self, TRUE);

  const double TempK = dt_bauhaus_slider_get(g->scale_k);
  const double tint = dt_bauhaus_slider_get(g->scale_tint);

  double coeffs[4];
  temp2mul(self, TempK, tint, coeffs);

  // normalize
  coeffs[0] /= coeffs[1];
  coeffs[2] /= coeffs[1];
  coeffs[3] /= coeffs[1];
  coeffs[1] = 1.0;
  for(int c = 0; c < 4; c++) p->coeffs[c] = g->mod_coeff[c] = coeffs[c];

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->scale_r, p->coeffs[0]);
  dt_bauhaus_slider_set(g->scale_g, p->coeffs[1]);
  dt_bauhaus_slider_set(g->scale_b, p->coeffs[2]);
  dt_bauhaus_slider_set(g->scale_g2, p->coeffs[3]);
  --darktable.gui->reset;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void tint_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  temp_changed(self);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->presets, 3);
}

static void temp_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  temp_changed(self);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->presets, 3);
}

static void rgb_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  dt_iop_color_picker_reset(self, TRUE);
  const float value = dt_bauhaus_slider_get(slider);
  if(slider == g->scale_r)
    p->coeffs[0] = g->mod_coeff[0] = value;
  else if(slider == g->scale_g)
    p->coeffs[1] = g->mod_coeff[1] = value;
  else if(slider == g->scale_b)
    p->coeffs[2] = g->mod_coeff[2] = value;
  else if(slider == g->scale_g2)
    p->coeffs[3] = g->mod_coeff[3] = value;

  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  dt_bauhaus_combobox_set(g->presets, 3);
}

static void apply_preset(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_color_picker_reset(self, TRUE);
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)self->default_params;
  const int pos = dt_bauhaus_combobox_get(g->presets);
  switch(pos)
  {
    case -1:
      return;
    case 0: // camera wb
      for(int k = 0; k < 4; k++)
        p->coeffs[k] = fp->coeffs[k];
      break;
    case 1: // camera neutral wb
      for(int k = 0; k < 4; k++)
        p->coeffs[k] = g->daylight_wb[k];
      break;
    case 2: // spot wb, expose callback will set p->coeffs.
      //reset previously stored color picker information
      for(int k = 0; k < 4; k++)
        old[k] = 0.0f;
        
      //g_signal_emit_by_name(G_OBJECT(g->colorpicker), "quad-pressed");
      break;
    case 3: // directly changing one of the coeff sliders also changes the mod_coeff
      for(int k = 0; k < 4; k++)
        p->coeffs[k] = g->mod_coeff[k];
      break;      

  }
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void presets_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  apply_preset(self);
}

static void gui_sliders_update(struct dt_iop_module_t *self)
{
  const dt_image_t *img = &self->dev->image_storage;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  if(FILTERS_ARE_CYGM(img->buf_dsc.filters))
  {
    dt_bauhaus_widget_set_label(g->scale_r, NULL, _("green"));
    dt_bauhaus_widget_set_label(g->scale_g, NULL, _("magenta"));
    dt_bauhaus_widget_set_label(g->scale_b, NULL, _("cyan"));
    dt_bauhaus_widget_set_label(g->scale_g2, NULL, _("yellow"));

    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_b, 0);
    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_g2, 1);
    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_g, 2);
    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_r, 3);
  }
  else
  {
    dt_bauhaus_widget_set_label(g->scale_r, NULL, _("red"));
    dt_bauhaus_widget_set_label(g->scale_g, NULL, _("green"));
    dt_bauhaus_widget_set_label(g->scale_b, NULL, _("blue"));
    dt_bauhaus_widget_set_label(g->scale_g2, NULL, _("emerald"));
    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_r, 0);
    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_g, 1);
    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_b, 2);
    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_g2, 3);
  }

  gtk_widget_set_visible(GTK_WIDGET(g->scale_g2), (img->flags & DT_IMAGE_4BAYER));
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = calloc(1, sizeof(dt_iop_temperature_gui_data_t));
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->default_params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  g->stack = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(g->stack), FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), g->stack, TRUE, TRUE, 0);

  g->box_enabled = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  for(int k = 0; k < 4; k++) g->daylight_wb[k] = 1.0;
  g->scale_tint
      = dt_bauhaus_slider_new_with_range(self, DT_IOP_LOWEST_TINT, DT_IOP_HIGHEST_TINT, .01, 1.0, 3);
  g->scale_k = dt_bauhaus_slider_new_with_range(self, DT_IOP_LOWEST_TEMPERATURE, DT_IOP_HIGHEST_TEMPERATURE,
                                                10., 5000.0, 0);

  g->coeff_widgets = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  g->scale_r = dt_bauhaus_slider_new_with_range(self, 0.0, 8.0, .001, p->coeffs[0], 3);
  g->scale_g = dt_bauhaus_slider_new_with_range(self, 0.0, 8.0, .001, p->coeffs[1], 3);
  g->scale_b = dt_bauhaus_slider_new_with_range(self, 0.0, 8.0, .001, p->coeffs[2], 3);
  g->scale_g2 = dt_bauhaus_slider_new_with_range(self, 0.0, 8.0, .001, p->coeffs[3], 3);

#if COLORED_SLIDERS
  // reflect actual black body colors for the temperature slider
  const double temp_step = (double)(DT_IOP_HIGHEST_TEMPERATURE - DT_IOP_LOWEST_TEMPERATURE) / (DT_BAUHAUS_SLIDER_MAX_STOPS - 1.0);
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = i / (DT_BAUHAUS_SLIDER_MAX_STOPS - 1.0);
    const double K = DT_IOP_LOWEST_TEMPERATURE + i * temp_step;
    const cmsCIEXYZ cmsXYZ = temperature_to_XYZ(K);
    float sRGB[3], XYZ[3] = {cmsXYZ.X, cmsXYZ.Y, cmsXYZ.Z};
    dt_XYZ_to_sRGB_clipped(XYZ, sRGB);
    dt_bauhaus_slider_set_stop(g->scale_k, stop, sRGB[0], sRGB[1], sRGB[2]);
  }

  dt_bauhaus_slider_set_stop(g->scale_tint, 0.0, 1.0, 0.0, 1.0);
  dt_bauhaus_slider_set_stop(g->scale_tint, 1.0, 0.0, 1.0, 0.0);
  dt_bauhaus_slider_set_stop(g->scale_r, 0.0, 0.0, 0.0, 0.0);
  dt_bauhaus_slider_set_stop(g->scale_r, 1.0, 1.0, 0.0, 0.0);
  dt_bauhaus_slider_set_stop(g->scale_g, 0.0, 0.0, 0.0, 0.0);
  dt_bauhaus_slider_set_stop(g->scale_g, 1.0, 0.0, 1.0, 0.0);
  dt_bauhaus_slider_set_stop(g->scale_b, 0.0, 0.0, 0.0, 0.0);
  dt_bauhaus_slider_set_stop(g->scale_b, 1.0, 0.0, 0.0, 1.0);
  dt_bauhaus_slider_set_stop(g->scale_g2, 0.0, 0.0, 0.0, 0.0);
  dt_bauhaus_slider_set_stop(g->scale_g2, 1.0, 0.0, 1.0, 0.0);
#endif

  dt_bauhaus_slider_set_format(g->scale_k, "%.0f K");
  dt_bauhaus_widget_set_label(g->scale_tint, NULL, _("tint"));
  dt_bauhaus_widget_set_label(g->scale_k, NULL, _("temperature"));

  gtk_box_pack_start(GTK_BOX(g->box_enabled), g->scale_tint, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->box_enabled), g->scale_k, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->coeff_widgets), g->scale_r, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->coeff_widgets), g->scale_g, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->coeff_widgets), g->scale_b, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->coeff_widgets), g->scale_g2, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->box_enabled), g->coeff_widgets, TRUE, TRUE, 0);
  gtk_widget_set_no_show_all(g->scale_g2, TRUE);

  gui_sliders_update(self);

  g->presets = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->presets, NULL, _("preset"));
  gtk_box_pack_start(GTK_BOX(g->box_enabled), g->presets, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->presets, _("choose white balance preset from camera"));

  gtk_widget_show_all(g->box_enabled);
  gtk_stack_add_named(GTK_STACK(g->stack), g->box_enabled, "enabled");

  g->label_disabled = gtk_label_new(_("white balance disabled for camera"));
  gtk_widget_set_halign(g->label_disabled, GTK_ALIGN_START);

  gtk_widget_show_all(g->label_disabled);
  gtk_stack_add_named(GTK_STACK(g->stack), g->label_disabled, "disabled");

  gtk_stack_set_visible_child_name(GTK_STACK(g->stack), self->hide_enable_button ? "disabled" : "enabled");
  
  self->gui_update(self);

  g_signal_connect(G_OBJECT(g->scale_tint), "value-changed", G_CALLBACK(tint_callback), self);
  g_signal_connect(G_OBJECT(g->scale_k), "value-changed", G_CALLBACK(temp_callback), self);
  g_signal_connect(G_OBJECT(g->scale_r), "value-changed", G_CALLBACK(rgb_callback), self);
  g_signal_connect(G_OBJECT(g->scale_g), "value-changed", G_CALLBACK(rgb_callback), self);
  g_signal_connect(G_OBJECT(g->scale_b), "value-changed", G_CALLBACK(rgb_callback), self);
  g_signal_connect(G_OBJECT(g->scale_g2), "value-changed", G_CALLBACK(rgb_callback), self);
  g_signal_connect(G_OBJECT(g->presets), "value-changed", G_CALLBACK(presets_changed), self);
 // g_signal_connect(G_OBJECT(g->finetune), "value-changed", G_CALLBACK(finetune_changed), self);
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
  gui_sliders_update(self);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
