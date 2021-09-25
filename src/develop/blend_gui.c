/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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
#include "develop/blend.h"
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/dtpthread.h"
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/darktable.h"
#include "control/conf.h"
#include "common/iop_profile.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/masks.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/gradientslider.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"

#include <assert.h>
#include <gmodule.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CLAMP_RANGE(x, y, z) (CLAMP(x, y, z))
#define NEUTRAL_GRAY 0.5

typedef enum _iop_gui_blendif_channel_t
{
  ch_L = 0,
  ch_a = 1,
  ch_b = 2,
  ch_gray = 0,
  ch_red = 1,
  ch_green = 2,
  ch_blue = 3,
  ch_max = 4
} _iop_gui_blendif_channel_t;

const dt_develop_name_value_t dt_develop_blend_mode_names[]
    = { { NC_("blendmode", "normal"), DEVELOP_BLEND_NORMAL2 },
        { NC_("blendmode", "multiply"), DEVELOP_BLEND_MULTIPLY },
        { NC_("blendmode", "average"), DEVELOP_BLEND_AVERAGE },
        { NC_("blendmode", "addition"), DEVELOP_BLEND_ADD },
        { NC_("blendmode", "subtract"), DEVELOP_BLEND_SUBSTRACT },
        { NC_("blendmode", "difference"), DEVELOP_BLEND_DIFFERENCE2 },
        { NC_("blendmode", "Lab lightness"), DEVELOP_BLEND_LAB_LIGHTNESS },
        { NC_("blendmode", "Lab color"), DEVELOP_BLEND_LAB_COLOR },
        { NC_("blendmode", "Lab L-channel"), DEVELOP_BLEND_LAB_L },
        { NC_("blendmode", "Lab a-channel"), DEVELOP_BLEND_LAB_A },
        { NC_("blendmode", "Lab b-channel"), DEVELOP_BLEND_LAB_B },
        { NC_("blendmode", "RGB red channel"), DEVELOP_BLEND_RGB_R },
        { NC_("blendmode", "RGB green channel"), DEVELOP_BLEND_RGB_G },
        { NC_("blendmode", "RGB blue channel"), DEVELOP_BLEND_RGB_B },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_mask_mode_names[]
    = { { N_("off"), DEVELOP_MASK_DISABLED },
        { N_("uniformly"), DEVELOP_MASK_ENABLED },
        { N_("drawn mask"), DEVELOP_MASK_MASK | DEVELOP_MASK_ENABLED },
        { N_("parametric mask"), DEVELOP_MASK_CONDITIONAL | DEVELOP_MASK_ENABLED },
        { N_("raster mask"), DEVELOP_MASK_RASTER | DEVELOP_MASK_ENABLED },
        { N_("drawn & parametric mask"), DEVELOP_MASK_MASK_CONDITIONAL | DEVELOP_MASK_ENABLED },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_combine_masks_names[]
    = { { N_("exclusive"), DEVELOP_COMBINE_NORM_EXCL },
        { N_("inclusive"), DEVELOP_COMBINE_NORM_INCL },
        { N_("exclusive & inverted"), DEVELOP_COMBINE_INV_EXCL },
        { N_("inclusive & inverted"), DEVELOP_COMBINE_INV_INCL },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_feathering_guide_names[]
    = { { N_("output image"), DEVELOP_MASK_GUIDE_OUT },
        { N_("input image"), DEVELOP_MASK_GUIDE_IN },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_invert_mask_names[]
    = { { N_("off"), DEVELOP_COMBINE_NORM },
        { N_("on"), DEVELOP_COMBINE_INV },
        { "", 0 } };

static const dt_iop_gui_blendif_colorstop_t _gradient_L[]
    = { { 0.0f,   { 0, 0, 0, 1.0 } },
        { 0.125f, { NEUTRAL_GRAY / 8, NEUTRAL_GRAY / 8, NEUTRAL_GRAY / 8, 1.0 } },
        { 0.25f,  { NEUTRAL_GRAY / 4, NEUTRAL_GRAY / 4, NEUTRAL_GRAY / 4, 1.0 } },
        { 0.5f,   { NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, 1.0 } },
        { 1.0f,   { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } } };

static const dt_iop_gui_blendif_colorstop_t _gradient_a[]
    = { { 0.0f,   { 0, 0.34 * NEUTRAL_GRAY * 2, 0.27 * NEUTRAL_GRAY * 2, 1.0 } },
        { 0.25f,  { 0.25 * NEUTRAL_GRAY * 2, 0.34 * NEUTRAL_GRAY * 2, 0.39 * NEUTRAL_GRAY * 2, 1.0 } },
        { 0.375f, { 0.375 * NEUTRAL_GRAY * 2, 0.46 * NEUTRAL_GRAY * 2, 0.45 * NEUTRAL_GRAY * 2, 1.0 } },
        { 0.5f,   { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } },
        { 0.625f, { 0.51 * NEUTRAL_GRAY * 2, 0.4 * NEUTRAL_GRAY * 2, 0.45 * NEUTRAL_GRAY * 2, 1.0 } },
        { 0.75f,  { 0.52 * NEUTRAL_GRAY * 2, 0.29 * NEUTRAL_GRAY * 2, 0.39 * NEUTRAL_GRAY * 2, 1.0 } },
        { 1.0f,   { 0.53 * NEUTRAL_GRAY * 2, 0.08 * NEUTRAL_GRAY * 2, 0.28 * NEUTRAL_GRAY * 2, 1.0 } } };

static const dt_iop_gui_blendif_colorstop_t _gradient_b[]
    = { { 0.0f,   { 0, 0.27 * NEUTRAL_GRAY * 2, 0.58 * NEUTRAL_GRAY * 2, 1.0 } },
        { 0.25f,  { 0.25 * NEUTRAL_GRAY * 2, 0.39 * NEUTRAL_GRAY * 2, 0.54 * NEUTRAL_GRAY * 2, 1.0 } },
        { 0.375f, { 0.38 * NEUTRAL_GRAY * 2, 0.45 * NEUTRAL_GRAY * 2, 0.52 * NEUTRAL_GRAY * 2, 1.0 } },
        { 0.5f,   { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } },
        { 0.625f, { 0.58 * NEUTRAL_GRAY * 2, 0.55 * NEUTRAL_GRAY * 2, 0.38 * NEUTRAL_GRAY * 2, 1.0 } },
        { 0.75f,  { 0.66 * NEUTRAL_GRAY * 2, 0.59 * NEUTRAL_GRAY * 2, 0.25 * NEUTRAL_GRAY * 2, 1.0 } },
        { 1.0f,   { 0.81 * NEUTRAL_GRAY * 2, 0.66 * NEUTRAL_GRAY * 2, 0, 1.0 } } };

static const dt_iop_gui_blendif_colorstop_t _gradient_gray[]
    = { { 0.0f,   { 0, 0, 0, 1.0 } },
        { 0.125f, { NEUTRAL_GRAY / 8, NEUTRAL_GRAY / 8, NEUTRAL_GRAY / 8, 1.0 } },
        { 0.25f,  { NEUTRAL_GRAY / 4, NEUTRAL_GRAY / 4, NEUTRAL_GRAY / 4, 1.0 } },
        { 0.5f,   { NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, 1.0 } },
        { 1.0f,   { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } } };

static const dt_iop_gui_blendif_colorstop_t _gradient_red[]
    = { { 0.0f,   { 0, 0, 0, 1.0 } },
        { 0.125f, { NEUTRAL_GRAY / 8, 0, 0, 1.0 } },
        { 0.25f,  { NEUTRAL_GRAY / 4, 0, 0, 1.0 } },
        { 0.5f,   { NEUTRAL_GRAY / 2, 0, 0, 1.0 } },
        { 1.0f,   { NEUTRAL_GRAY, 0, 0, 1.0 } } };

static const dt_iop_gui_blendif_colorstop_t _gradient_green[]
    = { { 0.0f,   { 0, 0, 0, 1.0 } },
        { 0.125f, { 0, NEUTRAL_GRAY / 8, 0, 1.0 } },
        { 0.25f,  { 0, NEUTRAL_GRAY / 8, 0, 1.0 } },
        { 0.5f,   { 0, NEUTRAL_GRAY / 2, 0, 1.0 } },
        { 1.0f,   { 0, NEUTRAL_GRAY, 0, 1.0 } } };

static const dt_iop_gui_blendif_colorstop_t _gradient_blue[]
    = { { 0.0f,   { 0, 0, 0, 1.0 } },
        { 0.125f, { 0, 0, NEUTRAL_GRAY / 8, 1.0 } },
        { 0.25f,  { 0, 0, NEUTRAL_GRAY / 4, 1.0 } },
        { 0.5f,   { 0, 0, NEUTRAL_GRAY / 2, 1.0 } },
        { 1.0f,   { 0, 0, NEUTRAL_GRAY, 1.0 } } };

static void _blendif_scale(dt_iop_colorspace_type_t cst, const float *in, float *out,
                           const dt_iop_order_iccprofile_info_t *work_profile)
{
  out[0] = out[1] = out[2] = out[3] = out[4] = out[5] = out[6] = out[7] = -1.0f;

  switch(cst)
  {
    case iop_cs_Lab:
      out[0] = CLAMP_RANGE(in[0] / 100.0f, 0.0f, 1.0f);
      out[1] = CLAMP_RANGE((in[1] + 128.0f) / 256.0f, 0.0f, 1.0f);
      out[2] = CLAMP_RANGE((in[2] + 128.0f) / 256.0f, 0.0f, 1.0f);
      break;
    case iop_cs_rgb:
      if(work_profile == NULL)
        out[0] = CLAMP_RANGE(0.3f * in[0] + 0.59f * in[1] + 0.11f * in[2], 0.0f, 1.0f);
      else
        out[0] = CLAMP_RANGE(dt_ioppr_get_rgb_matrix_luminance(in, work_profile->matrix_in), 0.0f, 1.0f);

      out[1] = CLAMP_RANGE(in[0], 0.0f, 1.0f);
      out[2] = CLAMP_RANGE(in[1], 0.0f, 1.0f);
      out[3] = CLAMP_RANGE(in[2], 0.0f, 1.0f);
      break;
    default:
      out[0] = out[1] = out[2] = out[3] = out[4] = out[5] = out[6] = out[7] = -1.0f;
  }
}

static void _blendif_cook(dt_iop_colorspace_type_t cst, const float *in, float *out,
                          const dt_iop_order_iccprofile_info_t *const work_profile)
{
  out[0] = out[1] = out[2] = out[3] = out[4] = out[5] = out[6] = out[7] = -1.0f;

  switch(cst)
  {
    case iop_cs_Lab:
      out[0] = in[0];
      out[1] = in[1];
      out[2] = in[2];
      break;
    case iop_cs_rgb:
      if(work_profile == NULL)
        out[0] = (0.3f * in[0] + 0.59f * in[1] + 0.11f * in[2]) * 255.0f;
      else
        out[0] = dt_ioppr_get_rgb_matrix_luminance(in, work_profile->matrix_in) * 255.0f;
      out[1] = in[0] * 255.0f;
      out[2] = in[1] * 255.0f;
      out[3] = in[2] * 255.0f;
      break;
    default:
      out[0] = out[1] = out[2] = out[3] = out[4] = out[5] = out[6] = out[7] = -1.0f;
  }
}

static inline int _blendif_print_digits_default(float value)
{
  int digits;

  if(value < 0.0001f)
    digits = 0;
  else if(value < 0.01f)
    digits = 2;
  else if(value < 0.1f)
    digits = 1;
  else
    digits = 0;

  return digits;
}

static inline int _blendif_print_digits_ab(float value)
{
  int digits;

  if(fabs(value) < 10.0f)
    digits = 1;
  else
    digits = 0;

  return digits;
}

static void _blendif_scale_print_L(float value, char *string, int n)
{
  snprintf(string, n, "%-5.*f", _blendif_print_digits_default(value), value * 100.0f);
}

static void _blendif_scale_print_ab(float value, char *string, int n)
{
  snprintf(string, n, "%-5.*f", _blendif_print_digits_ab(value * 256.0f - 128.0f), value * 256.0f - 128.0f);
}

static void _blendif_scale_print_rgb(float value, char *string, int n)
{
  snprintf(string, n, "%-5.*f", _blendif_print_digits_default(value), value * 255.0f);
}

static void _blendif_scale_print_default(float value, char *string, int n)
{
  snprintf(string, n, "%-5.*f", _blendif_print_digits_default(value), value * 100.0f);
}

static void _blendop_masks_mode_callback(const unsigned int mask_mode, dt_iop_gui_blend_data_t *data)
{
  data->module->blend_params->mask_mode = mask_mode;

  if(mask_mode & DEVELOP_MASK_ENABLED)
    gtk_widget_show(GTK_WIDGET(data->top_box));
  else
    gtk_widget_hide(GTK_WIDGET(data->top_box));

  dt_iop_set_mask_mode(data->module, mask_mode);

  if((mask_mode & DEVELOP_MASK_ENABLED)
     && ((data->masks_inited && (mask_mode & DEVELOP_MASK_MASK))
         || (data->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))))
  {
    if(data->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))
    {
      dt_bauhaus_combobox_set_from_value(data->masks_combine_combo,
                                         data->module->blend_params->mask_combine & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL));
      gtk_widget_hide(GTK_WIDGET(data->masks_invert_combo));
      gtk_widget_show(GTK_WIDGET(data->masks_combine_combo));
    }
    else
    {
      dt_bauhaus_combobox_set_from_value(data->masks_invert_combo, 
                                         data->module->blend_params->mask_combine & DEVELOP_COMBINE_INV);
      gtk_widget_show(GTK_WIDGET(data->masks_invert_combo));
      gtk_widget_hide(GTK_WIDGET(data->masks_combine_combo));
    }
    // if this iop is operating in raw space, no alpha
    if(data->module->blend_colorspace(data->module, NULL, NULL) == iop_cs_RAW)
    {
      data->module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
      dtgtk_button_set_active(DTGTK_BUTTON(data->showmask), FALSE);
      gtk_widget_hide(GTK_WIDGET(data->showmask));

      // disable also guided-filters on RAW based color space
      gtk_widget_set_sensitive(data->masks_feathering_guide_combo, FALSE);
      gtk_widget_hide(GTK_WIDGET(data->masks_feathering_guide_combo));
      gtk_widget_set_sensitive(data->feathering_radius_slider, FALSE);
      gtk_widget_hide(GTK_WIDGET(data->feathering_radius_slider));
      gtk_widget_set_sensitive(data->brightness_slider, FALSE);
      gtk_widget_hide(GTK_WIDGET(data->brightness_slider));
      gtk_widget_set_sensitive(data->contrast_slider, FALSE);
      gtk_widget_hide(GTK_WIDGET(data->contrast_slider));
    }
    else
      gtk_widget_show(GTK_WIDGET(data->showmask));

    gtk_widget_show(GTK_WIDGET(data->bottom_box));
  }
  else
  {
    data->module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    dtgtk_button_set_active(DTGTK_BUTTON(data->showmask), FALSE);
    data->module->suppress_mask = 0;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->suppress), FALSE);

    gtk_widget_hide(GTK_WIDGET(data->bottom_box));
  }

  if(data->masks_inited && (mask_mode & DEVELOP_MASK_MASK))
    gtk_widget_show(GTK_WIDGET(data->masks_box));
  else if(data->masks_inited)
  {
    for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->masks_shapes[n]), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->masks_edit), FALSE);
    dt_masks_set_edit_mode(data->module, DT_MASKS_EDIT_OFF);
    gtk_widget_hide(GTK_WIDGET(data->masks_box));
  }
  else
  {
    for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->masks_shapes[n]), FALSE);
    gtk_widget_hide(GTK_WIDGET(data->masks_box));
  }

  if(data->raster_inited && (mask_mode & DEVELOP_MASK_RASTER))
    gtk_widget_show(GTK_WIDGET(data->raster_box));
  else if(data->raster_inited)
    gtk_widget_hide(GTK_WIDGET(data->raster_box));
  else
    gtk_widget_hide(GTK_WIDGET(data->raster_box));

  if(data->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))
    gtk_widget_show(GTK_WIDGET(data->blendif_box));
  else if(data->blendif_inited)
  {
    /* switch off color picker */
    dt_iop_color_picker_reset(data->module, FALSE);
    gtk_widget_hide(GTK_WIDGET(data->blendif_box));
  }
  else
    gtk_widget_hide(GTK_WIDGET(data->blendif_box));

  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_masks_combine_callback(GtkWidget *combo, dt_iop_gui_blend_data_t *data)
{
  const unsigned combine = GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(data->masks_combine_combo));
  data->module->blend_params->mask_combine &= ~(DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL);
  data->module->blend_params->mask_combine |= combine;
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_masks_invert_callback(GtkWidget *combo, dt_iop_gui_blend_data_t *data)
{
  unsigned int invert = GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(data->masks_invert_combo))
                        & DEVELOP_COMBINE_INV;
  if(invert)
    data->module->blend_params->mask_combine |= DEVELOP_COMBINE_INV;
  else
    data->module->blend_params->mask_combine &= ~DEVELOP_COMBINE_INV;

  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_blendif_sliders_callback(GtkDarktableGradientSlider *slider, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset) return;

  dt_develop_blend_params_t *bp = data->module->blend_params;
  const int tab = data->tab;
  int ch;
  GtkLabel **label;

  if(slider == data->upper_slider)
  {
    ch = data->channels[tab][1];
    label = data->upper_label;
  }
  else
  {
    ch = data->channels[tab][0];
    label = data->lower_label;
  }

  if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker)) &&
     !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker_set_values)))
    dt_iop_color_picker_reset(data->module, FALSE);

  float *parameters = &(bp->blendif_parameters[4 * ch]);
  dt_pthread_mutex_lock(&data->lock);

  for(int k = 0; k < 4; k++)
    parameters[k] = dtgtk_gradient_slider_multivalue_get_value(slider, k);

  dt_pthread_mutex_unlock(&data->lock);

  for(int k = 0; k < 4; k++)
  {
    char text[256];
    (data->scale_print[tab])(parameters[k], text, sizeof(text));
    gtk_label_set_text(label[k], text);
  }
  /** de-activate processing of this channel if maximum span is selected */
  if(parameters[1] == 0.0f && parameters[2] == 1.0f)
    bp->blendif &= ~(1 << ch);
  else
    bp->blendif |= (1 << ch);

  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void _blendop_blendif_polarity_callback(GtkToggleButton *togglebutton, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset) return;

  int active = gtk_toggle_button_get_active(togglebutton);
  dt_develop_blend_params_t *bp = data->module->blend_params;
  const int tab = data->tab;
  const int ch = GTK_WIDGET(togglebutton) == data->lower_polarity ? data->channels[tab][0] : data->channels[tab][1];
  GtkDarktableGradientSlider *slider = GTK_WIDGET(togglebutton) == data->lower_polarity ? data->lower_slider
                                                                                        : data->upper_slider;

  if(!active)
    bp->blendif |= (1 << (ch + 16));
  else
    bp->blendif &= ~(1 << (ch + 16));

  dtgtk_gradient_slider_multivalue_set_marker(
      slider, active ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 0);
  dtgtk_gradient_slider_multivalue_set_marker(
      slider, active ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 1);
  dtgtk_gradient_slider_multivalue_set_marker(
      slider, active ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 2);
  dtgtk_gradient_slider_multivalue_set_marker(
      slider, active ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 3);

  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
  dt_control_queue_redraw_widget(GTK_WIDGET(togglebutton));
}

static float log10_scale_callback(GtkWidget *self, float inval, int dir)
{
  float outval;
  const float tiny = 1.0e-4f;
  switch(dir)
  {
    case GRADIENT_SLIDER_SET:
      outval = (log10(CLAMP_RANGE(inval, 0.0001f, 1.0f)) + 4.0f) / 4.0f;
      break;
    case GRADIENT_SLIDER_GET:
      outval = CLAMP_RANGE(exp(M_LN10 * (4.0f * inval - 4.0f)), 0.0f, 1.0f);
      if(outval <= tiny) outval = 0.0f;
      if(outval >= 1.0f - tiny) outval = 1.0f;
      break;
    default:
      outval = inval;
  }
  return outval;
}

static float magnifier_scale_callback(GtkWidget *self, float inval, int dir)
{
  float outval;
  const float range = 6.0f;
  const float invrange = 1.0f/range;
  const float scale = tanh(range * 0.5f);
  const float invscale = 1.0f/scale;
  const float eps = 1.0e-6f;
  const float tiny = 1.0e-4f;
  switch(dir)
  {
    case GRADIENT_SLIDER_SET:
      outval = (invscale * tanh(range * (CLAMP_RANGE(inval, 0.0f, 1.0f) - 0.5f)) + 1.0f) * 0.5f;
      if(outval <= tiny) outval = 0.0f;
      if(outval >= 1.0f - tiny) outval = 1.0f;
      break;
    case GRADIENT_SLIDER_GET:
      outval = invrange * atanh((2.0f * CLAMP_RANGE(inval, eps, 1.0f - eps) - 1.0f) * scale) + 0.5f;
      if(outval <= tiny) outval = 0.0f;
      if(outval >= 1.0f - tiny) outval = 1.0f;
      break;
    default:
      outval = inval;
  }
  return outval;
}

static int _blendop_blendif_disp_alternative_worker(GtkWidget *widget, dt_iop_module_t *module, int mode,
                                                    float (*scale_callback)(GtkWidget*, float, int), const char *label)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;
  GtkDarktableGradientSlider *slider = (GtkDarktableGradientSlider *)widget;
  const int uplow = (slider == data->lower_slider) ? 0 : 1;

  GtkLabel *head = (uplow == 0) ? data->lower_head : data->upper_head;
  const char *inout = (uplow == 0) ? _("input") : _("output");

  char text[32];
  int newmode = (mode == 1) ? 1 : 0;

  if(newmode == 1)
  {
    dtgtk_gradient_slider_multivalue_set_scale_callback(slider, scale_callback);
    snprintf(text, sizeof(text), "%s%s", inout, label);
    gtk_label_set_text(head, text);
  }
  else
  {
    dtgtk_gradient_slider_multivalue_set_scale_callback(slider, NULL);
    snprintf(text, sizeof(text), "%s%s", inout, "");
    gtk_label_set_text(head, text);
  }

  return newmode;
}

static int _blendop_blendif_disp_alternative_mag(GtkWidget *widget, dt_iop_module_t *module, int mode)
{
  return _blendop_blendif_disp_alternative_worker(widget, module, mode, magnifier_scale_callback, _(" (zoom)"));
}

static int _blendop_blendif_disp_alternative_log(GtkWidget *widget, dt_iop_module_t *module, int mode)
{
  return _blendop_blendif_disp_alternative_worker(widget, module, mode, log10_scale_callback, _(" (log)"));
}

static void _blendof_blendif_disp_alternative_reset(GtkWidget *widget, dt_iop_module_t *module)
{
  (void) _blendop_blendif_disp_alternative_worker(widget, module, 0, NULL, "");
}

static dt_iop_colorspace_type_t _blendop_blendif_get_picker_colorspace(dt_iop_gui_blend_data_t *bd)
{
  dt_iop_colorspace_type_t picker_cst = -1;

  if(bd->csp == iop_cs_rgb)
  {
    if(bd->tab < 4)
      picker_cst = iop_cs_rgb;
    else
      bd->tab = 0;
  }
  else if(bd->csp == iop_cs_Lab)
  {
    if(bd->tab < 3)
      picker_cst = iop_cs_Lab;
    else
      picker_cst = iop_cs_LCh;
  }

  return picker_cst;
}

static inline int _blendif_print_digits_picker(float value)
{
  int digits;
  if(value < 10.0f) digits = 2;
  else digits = 1;

  return digits;
}

static void _update_gradient_slider_pickers(GtkWidget *callback_dummy, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;
  dt_iop_color_picker_set_cst(module, _blendop_blendif_get_picker_colorspace(data));
  float *raw_mean, *raw_min, *raw_max;
  GtkDarktableGradientSlider *widget;
  GtkLabel *label;

  ++darktable.gui->reset;

  for (int s = 0; s < 2; s++)
  {
    if(s)
    {
      raw_mean = module->picked_color;
      raw_min = module->picked_color_min;
      raw_max = module->picked_color_max;
      widget = data->lower_slider;
      label = data->lower_picker_label;
    }
    else
    {
      raw_mean = module->picked_output_color;
      raw_min = module->picked_output_color_min;
      raw_max = module->picked_output_color_max;
      widget = data->upper_slider;
      label = data->upper_picker_label;
    }

    if((gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker)) ||
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker_set_values))) && 
       (raw_min[0] != INFINITY))
    {
      float picker_mean[8], picker_min[8], picker_max[8];
      float cooked[8];
      char text[256];

      const int cst = (dt_iop_color_picker_get_active_cst(module) == iop_cs_NONE)
                          ? data->csp
                          : dt_iop_color_picker_get_active_cst(module);
      const dt_iop_order_iccprofile_info_t *work_profile
          = dt_ioppr_get_iop_work_profile_info(module, module->dev->iop);
      _blendif_scale(cst, raw_mean, picker_mean, work_profile);
      _blendif_scale(cst, raw_min, picker_min, work_profile);
      _blendif_scale(cst, raw_max, picker_max, work_profile);
      _blendif_cook(cst, raw_mean, cooked, work_profile);

      snprintf(text, sizeof(text), "(%.*f)", _blendif_print_digits_picker(cooked[data->tab]), cooked[data->tab]);

      dtgtk_gradient_slider_multivalue_set_picker_meanminmax(
          widget, picker_mean[data->tab], picker_min[data->tab], picker_max[data->tab]);
      gtk_label_set_text(label, text);
    }
    else
    {
      dtgtk_gradient_slider_multivalue_set_picker(widget, NAN);
      gtk_label_set_text(label, "");
    }
  }

  --darktable.gui->reset;
}

static void _blendop_blendif_update_tab(dt_iop_module_t *module, const int tab)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;
  dt_develop_blend_params_t *bp = module->blend_params;
  dt_develop_blend_params_t *dp = module->default_blendop_params;

  ++darktable.gui->reset;

  const int in_ch = data->channels[tab][0];
  const int out_ch = data->channels[tab][1];

  float *iparameters = &(bp->blendif_parameters[4 * in_ch]);
  float *oparameters = &(bp->blendif_parameters[4 * out_ch]);
  float *idefaults = &(dp->blendif_parameters[4 * in_ch]);
  float *odefaults = &(dp->blendif_parameters[4 * out_ch]);

  const int ipolarity = !(bp->blendif & (1 << (in_ch + 16)));
  const int opolarity = !(bp->blendif & (1 << (out_ch + 16)));
  char text[256];

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->lower_polarity), ipolarity);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->upper_polarity), opolarity);

  dtgtk_gradient_slider_multivalue_set_marker(
      data->lower_slider,
      ipolarity ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 0);
  dtgtk_gradient_slider_multivalue_set_marker(
      data->lower_slider,
      ipolarity ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 1);
  dtgtk_gradient_slider_multivalue_set_marker(
      data->lower_slider,
      ipolarity ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 2);
  dtgtk_gradient_slider_multivalue_set_marker(
      data->lower_slider,
      ipolarity ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 3);

  dtgtk_gradient_slider_multivalue_set_marker(
      data->upper_slider,
      opolarity ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 0);
  dtgtk_gradient_slider_multivalue_set_marker(
      data->upper_slider,
      opolarity ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 1);
  dtgtk_gradient_slider_multivalue_set_marker(
      data->upper_slider,
      opolarity ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 2);
  dtgtk_gradient_slider_multivalue_set_marker(
      data->upper_slider,
      opolarity ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 3);

  dt_pthread_mutex_lock(&data->lock);

  for(int k = 0; k < 4; k++)
  {
    dtgtk_gradient_slider_multivalue_set_value(data->lower_slider, iparameters[k], k);
    dtgtk_gradient_slider_multivalue_set_value(data->upper_slider, oparameters[k], k);
    dtgtk_gradient_slider_multivalue_set_resetvalue(data->lower_slider, idefaults[k], k);
    dtgtk_gradient_slider_multivalue_set_resetvalue(data->upper_slider, odefaults[k], k);
  }

  dt_pthread_mutex_unlock(&data->lock);

  for(int k = 0; k < 4; k++)
  {
    (data->scale_print[tab])(iparameters[k], text, sizeof(text));
    gtk_label_set_text(data->lower_label[k], text);
    (data->scale_print[tab])(oparameters[k], text, sizeof(text));
    gtk_label_set_text(data->upper_label[k], text);
  }

  dtgtk_gradient_slider_multivalue_clear_stops(data->lower_slider);
  dtgtk_gradient_slider_multivalue_clear_stops(data->upper_slider);

  for(int k = 0; k < data->numberstops[tab]; k++)
  {
    dtgtk_gradient_slider_multivalue_set_stop(data->lower_slider, (data->colorstops[tab])[k].stoppoint,
                                              (data->colorstops[tab])[k].color);
    dtgtk_gradient_slider_multivalue_set_stop(data->upper_slider, (data->colorstops[tab])[k].stoppoint,
                                              (data->colorstops[tab])[k].color);
  }

  dtgtk_gradient_slider_multivalue_set_increment(data->lower_slider, data->increments[tab]);
  dtgtk_gradient_slider_multivalue_set_increment(data->upper_slider, data->increments[tab]);
  _update_gradient_slider_pickers(NULL, module);

  if(data->altdisplay[tab])
  {
    data->altmode[tab][0] = (data->altdisplay[tab])(GTK_WIDGET(data->lower_slider), module, data->altmode[tab][0]);
    data->altmode[tab][1] = (data->altdisplay[tab])(GTK_WIDGET(data->upper_slider), module, data->altmode[tab][1]);
  }
  else
  {
    _blendof_blendif_disp_alternative_reset(GTK_WIDGET(data->lower_slider), module);
    _blendof_blendif_disp_alternative_reset(GTK_WIDGET(data->upper_slider), module);
  }

  --darktable.gui->reset;
}

static void _blendop_blendif_tab_switch(GtkNotebook *notebook, GtkWidget *page, guint page_num,
                                        dt_iop_gui_blend_data_t *data)
{
  const int cst_old = _blendop_blendif_get_picker_colorspace(data);
  data->tab = page_num;                     

  if(cst_old != _blendop_blendif_get_picker_colorspace(data) &&
     (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker)) ||
      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker_set_values))))
  {
    dt_iop_color_picker_set_cst(data->module, _blendop_blendif_get_picker_colorspace(data));
    dt_dev_reprocess_all(data->module->dev);
    dt_control_queue_redraw();
  }

  _blendop_blendif_update_tab(data->module, data->tab);
}

static void _blendop_blendif_showmask_clicked(GtkWidget *button, GdkEventButton *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;

  if(event->button == 1)
  {
    const int has_mask_display = module->request_mask_display & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
    module->request_mask_display &= ~(DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL | DT_DEV_PIXELPIPE_DISPLAY_ANY);
    GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
    
    if((event->state & modifiers) == (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
      module->request_mask_display |= (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
    else if((event->state & modifiers) == GDK_SHIFT_MASK)
      module->request_mask_display |= DT_DEV_PIXELPIPE_DISPLAY_CHANNEL;
    else if((event->state & modifiers) == GDK_CONTROL_MASK)
      module->request_mask_display |= DT_DEV_PIXELPIPE_DISPLAY_MASK;
    else
      module->request_mask_display |= (has_mask_display ? 0 : DT_DEV_PIXELPIPE_DISPLAY_MASK);

    if(module->request_mask_display & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL))
      dtgtk_button_set_active(DTGTK_BUTTON(button), TRUE);
    else
      dtgtk_button_set_active(DTGTK_BUTTON(button), FALSE);

    if(module->off)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), TRUE);

    dt_iop_request_focus(module);
    dt_iop_refresh_center(module);
  }
}

static void _blendop_masks_modes_none_clicked(GtkWidget *button, GdkEventButton *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  
  dt_iop_gui_blend_data_t *data = module->blend_data;

  if(event->button == 1 && data->selected_mask_mode != button)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->selected_mask_mode), FALSE); // unsets currently toggled if any
    _blendop_masks_mode_callback(DEVELOP_MASK_DISABLED, data);
    data->selected_mask_mode = button;
    /* and finally remove hinter messages */
    dt_control_hinter_message(darktable.control, "");
  }
}

static void _blendop_masks_modes_toggle(GtkToggleButton *button, dt_iop_module_t *module, const unsigned int mask_mode)
{
  if(darktable.gui->reset) return;

  dt_iop_gui_blend_data_t *data = module->blend_data;
  const gboolean was_toggled = gtk_toggle_button_get_active(button);
  // avoids trying to untoggle the cancel button
  if(data->selected_mask_mode
     != g_list_nth_data(data->masks_modes_toggles,
                        g_list_index(data->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_DISABLED))))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->selected_mask_mode), FALSE);

  if(was_toggled)
  {
    _blendop_masks_mode_callback(mask_mode, data);
    data->selected_mask_mode = GTK_WIDGET(button);
  }
  else
  {
    _blendop_masks_mode_callback(DEVELOP_MASK_DISABLED, data);
    data->selected_mask_mode = GTK_WIDGET(
                      g_list_nth_data(data->masks_modes_toggles,
                      g_list_index(data->masks_modes, (gconstpointer)DEVELOP_MASK_DISABLED)));
  }
}

static void _blendop_masks_modes_uni_toggled(GtkToggleButton *button, dt_iop_module_t *module)
{
  _blendop_masks_modes_toggle(button, module, DEVELOP_MASK_ENABLED);
}

static void _blendop_masks_modes_drawn_toggled(GtkToggleButton *button, dt_iop_module_t *module)
{
  _blendop_masks_modes_toggle(button, module, DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK);
}

static void _blendop_masks_modes_param_toggled(GtkToggleButton *button, dt_iop_module_t *module)
{
  _blendop_masks_modes_toggle(button, module, DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL);
}

static void _blendop_masks_modes_both_toggled(GtkToggleButton *button, dt_iop_module_t *module)
{
  _blendop_masks_modes_toggle(button, module, DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL);
}

static void _blendop_masks_modes_raster_toggled(GtkToggleButton *button, dt_iop_module_t *module)
{
  _blendop_masks_modes_toggle(button, module, DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER);
}

static void _blendop_blendif_suppress_toggled(GtkToggleButton *togglebutton, dt_iop_module_t *module)
{
  module->suppress_mask = gtk_toggle_button_get_active(togglebutton);
  if(darktable.gui->reset) return;

  if(module->off)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), TRUE);

  dt_iop_request_focus(module);
  dt_control_queue_redraw_widget(GTK_WIDGET(togglebutton));
  dt_iop_refresh_center(module);
}

static void _blendop_blendif_reset(GtkButton *button, dt_iop_module_t *module)
{
  module->blend_params->blendif = module->default_blendop_params->blendif;
  memcpy(module->blend_params->blendif_parameters, module->default_blendop_params->blendif_parameters,
         4 * DEVELOP_BLENDIF_SIZE * sizeof(float));

  dt_iop_color_picker_reset(module, FALSE);
  dt_iop_gui_update_blendif(module);
  dt_dev_add_history_item(darktable.develop, module, TRUE);
}

static void _blendop_blendif_invert(GtkButton *button, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;

  const dt_iop_gui_blend_data_t *data = module->blend_data;
  unsigned int toggle_mask = 0;

  switch(data->csp)
  {
    case iop_cs_Lab:
      toggle_mask = DEVELOP_BLENDIF_Lab_MASK << 16;
      break;

    case iop_cs_rgb:
      toggle_mask = DEVELOP_BLENDIF_RGB_MASK << 16;
      break;

    case iop_cs_RAW:
      toggle_mask = 0;
      break;

    case iop_cs_LCh:
    case iop_cs_HSL:
    case iop_cs_NONE:
      toggle_mask = 0;
      break;
  }

  module->blend_params->blendif ^= toggle_mask;
  module->blend_params->mask_combine ^= DEVELOP_COMBINE_MASKS_POS;
  module->blend_params->mask_combine ^= DEVELOP_COMBINE_INCL;
  dt_iop_gui_update_blending(module);
  dt_dev_add_history_item(darktable.develop, module, TRUE);
}

static int _blendop_masks_add_shape(GtkWidget *widget, dt_iop_module_t *self, gboolean continuous)
{
  if(darktable.gui->reset)
    return FALSE;

  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;
  // find out who we are
  int this = -1;

  for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
    if(widget == bd->masks_shapes[n])
    {
      this = n;
      break;
    }

  if(this < 0) return FALSE;

  // set all shape buttons to inactive
  for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), FALSE);

  // we want to be sure that the iop has focus
  dt_iop_request_focus(self);
  dt_iop_color_picker_reset(self, FALSE);
  bd->masks_shown = DT_MASKS_EDIT_FULL;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), FALSE);
  // we create the new form
  dt_masks_form_t *form = dt_masks_create(bd->masks_type[this]);
  dt_masks_change_form_gui(form);
  darktable.develop->form_gui->creation = TRUE;
  darktable.develop->form_gui->creation_module = self;

  if (continuous)
  {
    darktable.develop->form_gui->creation_continuous = TRUE;
    darktable.develop->form_gui->creation_continuous_module = self;
  }

  dt_control_queue_redraw_center();
  return TRUE;
}

static int _blendop_masks_add_shape_callback(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(event->button ==1)
    return _blendop_masks_add_shape(widget, self, event->state & GDK_CONTROL_MASK);

  return FALSE;
}

static int _blendop_masks_show_and_edit(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;

  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;

  if(event->button == 1)
  {
    ++darktable.gui->reset;
    dt_iop_request_focus(self);
    dt_iop_color_picker_reset(self, FALSE);
    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, self->blend_params->mask_id);

    if(grp && (grp->type & DT_MASKS_GROUP) && g_list_length(grp->points) > 0)
    {
      const int control_button_pressed = event->state & GDK_CONTROL_MASK;

      switch(bd->masks_shown)
      {
        case DT_MASKS_EDIT_FULL:
          bd->masks_shown = control_button_pressed ? DT_MASKS_EDIT_RESTRICTED : DT_MASKS_EDIT_OFF;
          break;
        case DT_MASKS_EDIT_RESTRICTED:
          bd->masks_shown = !control_button_pressed ? DT_MASKS_EDIT_FULL : DT_MASKS_EDIT_OFF;
          break;
        default:
        case DT_MASKS_EDIT_OFF:
          bd->masks_shown = control_button_pressed ? DT_MASKS_EDIT_RESTRICTED : DT_MASKS_EDIT_FULL;
      }
    }
    else
    {
      bd->masks_shown = DT_MASKS_EDIT_OFF;
      dt_control_hinter_message(darktable.control, "");
    }

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), bd->masks_shown != DT_MASKS_EDIT_OFF);
    dt_masks_set_edit_mode(self, bd->masks_shown);

    // set all add shape buttons to inactive
    for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), FALSE);
  
    --darktable.gui->reset;

    return TRUE;
  }

  return FALSE;
}

static void _blendop_masks_polarity_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  const int active = gtk_toggle_button_get_active(togglebutton);
  dt_develop_blend_params_t *bp = (dt_develop_blend_params_t *)self->blend_params;

  if(active)
    bp->mask_combine |= DEVELOP_COMBINE_MASKS_POS;
  else
    bp->mask_combine &= ~DEVELOP_COMBINE_MASKS_POS;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  dt_control_queue_redraw_widget(GTK_WIDGET(togglebutton));
}

gboolean blend_color_picker_apply(dt_iop_module_t *module, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;
  if(picker == data->colorpicker_set_values)
  {
    if(darktable.gui->reset) return TRUE;

    ++darktable.gui->reset;

    dt_develop_blend_params_t *bp = module->blend_params;
    const int tab = data->tab;
    float *raw_mean, *raw_min, *raw_max;
    float picker_mean[8], picker_min[8], picker_max[8];
    float picker_values[4];
    GtkDarktableGradientSlider *slider;
    int lower_upper; // lower=0, upper=1

    if(dt_key_modifier_state() == GDK_CONTROL_MASK) 
    {
      lower_upper = 1;
      raw_mean = module->picked_output_color;
      raw_min = module->picked_output_color_min;
      raw_max = module->picked_output_color_max;
      slider = data->upper_slider;
    }
    else
    {
      lower_upper = 0;
      raw_mean = module->picked_color;
      raw_min = module->picked_color_min;
      raw_max = module->picked_color_max;
      slider = data->lower_slider;
    }

    const int ch = data->channels[tab][lower_upper];
    float *parameters = &(bp->blendif_parameters[4 * ch]);
    const int cst = (dt_iop_color_picker_get_active_cst(module) == iop_cs_NONE)
                        ? data->csp
                        : dt_iop_color_picker_get_active_cst(module);
    const dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
    _blendif_scale(cst, raw_mean, picker_mean, work_profile);
    _blendif_scale(cst, raw_min, picker_min, work_profile);
    _blendif_scale(cst, raw_max, picker_max, work_profile);

    const float feather = 0.01f;

    if(picker_min[tab] > picker_max[tab])
    {
      const float tmp = picker_min[tab];
      picker_min[tab] = picker_max[tab];
      picker_max[tab] = tmp;
    }

    picker_values[0] = CLAMP(picker_min[tab] - feather, 0.f, 1.f);
    picker_values[1] = CLAMP(picker_min[tab] + feather, 0.f, 1.f);
    picker_values[2] = CLAMP(picker_max[tab] - feather, 0.f, 1.f);
    picker_values[3] = CLAMP(picker_max[tab] + feather, 0.f, 1.f);

    if(picker_values[1] > picker_values[2])
    {
      picker_values[1] = CLAMP(picker_min[tab], 0.f, 1.f);
      picker_values[2] = CLAMP(picker_max[tab], 0.f, 1.f);
    }

    picker_values[0] = CLAMP(picker_values[0], 0.f, picker_values[1]);
    picker_values[3] = CLAMP(picker_values[3], picker_values[2], 1.f);
    dt_pthread_mutex_lock(&data->lock);

    for(int k = 0; k < 4; k++)
      dtgtk_gradient_slider_multivalue_set_value(slider, picker_values[k], k);

    dt_pthread_mutex_unlock(&data->lock);
    // update picked values
    _update_gradient_slider_pickers(NULL, module);

    for(int k = 0; k < 4; k++)
    {
      char text[256];
      (data->scale_print[tab])(dtgtk_gradient_slider_multivalue_get_value(slider, k), text, sizeof(text));
      if(lower_upper == 0)
        gtk_label_set_text(data->lower_label[k], text);
      else
        gtk_label_set_text(data->upper_label[k], text);
    }

    --darktable.gui->reset;
    // save values to parameters
    dt_pthread_mutex_lock(&data->lock);

    for(int k = 0; k < 4; k++)
      parameters[k] = dtgtk_gradient_slider_multivalue_get_value(slider, k);

    dt_pthread_mutex_unlock(&data->lock);
    // de-activate processing of this channel if maximum span is selected
    if(parameters[1] == 0.0f && parameters[2] == 1.0f)
      bp->blendif &= ~(1 << ch);
    else
      bp->blendif |= (1 << ch);

    dt_dev_add_history_item(darktable.develop, module, TRUE);
    return TRUE;
  }
  else if(picker == data->colorpicker)
  {
    if(darktable.gui->reset) return TRUE;

    _update_gradient_slider_pickers(NULL, module);
    return TRUE;
  }
  else return FALSE;
}

// activate channel/mask view
static void _blendop_blendif_channel_mask_view(GtkWidget *widget, dt_iop_module_t *module, dt_dev_pixelpipe_display_mask_t mode)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;
  dt_dev_pixelpipe_display_mask_t new_request_mask_display = module->request_mask_display | mode;
  // in case user requests channel display: get the cannel
  if(new_request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_CHANNEL)
  {
    int tab = data->tab;
    int inout = (widget == GTK_WIDGET(data->lower_slider)) ? 0 : 1;
    dt_dev_pixelpipe_display_mask_t channel = data->display_channel[tab][inout];
    new_request_mask_display &= ~DT_DEV_PIXELPIPE_DISPLAY_ANY;
    new_request_mask_display |= channel;
  }

  // only if something has changed: reprocess center view
  if(new_request_mask_display != module->request_mask_display)
  {
    module->request_mask_display = new_request_mask_display;
    dt_iop_refresh_center(module);
  }
}

// toggle channel/mask view
static void _blendop_blendif_channel_mask_view_toggle(GtkWidget *widget, dt_iop_module_t *module, dt_dev_pixelpipe_display_mask_t mode)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;
  dt_dev_pixelpipe_display_mask_t new_request_mask_display = module->request_mask_display & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY;

  // toggle mode
  if(module->request_mask_display & mode)
    new_request_mask_display &= ~mode;
  else
    new_request_mask_display |= mode;

  dt_pthread_mutex_lock(&data->lock);

  if(new_request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_STICKY)
    data->save_for_leave |= DT_DEV_PIXELPIPE_DISPLAY_STICKY;
  else
    data->save_for_leave &= ~DT_DEV_PIXELPIPE_DISPLAY_STICKY;

  dt_pthread_mutex_unlock(&data->lock);
  new_request_mask_display &= ~DT_DEV_PIXELPIPE_DISPLAY_ANY;
  // in case user requests channel display: get the cannel
  if(new_request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_CHANNEL)
  {
    int tab = data->tab;
    int inout = (widget == GTK_WIDGET(data->lower_slider)) ? 0 : 1;
    dt_dev_pixelpipe_display_mask_t channel = data->display_channel[tab][inout];

    new_request_mask_display &= ~DT_DEV_PIXELPIPE_DISPLAY_ANY;
    new_request_mask_display |= channel;
  }

  if(new_request_mask_display != module->request_mask_display)
  {
    module->request_mask_display = new_request_mask_display;
    dt_iop_refresh_center(module);
  }
}

// magic mode: if mouse cursor enters a gradient slider with shift and/or control pressed we
// enter channel display and/or mask display mode
static gboolean _blendop_blendif_enter(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return FALSE;

  dt_iop_gui_blend_data_t *data = module->blend_data;
  dt_dev_pixelpipe_display_mask_t mode = 0;
  // depending on shift modifiers we activate channel and/or mask display
  GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();

  if((event->state & modifiers) == (GDK_SHIFT_MASK | GDK_CONTROL_MASK))
    mode = (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
  else if((event->state & modifiers) == GDK_SHIFT_MASK)
    mode = DT_DEV_PIXELPIPE_DISPLAY_CHANNEL;
  else if((event->state & modifiers) == GDK_CONTROL_MASK)
    mode = DT_DEV_PIXELPIPE_DISPLAY_MASK;

  dt_pthread_mutex_lock(&data->lock);

  if(mode && data->timeout_handle)
  {
    // purge any remaining timeout handlers
    g_source_remove(data->timeout_handle);
    data->timeout_handle = 0;
  }
  else if(!data->timeout_handle && !(data->save_for_leave & DT_DEV_PIXELPIPE_DISPLAY_STICKY))
    // save request_mask_display to restore later
    data->save_for_leave = module->request_mask_display & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY;

  dt_pthread_mutex_unlock(&data->lock);
  _blendop_blendif_channel_mask_view(widget, module, mode);
  dt_control_key_accelerators_off(darktable.control);
  gtk_widget_grab_focus(widget);
  return FALSE;
}


// handler for delayed mask/channel display mode switch-off
static gboolean _blendop_blendif_leave_delayed(gpointer data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)data;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  int reprocess = 0;

  dt_pthread_mutex_lock(&bd->lock);
  // restore saved request_mask_display and reprocess image
  if(bd->timeout_handle && (module->request_mask_display != (bd->save_for_leave & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY)))
  {
    module->request_mask_display = bd->save_for_leave & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY;
    reprocess = 1;
  }

  bd->timeout_handle = 0;
  dt_pthread_mutex_unlock(&bd->lock);

  if(reprocess)
    dt_iop_refresh_center(module);  // return FALSE and thereby terminate the handler

  return FALSE;
}

// de-activate magic mode when leaving the gradient slider
static gboolean _blendop_blendif_leave(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return FALSE;

  dt_iop_gui_blend_data_t *data = module->blend_data;
  // do not immediately switch-off mask/channel display in case user leaves gradient only briefly.
  // instead we activate a handler function that gets triggered after some timeout
  dt_pthread_mutex_lock(&data->lock);

  if(!(module->request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_STICKY) && !data->timeout_handle &&
      (module->request_mask_display != (data->save_for_leave & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY)))
    data->timeout_handle = g_timeout_add(1000, _blendop_blendif_leave_delayed, module);

  dt_pthread_mutex_unlock(&data->lock);

  if(!darktable.control->key_accelerators_on)
    dt_control_key_accelerators_on(darktable.control);
    
  return FALSE;
}


static gboolean _blendop_blendif_key_press(GtkWidget *widget, GdkEventKey *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return FALSE;

  dt_iop_gui_blend_data_t *data = module->blend_data;
  gboolean handled = FALSE;
  const int tab = data->tab;
  GtkDarktableGradientSlider *slider = (GtkDarktableGradientSlider *)widget;
  const int uplow = (slider == data->lower_slider) ? 0 : 1;

  switch(event->keyval)
  {
    case GDK_KEY_a:
    case GDK_KEY_A:
      if(data->altdisplay[tab])
        data->altmode[tab][uplow] = (data->altdisplay[tab])(widget, module, data->altmode[tab][uplow] + 1);
      handled = TRUE;
      break;
    case GDK_KEY_c:
      _blendop_blendif_channel_mask_view_toggle(widget, module, DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
      handled = TRUE;
      break;
    case GDK_KEY_C:
      _blendop_blendif_channel_mask_view_toggle(widget, module, DT_DEV_PIXELPIPE_DISPLAY_CHANNEL | DT_DEV_PIXELPIPE_DISPLAY_STICKY);
      handled = TRUE;
      break;
    case GDK_KEY_m:
    case GDK_KEY_M:
      _blendop_blendif_channel_mask_view_toggle(widget, module, DT_DEV_PIXELPIPE_DISPLAY_MASK);
      handled = TRUE;
  }

  if(handled)
    dt_iop_request_focus(module);

  return handled;
}

void dt_iop_gui_update_blendif(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;

  if(!data || !data->blendif_support || !data->blendif_inited) return;

  ++darktable.gui->reset;

  dt_pthread_mutex_lock(&data->lock);
  if(data->timeout_handle)
  {
    g_source_remove(data->timeout_handle);
    data->timeout_handle = 0;
    if(module->request_mask_display != (data->save_for_leave & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY))
    {
      module->request_mask_display = data->save_for_leave & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY;
      dt_dev_reprocess_all(module->dev);//DBG
    }
  }
  dt_pthread_mutex_unlock(&data->lock);

  const int tab = data->tab;
  _blendop_blendif_update_tab(module, tab);
  --darktable.gui->reset;
}

void dt_iop_gui_init_blendif(GtkBox *blendw, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  bd->blendif_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));
  // add event box so that one can click into the area to get help for parametric masks
  GtkWidget* event_box = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(blendw), event_box);
  // create and add blendif support if module supports it
  if(bd->blendif_support)
  {
    char *Lab_labels[] = { "L", "a", "b" };
    char *Lab_tooltips[]
        = { _("sliders for L channel"), _("sliders for a channel"), _("sliders for b channel") };
    char *rgb_labels[] = { _("gr"), _("R"), _("G"), _("B") };
    char *rgb_tooltips[]
        = { _("sliders for gray value"), _("sliders for red channel"), _("sliders for green channel"),
            _("sliders for blue channel") };
    char *ttinput = _("adjustment based on input received by this module:\n* range defined by upper markers: "
                      "blend fully\n* range defined by lower markers: do not blend at all\n* range between "
                      "adjacent upper/lower markers: blend gradually");
    char *ttoutput = _("adjustment based on unblended output of this module:\n* range defined by upper "
                       "markers: blend fully\n* range defined by lower markers: do not blend at all\n* range "
                       "between adjacent upper/lower markers: blend gradually");
    bd->tab = 0;
    int maxchannels = 0;
    char **labels = NULL;
    char **tooltips = NULL;

    switch(bd->csp)
    {
      case iop_cs_Lab:
        maxchannels = 3;
        labels = Lab_labels;
        tooltips = Lab_tooltips;
        bd->scale_print[0] = _blendif_scale_print_L;
        bd->scale_print[1] = _blendif_scale_print_ab;
        bd->scale_print[2] = _blendif_scale_print_ab;
        bd->scale_print[3] = _blendif_scale_print_default;
        bd->increments[0] = 1.0f / 100.0f;
        bd->increments[1] = 1.0f / 256.0f;
        bd->increments[2] = 1.0f / 256.0f;
        bd->increments[3] = 1.0f / 100.0f;
        bd->increments[4] = 1.0f / 360.0f;
        bd->channels[0][0] = DEVELOP_BLENDIF_L_in;
        bd->channels[0][1] = DEVELOP_BLENDIF_L_out;
        bd->channels[1][0] = DEVELOP_BLENDIF_A_in;
        bd->channels[1][1] = DEVELOP_BLENDIF_A_out;
        bd->channels[2][0] = DEVELOP_BLENDIF_B_in;
        bd->channels[2][1] = DEVELOP_BLENDIF_B_out;
        bd->display_channel[0][0] = DT_DEV_PIXELPIPE_DISPLAY_L;
        bd->display_channel[0][1] = DT_DEV_PIXELPIPE_DISPLAY_L | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->display_channel[1][0] = DT_DEV_PIXELPIPE_DISPLAY_a;
        bd->display_channel[1][1] = DT_DEV_PIXELPIPE_DISPLAY_a | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->display_channel[2][0] = DT_DEV_PIXELPIPE_DISPLAY_b;
        bd->display_channel[2][1] = DT_DEV_PIXELPIPE_DISPLAY_b | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->colorstops[0] = _gradient_L;
        bd->numberstops[0] = sizeof(_gradient_L) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[1] = _gradient_a;
        bd->numberstops[1] = sizeof(_gradient_a) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[2] = _gradient_b;
        bd->numberstops[2] = sizeof(_gradient_b) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->altdisplay[0] = _blendop_blendif_disp_alternative_log;
        bd->altdisplay[1] = _blendop_blendif_disp_alternative_mag;
        bd->altdisplay[2] = _blendop_blendif_disp_alternative_mag;
        bd->altdisplay[3] = _blendop_blendif_disp_alternative_log;
        break;
      case iop_cs_rgb:
        maxchannels = 4;
        labels = rgb_labels;
        tooltips = rgb_tooltips;
        bd->scale_print[0] = _blendif_scale_print_rgb;
        bd->scale_print[1] = _blendif_scale_print_rgb;
        bd->scale_print[2] = _blendif_scale_print_rgb;
        bd->scale_print[3] = _blendif_scale_print_rgb;
        bd->scale_print[5] = _blendif_scale_print_default;
        bd->scale_print[6] = _blendif_scale_print_L;
        bd->increments[0] = 1.0f / 255.0f;
        bd->increments[1] = 1.0f / 255.0f;
        bd->increments[2] = 1.0f / 255.0f;
        bd->increments[3] = 1.0f / 255.0f;
        bd->increments[4] = 1.0f / 360.0f;
        bd->increments[5] = 1.0f / 100.0f;
        bd->increments[6] = 1.0f / 100.0f;
        bd->channels[0][0] = DEVELOP_BLENDIF_GRAY_in;
        bd->channels[0][1] = DEVELOP_BLENDIF_GRAY_out;
        bd->channels[1][0] = DEVELOP_BLENDIF_RED_in;
        bd->channels[1][1] = DEVELOP_BLENDIF_RED_out;
        bd->channels[2][0] = DEVELOP_BLENDIF_GREEN_in;
        bd->channels[2][1] = DEVELOP_BLENDIF_GREEN_out;
        bd->channels[3][0] = DEVELOP_BLENDIF_BLUE_in;
        bd->channels[3][1] = DEVELOP_BLENDIF_BLUE_out;
        bd->display_channel[0][0] = DT_DEV_PIXELPIPE_DISPLAY_GRAY;
        bd->display_channel[0][1] = DT_DEV_PIXELPIPE_DISPLAY_GRAY | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->display_channel[1][0] = DT_DEV_PIXELPIPE_DISPLAY_R;
        bd->display_channel[1][1] = DT_DEV_PIXELPIPE_DISPLAY_R | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->display_channel[2][0] = DT_DEV_PIXELPIPE_DISPLAY_G;
        bd->display_channel[2][1] = DT_DEV_PIXELPIPE_DISPLAY_G | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->display_channel[3][0] = DT_DEV_PIXELPIPE_DISPLAY_B;
        bd->display_channel[3][1] = DT_DEV_PIXELPIPE_DISPLAY_B | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;
        bd->colorstops[0] = _gradient_gray;
        bd->numberstops[0] = sizeof(_gradient_gray) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[1] = _gradient_red;
        bd->numberstops[1] = sizeof(_gradient_red) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[2] = _gradient_green;
        bd->numberstops[2] = sizeof(_gradient_green) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[3] = _gradient_blue;
        bd->numberstops[3] = sizeof(_gradient_blue) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->colorstops[6] = _gradient_gray;
        bd->numberstops[6] = sizeof(_gradient_gray) / sizeof(dt_iop_gui_blendif_colorstop_t);
        bd->altdisplay[0] = _blendop_blendif_disp_alternative_log;
        bd->altdisplay[1] = _blendop_blendif_disp_alternative_log;
        bd->altdisplay[2] = _blendop_blendif_disp_alternative_log;
        bd->altdisplay[3] = _blendop_blendif_disp_alternative_log;
        bd->altdisplay[5] = _blendop_blendif_disp_alternative_log;
        bd->altdisplay[6] = _blendop_blendif_disp_alternative_log;
        break;
      default:
        assert(FALSE); // blendif not supported for RAW, already caught upstream; we should not get here
    }

    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *uplabel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *lowlabel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *upslider = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *lowslider = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    gtk_box_pack_start(GTK_BOX(section), dt_ui_section_label_new(_("parametric mask")), TRUE, TRUE, 0);
    GtkWidget *res = dtgtk_button_new(dtgtk_cairo_paint_reset, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(res, _("reset blend mask settings"));
    gtk_box_pack_end(GTK_BOX(section), GTK_WIDGET(res), FALSE, FALSE, 0);

    bd->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());

    for(int ch = 0; ch < maxchannels; ch++)
      dt_ui_notebook_page(bd->channel_tabs, labels[ch], tooltips[ch]);

    gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(bd->channel_tabs, bd->tab)));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(bd->channel_tabs), bd->tab);
    gtk_notebook_set_scrollable(bd->channel_tabs, TRUE);
    gtk_box_pack_start(GTK_BOX(header), GTK_WIDGET(bd->channel_tabs), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(header), gtk_grid_new(), TRUE, TRUE, 0);

    bd->colorpicker = dt_color_picker_new(module, DT_COLOR_PICKER_POINT_AREA, header);
    gtk_widget_set_tooltip_text(bd->colorpicker, _("pick GUI color from image\nctrl+click to select an area"));
    gtk_widget_set_name(bd->colorpicker, "keep-active");

    bd->colorpicker_set_values = dt_color_picker_new(module, DT_COLOR_PICKER_AREA, header);
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(bd->colorpicker_set_values),
                                 dtgtk_cairo_paint_colorpicker_set_values, 
                                 CPF_STYLE_FLAT | CPF_BG_TRANSPARENT, NULL);
    gtk_widget_set_tooltip_text(bd->colorpicker_set_values, _("set the range based on an area from the image\n"
                                                              "drag to use the input image\n"
                                                              "ctrl+drag to use the output image"));

    GtkWidget *inv = dtgtk_button_new(dtgtk_cairo_paint_invert, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(inv, _("invert all channel's polarities"));
    gtk_box_pack_end(GTK_BOX(header), GTK_WIDGET(inv), FALSE, FALSE, 0);

    bd->lower_slider = DTGTK_GRADIENT_SLIDER_MULTIVALUE(dtgtk_gradient_slider_multivalue_new_with_name(4, "blend-lower"));
    bd->upper_slider = DTGTK_GRADIENT_SLIDER_MULTIVALUE(dtgtk_gradient_slider_multivalue_new_with_name(4, "blend-upper"));

    bd->lower_polarity
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_plusminus, CPF_STYLE_FLAT | CPF_BG_TRANSPARENT | CPF_IGNORE_FG_STATE, NULL);
    gtk_widget_set_tooltip_text(bd->lower_polarity, _("toggle polarity. best seen by enabling 'display mask'"));

    bd->upper_polarity
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_plusminus, CPF_STYLE_FLAT | CPF_BG_TRANSPARENT | CPF_IGNORE_FG_STATE, NULL);
    gtk_widget_set_tooltip_text(bd->upper_polarity, _("toggle polarity. best seen by enabling 'display mask'"));

    gtk_box_pack_start(GTK_BOX(upslider), GTK_WIDGET(bd->upper_slider), TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(upslider), GTK_WIDGET(bd->upper_polarity), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lowslider), GTK_WIDGET(bd->lower_slider), TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(lowslider), GTK_WIDGET(bd->lower_polarity), FALSE, FALSE, 0);

    bd->upper_head = GTK_LABEL(gtk_label_new(_("output")));
    gtk_label_set_ellipsize(GTK_LABEL(bd->upper_head), PANGO_ELLIPSIZE_END);
    bd->upper_picker_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_ellipsize(GTK_LABEL(bd->upper_picker_label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(uplabel), GTK_WIDGET(bd->upper_head), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(uplabel), GTK_WIDGET(bd->upper_picker_label), TRUE, TRUE, 0);

    for(int k = 0; k < 4; k++)
    {
      bd->upper_label[k] = GTK_LABEL(gtk_label_new(NULL));
      gtk_label_set_ellipsize(GTK_LABEL(bd->upper_label[k]), PANGO_ELLIPSIZE_END);
      gtk_box_pack_start(GTK_BOX(uplabel), GTK_WIDGET(bd->upper_label[k]), FALSE, FALSE, 0);
    }

    bd->lower_head = GTK_LABEL(gtk_label_new(_("input")));
    gtk_label_set_ellipsize(GTK_LABEL(bd->lower_head), PANGO_ELLIPSIZE_END);
    bd->lower_picker_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_ellipsize(GTK_LABEL(bd->lower_picker_label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(lowlabel), GTK_WIDGET(bd->lower_head), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lowlabel), GTK_WIDGET(bd->lower_picker_label), TRUE, TRUE, 0);

    for(int k = 0; k < 4; k++)
    {
      bd->lower_label[k] = GTK_LABEL(gtk_label_new(NULL));
      gtk_label_set_ellipsize(GTK_LABEL(bd->lower_label[k]), PANGO_ELLIPSIZE_END);
      gtk_box_pack_start(GTK_BOX(lowlabel), GTK_WIDGET(bd->lower_label[k]), FALSE, FALSE, 0);
    }

    gtk_widget_set_tooltip_text(GTK_WIDGET(bd->lower_slider),
      _("double click to reset. to toggle: press 'a' for slider.\npress 'c' for channel. press 'm' for mask view."));
    gtk_widget_set_tooltip_text(GTK_WIDGET(bd->upper_slider),
      _("double click to reset. to toggle: press 'a' for slider.\npress 'c' for channel. press 'm' for mask view."));
    gtk_widget_set_tooltip_text(GTK_WIDGET(bd->lower_head), ttinput);
    gtk_widget_set_tooltip_text(GTK_WIDGET(bd->upper_head), ttoutput);

    g_signal_connect(G_OBJECT(bd->channel_tabs), "switch_page", G_CALLBACK(_blendop_blendif_tab_switch), bd);
    g_signal_connect(G_OBJECT(bd->upper_slider), "value-changed", G_CALLBACK(_blendop_blendif_sliders_callback), bd);
    g_signal_connect(G_OBJECT(bd->lower_slider), "value-changed", G_CALLBACK(_blendop_blendif_sliders_callback), bd);
    g_signal_connect(G_OBJECT(bd->lower_slider), "leave-notify-event", G_CALLBACK(_blendop_blendif_leave), module);
    g_signal_connect(G_OBJECT(bd->upper_slider), "leave-notify-event", G_CALLBACK(_blendop_blendif_leave), module);
    g_signal_connect(G_OBJECT(bd->lower_slider), "enter-notify-event", G_CALLBACK(_blendop_blendif_enter), module);
    g_signal_connect(G_OBJECT(bd->upper_slider), "enter-notify-event", G_CALLBACK(_blendop_blendif_enter), module);
    g_signal_connect(G_OBJECT(bd->lower_slider), "key-press-event", G_CALLBACK(_blendop_blendif_key_press), module);
    g_signal_connect(G_OBJECT(bd->upper_slider), "key-press-event", G_CALLBACK(_blendop_blendif_key_press), module);
    g_signal_connect(G_OBJECT(bd->colorpicker), "toggled", G_CALLBACK(_update_gradient_slider_pickers), module);
    g_signal_connect(G_OBJECT(bd->colorpicker_set_values), "toggled", G_CALLBACK(_update_gradient_slider_pickers), module);
    g_signal_connect(G_OBJECT(res), "clicked", G_CALLBACK(_blendop_blendif_reset), module);
    g_signal_connect(G_OBJECT(inv), "clicked", G_CALLBACK(_blendop_blendif_invert), module);
    g_signal_connect(G_OBJECT(bd->lower_polarity), "toggled", G_CALLBACK(_blendop_blendif_polarity_callback), bd);
    g_signal_connect(G_OBJECT(bd->upper_polarity), "toggled", G_CALLBACK(_blendop_blendif_polarity_callback), bd);

    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(section), TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(header), TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(uplabel), TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(upslider), TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(lowlabel), TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(lowslider), TRUE, FALSE, 0);

    bd->blendif_inited = 1;
  }

  gtk_container_add(GTK_CONTAINER(event_box), GTK_WIDGET(bd->blendif_box));
}

void dt_iop_gui_update_masks(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  dt_develop_blend_params_t *bp = module->blend_params;

  if(!bd || !bd->masks_support || !bd->masks_inited) return;

  ++darktable.gui->reset;
  /* update masks state */
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, module->blend_params->mask_id);
  dt_bauhaus_combobox_clear(bd->masks_combo);

  if(grp && (grp->type & DT_MASKS_GROUP) && g_list_length(grp->points) > 0)
  {
    char txt[512];
    const guint n = g_list_length(grp->points);
    snprintf(txt, sizeof(txt), ngettext("%d shape used", "%d shapes used", n), n);
    dt_bauhaus_combobox_add(bd->masks_combo, txt);
  }
  else
  {
    dt_bauhaus_combobox_add(bd->masks_combo, _("no mask used"));
    bd->masks_shown = DT_MASKS_EDIT_OFF;
    // reset the gui
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
  }

  dt_bauhaus_combobox_set(bd->masks_combo, 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), bd->masks_shown != DT_MASKS_EDIT_OFF);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_polarity),
                               bp->mask_combine & DEVELOP_COMBINE_MASKS_POS);
  // update buttons status
  for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
  {
    if(module->dev->form_gui && module->dev->form_visible && module->dev->form_gui->creation
         && module->dev->form_gui->creation_module == module
         && module->dev->form_visible->type & bd->masks_type[n])
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), TRUE);
    else
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), FALSE);
  }

  --darktable.gui->reset;
}

void dt_iop_gui_init_masks(GtkBox *blendw, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  bd->masks_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  // add event box so that one can click into the area to get help for drawn masks
  GtkWidget* event_box = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(blendw), event_box);

  /* create and add masks support if module supports it */
  if(bd->masks_support)
  {
    bd->masks_combo_ids = NULL;
    bd->masks_shown = DT_MASKS_EDIT_OFF;

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *abox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    bd->masks_combo = dt_bauhaus_combobox_new(module);
    dt_bauhaus_widget_set_label(bd->masks_combo, _("blend"), _("drawn mask"));
    dt_bauhaus_combobox_add(bd->masks_combo, _("no mask used"));
    dt_bauhaus_combobox_set(bd->masks_combo, 0);
    g_signal_connect(G_OBJECT(bd->masks_combo), "value-changed",
                     G_CALLBACK(dt_masks_iop_value_changed_callback), module);
    dt_bauhaus_combobox_add_populate_fct(bd->masks_combo, dt_masks_iop_combo_populate);
    gtk_box_pack_start(GTK_BOX(hbox), bd->masks_combo, TRUE, TRUE, 0);

    bd->masks_edit
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_eye, CPF_STYLE_FLAT, NULL);
    g_signal_connect(G_OBJECT(bd->masks_edit), "button-press-event", G_CALLBACK(_blendop_masks_show_and_edit),
                     module);
    gtk_widget_set_tooltip_text(bd->masks_edit, _("show and edit mask elements"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), FALSE);
    gtk_box_pack_start(GTK_BOX(hbox), bd->masks_edit, FALSE, FALSE, 0);

    bd->masks_polarity
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_plusminus, CPF_STYLE_FLAT | CPF_BG_TRANSPARENT | CPF_IGNORE_FG_STATE, NULL);
    gtk_widget_set_tooltip_text(bd->masks_polarity, _("toggle polarity of drawn mask"));
    g_signal_connect(G_OBJECT(bd->masks_polarity), "toggled", G_CALLBACK(_blendop_masks_polarity_callback),
                     module);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_polarity), FALSE);
    gtk_box_pack_start(GTK_BOX(hbox), bd->masks_polarity, FALSE, FALSE, 0);

    bd->masks_type[0] = DT_MASKS_GRADIENT;
    bd->masks_shapes[0]
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_gradient, CPF_STYLE_FLAT, NULL);
    g_signal_connect(G_OBJECT(bd->masks_shapes[0]), "button-press-event",
                     G_CALLBACK(_blendop_masks_add_shape_callback), module);
    gtk_widget_set_tooltip_text(bd->masks_shapes[0], _("add gradient\nctrl+click to add multiple gradients"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[0]), FALSE);
    gtk_box_pack_end(GTK_BOX(abox), bd->masks_shapes[0], FALSE, FALSE, 0);

    bd->masks_type[1] = DT_MASKS_PATH;
    bd->masks_shapes[1]
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_path, CPF_STYLE_FLAT, NULL);
    g_signal_connect(G_OBJECT(bd->masks_shapes[1]), "button-press-event",
                     G_CALLBACK(_blendop_masks_add_shape_callback), module);
    gtk_widget_set_tooltip_text(bd->masks_shapes[1], _("add path\nctrl+click to add multiple paths"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[1]), FALSE);
    gtk_box_pack_end(GTK_BOX(abox), bd->masks_shapes[1], FALSE, FALSE, 0);

    bd->masks_type[2] = DT_MASKS_ELLIPSE;
    bd->masks_shapes[2]
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_ellipse, CPF_STYLE_FLAT, NULL);
    g_signal_connect(G_OBJECT(bd->masks_shapes[2]), "button-press-event",
                     G_CALLBACK(_blendop_masks_add_shape_callback), module);
    gtk_widget_set_tooltip_text(bd->masks_shapes[2], _("add ellipse\nctrl+click to add multiple ellipses"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[2]), FALSE);
    gtk_box_pack_end(GTK_BOX(abox), bd->masks_shapes[2], FALSE, FALSE, 0);

    bd->masks_type[3] = DT_MASKS_CIRCLE;
    bd->masks_shapes[3]
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_circle, CPF_STYLE_FLAT, NULL);
    g_signal_connect(G_OBJECT(bd->masks_shapes[3]), "button-press-event",
                     G_CALLBACK(_blendop_masks_add_shape_callback), module);
    gtk_widget_set_tooltip_text(bd->masks_shapes[3], _("add circle\nctrl+click to add multiple circles"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[3]), FALSE);
    gtk_box_pack_end(GTK_BOX(abox), bd->masks_shapes[3], FALSE, FALSE, 0);

    bd->masks_type[4] = DT_MASKS_BRUSH;
    bd->masks_shapes[4]
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_brush, CPF_STYLE_FLAT, NULL);
    g_signal_connect(G_OBJECT(bd->masks_shapes[4]), "button-press-event",
                     G_CALLBACK(_blendop_masks_add_shape_callback), module);
    gtk_widget_set_tooltip_text(bd->masks_shapes[4], _("add brush\nctrl+click to add multiple brush strokes"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[4]), FALSE);
    gtk_box_pack_end(GTK_BOX(abox), bd->masks_shapes[4], FALSE, FALSE, 0);


    gtk_box_pack_start(GTK_BOX(bd->masks_box), dt_ui_section_label_new(_("drawn mask")), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->masks_box), GTK_WIDGET(hbox), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->masks_box), GTK_WIDGET(abox), TRUE, TRUE, 0);

    bd->masks_inited = 1;
  }
  gtk_container_add(GTK_CONTAINER(event_box), GTK_WIDGET(bd->masks_box));
}

typedef struct raster_combo_entry_t
{
  dt_iop_module_t *module;
  int id;
} raster_combo_entry_t;

static void _raster_combo_populate(GtkWidget *w, struct dt_iop_module_t **m)
{
  dt_iop_module_t *module = *m;
  dt_iop_request_focus(module);
  dt_bauhaus_combobox_clear(w);

  raster_combo_entry_t *entry = (raster_combo_entry_t *)malloc(sizeof(raster_combo_entry_t));
  entry->module = NULL;
  entry->id = 0;
  dt_bauhaus_combobox_add_full(w, _("no mask used"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, entry, free, TRUE);
  int i = 1;

  for(GList* iter = darktable.develop->iop; iter; iter = g_list_next(iter))
  {
    dt_iop_module_t *iop = (dt_iop_module_t *)iter->data;

    if(iop == module)
      break;

    GHashTableIter masks_iter;
    gpointer key, value;
    g_hash_table_iter_init(&masks_iter, iop->raster_mask.source.masks);

    while(g_hash_table_iter_next(&masks_iter, &key, &value))
    {
      const int id = GPOINTER_TO_INT(key);
      const char *modulename = (char *)value;
      entry = (raster_combo_entry_t *)malloc(sizeof(raster_combo_entry_t));
      entry->module = iop;
      entry->id = id;
      dt_bauhaus_combobox_add_full(w, modulename, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, entry, free, TRUE);

      if(iop == module->raster_mask.sink.source && module->raster_mask.sink.id == id)
        dt_bauhaus_combobox_set(w, i);

      i++;
    }
  }
}

static void _raster_value_changed_callback(GtkWidget *widget, struct dt_iop_module_t *module)
{
  raster_combo_entry_t *entry = dt_bauhaus_combobox_get_data(widget);
  // nothing to do
  if(entry->module == module->raster_mask.sink.source && entry->id == module->raster_mask.sink.id)
    return;

  if(module->raster_mask.sink.source)
    // we no longer use this one
    g_hash_table_remove(module->raster_mask.sink.source->raster_mask.source.users, module);

  module->raster_mask.sink.source = entry->module;
  module->raster_mask.sink.id = entry->id;
  gboolean reprocess = FALSE;

  if(entry->module)
  {
    reprocess = dt_iop_is_raster_mask_used(entry->module, 0) == FALSE;
    g_hash_table_add(entry->module->raster_mask.source.users, module);
    // update blend_params!
    memcpy(module->blend_params->raster_mask_source, entry->module->op, sizeof(module->blend_params->raster_mask_source));
    module->blend_params->raster_mask_instance = entry->module->multi_priority;
    module->blend_params->raster_mask_id = entry->id;
  }
  else
  {
    memset(module->blend_params->raster_mask_source, 0, sizeof(module->blend_params->raster_mask_source));
    module->blend_params->raster_mask_instance = 0;
    module->blend_params->raster_mask_id = 0;
  }

  dt_dev_add_history_item(module->dev, module, TRUE);

  if(reprocess)
    dt_dev_reprocess_all(module->dev);
}

void dt_iop_gui_update_raster(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  dt_develop_blend_params_t *bp = module->blend_params;

  if(!bd || !bd->masks_support || !bd->raster_inited) return;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->raster_polarity), bp->raster_mask_invert);
  _raster_combo_populate(bd->raster_combo, &module);
}

static void _raster_polarity_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_develop_blend_params_t *bp = (dt_develop_blend_params_t *)self->blend_params;
  bp->raster_mask_invert = gtk_toggle_button_get_active(togglebutton);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  dt_control_queue_redraw_widget(GTK_WIDGET(togglebutton));
}

void dt_iop_gui_init_raster(GtkBox *blendw, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  bd->raster_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  // add event box so that one can click into the area to get help for drawn masks
  GtkWidget* event_box = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(blendw), event_box);

  /* create and add raster support if module supports it (it's coupled to masks at the moment) */
  if(bd->masks_support)
  {
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    bd->raster_combo = dt_bauhaus_combobox_new(module);
    dt_bauhaus_widget_set_label(bd->raster_combo, _("blend"), _("raster mask"));
    dt_bauhaus_combobox_add(bd->raster_combo, _("no mask used"));
    dt_bauhaus_combobox_set(bd->raster_combo, 0);
    g_signal_connect(G_OBJECT(bd->raster_combo), "value-changed",
                     G_CALLBACK(_raster_value_changed_callback), module);
    dt_bauhaus_combobox_add_populate_fct(bd->raster_combo, _raster_combo_populate);
    gtk_box_pack_start(GTK_BOX(hbox), bd->raster_combo, TRUE, TRUE, 0);

    bd->raster_polarity = dtgtk_togglebutton_new(dtgtk_cairo_paint_plusminus, CPF_STYLE_FLAT | CPF_BG_TRANSPARENT
                                                 | CPF_IGNORE_FG_STATE, NULL);
    gtk_widget_set_tooltip_text(bd->raster_polarity, _("toggle polarity of raster mask"));
    g_signal_connect(G_OBJECT(bd->raster_polarity), "toggled", G_CALLBACK(_raster_polarity_callback), module);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->raster_polarity), FALSE);
    gtk_box_pack_start(GTK_BOX(hbox), bd->raster_polarity, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bd->raster_box), GTK_WIDGET(hbox), TRUE, TRUE, 0);

    bd->raster_inited = 1;
  }
  gtk_container_add(GTK_CONTAINER(event_box), GTK_WIDGET(bd->raster_box));
}

void dt_iop_gui_cleanup_blending(dt_iop_module_t *module)
{
  if(!module->blend_data) return;
  
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  dt_pthread_mutex_lock(&bd->lock);

  if(bd->timeout_handle)
    g_source_remove(bd->timeout_handle);

  g_list_free(bd->masks_modes);
  g_list_free(bd->masks_modes_toggles);
  free(bd->masks_combo_ids);
  dt_pthread_mutex_unlock(&bd->lock);
  dt_pthread_mutex_destroy(&bd->lock);

  g_free(module->blend_data);
  module->blend_data = NULL;
}


static gboolean _add_blendmode_combo(GtkWidget *combobox, dt_develop_blend_mode_t mode)
{
  for(const dt_develop_name_value_t *bm = dt_develop_blend_mode_names; *bm->name; bm++)
    if(bm->value == mode)
    {
      dt_bauhaus_combobox_add_full(combobox, g_dpgettext2(NULL, "blendmode", bm->name),
                                   DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GUINT_TO_POINTER(bm->value), NULL, TRUE);
      return TRUE;
    }

  return FALSE;
}

static GtkWidget *_combobox_new_from_list(dt_iop_module_t *module, const gchar *label, 
                                          const dt_develop_name_value_t *list, const gchar *tooltip)
{
  GtkWidget *combo = dt_bauhaus_combobox_new(module);
  dt_bauhaus_widget_set_label(combo, _("blend"), _(label));
  gtk_widget_set_tooltip_text(combo, _(tooltip));

  for(; *list->name; list++)
    dt_bauhaus_combobox_add_full(combo, _(list->name), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, 
                                 GUINT_TO_POINTER(list->value), NULL, TRUE);

  return combo;
}

void dt_iop_gui_update_blending(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  if(!(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) || !bd || !bd->blend_inited) return;

  ++darktable.gui->reset;
  const unsigned int mode = g_list_index(bd->masks_modes, GUINT_TO_POINTER(module->blend_params->mask_mode));
  // unsets currently toggled if any, won't try to untoggle the cancel button
  if(bd->selected_mask_mode
     != g_list_nth_data(bd->masks_modes_toggles,
                        g_list_index(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_DISABLED))))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->selected_mask_mode), FALSE);

  if(mode > 0)
  {
    GtkToggleButton *to_be_activated = GTK_TOGGLE_BUTTON(g_list_nth_data(bd->masks_modes_toggles, mode));
    gtk_toggle_button_set_active(to_be_activated, TRUE);
    bd->selected_mask_mode = GTK_WIDGET(to_be_activated);
  }
  else
    bd->selected_mask_mode = g_list_nth_data(
        bd->masks_modes_toggles, g_list_index(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_DISABLED)));

  if(!dt_bauhaus_combobox_set_from_value(bd->blend_modes_combo, module->blend_params->blend_mode))
  {
    // add deprecated blend mode
    if(!_add_blendmode_combo(bd->blend_modes_combo, module->blend_params->blend_mode))
    {
      // should never happen: unknown blend mode 
      dt_control_log("unknown blend mode '%d' in module '%s'", module->blend_params->blend_mode, module->op);
      module->blend_params->blend_mode = DEVELOP_BLEND_NORMAL2;
    }

    dt_bauhaus_combobox_set_from_value(bd->blend_modes_combo, module->blend_params->blend_mode);
  }

  dt_bauhaus_combobox_set_from_value(bd->masks_combine_combo, 
                                     module->blend_params->mask_combine & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL));
  dt_bauhaus_combobox_set_from_value(bd->masks_invert_combo, 
                                     module->blend_params->mask_combine & DEVELOP_COMBINE_INV);
  dt_bauhaus_slider_set(bd->opacity_slider, module->blend_params->opacity);
  dt_bauhaus_combobox_set_from_value(bd->masks_feathering_guide_combo,
                                     module->blend_params->feathering_guide);
  dt_bauhaus_slider_set(bd->feathering_radius_slider, module->blend_params->feathering_radius);
  dt_bauhaus_slider_set(bd->blur_radius_slider, module->blend_params->blur_radius);
  dt_bauhaus_slider_set(bd->brightness_slider, module->blend_params->brightness);
  dt_bauhaus_slider_set(bd->contrast_slider, module->blend_params->contrast);

  /* reset all alternative display modes for blendif */
  memset(bd->altmode, 0, sizeof(bd->altmode));
  dt_iop_gui_update_blendif(module);
  dt_iop_gui_update_masks(module);
  dt_iop_gui_update_raster(module);
  /* now show hide controls as required */
  const unsigned int mask_mode = module->blend_params->mask_mode;

  if(mask_mode & DEVELOP_MASK_ENABLED)
    gtk_widget_show(GTK_WIDGET(bd->top_box));
  else
    gtk_widget_hide(GTK_WIDGET(bd->top_box));

  if((mask_mode & DEVELOP_MASK_ENABLED) && ((bd->masks_inited && (mask_mode & DEVELOP_MASK_MASK))
                                            || (bd->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))))
  {
    if(bd->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))
    {
      gtk_widget_hide(GTK_WIDGET(bd->masks_invert_combo));
      gtk_widget_show(GTK_WIDGET(bd->masks_combine_combo));
    }
    else
    {
      gtk_widget_show(GTK_WIDGET(bd->masks_invert_combo));
      gtk_widget_hide(GTK_WIDGET(bd->masks_combine_combo));
    }
    // if this iop is operating in raw space, no alpha channel
    // TODO: revisit if/once there semi-raw iops (e.g temperature) with blending
    if(module->blend_colorspace(module, NULL, NULL) == iop_cs_RAW)
    {
      module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
      dtgtk_button_set_active(DTGTK_BUTTON(bd->showmask), FALSE);
      gtk_widget_hide(GTK_WIDGET(bd->showmask));
    }
    else
      gtk_widget_show(GTK_WIDGET(bd->showmask));

    gtk_widget_show(GTK_WIDGET(bd->bottom_box));
  }
  else
  {
    module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    dtgtk_button_set_active(DTGTK_BUTTON(bd->showmask), FALSE);
    module->suppress_mask = 0;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->suppress), FALSE);

    gtk_widget_hide(GTK_WIDGET(bd->bottom_box));
  }

  if(bd->masks_inited && (mask_mode & DEVELOP_MASK_MASK))
    gtk_widget_show(GTK_WIDGET(bd->masks_box));
  else if(bd->masks_inited)
  {
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
    gtk_widget_hide(GTK_WIDGET(bd->masks_box));
  }
  else
    gtk_widget_hide(GTK_WIDGET(bd->masks_box));

  if(bd->raster_inited && (mask_mode & DEVELOP_MASK_RASTER))
    gtk_widget_show(GTK_WIDGET(bd->raster_box));
  else if(bd->raster_inited)
    gtk_widget_hide(GTK_WIDGET(bd->raster_box));
  else
    gtk_widget_hide(GTK_WIDGET(bd->raster_box));

  if(bd->blendif_inited && (mask_mode & DEVELOP_MASK_CONDITIONAL))
    gtk_widget_show(GTK_WIDGET(bd->blendif_box));
  else if(bd->blendif_inited)
  {
    /* switch off color picker */
    dt_iop_color_picker_reset(module, FALSE);
    gtk_widget_hide(GTK_WIDGET(bd->blendif_box));
  }
  else
    gtk_widget_hide(GTK_WIDGET(bd->blendif_box));

  if(module->hide_enable_button)
    gtk_widget_hide(GTK_WIDGET(bd->masks_modes_box));
  else
    gtk_widget_show(GTK_WIDGET(bd->masks_modes_box));

  --darktable.gui->reset;
}

void dt_iop_gui_blending_lose_focus(dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  if(!module) return;

  const int has_mask_display = module->request_mask_display & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
  const int suppress = module->suppress_mask;

  if((module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && module->blend_data)
  {
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
    dtgtk_button_set_active(DTGTK_BUTTON(bd->showmask), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->suppress), FALSE);
    module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    module->suppress_mask = 0;
    dt_pthread_mutex_lock(&bd->lock);
    bd->save_for_leave = DT_DEV_PIXELPIPE_DISPLAY_NONE;

    if(bd->timeout_handle)
    {
      // purge any remaining timeout handlers
      g_source_remove(bd->timeout_handle);
      bd->timeout_handle = 0;
    }

    dt_pthread_mutex_unlock(&bd->lock);
    // reprocess main center image if needed
    if (has_mask_display || suppress)
        dt_iop_refresh_center(module);                                    
  }
}

void dt_iop_gui_init_blending(GtkWidget *iopw, dt_iop_module_t *module)
{
  /* create and add blend mode if module supports it */
  if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
  {
    module->blend_data = g_malloc0(sizeof(dt_iop_gui_blend_data_t));
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

    bd->iopw = iopw;
    bd->module = module;
    bd->csp = module->blend_colorspace(module, NULL, NULL);
    bd->blendif_support = (bd->csp == iop_cs_Lab || bd->csp == iop_cs_rgb);
    bd->masks_support = !(module->flags() & IOP_FLAGS_NO_MASKS);

    bd->masks_modes = NULL;
    bd->masks_modes_toggles = NULL;
    dt_pthread_mutex_init(&bd->lock, NULL);
    dt_pthread_mutex_lock(&bd->lock);
    bd->timeout_handle = 0;
    bd->save_for_leave = 0;
    dt_pthread_mutex_unlock(&bd->lock);
    //toggle buttons creation for masks modes
    GtkWidget *but = NULL;
    // DEVELOP_MASK_DISABLED
    but = dtgtk_button_new(dtgtk_cairo_paint_cancel, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(but, _("off"));
    bd->masks_modes = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_DISABLED));
    bd->masks_modes_toggles = g_list_append(bd->masks_modes_toggles , GTK_WIDGET(but));
    g_signal_connect(G_OBJECT(but), "button-press-event", G_CALLBACK(_blendop_masks_modes_none_clicked), module);
    // DEVELOP_MASK_ENABLED
    but = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_uniform, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(but, _("uniformly"));
    bd->masks_modes = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_ENABLED));
    bd->masks_modes_toggles  = g_list_append(bd->masks_modes_toggles , GTK_WIDGET(but));
    g_signal_connect(G_OBJECT(but), "toggled", G_CALLBACK(_blendop_masks_modes_uni_toggled), module);

    if(bd->masks_support) //DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK
    {
      but = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_drawn, CPF_STYLE_FLAT, NULL);
      gtk_widget_set_tooltip_text(but, _("drawn mask"));
      bd->masks_modes = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK));
      bd->masks_modes_toggles = g_list_append(bd->masks_modes_toggles, GTK_WIDGET(but));
      g_signal_connect(G_OBJECT(but), "toggled", G_CALLBACK(_blendop_masks_modes_drawn_toggled), module);
    }

    if(bd->blendif_support) //DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL
    {
      but = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_parametric, CPF_STYLE_FLAT, NULL);
      gtk_widget_set_tooltip_text(but, _("parametric mask"));
      bd->masks_modes
          = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL));
      bd->masks_modes_toggles = g_list_append(bd->masks_modes_toggles, GTK_WIDGET(but));
      g_signal_connect(G_OBJECT(but), "toggled", G_CALLBACK(_blendop_masks_modes_param_toggled), module);
    }

    if(bd->blendif_support && bd->masks_support) //DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL
    {
      but = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_drawn_and_parametric,
                                   CPF_STYLE_FLAT, NULL); // overlays and
      gtk_widget_set_tooltip_text(but, _("drawn & parametric mask"));
      bd->masks_modes
          = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL));
      bd->masks_modes_toggles = g_list_append(bd->masks_modes_toggles, GTK_WIDGET(but));
      g_signal_connect(G_OBJECT(but), "toggled", G_CALLBACK(_blendop_masks_modes_both_toggled), module);
    }

    if(bd->masks_support) //DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER
    {
      but = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_raster, CPF_STYLE_FLAT, NULL);
      gtk_widget_set_tooltip_text(but, _("raster mask"));
      bd->masks_modes
          = g_list_append(bd->masks_modes, GUINT_TO_POINTER(DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER));
      bd->masks_modes_toggles = g_list_append(bd->masks_modes_toggles, GTK_WIDGET(but));
      g_signal_connect(G_OBJECT(but), "toggled", G_CALLBACK(_blendop_masks_modes_raster_toggled), module);
    }
    // initial state is no mask
    bd->selected_mask_mode = GTK_WIDGET(
        g_list_nth_data(bd->masks_modes_toggles,
                        g_list_index(bd->masks_modes, (gconstpointer)DEVELOP_MASK_DISABLED)));

    bd->blend_modes_combo = dt_bauhaus_combobox_new(module);
    dt_bauhaus_widget_set_label(bd->blend_modes_combo, _("blend"), _("blend mode"));
    gtk_widget_set_tooltip_text(bd->blend_modes_combo, _("choose blending mode"));

    if(bd->csp == iop_cs_Lab ||
       bd->csp == iop_cs_rgb ||
       bd->csp == iop_cs_RAW )
    {
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("normal & difference modes"));
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_NORMAL2);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_AVERAGE);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_DIFFERENCE2);
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("lighten modes"));
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_ADD);
      dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("darken modes"));
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_SUBSTRACT);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_MULTIPLY);

      if(bd->csp == iop_cs_Lab)
      {
        dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("color channel modes"));
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LAB_A);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LAB_B);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LAB_COLOR);
      }
      else if(bd->csp == iop_cs_rgb)
      {
        dt_bauhaus_combobox_add_section(bd->blend_modes_combo, _("color channel modes"));
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_RGB_R);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_RGB_G);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_RGB_B);
      }
      else if(bd->csp == iop_cs_RAW)
      {
      }
    }
    else if(bd->csp == iop_cs_LCh ||
            bd->csp == iop_cs_NONE )
    {
    }

    g_signal_connect(G_OBJECT(bd->blend_modes_combo), "value-changed",
                     G_CALLBACK(dt_iop_combobox_enum_callback), &module->blend_params->blend_mode);

    bd->opacity_slider = dt_bauhaus_slider_new_with_range(module, 0.0, 100.0, 1, 100.0, 0);
    dt_bauhaus_widget_set_label(bd->opacity_slider, _("blend"), _("opacity"));
    dt_bauhaus_slider_set_format(bd->opacity_slider, "%.0f%%");
    module->fusion_slider = bd->opacity_slider;
    gtk_widget_set_tooltip_text(bd->opacity_slider, _("set the opacity of the blending"));
    g_signal_connect(G_OBJECT(bd->opacity_slider), "value-changed", G_CALLBACK(dt_iop_slider_float_callback), &module->blend_params->opacity);

    bd->masks_combine_combo = _combobox_new_from_list(module, "combine masks", dt_develop_combine_masks_names, 
                                                      "how to combine individual drawn mask and different channels of parametric mask");
    g_signal_connect(G_OBJECT(bd->masks_combine_combo), "value-changed",
                     G_CALLBACK(_blendop_masks_combine_callback), bd);

    bd->masks_invert_combo = _combobox_new_from_list(module, "invert mask", dt_develop_invert_mask_names,
                                                     "apply mask in normal or inverted mode");
    g_signal_connect(G_OBJECT(bd->masks_invert_combo), "value-changed",
                     G_CALLBACK(_blendop_masks_invert_callback), bd);

    bd->masks_feathering_guide_combo = _combobox_new_from_list(module, "feathering guide", dt_develop_feathering_guide_names,
                                                               "choose to guide mask by input or output image");
    g_signal_connect(G_OBJECT(bd->masks_feathering_guide_combo), "value-changed",
                     G_CALLBACK(dt_iop_combobox_enum_callback), &module->blend_params->feathering_guide);

    bd->feathering_radius_slider = dt_bauhaus_slider_new_with_range(module, 0.0, 250.0, 0.1, 0.0, 1);
    dt_bauhaus_widget_set_label(bd->feathering_radius_slider, _("blend"), _("feathering radius"));
    dt_bauhaus_slider_set_format(bd->feathering_radius_slider, "%.1f");
    gtk_widget_set_tooltip_text(bd->feathering_radius_slider, _("spatial radius of feathering"));
    g_signal_connect(G_OBJECT(bd->feathering_radius_slider), "value-changed",
                     G_CALLBACK(dt_iop_slider_float_callback), &module->blend_params->feathering_radius);

    bd->blur_radius_slider = dt_bauhaus_slider_new_with_range(module, 0.0, 100.0, 0.1, 0.0, 1);
    dt_bauhaus_widget_set_label(bd->blur_radius_slider, _("blend"), _("mask blur"));
    dt_bauhaus_slider_set_format(bd->blur_radius_slider, "%.1f");
    gtk_widget_set_tooltip_text(bd->blur_radius_slider, _("radius for gaussian blur of blend mask"));
    g_signal_connect(G_OBJECT(bd->blur_radius_slider), "value-changed",
                     G_CALLBACK(dt_iop_slider_float_callback), &module->blend_params->blur_radius);

    bd->brightness_slider = dt_bauhaus_slider_new_with_range(module, -1.0, 1.0, 0.01, 0.0, 2);
    dt_bauhaus_widget_set_label(bd->brightness_slider, _("blend"), _("mask opacity"));
    dt_bauhaus_slider_set_format(bd->brightness_slider, "%.2f");
    gtk_widget_set_tooltip_text(bd->brightness_slider, _("shifts and tilts the tone curve of the blend mask to adjust its "
                                                         "brightness without affecting fully transparent/fully opaque "
                                                         "regions"));
    g_signal_connect(G_OBJECT(bd->brightness_slider), "value-changed",
                     G_CALLBACK(dt_iop_slider_float_callback), &module->blend_params->brightness);

    bd->contrast_slider = dt_bauhaus_slider_new_with_range(module, -1.0, 1.0, 0.01, 0.0, 2);
    dt_bauhaus_widget_set_label(bd->contrast_slider, _("blend"), _("mask contrast"));
    dt_bauhaus_slider_set_format(bd->contrast_slider, "%.2f");
    gtk_widget_set_tooltip_text(bd->contrast_slider, _("gives the tone curve of the blend mask an s-like shape to "
                                                       "adjust its contrast"));
    g_signal_connect(G_OBJECT(bd->contrast_slider), "value-changed",
                     G_CALLBACK(dt_iop_slider_float_callback), &module->blend_params->contrast);

    bd->showmask = dtgtk_button_new(dtgtk_cairo_paint_showmask, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(bd->showmask, _("display mask and/or color channel. ctrl+click to display mask, "
                                                "shift+click to display channel. hover over parametric mask slider to "
                                                "select channel for display"));
    g_signal_connect(G_OBJECT(bd->showmask), "button-press-event", G_CALLBACK(_blendop_blendif_showmask_clicked), module);
    gtk_widget_set_name(bd->showmask, "show_mask_button");

    bd->suppress
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_eye_toggle, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(bd->suppress, _("temporarily switch off blend mask. only for module in focus"));
    g_signal_connect(G_OBJECT(bd->suppress), "toggled", G_CALLBACK(_blendop_blendif_suppress_toggled), module);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(iopw), GTK_WIDGET(box), TRUE, TRUE, 0);

    //box enclosing the mask mode selection buttons
    bd->masks_modes_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
    //mask selection buttons packing in mask_box
    for (int i = 0; i < g_list_length(bd->masks_modes_toggles); i++)
      gtk_box_pack_start(GTK_BOX(bd->masks_modes_box), GTK_WIDGET(g_list_nth_data(bd->masks_modes_toggles, i)), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(bd->masks_modes_box), FALSE, FALSE, 0);
    gtk_widget_set_name(GTK_WIDGET(bd->masks_modes_box), "blending-tabs");

    bd->top_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_box_pack_start(GTK_BOX(bd->top_box), bd->blend_modes_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->top_box), bd->opacity_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(bd->top_box), TRUE, TRUE, 0);

    dt_iop_gui_init_masks(GTK_BOX(iopw), module);
    dt_iop_gui_init_raster(GTK_BOX(iopw), module);
    dt_iop_gui_init_blendif(GTK_BOX(iopw), module);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_end(GTK_BOX(hbox), GTK_WIDGET(bd->showmask), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), GTK_WIDGET(bd->suppress), FALSE, FALSE, 0);
    bd->bottom_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), GTK_WIDGET(bd->masks_combine_combo), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), GTK_WIDGET(bd->masks_invert_combo), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), dt_ui_section_label_new(_("mask refinement")), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->masks_feathering_guide_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->feathering_radius_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->blur_radius_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->brightness_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), bd->contrast_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bd->bottom_box), hbox, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(iopw), GTK_WIDGET(bd->bottom_box), TRUE, TRUE, 0);

    gtk_widget_set_name(GTK_WIDGET(bd->top_box), "blending-box");
    gtk_widget_set_name(GTK_WIDGET(bd->masks_box), "blending-box");
    gtk_widget_set_name(GTK_WIDGET(bd->bottom_box), "blending-box");
    gtk_widget_set_name(GTK_WIDGET(bd->raster_box), "blending-box");
    gtk_widget_set_name(GTK_WIDGET(bd->blendif_box), "blending-box");
    gtk_widget_set_name(GTK_WIDGET(iopw), "blending-wrapper");

    bd->blend_inited = 1;
    gtk_widget_queue_draw(GTK_WIDGET(iopw));
    dt_iop_gui_update_blending(module);

  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
