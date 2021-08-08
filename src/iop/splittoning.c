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
#include "common/colorspaces_inline_conversions.h"
#include "common/debug.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "dtgtk/button.h"
#include "dtgtk/gradientslider.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CLIP(x) ((x < 0) ? 0.0 : (x > 1.0) ? 1.0 : x)
DT_MODULE_INTROSPECTION(1, dt_iop_splittoning_params_t)

const float half = 50.0f; // sets luminance for sliders
const float c_chr = 80.0f; // sets chrominance ref 128.0f

typedef struct dt_iop_splittoning_params_t
{
  float shadow_hue;           // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float shadow_chroma;        // $MIN: 0.0 $MAX: 128.0 $DEFAULT: 60.0 $DESCRIPTION: "chroma"
  float highlight_hue;        // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2 $DESCRIPTION: "hue"
  float highlight_chroma;     // $MIN: 0.0 $MAX: 128.0 $DEFAULT: 60.0 $DESCRIPTION: "chroma"
  float balance;              // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 center luminance of gradient
  float compress;             // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 33.0 Compress range
} dt_iop_splittoning_params_t;

typedef struct dt_iop_splittoning_gui_data_t
{
  GtkWidget *balance_scale, *compress_scale;
  GtkWidget *shadow_colorpick, *highlight_colorpick;
  GtkWidget *shadow_hue_gslider, *shadow_chroma_gslider;
  GtkWidget *highlight_hue_gslider, *highlight_chroma_gslider;
} dt_iop_splittoning_gui_data_t;

typedef struct dt_iop_splittoning_data_t
{
  float shadow_hue;
  float shadow_chroma;
  float highlight_hue;
  float highlight_chroma;
  float balance;  // luminance center of gradient}
  float compress; // Compress range
} dt_iop_splittoning_data_t;

const char *name()
{
  return _("split-toning");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_Lab;
}

void init_presets(dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "BEGIN", NULL, NULL, NULL);

  // shadows: #ED7212
  // highlights: #ECA413
  // balance : 63
  // compress : 0
  dt_gui_presets_add_generic(
      _("authentic sepia"), self->op, self->version(),
      &(dt_iop_splittoning_params_t){ 61.5 / 360.0, 101.5, 72.4 / 360.0, 96.8, 0.63, 40.0 },
      sizeof(dt_iop_splittoning_params_t), 1);

  // shadows: #446CBB
  // highlights: #446CBB
  // balance : 0
  // compress : 5.22
  dt_gui_presets_add_generic(
      _("authentic cyanotype"), self->op, self->version(),
      &(dt_iop_splittoning_params_t){ 46.2 / 360.0, 61.2, 46.3 / 360.0, 61.2, 0.5, 30.0 },
      sizeof(dt_iop_splittoning_params_t), 1);

  // shadows : #A16C5E
  // highlights : #A16C5E
  // balance : 100
  // compress : 0
  dt_gui_presets_add_generic(
      _("authentic platinotype"), self->op, self->version(),
      &(dt_iop_splittoning_params_t){ 50.9 / 360.0, 33.4, 50.9 / 360.0, 33.4, 0.3, 30.0 },
      sizeof(dt_iop_splittoning_params_t), 1);

  // shadows: #211A14
  // highlights: #D9D0C7
  // balance : 60
  // compress : 0
  dt_gui_presets_add_generic(
      _("chocolate brown"), self->op, self->version(),
      &(dt_iop_splittoning_params_t){ 9.4 / 360.0, 7.3, 84.3 / 360.0, 7.2, 0.60, 50.0 },
      sizeof(dt_iop_splittoning_params_t), 1);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "COMMIT", NULL, NULL, NULL);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_splittoning_data_t *data = (dt_iop_splittoning_data_t *)piece->data;
  const int ch = piece->colors;
  const int bch = ch < 4 ? ch : ch - 1;
  const int ch_out = 4;
  const int npixels = 4 * roi_out->width * roi_out->height;
  piece->colors = ch_out;
  const float compress = data->compress / 100.0;
  const float chr_shad = data->shadow_chroma;
  const float chr_high = data->highlight_chroma;
  const float hue_shad = data->shadow_hue;
  const float hue_high = data->highlight_hue;
  const float thresh_low = data->balance * (1.0f - compress);
  const float thresh_high = data->balance + (1.0f - data->balance) * compress; 
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(bch, compress, thresh_low, thresh_high, chr_shad, chr_high, \
  hue_shad, hue_high, ivoid, ovoid, npixels) \
  schedule(static)
#endif
  for(int k = 0; k < npixels; k += 4)
  {
    const float *const in = (const float *const)ivoid + (size_t)k;
    float *out = (float *)ovoid + (size_t)k;
    out[0] = in[0];
    out[1] = bch > 1 ? in[1] : 0.0f;
    out[2] = bch > 1 ? in[2] : 0.0f;
    out[3] = in[3];
    const float lum = in[0] / 100.0f;
    
    if (lum < thresh_low)
    {
      const float hue_chrom_mix_r = chr_shad;
      const float hue_chrom_mix_theta = hue_shad;
      const float ra = lum * (thresh_low - lum) * 2.0f / thresh_low;
      const float in_temp_r = hue_chrom_mix_r * ra;
      const float in_temp_theta = hue_chrom_mix_theta * ra;
      out[1] += cosf(2.0f * DT_M_PI_F * in_temp_theta) * in_temp_r + in[1];
      out[2] += sinf(2.0f * DT_M_PI_F * in_temp_theta) * in_temp_r + in[2];
    }
    else if (lum > thresh_high)
    {
      const float hue_chrom_mix_r = chr_high;
      const float hue_chrom_mix_theta = hue_high;
      const float ra = (1.0f - lum) * (lum - thresh_high) * 2.0f / thresh_high;
      const float in_temp_r = hue_chrom_mix_r * ra;
      const float in_temp_theta = hue_chrom_mix_theta * ra;
      out[1] += cosf(2.0f * DT_M_PI_F * in_temp_theta) * in_temp_r + in[1];
      out[2] += sinf(2.0f * DT_M_PI_F * in_temp_theta) * in_temp_r + in[2];
    }
  }
}

static inline void update_colorpicker_color(GtkWidget *colorpicker, float hue, float chroma)
{
  float rgb[3];
  LCh2rgb(&half, &chroma, &hue, rgb);
  GdkRGBA color = (GdkRGBA){.red = rgb[0], .green = rgb[1], .blue = rgb[2], .alpha = 1.0 };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(colorpicker), &color);
}

static inline void update_chroma_slider_end_color(GtkWidget *slider, float hue)
{
  float rgb[3];
  LCh2rgb(&half, &c_chr, &hue, rgb);
  dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
}

static inline void update_balance_slider_colors( GtkWidget *slider, float shadow_hue, float highlight_hue)
{
  float rgb[3];
  if(shadow_hue != -1)
  {
    LCh2rgb(&half, &c_chr, &shadow_hue, rgb);
    dt_bauhaus_slider_set_stop(slider, 0.0, rgb[0], rgb[1], rgb[2]);
  }
  if(highlight_hue != -1)
  {
    LCh2rgb(&half, &c_chr, &highlight_hue, rgb);
    dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
  }

  gtk_widget_queue_draw(GTK_WIDGET(slider));
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;

  if(w == g->shadow_chroma_gslider || w == g->shadow_hue_gslider)
  {
    update_colorpicker_color(g->shadow_colorpick, p->shadow_hue, p->shadow_chroma);
    if(w == g->shadow_hue_gslider)
    {
      update_balance_slider_colors(g->balance_scale, p->shadow_hue, -1);
      update_chroma_slider_end_color(g->shadow_chroma_gslider, p->shadow_hue);
      gtk_widget_queue_draw(GTK_WIDGET(g->shadow_chroma_gslider));
    }
  }
  else if(w == g->highlight_chroma_gslider || w == g->highlight_hue_gslider)
  {
    update_colorpicker_color(g->highlight_colorpick, p->highlight_hue, p->highlight_chroma);

    if(w == g->highlight_hue_gslider)
    {
      update_balance_slider_colors(g->balance_scale, -1, p->highlight_hue);
      update_chroma_slider_end_color(g->highlight_chroma_gslider, p->highlight_hue);
      gtk_widget_queue_draw(GTK_WIDGET(g->highlight_chroma_gslider));
    }
  }
}

static void colorpick_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset)
    return;

  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;
  float color[3], h, chr, lum;
  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  color[0] = c.red;
  color[1] = c.green;
  color[2] = c.blue;
  rgb2LCh(color, &lum, &chr, &h);

  if (GTK_WIDGET(widget) == g->shadow_colorpick)
  {
      dt_bauhaus_slider_set(g->shadow_hue_gslider, h);
      dt_bauhaus_slider_set(g->shadow_chroma_gslider, chr);
      update_balance_slider_colors(g->balance_scale, h, -1);
  }
  else
  {
      dt_bauhaus_slider_set(g->highlight_hue_gslider, h);
      dt_bauhaus_slider_set(g->highlight_chroma_gslider, chr);
      update_balance_slider_colors(g->balance_scale, -1,  h);
  }

  gtk_widget_queue_draw(GTK_WIDGET(g->balance_scale));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;

  float *p_hue, *p_chroma;
  GtkWidget *chroma, *hue, *colorpicker;
  // convert picker RGB 2 LCh
  float H = 0.0f, Chr = 0.0f, L = 0.0f;
  rgb2LCh(self->picked_color, &L, &Chr, &H);

  if(picker == g->highlight_hue_gslider)
  {
    p_hue = &p->highlight_hue;
    p_chroma = &p->highlight_chroma;
    hue = g->highlight_hue_gslider;
    chroma = g->highlight_chroma_gslider;
    colorpicker = g->highlight_colorpick;
    update_balance_slider_colors(g->balance_scale, -1, H);
  }
  else
  {
    p_hue = &p->shadow_hue;
    p_chroma = &p->shadow_chroma;
    hue = g->shadow_hue_gslider;
    chroma = g->shadow_chroma_gslider;
    colorpicker = g->shadow_colorpick;
    update_balance_slider_colors(g->balance_scale, H, -1);
  }

  if(fabsf(*p_hue - H) < 0.0001f && fabsf(*p_chroma - Chr) < 0.0001f)
    return;

  *p_hue = H;
  *p_chroma = Chr;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(hue, H);
  dt_bauhaus_slider_set(chroma, Chr);
  update_colorpicker_color(colorpicker, H, Chr);
  update_chroma_slider_end_color(chroma, H);
  --darktable.gui->reset;

  gtk_widget_queue_draw(GTK_WIDGET(g->balance_scale));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)p1;
  dt_iop_splittoning_data_t *d = (dt_iop_splittoning_data_t *)piece->data;

  d->shadow_hue = p->shadow_hue;
  d->highlight_hue = p->highlight_hue;
  d->shadow_chroma = p->shadow_chroma;
  d->highlight_chroma = p->highlight_chroma;
  d->balance = p->balance;
  d->compress = p->compress;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_splittoning_data_t));
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
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)module->params;

  dt_bauhaus_slider_set(g->shadow_hue_gslider, p->shadow_hue);
  dt_bauhaus_slider_set(g->shadow_chroma_gslider, p->shadow_chroma);
  dt_bauhaus_slider_set(g->highlight_hue_gslider, p->highlight_hue);
  dt_bauhaus_slider_set(g->highlight_chroma_gslider, p->highlight_chroma);
  dt_bauhaus_slider_set(g->balance_scale, p->balance);
  dt_bauhaus_slider_set(g->compress_scale, p->compress);

  update_colorpicker_color(GTK_WIDGET(g->shadow_colorpick), p->shadow_hue, p->shadow_chroma);
  update_colorpicker_color(GTK_WIDGET(g->highlight_colorpick), p->highlight_hue, p->highlight_chroma);
  update_chroma_slider_end_color(g->shadow_chroma_gslider, p->shadow_hue);
  update_chroma_slider_end_color(g->highlight_chroma_gslider, p->highlight_hue);

  update_balance_slider_colors(g->balance_scale, p->shadow_hue, p->highlight_hue);
}

static inline void gui_init_section(struct dt_iop_module_t *self, char *section, GtkWidget *slider_box, 
                                              GtkWidget *hue, GtkWidget *chroma, GtkWidget **picker, gboolean top)
{
  GtkWidget *label = dt_ui_section_label_new(section);

  if(top)
  {
    GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(label));
    gtk_style_context_add_class(context, "section_label_top");
  }

  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);
  dt_color_picker_new(self, DT_COLOR_PICKER_POINT, hue);

  dt_bauhaus_slider_set_stop(chroma, 0.0f, 0.2f, 0.2f, 0.2f);
  dt_bauhaus_slider_set_stop(chroma, 1.0f, 1.0f, 1.0f, 1.0f);

  *picker = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(*picker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(*picker), _("select tone color"));
  g_signal_connect(G_OBJECT(*picker), "color-set", G_CALLBACK(colorpick_callback), self);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(hbox), slider_box, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(hbox), *picker, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_splittoning_gui_data_t));
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;

  GtkWidget *shadows_box = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  g->shadow_hue_gslider = dt_bauhaus_slider_from_params(self, "shadow_hue");
  g->shadow_chroma_gslider = dt_bauhaus_slider_from_params(self, "shadow_chroma");

  GtkWidget *highlights_box = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  g->highlight_hue_gslider = dt_bauhaus_slider_from_params(self, "highlight_hue");
  g->highlight_chroma_gslider = dt_bauhaus_slider_from_params(self, "highlight_chroma");
  // start building top level widget
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gui_init_section(self, _("shadows"), shadows_box, g->shadow_hue_gslider, g->shadow_chroma_gslider,
                   &g->shadow_colorpick, TRUE);
  gui_init_section(self, _("highlights"), highlights_box, g->highlight_hue_gslider, g->highlight_chroma_gslider,
                   &g->highlight_colorpick, FALSE);
  // Additional parameters
  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("properties")), FALSE, FALSE, 0);

  g->balance_scale = dt_bauhaus_slider_from_params(self, N_("balance"));
  dt_bauhaus_slider_set_feedback(g->balance_scale, 0);
  dt_bauhaus_slider_set_step(g->balance_scale, 0.001);
  dt_bauhaus_slider_set_digits(g->balance_scale, 4);
  dt_bauhaus_slider_set_factor(g->balance_scale, -100.0);
  dt_bauhaus_slider_set_offset(g->balance_scale, +100.0);
  dt_bauhaus_slider_set_format(g->balance_scale, "%.2f");
  dt_bauhaus_slider_set_stop(g->balance_scale, 0.0f, 0.5f, 0.5f, 0.5f);
  dt_bauhaus_slider_set_stop(g->balance_scale, 1.0f, 0.5f, 0.5f, 0.5f);
  gtk_widget_set_tooltip_text(g->balance_scale, _("center of split-toning"));

  g->compress_scale = dt_bauhaus_slider_from_params(self, N_("compress"));
  dt_bauhaus_slider_set_format(g->compress_scale, "%.2f%%");
  gtk_widget_set_tooltip_text(g->compress_scale, _("mid-tones unaffected"));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
