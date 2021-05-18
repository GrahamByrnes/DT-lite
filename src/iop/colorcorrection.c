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
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "dtgtk/drawingarea.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(1, dt_iop_colorcorrection_params_t)

#define DT_COLORCORRECTION_INSET DT_PIXEL_APPLY_DPI(5)
#define DT_COLORCORRECTION_MAX 40.

typedef struct dt_iop_colorcorrection_params_t
{
  float hia, hib, loa, lob;  // directly manipulated from gui; don't follow normal gui_update etc
  float saturation;          // $MIN: -3.0 $MAX: 3.0 $DEFAULT: 1.0
} dt_iop_colorcorrection_params_t;

typedef struct dt_iop_colorcorrection_gui_data_t
{
  GtkDrawingArea *area;
  GtkWidget *slider;
  int selected;
  cmsHTRANSFORM xform;
} dt_iop_colorcorrection_gui_data_t;

typedef struct dt_iop_colorcorrection_data_t
{
  float a_scale, a_base, b_scale, b_base, saturation;
} dt_iop_colorcorrection_data_t;

typedef struct dt_iop_colorcorrection_global_data_t
{
  int kernel_colorcorrection;
} dt_iop_colorcorrection_global_data_t;

const char *name()
{
  return _("color correction");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_Lab;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_colorcorrection_params_t p;

  p.loa = 0.0f;
  p.lob = 0.0f;
  p.hia = 0.0f;
  p.hib = 3.0f;
  p.saturation = 1.0f;
  dt_gui_presets_add_generic(_("warm tone"), self->op, self->version(), &p, sizeof(p), 1);

  p.loa = 3.55f;
  p.lob = 0.0f;
  p.hia = -0.95f;
  p.hib = 4.5f;
  p.saturation = 1.0f;
  dt_gui_presets_add_generic(_("warming filter"), self->op, self->version(), &p, sizeof(p), 1);

  p.loa = -3.55f;
  p.lob = -0.0f;
  p.hia = 0.95f;
  p.hib = -4.5f;
  p.saturation = 1.0f;
  dt_gui_presets_add_generic(_("cooling filter"), self->op, self->version(), &p, sizeof(p), 1);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorcorrection_data_t *d = (dt_iop_colorcorrection_data_t *)piece->data;
  const int ch = piece->colors;
  piece->colors = 4;
  const int height = roi_out->height;
  const int width = roi_out->width;
  const float saturation = d-> saturation;
  const float ascale = d-> a_scale;
  const float bscale = d-> b_scale;
  const float abase = d-> a_base;
  const float bbase = d-> b_base;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, width, height, ivoid, ovoid, saturation, abase, ascale, bbase, bscale) \
  schedule(static)
#endif
  for(int k = 0; k < (size_t)4 * width * height; k += 4)
  {
    float *in = ((float *)ivoid) + k;
    float *out = ((float *)ovoid) + k;
    out[0] = in[0];
    out[1] = saturation * (in[0] * ascale + abase);
    out[2] = saturation * (in[0] * bscale + bbase);

    if(ch > 1)
    {
      out[1] += saturation * in[1];
      out[2] += saturation * in[2];
    }
    out[3] = in[3];
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)p1;
  dt_iop_colorcorrection_data_t *d = (dt_iop_colorcorrection_data_t *)piece->data;
  d->a_scale = (p->hia - p->loa) / 100.0f;
  d->a_base = p->loa;
  d->b_scale = (p->hib - p->lob) / 100.0f;
  d->b_base = p->lob;
  d->saturation = p->saturation;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colorcorrection_data_t));
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
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)module->params;
  dt_bauhaus_slider_set(g->slider, p->saturation);
  gtk_widget_queue_draw(self->widget);
}

static gboolean dt_iop_colorcorrection_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data);
static gboolean dt_iop_colorcorrection_motion_notify(GtkWidget *widget, GdkEventMotion *event,
                                                     gpointer user_data);
static gboolean dt_iop_colorcorrection_button_press(GtkWidget *widget, GdkEventButton *event,
                                                    gpointer user_data);
static gboolean dt_iop_colorcorrection_leave_notify(GtkWidget *widget, GdkEventCrossing *event,
                                                    gpointer user_data);
static gboolean dt_iop_colorcorrection_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
static gboolean dt_iop_colorcorrection_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_colorcorrection_gui_data_t));
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;

  g->selected = 0;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->area), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("drag the line for split-toning. "
                                                     "bright means highlights, dark means shadows. "
                                                     "use mouse wheel to change saturation."));

  gtk_widget_add_events(GTK_WIDGET(g->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                                 | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                 | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK
                                                 | darktable.gui->scroll_mask);
  gtk_widget_set_can_focus(GTK_WIDGET(g->area), TRUE);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(dt_iop_colorcorrection_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event", G_CALLBACK(dt_iop_colorcorrection_button_press),
                   self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event", G_CALLBACK(dt_iop_colorcorrection_motion_notify),
                   self);
  g_signal_connect(G_OBJECT(g->area), "leave-notify-event", G_CALLBACK(dt_iop_colorcorrection_leave_notify),
                   self);
  g_signal_connect(G_OBJECT(g->area), "scroll-event", G_CALLBACK(dt_iop_colorcorrection_scrolled), self);
  g_signal_connect(G_OBJECT(g->area), "key-press-event", G_CALLBACK(dt_iop_colorcorrection_key_press), self);

  g->slider = dt_bauhaus_slider_from_params(self, N_("saturation"));
  gtk_widget_set_tooltip_text(g->slider, _("set the global saturation"));

  cmsHPROFILE hsRGB = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_IN)->profile;
  cmsHPROFILE hLab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;
  g->xform = cmsCreateTransform(hLab, TYPE_Lab_DBL, hsRGB, TYPE_RGB_DBL, INTENT_PERCEPTUAL, 0);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  cmsDeleteTransform(g->xform);
  free(self->gui_data);
  self->gui_data = NULL;
}

static gboolean dt_iop_colorcorrection_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;

  const int inset = DT_COLORCORRECTION_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  width -= 2 * inset;
  height -= 2 * inset;
  // flip y:
  cairo_translate(cr, 0, height);
  cairo_scale(cr, 1., -1.);
  const int cells = 8;
  for(int j = 0; j < cells; j++)
    for(int i = 0; i < cells; i++)
    {
      double rgb[3] = { 0.5, 0.5, 0.5 }; // Lab: rgb grey converted to Lab
      cmsCIELab Lab;
      Lab.L = 53.390011;
      Lab.a = Lab.b = 0; // grey
      Lab.a = p->saturation * (Lab.a + Lab.L * .05 * DT_COLORCORRECTION_MAX * (i / (cells - 1.0) - .5));
      Lab.b = p->saturation * (Lab.b + Lab.L * .05 * DT_COLORCORRECTION_MAX * (j / (cells - 1.0) - .5));
      cmsDoTransform(g->xform, &Lab, rgb, 1);
      cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
      cairo_rectangle(cr, width * i / (float)cells, height * j / (float)cells,
                      width / (float)cells - DT_PIXEL_APPLY_DPI(1),
                      height / (float)cells - DT_PIXEL_APPLY_DPI(1));
      cairo_fill(cr);
    }
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
  float loa, hia, lob, hib;
  loa = .5f * (width + width * p->loa / (float)DT_COLORCORRECTION_MAX);
  hia = .5f * (width + width * p->hia / (float)DT_COLORCORRECTION_MAX);
  lob = .5f * (height + height * p->lob / (float)DT_COLORCORRECTION_MAX);
  hib = .5f * (height + height * p->hib / (float)DT_COLORCORRECTION_MAX);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  cairo_move_to(cr, loa, lob);
  cairo_line_to(cr, hia, hib);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
  
  if(g->selected == 1)
    cairo_arc(cr, loa, lob, DT_PIXEL_APPLY_DPI(5), 0, 2. * M_PI);
  else
    cairo_arc(cr, loa, lob, DT_PIXEL_APPLY_DPI(3), 0, 2. * M_PI);
    
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
  
  if(g->selected == 2)
    cairo_arc(cr, hia, hib, DT_PIXEL_APPLY_DPI(5), 0, 2. * M_PI);
  else
    cairo_arc(cr, hia, hib, DT_PIXEL_APPLY_DPI(3), 0, 2. * M_PI);
    
  cairo_fill(cr);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean dt_iop_colorcorrection_motion_notify(GtkWidget *widget, GdkEventMotion *event,
                                                     gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;
  const int inset = DT_COLORCORRECTION_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width - 2 * inset, height = allocation.height - 2 * inset;
  const float mouse_x = CLAMP(event->x - inset, 0, width);
  const float mouse_y = CLAMP(height - 1 - event->y + inset, 0, height);
  const float ma = (2.0 * mouse_x - width) * DT_COLORCORRECTION_MAX / (float)width;
  const float mb = (2.0 * mouse_y - height) * DT_COLORCORRECTION_MAX / (float)height;
  if(event->state & GDK_BUTTON1_MASK)
  {
    if(g->selected == 1)
    {
      p->loa = ma;
      p->lob = mb;
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
    else if(g->selected == 2)
    {
      p->hia = ma;
      p->hib = mb;
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
  }
  else
  {
    g->selected = 0;
    const float thrs = DT_PIXEL_APPLY_DPI(5.0f);
    const float distlo = (p->loa - ma) * (p->loa - ma) + (p->lob - mb) * (p->lob - mb);
    const float disthi = (p->hia - ma) * (p->hia - ma) + (p->hib - mb) * (p->hib - mb);
    if(distlo < thrs * thrs && distlo < disthi)
      g->selected = 1;
    else if(disthi < thrs * thrs && disthi <= distlo)
      g->selected = 2;
  }
  if(g->selected > 0) gtk_widget_grab_focus(widget);
  gtk_widget_queue_draw(self->widget);
  return TRUE;
}

static gboolean dt_iop_colorcorrection_button_press(GtkWidget *widget, GdkEventButton *event,
                                                    gpointer user_data)
{
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    // double click resets:
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
    dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;
    switch(g->selected)
    {
      case 1: // only reset lo
        p->loa = p->lob = 0.0;
        dt_dev_add_history_item(darktable.develop, self, TRUE);
        break;
      case 2: // only reset hi
        p->hia = p->hib = 0.0;
        dt_dev_add_history_item(darktable.develop, self, TRUE);
        break;
      default: // reset everything
      {
        dt_iop_colorcorrection_params_t *d = (dt_iop_colorcorrection_params_t *)self->default_params;
        memcpy(p, d, sizeof(*p));
        dt_dev_add_history_item(darktable.develop, self, TRUE);
      }
    }
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_colorcorrection_leave_notify(GtkWidget *widget, GdkEventCrossing *event,
                                                    gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  gtk_widget_queue_draw(self->widget);
  return TRUE;
}

static gboolean dt_iop_colorcorrection_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;

  if(dt_gui_ignore_scroll(event)) return FALSE;

  gdouble delta_y;
  if(dt_gui_get_scroll_deltas(event, NULL, &delta_y))
  {
     p->saturation = CLAMP(p->saturation - 0.1 * delta_y, -3.0, 3.0);
     dt_bauhaus_slider_set(g->slider, p->saturation);
     gtk_widget_queue_draw(widget);
  }

  return TRUE;
}

#define COLORCORRECTION_DEFAULT_STEP (0.5f)

static gboolean dt_iop_colorcorrection_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;
  if(g->selected < 1) return FALSE;

  int handled = 0;
  float dx = 0.0f, dy = 0.0f;
  if(event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up)
  {
    handled = 1;
    dy = COLORCORRECTION_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down)
  {
    handled = 1;
    dy = -COLORCORRECTION_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Right || event->keyval == GDK_KEY_KP_Right)
  {
    handled = 1;
    dx = COLORCORRECTION_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Left || event->keyval == GDK_KEY_KP_Left)
  {
    handled = 1;
    dx = -COLORCORRECTION_DEFAULT_STEP;
  }

  if(!handled) return TRUE;

  float multiplier;

  GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
  if((event->state & modifiers) == GDK_SHIFT_MASK)
    multiplier = dt_conf_get_float("darkroom/ui/scale_rough_step_multiplier");
  else if((event->state & modifiers) == GDK_CONTROL_MASK)
    multiplier = dt_conf_get_float("darkroom/ui/scale_precise_step_multiplier");
  else
    multiplier = dt_conf_get_float("darkroom/ui/scale_step_multiplier");

  dx *= multiplier;
  dy *= multiplier;

  switch(g->selected)
  {
    case 1: // only set lo
      p->loa = CLAMP(p->loa + dx, -DT_COLORCORRECTION_MAX, DT_COLORCORRECTION_MAX);
      p->lob = CLAMP(p->lob + dy, -DT_COLORCORRECTION_MAX, DT_COLORCORRECTION_MAX);
      break;
    case 2: // only set hi
      p->hia = CLAMP(p->hia + dx, -DT_COLORCORRECTION_MAX, DT_COLORCORRECTION_MAX);
      p->hib = CLAMP(p->hib + dy, -DT_COLORCORRECTION_MAX, DT_COLORCORRECTION_MAX);
      break;
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(widget);

  return TRUE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
