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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/debug.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(1, dt_iop_channelmixer_params_t)

typedef enum _channelmixer_output_t
{
  /** mixes into hue channel */
  CHANNEL_HUE = 0,
  /** mixes into lightness channel */
  CHANNEL_SATURATION,
  /** mixes into lightness channel */
  CHANNEL_LIGHTNESS,
  /** mixes into red channel of image */
  CHANNEL_RED,
  /** mixes into green channel of image */
  CHANNEL_GREEN,
  /** mixes into blue channel of image */
  CHANNEL_BLUE,
  /** mixes into gray channel of image = monochrome*/
  CHANNEL_GRAY,

  CHANNEL_SIZE
} _channelmixer_output_t;

#define CHANNEL_OFFSET CHANNEL_RED

typedef struct dt_iop_channelmixer_params_t
{
  //_channelmixer_output_t output_channel;
  /** amount of red to mix value */
  float red[CHANNEL_SIZE]; 
  /** amount of green to mix value */
  float green[CHANNEL_SIZE];
  /** amount of blue to mix value */
  float blue[CHANNEL_SIZE];
} dt_iop_channelmixer_params_t;

typedef struct dt_iop_channelmixer_gui_data_t
{
  GtkBox *vbox;
  GtkWidget *output_channel;                          // Output channel
  GtkWidget *scale_red, *scale_green, *scale_blue;    // red, green, blue
  GtkWidget *normalise;              // normalise inputs for greymix to sum to 1.0
} dt_iop_channelmixer_gui_data_t;

typedef struct dt_iop_channelmixer_data_t
{
  float red[CHANNEL_SIZE];
  float green[CHANNEL_SIZE];
  float blue[CHANNEL_SIZE];
  //_channelmixer_output_t output_channel;
} dt_iop_channelmixer_data_t;

typedef struct dt_iop_channelmixer_global_data_t
{
  int kernel_channelmixer;
} dt_iop_channelmixer_global_data_t;

const char *name()
{
  return _("channel mixer");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

static int dt_channel_changer()
{
  //int ret = 4;
  //if(strcmp(dt_conf_get_string("darkroom/ui/gray_channels"), "experimental (1)") == 0)
  //  ret = 1;
  return 1;
}

const int which_channel(const dt_iop_channelmixer_params_t *p)
{
  const gboolean do_gray = p->red[CHANNEL_GRAY]
                           || p->green[CHANNEL_GRAY] || p->blue[CHANNEL_GRAY];
  return do_gray ? CHANNEL_GRAY : CHANNEL_RED;
}

inline static void matrix3k(const float *in, float *out, const float *variable,
                            const int row, const int coln, const int deficit)
{
  for(int out_i = 0; out_i < row; out_i++)
  {
    float *ptr = out_i + out;
    *ptr = 0.0f;
    for(int in_i = 0; in_i < coln; in_i++)
      *ptr += *(variable + out_i + in_i * CHANNEL_SIZE) * *(in_i + in);
  }
  for(int out_i = row; out_i < deficit; out_i ++)
  {
    float *ptr = out_i + out;
    *ptr = *out;
  }
}

inline static void run_process(const float *data_start, const int channel, const int mat_row, 
                               const int mat_coln, const int row_deficit, const float *ivoid,
                               float *ovoid, const size_t npix)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(channel, mat_row, mat_coln, row_deficit, npix, ivoid, ovoid) \
  shared(data_start) \
  schedule(static)
#endif
 for(size_t k = 0; k < 4 * npix; k += 4)
  {
    const float *in = ((float *)ivoid) + (size_t)k;
    float *out = ((float *)ovoid) + (size_t)k;
    matrix3k(in, out, data_start + channel, mat_row, mat_coln, row_deficit);
    out[3] = in[3];
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_channelmixer_data_t *data = (dt_iop_channelmixer_data_t *)piece->data;
  const size_t npixels = roi_out->width * roi_out->height;
  const int ch_gray_out = dt_channel_changer();
  const float *start_address = data->red;
  int gray_mix_mode = 0;
  
  for(int col = 0; col < 3; col++)
    gray_mix_mode |= *((data->red) + col * CHANNEL_SIZE + CHANNEL_GRAY) != 0.0;
  
  if(gray_mix_mode)
  {
    piece->colors = ch_gray_out;
    const int out_bch = ch_gray_out < 4 ? ch_gray_out : ch_gray_out - 1;
    run_process(start_address, CHANNEL_GRAY, 1, 3, out_bch, ivoid, ovoid, npixels);
  }
  else
  {
    piece->colors = 4;
    run_process(start_address, CHANNEL_RED, 3, 3, 3, ivoid, ovoid, npixels);
  }
}

static void setting_limits(dt_iop_module_t *self, const int color)
{
  if(darktable.gui->reset) return;

  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  const int output = dt_bauhaus_combobox_get(g->output_channel) + CHANNEL_OFFSET;
  const float low_lim = dt_conf_get_float("channel_mixer_lower_limit");
  const float up_lim = dt_conf_get_float("channel_mixer_upper_limit");
  const float offset=fmaxf(0.1f, -2.0f * low_lim);
  
  float chan[] = { p->red[output], p->green[output], p->blue[output] };
  float delta[3] = { 0 };
  delta[0] = dt_bauhaus_slider_get(g->scale_red);
  delta[1] = dt_bauhaus_slider_get(g->scale_green);
  delta[2] = dt_bauhaus_slider_get(g->scale_blue);
  
  chan[color] = delta[color];
  float sum_p = 0;
  for(int i = 0; i < 3; i++)
  {
    chan[i] = CLAMP(chan[i], low_lim, up_lim) + offset;
    sum_p += chan[i];
  }

  for(int i = 0; i < 3; i++)
  {
    chan[i] *= (1.0f + 3.0f * offset) / sum_p;
    chan[i] -= offset;
  }

  p->red[output] = chan[0];
  p->green[output] = chan[1];
  p->blue[output] = chan[2];
  
  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->scale_red, chan[0]);
  dt_bauhaus_slider_set(g->scale_green, chan[1]);
  dt_bauhaus_slider_set(g->scale_blue, chan[2]);
  --darktable.gui->reset;
}

static void red_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  const int output_channel_index = dt_bauhaus_combobox_get(g->output_channel) + CHANNEL_OFFSET;
  const gboolean normal = dt_bauhaus_combobox_get(g->normalise);
  
  if(output_channel_index >= CHANNEL_OFFSET)
  {
    if((output_channel_index >= CHANNEL_RED) & normal)
      setting_limits(self, 0);
    else
      p->red[output_channel_index] = dt_bauhaus_slider_get(slider);
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void green_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  const int output_channel_index = dt_bauhaus_combobox_get(g->output_channel) + CHANNEL_OFFSET;
  const gboolean normal = dt_bauhaus_combobox_get(g->normalise);
  
  if(output_channel_index >= CHANNEL_OFFSET)
  {
    if((output_channel_index >= CHANNEL_RED) & normal)
      setting_limits(self, 1);
    else
      p->green[output_channel_index] = dt_bauhaus_slider_get(slider);
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void blue_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  const int output_channel_index = dt_bauhaus_combobox_get(g->output_channel) + CHANNEL_OFFSET;
  const gboolean normal = dt_bauhaus_combobox_get(g->normalise);
  
  if(output_channel_index >= CHANNEL_OFFSET)
  {
    if((output_channel_index >= CHANNEL_RED) & normal)
      setting_limits(self, 2);
    else
      p->blue[output_channel_index] = dt_bauhaus_slider_get(slider);
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void output_callback(GtkComboBox *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  
  const dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  const int output_channel_index = dt_bauhaus_combobox_get(g->output_channel) + CHANNEL_OFFSET;

  dt_bauhaus_slider_set(g->scale_red, p->red[output_channel_index]);
  dt_bauhaus_slider_set(g->scale_green, p->green[output_channel_index]);
  dt_bauhaus_slider_set(g->scale_blue, p->blue[output_channel_index]);
  dt_bauhaus_combobox_set(g->normalise, output_channel_index >= CHANNEL_RED);
  gtk_widget_set_visible(g->normalise, output_channel_index >= CHANNEL_RED);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)p1;
  dt_iop_channelmixer_data_t *d = (dt_iop_channelmixer_data_t *)piece->data;

  for(int i = CHANNEL_OFFSET; i < CHANNEL_SIZE; i++)
  {
    d->red[i] = p->red[i];
    d->blue[i] = p->blue[i];
    d->green[i] = p->green[i];
  }
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_channelmixer_data_t));
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
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  const dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)module->params;

  const int use_channel = which_channel(p);
  dt_bauhaus_combobox_set(g->output_channel, use_channel);
  dt_bauhaus_combobox_set(g->normalise, use_channel >= CHANNEL_RED);
  dt_bauhaus_slider_set(g->scale_red, p->red[use_channel]);
  dt_bauhaus_slider_set(g->scale_green, p->green[use_channel]);
  dt_bauhaus_slider_set(g->scale_blue, p->blue[use_channel]);
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);
  dt_iop_channelmixer_params_t *d = module->default_params;
  const int use_channel = which_channel(d);
  
  if(use_channel != CHANNEL_GRAY)
    d->red[CHANNEL_RED] = d->green[CHANNEL_GREEN] = d->blue[CHANNEL_BLUE] = 1.0f;
  else
    d->red[CHANNEL_GRAY] = d->green[CHANNEL_GRAY] = d->blue[CHANNEL_GRAY] = 1.0f/3.0f;

  memcpy(module->params, module->default_params, sizeof(dt_iop_channelmixer_params_t));
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_channelmixer_gui_data_t));
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  // output 
  g->output_channel = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->output_channel, NULL, _("destination"));
  dt_bauhaus_combobox_add(g->output_channel, _("red"));
  dt_bauhaus_combobox_add(g->output_channel, _("green"));
  dt_bauhaus_combobox_add(g->output_channel, _("blue"));
  dt_bauhaus_combobox_add(g->output_channel, C_("channelmixer", "gray"));
  
  g_signal_connect(G_OBJECT(g->output_channel), "value-changed", G_CALLBACK(output_callback), self);
  const int use_channel = which_channel(p);
  dt_bauhaus_combobox_set(g->output_channel, use_channel - CHANNEL_OFFSET);
  
  // normalise 
  g->normalise = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->normalise, NULL, _("normalise"));
  dt_bauhaus_combobox_add(g->normalise, _("no"));
  dt_bauhaus_combobox_add(g->normalise, _("yes"));
  dt_bauhaus_combobox_set(g->normalise, use_channel >= CHANNEL_RED);
  gtk_widget_set_visible(g->normalise, use_channel >= CHANNEL_RED);
  gtk_widget_set_tooltip_text(g->normalise, _("inputs sum to one"));
  
  const float low_lim = dt_conf_get_float("channel_mixer_lower_limit");
  const float up_lim = dt_conf_get_float("channel_mixer_upper_limit");
  const float step = 0.01f;

  // red 
  g->scale_red = dt_bauhaus_slider_new_with_range(self, low_lim, up_lim, step, p->red[use_channel], 2);
  gtk_widget_set_tooltip_text(g->scale_red, _("amount of red channel in the output channel"));
  dt_bauhaus_widget_set_label(g->scale_red, NULL, _("red"));
  g_signal_connect(G_OBJECT(g->scale_red), "value-changed", G_CALLBACK(red_callback), self);

  // green 
  g->scale_green = dt_bauhaus_slider_new_with_range(self, low_lim, up_lim, step, p->green[use_channel], 2);
  gtk_widget_set_tooltip_text(g->scale_green, _("amount of green channel in the output channel"));
  dt_bauhaus_widget_set_label(g->scale_green, NULL, _("green"));
  g_signal_connect(G_OBJECT(g->scale_green), "value-changed", G_CALLBACK(green_callback), self);

  // blue 
  g->scale_blue = dt_bauhaus_slider_new_with_range(self, low_lim, up_lim, step, p->blue[use_channel], 2);
  gtk_widget_set_tooltip_text(g->scale_blue, _("amount of blue channel in the output channel"));
  dt_bauhaus_widget_set_label(g->scale_blue, NULL, _("blue"));
  g_signal_connect(G_OBJECT(g->scale_blue), "value-changed", G_CALLBACK(blue_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->normalise), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->output_channel), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale_red), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale_green), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale_blue), TRUE, TRUE, 0);
}

void init_presets(dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "BEGIN", NULL, NULL, NULL);

  dt_gui_presets_add_generic(_("swap R and B"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 0, 0, 1, 0 },
                                                              { 0, 0, 0, 0, 1, 0, 0 },
                                                              { 0, 0, 0, 1, 0, 0, 0 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("swap G and B"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0 },
                                                              { 0, 0, 0, 0, 0, 1, 0 },
                                                              { 0, 0, 0, 0, 1, 0, 0 } },
                              sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("swap R and G"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 0, 1, 0, 0 },
                                                              { 0, 0, 0, 1, 0, 0, 0 },
                                                              { 0, 0, 0, 0, 0, 1, 0 } },
                              sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("B/W luminance-based"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0.21 },
                                                              { 0, 0, 0, 0, 1, 0, 0.72 },
                                                              { 0, 0, 0, 0, 0, 1, 0.07 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("B/W proportional"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0.25 },
                                                              { 0, 0, 0, 0, 1, 0, 0.50 },
                                                              { 0, 0, 0, 0, 0, 1, 0.25 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("Color"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0 },
                                                              { 0, 0, 0, 0, 1, 0, 0 },
                                                              { 0, 0, 0, 0, 0, 1, 0 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);


  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "COMMIT", NULL, NULL, NULL);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
