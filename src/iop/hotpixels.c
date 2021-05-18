/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_hotpixels_params_t)

typedef struct dt_iop_hotpixels_params_t
{
  float strength;  // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.25
  float threshold; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.05
  gboolean markfixed;  // $DEFAULT: FALSE $DESCRIPTION: "mark fixed pixels"
  gboolean permissive; // $DEFAULT: FALSE $DESCRIPTION: "detect by 3 neighbors"
} dt_iop_hotpixels_params_t;

typedef struct dt_iop_hotpixels_gui_data_t
{
  GtkWidget *box_raw;
  GtkWidget *threshold, *strength;
  GtkToggleButton *markfixed;
  GtkToggleButton *permissive;
  GtkLabel *message;
  int pixels_fixed;
  GtkWidget *label_non_raw;
} dt_iop_hotpixels_gui_data_t;

typedef struct dt_iop_hotpixels_data_t
{
  uint32_t filters;
  float threshold;
  float multiplier;
  gboolean permissive;
  gboolean markfixed;
} dt_iop_hotpixels_data_t;


const char *name()
{
  return _("hot pixels");
}

int default_group()
{
  return IOP_GROUP_CORRECT;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ONE_INSTANCE;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_RAW;
}

/* Detect hot sensor pixels based on the 4 surrounding sites. Pixels
 * having 3 or 4 (depending on permissive setting) surrounding pixels that
 * than value*multiplier are considered "hot", and are replaced by the maximum of
 * the neighbour pixels. The permissive variant allows for
 * correcting pairs of hot pixels in adjacent sites. Replacement using
 * the maximum produces fewer artifacts when inadvertently replacing
 * non-hot pixels.
 * This is the Bayer sensor variant. */
static int process_bayer(const dt_iop_hotpixels_data_t *data,
                         const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_out)
{
  const float threshold = data->threshold;
  const float multiplier = data->multiplier;
  const gboolean markfixed = data->markfixed;
  const int min_neighbours = data->permissive ? 3 : 4;
  const int width = roi_out->width;
  const int widthx2 = width * 2;
  int fixed = 0;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ivoid, markfixed, min_neighbours, multiplier, ovoid, \
                      roi_out, threshold, width, widthx2) \
  reduction(+ : fixed) \
  schedule(static)
#endif
  for(int row = 2; row < roi_out->height - 2; row++)
  {
    const float *in = (float *)ivoid + (size_t)width * row + 2;
    float *out = (float *)ovoid + (size_t)width * row + 2;
    for(int col = 2; col < width - 2; col++, in++, out++)
    {
      float mid = *in * multiplier;
      if(*in > threshold)
      {
        int count = 0;
        float maxin = 0.0;
        float other;
#define TESTONE(OFFSET)                                                                                      \
  other = in[OFFSET];                                                                                        \
  if(mid > other)                                                                                            \
  {                                                                                                          \
    count++;                                                                                                 \
    if(other > maxin) maxin = other;                                                                         \
  }
        TESTONE(-2);
        TESTONE(-widthx2);
        TESTONE(+2);
        TESTONE(+widthx2);
#undef TESTONE
        if(count >= min_neighbours)
        {
          *out = maxin;
          fixed++;
          if(markfixed)
          {
            for(int i = -2; i >= -10 && i >= -col; i -= 2) out[i] = *in;
            for(int i = 2; i <= 10 && i < width - col; i += 2) out[i] = *in;
          }
        }
      }
    }
  }

  return fixed;
}

/* X-Trans sensor equivalent of process_bayer(). */
static int process_xtrans(const dt_iop_hotpixels_data_t *data,
                          const void *const ivoid, void *const ovoid,
                          const dt_iop_roi_t *const roi_out, const uint8_t (*const xtrans)[6])
{
  // for each cell of sensor array, pre-calculate, a list of the x/y
  // offsets of the four radially nearest pixels of the same color
  int offsets[6][6][4][2];
  // increasing offsets from pixel to find nearest like-colored pixels
  const int search[20][2] = { { -1, 0 },
                              { 1, 0 },
                              { 0, -1 },
                              { 0, 1 },
                              { -1, -1 },
                              { -1, 1 },
                              { 1, -1 },
                              { 1, 1 },
                              { -2, 0 },
                              { 2, 0 },
                              { 0, -2 },
                              { 0, 2 },
                              { -2, -1 },
                              { -2, 1 },
                              { 2, -1 },
                              { 2, 1 },
                              { -1, -2 },
                              { 1, -2 },
                              { -1, 2 },
                              { 1, 2 } };
  for(int j = 0; j < 6; ++j)
  {
    for(int i = 0; i < 6; ++i)
    {
      const uint8_t c = FCxtrans(j, i, roi_out, xtrans);
      for(int s = 0, found = 0; s < 20 && found < 4; ++s)
      {
        if(c == FCxtrans(j + search[s][1], i + search[s][0], roi_out, xtrans))
        {
          offsets[j][i][found][0] = search[s][0];
          offsets[j][i][found][1] = search[s][1];
          ++found;
        }
      }
    }
  }

  const float threshold = data->threshold;
  const float multiplier = data->multiplier;
  const gboolean markfixed = data->markfixed;
  const int min_neighbours = data->permissive ? 3 : 4;
  const int width = roi_out->width;
  int fixed = 0;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ivoid, markfixed, min_neighbours, multiplier, ovoid, \
                      roi_out, threshold, xtrans, width) \
  shared(offsets) \
  reduction(+ : fixed) \
  schedule(static)
#endif
  for(int row = 2; row < roi_out->height - 2; row++)
  {
    const float *in = (float *)ivoid + (size_t)width * row + 2;
    float *out = (float *)ovoid + (size_t)width * row + 2;
    for(int col = 2; col < width - 2; col++, in++, out++)
    {
      float mid = *in * multiplier;
      if(*in > threshold)
      {
        int count = 0;
        float maxin = 0.0;
        for(int n = 0; n < 4; ++n)
        {
          int xx = offsets[row % 6][col % 6][n][0];
          int yy = offsets[row % 6][col % 6][n][1];
          float other = *(in + xx + yy * (size_t)width);
          if(mid > other)
          {
            count++;
            if(other > maxin) maxin = other;
          }
        }
        // NOTE: it seems that detecting by 2 neighbors would help for extreme cases
        if(count >= min_neighbours)
        {
          *out = maxin;
          fixed++;
          if(markfixed)
          {
            const uint8_t c = FCxtrans(row, col, roi_out, xtrans);
            for(int i = -2; i >= -10 && i >= -col; --i)
            {
              if(c == FCxtrans(row, col+i, roi_out, xtrans))
              {
                out[i] = *in;
              }
            }
            for(int i = 2; i <= 10 && i < width - col; ++i)
            {
              if(c == FCxtrans(row, col+i, roi_out, xtrans))
              {
                out[i] = *in;
              }
            }
          }
        }
      }
    }
  }

  return fixed;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_hotpixels_gui_data_t *g = (dt_iop_hotpixels_gui_data_t *)self->gui_data;
  const dt_iop_hotpixels_data_t *data = (dt_iop_hotpixels_data_t *)piece->data;

  // The processing loop should output only a few pixels, so just copy everything first
  memcpy(ovoid, ivoid, (size_t)roi_out->width * roi_out->height * sizeof(float));

  int fixed;
  if(piece->pipe->dsc.filters == 9u)
    fixed = process_xtrans(data, ivoid, ovoid, roi_out, (const uint8_t(*const)[6])piece->pipe->dsc.xtrans);
  else
    fixed = process_bayer(data, ivoid, ovoid, roi_out);

  if(g != NULL && self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL) == DT_DEV_PIXELPIPE_FULL)
    g->pixels_fixed = fixed;
}

void reload_defaults(dt_iop_module_t *module)
{
  // we might be called from presets update infrastructure => there is no image
  if(!module->dev) return;

  // can't be switched on for non-raw images:
  module->hide_enable_button = !dt_image_is_raw(&module->dev->image_storage);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_hotpixels_params_t *p = (dt_iop_hotpixels_params_t *)params;
  dt_iop_hotpixels_data_t *d = (dt_iop_hotpixels_data_t *)piece->data;
  d->filters = piece->pipe->dsc.filters;
  d->multiplier = p->strength / 2.0;
  d->threshold = p->threshold;
  d->permissive = p->permissive;
  d->markfixed = p->markfixed && ((pipe->type & DT_DEV_PIXELPIPE_EXPORT) != DT_DEV_PIXELPIPE_EXPORT)
    && ((pipe->type & DT_DEV_PIXELPIPE_THUMBNAIL) != DT_DEV_PIXELPIPE_THUMBNAIL);
  if(!(dt_image_is_raw(&pipe->image)) || p->strength == 0.0) piece->enabled = 0;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_hotpixels_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


void gui_update(dt_iop_module_t *self)
{
  dt_iop_hotpixels_gui_data_t *g = (dt_iop_hotpixels_gui_data_t *)self->gui_data;
  dt_iop_hotpixels_params_t *p = (dt_iop_hotpixels_params_t *)self->params;
  dt_bauhaus_slider_set(g->strength, p->strength);
  dt_bauhaus_slider_set(g->threshold, p->threshold);
  gtk_toggle_button_set_active(g->markfixed, p->markfixed);
  gtk_toggle_button_set_active(g->permissive, p->permissive);
  g->pixels_fixed = -1;
  gtk_label_set_text(g->message, "");

  if(!self->hide_enable_button)
  {
    gtk_widget_show(g->box_raw);
    gtk_widget_hide(g->label_non_raw);
  }
  else
  {
    gtk_widget_hide(g->box_raw);
    gtk_widget_show(g->label_non_raw);
  }
}

static gboolean draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  dt_iop_hotpixels_gui_data_t *g = (dt_iop_hotpixels_gui_data_t *)self->gui_data;
  if(darktable.gui->reset) return FALSE;

  if(g->pixels_fixed < 0) return FALSE;

  char *str = g_strdup_printf(ngettext("fixed %d pixel", "fixed %d pixels", g->pixels_fixed), g->pixels_fixed);
  g->pixels_fixed = -1;

  ++darktable.gui->reset;
  gtk_label_set_text(g->message, str);
  --darktable.gui->reset;

  g_free(str);

  return FALSE;
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_hotpixels_gui_data_t));
  dt_iop_hotpixels_gui_data_t *g = (dt_iop_hotpixels_gui_data_t *)self->gui_data;

  g->pixels_fixed = -1;

  g->box_raw = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  g_signal_connect(G_OBJECT(g->box_raw), "draw", G_CALLBACK(draw), self);

  g->threshold = dt_bauhaus_slider_from_params(self, N_("threshold"));
  dt_bauhaus_slider_set_step(g->threshold, 0.005);
  dt_bauhaus_slider_set_digits(g->threshold, 4);
  gtk_widget_set_tooltip_text(g->threshold, _("lower threshold for hot pixel"));

  g->strength = dt_bauhaus_slider_from_params(self, N_("strength"));
  dt_bauhaus_slider_set_digits(g->strength, 4);
  gtk_widget_set_tooltip_text(g->strength, _("strength of hot pixel correction"));

  // 3 neighbours
  g->permissive = GTK_TOGGLE_BUTTON(dt_bauhaus_toggle_from_params(self, "permissive"));

  // mark fixed pixels
  GtkWidget *hbox = self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  g->markfixed = GTK_TOGGLE_BUTTON(dt_bauhaus_toggle_from_params(self, "markfixed"));
  g->message = GTK_LABEL(gtk_label_new("")); // This gets filled in by process
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->message), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->box_raw), hbox, TRUE, TRUE, 0);

  // start building top level widget
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  gtk_box_pack_start(GTK_BOX(self->widget), g->box_raw, FALSE, FALSE, 0);

  g->label_non_raw = gtk_label_new(_("hot pixel correction\nonly works for raw images."));
  gtk_widget_set_halign(g->label_non_raw, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(self->widget), g->label_non_raw, FALSE, FALSE, 0);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
