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
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/rgb_norms.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/paint.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"
#include "libs/colorpicker.h"

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(1)
#define DT_GUI_CURVE_INFL .3f

#define DT_IOP_TONECURVE_RES 256
#define DT_IOP_TONECURVE_MAXNODES 20
#define MAX_LOG_BASE 20.0f

DT_MODULE_INTROSPECTION(5, dt_iop_tonecurve_params_t)

static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data);
static gboolean dt_iop_tonecurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
static gboolean dt_iop_tonecurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_iop_tonecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
static gboolean dt_iop_tonecurve_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
static gboolean dt_iop_tonecurve_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);

typedef enum tonecurve_channel_t
{
  ch_L = 0,
  ch_a = 1,
  ch_b = 2,
  ch_max = 3
} tonecurve_channel_t;

typedef struct dt_iop_tonecurve_node_t
{
  float x;
  float y;
} dt_iop_tonecurve_node_t;

typedef enum dt_iop_tonecurve_autoscale_t
{
  DT_S_SCALE_MANUAL = 0,           /* $DESCRIPTION: "Lab, independent channels" */
  DT_S_SCALE_AUTOMATIC = 1,        /* $DESCRIPTION: "Lab, linked channels" */
} dt_iop_tonecurve_autoscale_t;

typedef struct dt_iop_tonecurve_params_t
{
  dt_iop_tonecurve_node_t tonecurve[3][DT_IOP_TONECURVE_MAXNODES]; // three curves (L, a, b) with max number
                                                                   // of nodes
  int tonecurve_nodes[3];
  int tonecurve_type[3]; // $DEFAULT: CUBIC_SPLINE
  dt_iop_tonecurve_autoscale_t tonecurve_autoscale_ab; //$DEFAULT: DT_S_SCALE_AUTOMATIC $DESCRIPTION: "color space"
  int tonecurve_preset; // $DEFAULT: 0
  int tonecurve_unbound_ab; // GB changed default to 0
  dt_iop_rgb_norms_t preserve_colors; // $DEFAULT: DT_RGB_NORM_LUMINANCE 
} dt_iop_tonecurve_params_t;

typedef struct dt_iop_tonecurve_gui_data_t
{
  dt_draw_curve_t *minmax_curve[3]; // curves for gui to draw
  int minmax_curve_nodes[3];
  int minmax_curve_type[3];
  GtkBox *hbox;
  GtkDrawingArea *area;
  GtkSizeGroup *sizegroup;
  GtkWidget *autoscale_ab;
  GtkNotebook *channel_tabs;
  GtkWidget *colorpicker;
  GtkWidget *interpolator;
  tonecurve_channel_t channel;
  double mouse_x, mouse_y;
  int selected;
  float draw_xs[DT_IOP_TONECURVE_RES], draw_ys[DT_IOP_TONECURVE_RES];
  float draw_min_xs[DT_IOP_TONECURVE_RES], draw_min_ys[DT_IOP_TONECURVE_RES];
  float draw_max_xs[DT_IOP_TONECURVE_RES], draw_max_ys[DT_IOP_TONECURVE_RES];
  float loglogscale;
  int semilog;
  GtkWidget *logbase;
} dt_iop_tonecurve_gui_data_t;

typedef struct dt_iop_tonecurve_data_t
{
  dt_draw_curve_t *curve[3];     // curves for pipe piece and pixel processing
  int curve_nodes[3];            // number of nodes
  int curve_type[3];             // curve style (e.g. CUBIC_SPLINE)
  float table[3][0x10000];       // precomputed look-up tables for tone curve
  float unbounded_coeffs_L[3];   // approximation for extrapolation of L
  float unbounded_coeffs_ab[12]; // approximation for extrapolation of ab
  int autoscale_ab;
  int unbound_ab;
  int preserve_colors;
} dt_iop_tonecurve_data_t;

typedef struct dt_iop_tonecurve_global_data_t
{
  float picked_color[3];
  float picked_color_min[3];
  float picked_color_max[3];
  float picked_output_color[3];
  int kernel_tonecurve;
} dt_iop_tonecurve_global_data_t;


const char *name()
{
  return _("tone curve");
}

int default_group()
{
  return IOP_GROUP_TONE;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_Lab;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  return 1;
}

void run_auto_process(const void *const ivoid, void *const ovoid, const int ch,
                      const int npix, const float *unbounded_coeffs, const float *table_L)
{
  const int bch = ch < 4 ? ch : ch - 1;
  const float xm_L = 1.0f / unbounded_coeffs[0];
  const float low_approx = table_L[(int)(0.01f * 0x10000ul)];
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(bch, npix, ivoid, ovoid, \
                      low_approx, xm_L, unbounded_coeffs) \
  shared(table_L) \
  schedule(static)
#endif
  for(size_t k = 0; k < 4 * npix; k += 4)
  {
    const float *in = (const float *)ivoid + (size_t)k;
    float *out = (float *)ovoid + (size_t)k;
    
    const float L_in = in[0] / 100.0f;
    out[0] = (L_in < xm_L) ? table_L[CLAMP((int)(L_in * 0x10000ul), 0, 0xffff)]
                               : dt_iop_eval_exp(unbounded_coeffs, L_in);
    if(bch > 1)
      for(int j = 1; j < bch; j++)
        out[j] = L_in > 0.01f ? in[j] * out[0] / in[0] : in[j] * low_approx;
    
    out[3] = in[3];
  }
}

void run_manual_process(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                        const int npix)
{
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  const float xm_L = 1.0f / d->unbounded_coeffs_L[0];
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npix, ivoid, ovoid, xm_L)\
  shared(d) \
  schedule(static)
#endif
  for(size_t k = 0; k < 4 * npix; k += 4)
  {
    const float *in = (const float *)ivoid + (size_t)k;
    float *out = (float *)ovoid + (size_t)k;
    
    const float L_in = in[0] / 100.0f;
    out[0] = (L_in < xm_L) ? d->table[ch_L][CLAMP((int)(L_in * 0x10000ul), 0, 0xffff)]
                               : dt_iop_eval_exp(d->unbounded_coeffs_L, L_in);
    
    if(in[1] == 0.0f && in[2] == 0.0f)
      out[1] = out[2] = 0.0f;
    else
    {
      const float a_in = (in[1] + 128.0f) / 256.0f;
      const float b_in = (in[2] + 128.0f) / 256.0f;
      out[1] = d->table[ch_a][CLAMP((int)(a_in * 0x10000ul), 0, 0xffff)];
      out[2] = d->table[ch_b][CLAMP((int)(b_in * 0x10000ul), 0, 0xffff)];
    }
    out[3] = in[3];
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);

  const int npixels = roi_out->width * roi_out->height;
  const int ch = piece->colors;
  const int autoscale_ab = d->autoscale_ab;
  
  const float *unbounded_coeffs_L = d->unbounded_coeffs_L;
  const float *table_L = d->table[ch_L];

  if(autoscale_ab == DT_S_SCALE_AUTOMATIC || ch == 1)
    run_auto_process(ivoid, ovoid, ch, npixels, unbounded_coeffs_L, table_L);
  else
    run_manual_process(piece, ivoid, ovoid, npixels);
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_tonecurve_params_t p;
  memset(&p, 0, sizeof(p));
  p.tonecurve_nodes[ch_L] = 7;
  p.tonecurve_nodes[ch_a] = 7;
  p.tonecurve_nodes[ch_b] = 7;
  p.tonecurve_type[ch_L] = CUBIC_SPLINE;
  p.tonecurve_type[ch_a] = CUBIC_SPLINE;
  p.tonecurve_type[ch_b] = CUBIC_SPLINE;
  p.tonecurve_preset = 0;
  p.tonecurve_autoscale_ab = DT_S_SCALE_AUTOMATIC;
  p.tonecurve_unbound_ab = 1;
  float linear_L[7] = { 0.0, 0.08, 0.17, 0.50, 0.83, 0.92, 1.0 };

  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].y = linear_L[k];
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)p1;
  const int ch = piece->colors;
  const int bch = ch < 4 ? ch : ch - 1;

  if((pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW)
    piece->request_histogram |= (DT_REQUEST_ON);
  else
    piece->request_histogram &= ~(DT_REQUEST_ON);

  for(int c = 0; c < bch; c++)  //bch?
  {
    // take care of possible change of curve type or number of nodes (not yet implemented in UI)
    if(d->curve_type[c] != p->tonecurve_type[c] || d->curve_nodes[c] != p->tonecurve_nodes[c])
    {
      dt_draw_curve_destroy(d->curve[c]);
      d->curve[c] = dt_draw_curve_new(0.0, 1.0, p->tonecurve_type[c]);
      d->curve_nodes[c] = p->tonecurve_nodes[c];
      d->curve_type[c] = p->tonecurve_type[c];
      for(int k = 0; k < p->tonecurve_nodes[c]; k++)
        (void)dt_draw_curve_add_point(d->curve[c], p->tonecurve[c][k].x, p->tonecurve[c][k].y);
    }
    else
    { 
      for(int k = 0; k < p->tonecurve_nodes[c]; k++)
        dt_draw_curve_set_point(d->curve[c], k, p->tonecurve[c][k].x, p->tonecurve[c][k].y);
    }
    dt_draw_curve_calc_values(d->curve[c], 0.0f, 1.0f, 0x10000, NULL, d->table[c]);
  }
  
  dt_draw_curve_calc_values(d->curve[ch_L], 0.0f, 1.0f, 0x10000, NULL, d->table[0]);
  for(int k = 0; k < 0x10000; k++)
    d->table[ch_L][k] *= 100.0f;

  // extrapolation for L-curve (no extrapolation below zero!):
  const float xm_L = p->tonecurve[ch_L][p->tonecurve_nodes[ch_L] - 1].x;
  const float x_L[4] = { 0.7f * xm_L, 0.8f * xm_L, 0.9f * xm_L, 1.0f * xm_L };
  const float y_L[4] = { d->table[ch_L][CLAMP((int)(x_L[0] * 0x10000ul), 0, 0xffff)],
                         d->table[ch_L][CLAMP((int)(x_L[1] * 0x10000ul), 0, 0xffff)],
                         d->table[ch_L][CLAMP((int)(x_L[2] * 0x10000ul), 0, 0xffff)],
                         d->table[ch_L][CLAMP((int)(x_L[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x_L, y_L, 4, d->unbounded_coeffs_L);
  
  if(bch > 1)
  {
    dt_draw_curve_calc_values(d->curve[ch_a], 0.0f, 1.0f, 0x10000, NULL, d->table[ch_a]);
    dt_draw_curve_calc_values(d->curve[ch_b], 0.0f, 1.0f, 0x10000, NULL, d->table[ch_b]);
  
    for(int k = 0; k < 0x10000; k++)
    {
      d->table[ch_a][k] = d->table[ch_a][k] * 256.0f - 128.0f;
      d->table[ch_b][k] = d->table[ch_b][k] * 256.0f - 128.0f;
    }

    d->autoscale_ab = p->tonecurve_autoscale_ab;
    d->unbound_ab = p->tonecurve_unbound_ab;
  }
}

static float eval_grey(float x)
{
  return x;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the pixelpipe
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)malloc(sizeof(dt_iop_tonecurve_data_t));
  dt_iop_tonecurve_params_t *default_params = (dt_iop_tonecurve_params_t *)self->default_params;
  piece->data = (void *)d;
  const int ch = piece->colors;
  const int bch = ch < 4 ? ch : ch - 1;
  d->autoscale_ab = DT_S_SCALE_AUTOMATIC;
  d->unbound_ab = 1;
  for(int c = 0; c < bch; c++)
  {
    d->curve[c] = dt_draw_curve_new(0.0, 1.0, default_params->tonecurve_type[c]);
    d->curve_nodes[c] = default_params->tonecurve_nodes[c];
    d->curve_type[c] = default_params->tonecurve_type[c];
    for(int k = 0; k < default_params->tonecurve_nodes[c]; k++)
      (void)dt_draw_curve_add_point(d->curve[c], default_params->tonecurve[c][k].x,
                                    default_params->tonecurve[c][k].y);
  }
  for(int k = 0; k < 0x10000; k++)
  {
    d->table[ch_L][k] = 100.0f * k / 0x10000;          // identity for L

    if(ch > 1)
    {
      d->table[ch_a][k] = 256.0f * k / 0x10000 - 128.0f; // identity for a or b
      d->table[ch_b][k] = 256.0f * k / 0x10000 - 128.0f;
    }
  }
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  const int ch = piece->colors;
  const int bch = ch < 4 ? ch : ch - 1;
  for(int c = 0; c < bch; c++)
    dt_draw_curve_destroy(d->curve[c]);
  free(piece->data);
  piece->data = NULL;
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_bauhaus_combobox_set(g->interpolator, p->tonecurve_type[ch_L]);
  dt_bauhaus_slider_set(g->logbase, 0);
  g->loglogscale = 0;
  g->semilog = 0;

  g->channel = (tonecurve_channel_t)ch_L;
  gtk_widget_set_visible(g->logbase, g->channel == ch_L);

  gtk_widget_queue_draw(self->widget);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  dt_bauhaus_combobox_set_from_value(g->autoscale_ab, p->tonecurve_autoscale_ab);
  gui_changed(self, g->autoscale_ab, NULL);

  dt_bauhaus_combobox_set(g->interpolator, p->tonecurve_type[ch_L]);
  g->loglogscale = eval_grey(dt_bauhaus_slider_get(g->logbase));
  // that's all, gui curve is read directly from params during expose event.
  dt_iop_cancel_history_update(self);
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_tonecurve_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_tonecurve_params_t));
  module->default_enabled = 0;
  module->request_histogram |= (DT_REQUEST_ON);
  module->params_size = sizeof(dt_iop_tonecurve_params_t);
  module->gui_data = NULL;
  dt_iop_tonecurve_params_t tmp = (dt_iop_tonecurve_params_t){
    {  // three curves (L, a, b) with a number of nodes
      { { 0.0, 0.0 }, { 1.0, 1.0 } },
      { { 0.0, 0.0 }, { 0.5, 0.5 }, { 1.0, 1.0 } },
      { { 0.0, 0.0 }, { 0.5, 0.5 }, { 1.0, 1.0 } } },
    { 2, 3, 3 }, // number of nodes per curve
    // { CATMULL_ROM, CATMULL_ROM, CATMULL_ROM},  // curve types
    // { MONOTONE_HERMITE, MONOTONE_HERMITE, MONOTONE_HERMITE },
    { CUBIC_SPLINE, CUBIC_SPLINE, CUBIC_SPLINE},
    DT_S_SCALE_AUTOMATIC, // autoscale_ab
    0,
    1, // unbound_ab   was 1
    DT_RGB_NORM_LUMINANCE }; // preserve color = Average rgb
    
  memcpy(module->params, &tmp, sizeof(dt_iop_tonecurve_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_tonecurve_params_t));
}

static float to_log(const float x, const float base, const int semilog, const int chan, const int is_ordinate)
{
  if(base > 0.0f)
  {
    if ((semilog == 1 && is_ordinate == 1) || (semilog == -1 && is_ordinate == 0))
      return x;
    else if(chan == ch_L)
      return logf(x * base + 1.0f) / logf(base + 1.0f);
  }
  return x;
}

static float to_lin(const float x, const float base, const int semilog, const int chan, const int is_ordinate)
{
  if(base > 0.0f)
  {
    if ((semilog == 1 && is_ordinate == 1) || (semilog == -1 && is_ordinate == 0))
      return x;
    else if(chan == ch_L)
      return (powf(base + 1.0f, x) - 1.0f) / base;
  }
  return x;
}

static void logbase_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset)
    return;
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  g->loglogscale = eval_grey(dt_bauhaus_slider_get(g->logbase));
  
  gtk_widget_set_visible(g->logbase, g->channel == ch_L);
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  
  if(w == g->autoscale_ab)
  {
    g->channel = (tonecurve_channel_t)ch_L;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g->channel_tabs), ch_L);
    gtk_notebook_set_show_tabs(g->channel_tabs, p->tonecurve_autoscale_ab == DT_S_SCALE_MANUAL);
    gtk_widget_queue_draw(self->widget);
  }
}

static void interpolator_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset)
    return;
  const int ch = self->histogram_stats.ch;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  const int combo = dt_bauhaus_combobox_get(widget);
  
  if(combo == 0) p->tonecurve_type[ch_L] = CUBIC_SPLINE;
  if(combo == 1) p->tonecurve_type[ch_L] = CATMULL_ROM;
  if(combo == 2) p->tonecurve_type[ch_L] = MONOTONE_HERMITE;
  
  if(ch > 1)
  {
    if(combo == 0) p->tonecurve_type[ch_a] = p->tonecurve_type[ch_b] = CUBIC_SPLINE;
    if(combo == 1) p->tonecurve_type[ch_a] = p->tonecurve_type[ch_b] = CATMULL_ROM;
    if(combo == 2) p->tonecurve_type[ch_a] = p->tonecurve_type[ch_b] = MONOTONE_HERMITE;
  }
      
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

static void tab_switch(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  if(darktable.gui->reset)
    return;

  g->channel = (tonecurve_channel_t)page_num;
  gtk_widget_set_visible(g->logbase, g->channel == ch_L);
  gtk_widget_queue_draw(self->widget);
}

static gboolean area_resized(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  GtkRequisition r;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  r.width = allocation.width;
  r.height = allocation.width;
  gtk_widget_get_preferred_size(widget, &r, NULL);
  return TRUE;
}

void init_global(dt_iop_module_so_t *module)
{
  dt_iop_tonecurve_global_data_t *gd
      = malloc(sizeof(dt_iop_tonecurve_global_data_t));
  module->data = gd;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  free(module->data);
  module->data = NULL;
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)self->global_data;
  const int ch = piece->colors;
  const int bch = ch < 4 ? ch : ch - 1;
  
  for(int k = 0; k < bch; k++)
  {
    gd->picked_color[k] = self->picked_color[k];
    gd->picked_color_min[k] = self->picked_color_min[k];
    gd->picked_color_max[k] = self->picked_color_max[k];
    gd->picked_output_color[k] = self->picked_output_color[k];
  }
  dt_control_queue_redraw_widget(self->widget);
}

static void dt_iop_tonecurve_sanity_check(dt_iop_module_t *self, GtkWidget *widget)
{
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  int chan = c->channel;
  int nodes = p->tonecurve_nodes[chan];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[chan];
  int autoscale_ab = p->tonecurve_autoscale_ab;
  // if autoscale_ab is on: do not modify a and b curves
  if((autoscale_ab != DT_S_SCALE_MANUAL) && chan != ch_L)
    return;

  if(nodes <= 2)
    return;

  const float mx = tonecurve[c->selected].x;
  // delete vertex if order has changed
  // for all points, x coordinates monotonic
  if((c->selected > 0 && (tonecurve[c->selected - 1].x >= mx))
     || (c->selected < nodes - 1 && (tonecurve[c->selected + 1].x <= mx)))
  {
    for(int k = c->selected; k < nodes - 1; k++)
    {
      tonecurve[k].x = tonecurve[k + 1].x;
      tonecurve[k].y = tonecurve[k + 1].y;
    }
    c->selected = -2; // avoid re-insertion of that point immediately after this
    p->tonecurve_nodes[chan]--;
  }
}

static gboolean _move_point_internal(dt_iop_module_t *self, GtkWidget *widget, float dx, float dy, guint state)
{
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  int chan = c->channel;
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[chan];
  float multiplier;

  GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
  
  if((state & modifiers) == GDK_SHIFT_MASK)
    multiplier = dt_conf_get_float("darkroom/ui/scale_rough_step_multiplier");
  else if((state & modifiers) == GDK_CONTROL_MASK)
    multiplier = dt_conf_get_float("darkroom/ui/scale_precise_step_multiplier");
  else
    multiplier = dt_conf_get_float("darkroom/ui/scale_step_multiplier");

  dx *= multiplier;
  dy *= multiplier;

  tonecurve[c->selected].x = CLAMP(tonecurve[c->selected].x + dx, 0.0f, 1.0f);
  tonecurve[c->selected].y = CLAMP(tonecurve[c->selected].y + dy, 0.0f, 1.0f);

  dt_iop_tonecurve_sanity_check(self, widget);
  gtk_widget_queue_draw(widget);
  dt_iop_queue_history_update(self, FALSE);
  
  return TRUE;
}

#define TONECURVE_DEFAULT_STEP (0.001f)

static gboolean _scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;

  if(dt_gui_ignore_scroll(event))
    return FALSE;

  int ch = c->channel;
  int autoscale_ab = p->tonecurve_autoscale_ab;
  
  if((autoscale_ab != DT_S_SCALE_MANUAL) && ch != ch_L)
    return TRUE;

  if(c->selected < 0)
    return TRUE;

  gdouble delta_y;
  if(dt_gui_get_scroll_delta(event, &delta_y))
  {
    delta_y *= -TONECURVE_DEFAULT_STEP;
    return _move_point_internal(self, widget, 0.0, delta_y, event->state);
  }

  return TRUE;
}

static gboolean dt_iop_tonecurve_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;

  int ch = c->channel;
  int autoscale_ab = p->tonecurve_autoscale_ab;

  if((autoscale_ab != DT_S_SCALE_MANUAL) && ch != ch_L)
    return TRUE;

  if(c->selected < 0)
    return TRUE;

  int handled = 0;
  float dx = 0.0f, dy = 0.0f;
  if(event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up)
  {
    handled = 1;
    dy = TONECURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down)
  {
    handled = 1;
    dy = -TONECURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Right || event->keyval == GDK_KEY_KP_Right)
  {
    handled = 1;
    dx = TONECURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Left || event->keyval == GDK_KEY_KP_Left)
  {
    handled = 1;
    dx = -TONECURVE_DEFAULT_STEP;
  }
  if(!handled)
    return TRUE;

  return _move_point_internal(self, widget, dx, dy, event->state);
}

#undef TONECURVE_DEFAULT_STEP

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_tonecurve_gui_data_t));
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  const int ch = 4; ////self->histogram_stats.ch;  should be ok for initial setup/////////
  const int bch = ch < 4 ? ch : ch -1;

  for(int chan = 0; chan < bch; chan++)
  {
    c->minmax_curve[chan] = dt_draw_curve_new(0.0, 1.0, p->tonecurve_type[chan]);
    c->minmax_curve_nodes[chan] = p->tonecurve_nodes[chan];
    c->minmax_curve_type[chan] = p->tonecurve_type[chan];
    for(int k = 0; k < p->tonecurve_nodes[chan]; k++)
      (void)dt_draw_curve_add_point(c->minmax_curve[chan], p->tonecurve[chan][k].x, p->tonecurve[chan][k].y);
  }

  c->channel = ch_L;
  c->mouse_x = c->mouse_y = -1.0;
  c->selected = -1;
  c->loglogscale = 0;
  c->semilog = 0;
  self->timeout_handle = 0;

  c->autoscale_ab = dt_bauhaus_combobox_from_params(self, "tonecurve_autoscale_ab");
  gtk_widget_set_tooltip_text(c->autoscale_ab, _("if set to auto, a and b curves have no effect and are "
                                                 "not displayed. chroma values (a and b) of each pixel are "
                                                 "then adjusted based on L curve data.")); 
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  c->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());
  dt_ui_notebook_page(c->channel_tabs, _("L"), _("tonecurve for L channel"));
  dt_ui_notebook_page(c->channel_tabs, _("a"), _("tonecurve for a channel"));
  dt_ui_notebook_page(c->channel_tabs, _("b"), _("tonecurve for b channel"));
  g_signal_connect(G_OBJECT(c->channel_tabs), "switch_page", G_CALLBACK(tab_switch), self);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(c->channel_tabs), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), gtk_grid_new(), TRUE, TRUE, 0);

  c->colorpicker = dt_color_picker_new(self, DT_COLOR_PICKER_POINT_AREA, hbox);
  gtk_widget_set_tooltip_text(c->colorpicker, _("ctrl+click to select an area"));

  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);

  c->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->area), TRUE, TRUE, 0);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                                 | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                 | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK
                                                 | darktable.gui->scroll_mask);
  gtk_widget_set_can_focus(GTK_WIDGET(c->area), TRUE);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(dt_iop_tonecurve_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(dt_iop_tonecurve_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(dt_iop_tonecurve_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(dt_iop_tonecurve_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "enter-notify-event", G_CALLBACK(dt_iop_tonecurve_enter_notify), self);
  g_signal_connect(G_OBJECT(c->area), "configure-event", G_CALLBACK(area_resized), self);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(_scrolled), self);
  g_signal_connect(G_OBJECT(c->area), "key-press-event", G_CALLBACK(dt_iop_tonecurve_key_press), self);

  /* From src/common/curve_tools.h :
    #define CUBIC_SPLINE 0
    #define CATMULL_ROM 1
    #define MONOTONE_HERMITE 2
  */
  c->interpolator = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(c->interpolator, NULL, _("interpolation method"));
  dt_bauhaus_combobox_add(c->interpolator, _("cubic spline"));
  dt_bauhaus_combobox_add(c->interpolator, _("centripetal spline"));
  dt_bauhaus_combobox_add(c->interpolator, _("monotonic spline"));
  gtk_box_pack_start(GTK_BOX(self->widget), c->interpolator , TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(c->interpolator, _("change this method if you see oscillations or cusps in the curve\n"
                                                 "- cubic spline is better to produce smooth curves but oscillates when nodes are too close\n"
                                                 "- centripetal is better to avoids cusps and oscillations with close nodes but is less smooth\n"
                                                 "- monotonic is better for accuracy of pure analytical functions (log, gamma, exp)\n"));
  g_signal_connect(G_OBJECT(c->interpolator), "value-changed", G_CALLBACK(interpolator_callback), self);

  c->logbase = dt_bauhaus_slider_new_with_range(self, 0.0f, MAX_LOG_BASE, 0.5f, 0.0f, 2);
  dt_bauhaus_widget_set_label(c->logbase, NULL, _("scale for graph"));
  gtk_box_pack_start(GTK_BOX(self->widget), c->logbase , TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(c->logbase), "value-changed", G_CALLBACK(logbase_callback), self);
  gtk_widget_set_visible(c->logbase, c->channel==ch_L);

  c->sizegroup = GTK_SIZE_GROUP(gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL));
  gtk_size_group_add_widget(c->sizegroup, GTK_WIDGET(c->area));
  if(c->autoscale_ab)
    gtk_size_group_add_widget(c->sizegroup, GTK_WIDGET(c->channel_tabs));
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  // this one we need to unref manually. not so the initially un-owned widgets.
  g_object_unref(c->sizegroup);
  const int ch = (int)self->histogram_stats.ch;
  dt_draw_curve_destroy(c->minmax_curve[ch_L]);
  if(ch > 1 || c->minmax_curve[ch_a] || c->minmax_curve[ch_b])
  {
    dt_draw_curve_destroy(c->minmax_curve[ch_a]);
    dt_draw_curve_destroy(c->minmax_curve[ch_b]);
  }
  dt_iop_cancel_history_update(self);
  free(self->gui_data);
  self->gui_data = NULL;
}

static gboolean dt_iop_tonecurve_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_tonecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static void picker_scale(const float *in, float *out, const int bch)
{
  out[0] = CLAMP(in[0] / 100.0f, 0.0f, 1.0f);
  for(int c = 1; c < bch; c++)
    out[c] = CLAMP((in[c] + 128.0f) / 256.0f, 0.0f, 1.0f);
}

static gboolean draw_picker_helper(gpointer user_data, double height, double width,
                                        cairo_surface_t *cst, cairo_t *cr)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)self->global_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  
  int chan = c->channel;
  const int bch = chan > 1 ? 3 : 1;
  
  if(!self->enabled)
    return FALSE;

  char text[256];
  float *raw_mean, *raw_min, *raw_max;
  float *raw_mean_output;
  float picker_mean[3] = { 0.0f }, picker_min[3] = { 0.0f }, picker_max[3] = { 0.0f };
  
  raw_mean = gd->picked_color;
  raw_min = gd->picked_color_min;
  raw_max = gd->picked_color_max;
  raw_mean_output = gd->picked_output_color;

  cairo_move_to(cr, 0, height);
  
  if(self->request_color_pick == DT_REQUEST_COLORPICK_MODULE &&
     gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c->colorpicker)))
  {
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3));

    if(raw_max[0] >= 0.0f)
    {
      cairo_save(cr);
      PangoLayout *layout;
      PangoRectangle ink;
      PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
      pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
      pango_font_description_set_absolute_size(desc, PANGO_SCALE);
      layout = pango_cairo_create_layout(cr);
      pango_layout_set_font_description(layout, desc);

      picker_scale(raw_mean, picker_mean, bch);
      picker_scale(raw_min, picker_min, bch);
      picker_scale(raw_max, picker_max, bch);

      picker_min[chan] = to_log(picker_min[chan], c->loglogscale, c->semilog, chan, 0);
      picker_max[chan] = to_log(picker_max[chan], c->loglogscale, c->semilog, chan, 0);
      picker_mean[chan] = to_log(picker_mean[chan], c->loglogscale, c->semilog, chan, 0);

      cairo_set_source_rgba(cr, 0.7, 0.5, 0.5, 0.35);
      cairo_rectangle(cr, width * picker_min[chan], 0, width * fmax(picker_max[chan] - picker_min[chan], 0.0f),
                        -height);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, 0.9, 0.7, 0.7, 0.5);
      cairo_move_to(cr, width * picker_mean[chan], 0);
      cairo_line_to(cr, width * picker_mean[chan], -height);
      cairo_stroke(cr);

      snprintf(text, sizeof(text), "%.1f → %.1f", raw_mean[chan], raw_mean_output[chan]);
      set_color(cr, darktable.bauhaus->graph_fg);
      cairo_set_font_size(cr, DT_PIXEL_APPLY_DPI(0.04) * height);
      pango_layout_set_text(layout, text, -1);
      pango_layout_get_pixel_extents(layout, &ink, NULL);
      cairo_move_to(cr, 0.02f * width, -0.94 * height - ink.height - ink.y);
      pango_cairo_show_layout(cr, layout);
      cairo_stroke(cr);

      pango_font_description_free(desc);
      g_object_unref(layout);
      cairo_restore(cr);
    }
  }
  
  return TRUE;
}

static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  int chan = c->channel;
  int nodes = p->tonecurve_nodes[chan];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[chan];

  if(c->minmax_curve_type[chan] != p->tonecurve_type[chan] || c->minmax_curve_nodes[chan] != p->tonecurve_nodes[chan])
  {
    dt_draw_curve_destroy(c->minmax_curve[chan]);

    c->minmax_curve[chan] = dt_draw_curve_new(0.0, 1.0, p->tonecurve_type[chan]);
    c->minmax_curve_nodes[chan] = p->tonecurve_nodes[chan];
    c->minmax_curve_type[chan] = p->tonecurve_type[chan];
    for(int k = 0; k < p->tonecurve_nodes[chan]; k++)
      (void)dt_draw_curve_add_point(c->minmax_curve[chan], p->tonecurve[chan][k].x, p->tonecurve[chan][k].y);
  }
  else
  {
    for(int k = 0; k < p->tonecurve_nodes[chan]; k++)
      dt_draw_curve_set_point(c->minmax_curve[chan], k, p->tonecurve[chan][k].x, p->tonecurve[chan][k].y);
  }
  dt_draw_curve_t *minmax_curve = c->minmax_curve[chan];
  dt_draw_curve_calc_values(minmax_curve, 0.0, 1.0, DT_IOP_TONECURVE_RES, c->draw_xs, c->draw_ys);

  float unbounded_coeffs[3] = { 0 };
  const float xm = tonecurve[nodes - 1].x;

  if(chan == ch_L)
  {
    const float x[4] = { 0.7f * xm, 0.8f * xm, 0.9f * xm, 1.0f * xm };
    
    const float y[4] = { c->draw_ys[CLAMP((int)(x[0] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)],
                         c->draw_ys[CLAMP((int)(x[1] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)],
                         c->draw_ys[CLAMP((int)(x[2] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)],
                         c->draw_ys[CLAMP((int)(x[3] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)] };
    dt_iop_estimate_exp(x, y, 4, unbounded_coeffs);
  }

  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;
  char text[256];

  // Draw frame borders
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke_preserve(cr);

  cairo_set_source_rgb(cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);
  // draw grid
  set_color(cr, darktable.bauhaus->graph_border);
  
  if (c->loglogscale > 0.0f && chan == ch_L)
  {
    if (c->semilog == 0)
      dt_draw_loglog_grid(cr, 4, 0, height, width, 0, c->loglogscale + 1.0f);
    else if (c->semilog == 1)
      dt_draw_semilog_x_grid(cr, 4, 0, height, width, 0, c->loglogscale + 1.0f);
    else if (c->semilog == -1)
      dt_draw_semilog_y_grid(cr, 4, 0, height, width, 0, c->loglogscale + 1.0f);
  }
  else
    dt_draw_grid(cr, 4, 0, 0, width, height);

  // draw identity line
  cairo_move_to(cr, 0, height);
  cairo_line_to(cr, width, 0);
  cairo_stroke(cr);
  cairo_translate(cr, 0, height);
  //draw_histogram_helper(user_data, height, width, cst, cr);//////////////////////
  draw_picker_helper(user_data, height, width, cst, cr);
  // draw curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3));
  set_color(cr, darktable.bauhaus->graph_fg);

  for(int k = 0; k < DT_IOP_TONECURVE_RES; k++)
  {
    const float xx = k / (DT_IOP_TONECURVE_RES - 1.0f);
    float yy;

    if(xx > xm)
      yy = dt_iop_eval_exp(unbounded_coeffs, xx);
    else
      yy = c->draw_ys[k];

    const float x = to_log(xx, c->loglogscale, c->semilog, chan, 0),
                y = to_log(yy, c->loglogscale, c->semilog, chan, 1);

    cairo_line_to(cr, x * width, -height * y);
  }
  cairo_stroke(cr);

  // draw node positions
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3));
  for(int k = 0; k < nodes; k++)
  {
    const float x = to_log(tonecurve[k].x, c->loglogscale, c->semilog, chan, 0),
                y = to_log(tonecurve[k].y, c->loglogscale, c->semilog, chan, 1);

    cairo_arc(cr, x * width, -y * height, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
    set_color(cr, darktable.bauhaus->graph_fg);
    cairo_stroke_preserve(cr);
    set_color(cr, darktable.bauhaus->graph_bg);
    cairo_fill(cr);
  }
  // draw selected cursor
  if(c->selected >= 0)
  {
    // draw information about current selected node
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, PANGO_SCALE);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);

    // scale conservatively to 100% of width:
    snprintf(text, sizeof(text), "100.00 / 100.00 ( +100.00)");
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    pango_font_description_set_absolute_size(desc, width*1.0/ink.width * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);

    const float min_scale_value = chan == ch_L ? 0.0f : -128.0f;
    const float max_scale_value = chan == ch_L ? 100.0f : 128.0f;

    const float x_node_value = tonecurve[c->selected].x * (max_scale_value - min_scale_value) + min_scale_value;
    const float y_node_value = tonecurve[c->selected].y * (max_scale_value - min_scale_value) + min_scale_value;
    const float d_node_value = y_node_value - x_node_value;
    snprintf(text, sizeof(text), "%.1f / %.1f ( %+.1f)", x_node_value, y_node_value, d_node_value);

    set_color(cr, darktable.bauhaus->graph_fg);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, 0.98f * width - ink.width - ink.x, -0.02 * height - ink.height - ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);

    // enlarge selected node
    set_color(cr, darktable.bauhaus->graph_fg_active);
    const float x = to_log(tonecurve[c->selected].x, c->loglogscale, c->semilog, chan, 0),
                y = to_log(tonecurve[c->selected].y, c->loglogscale, c->semilog, chan, 1);

    cairo_arc(cr, x * width, -y * height, DT_PIXEL_APPLY_DPI(6), 0, 2. * M_PI);
    cairo_fill(cr);
  }

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static inline int _add_node(dt_iop_tonecurve_node_t *tonecurve, int *nodes, float x, float y)
{
  int selected = -1;
  
  if(tonecurve[0].x > x)
    selected = 0;
  else
    for(int k = 1; k < *nodes; k++)
      if(tonecurve[k].x > x)
      {
        selected = k;
        break;
      }

  if(selected == -1) selected = *nodes;
  
  for(int i = *nodes; i > selected; i--)
  {
    tonecurve[i].x = tonecurve[i - 1].x;
    tonecurve[i].y = tonecurve[i - 1].y;
  }
  // found a new point
  tonecurve[selected].x = x;
  tonecurve[selected].y = y;
  (*nodes)++;
  return selected;
}

static gboolean dt_iop_tonecurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  int chan = c->channel;
  int nodes = p->tonecurve_nodes[chan];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[chan];
  int autoscale_ab = p->tonecurve_autoscale_ab;

  // if autoscale_ab is on: do not modify a and b curves
  if((autoscale_ab != DT_S_SCALE_MANUAL) && chan != ch_L)
    goto finally;

  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
  double old_m_x = c->mouse_x;
  double old_m_y = c->mouse_y;
  c->mouse_x = event->x - inset;
  c->mouse_y = event->y - inset;

  const float mx = CLAMP(c->mouse_x, 0, width) / width;
  const float my = 1.0f - CLAMP(c->mouse_y, 0, height) / height;
  const float linx = to_lin(mx, c->loglogscale, c->semilog, chan, 0),
              liny = to_lin(my, c->loglogscale, c->semilog, chan, 1);

  if(event->state & GDK_BUTTON1_MASK)
  {
    // got a vertex selected:
    if(c->selected >= 0)
    {
      // this is used to translate mouse position in loglogscale to make this behavior unified with linear scale.
      const float translate_mouse_x = old_m_x / width
                    - to_log(tonecurve[c->selected].x, c->loglogscale, c->semilog, chan, 0);
      const float translate_mouse_y = 1 - old_m_y / height
                    - to_log(tonecurve[c->selected].y, c->loglogscale, c->semilog, chan, 1);
      // dx & dy are in linear coordinates
      const float dx = to_lin(c->mouse_x / width - translate_mouse_x, c->loglogscale, c->semilog, chan, 0)
                        - to_lin(old_m_x / width - translate_mouse_x, c->loglogscale, c->semilog, chan, 0);
      const float dy = to_lin(1 - c->mouse_y / height - translate_mouse_y, c->loglogscale, c->semilog, chan, 1)
                        - to_lin(1 - old_m_y / height - translate_mouse_y, c->loglogscale, c->semilog, chan, 1);
      return _move_point_internal(self, widget, dx, dy, event->state);
    }
    else if(nodes < DT_IOP_TONECURVE_MAXNODES && c->selected >= -1)
    {
      // no vertex was close, create a new one!
      c->selected = _add_node(tonecurve, &p->tonecurve_nodes[chan], linx, liny);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
  }
  else
  {
    // minimum area around the node to select it:
    float min = .04f;
    min *= min; // comparing against square
    int nearest = -1;
    for(int k = 0; k < nodes; k++)
    {
      float dist
          = (my - to_log(tonecurve[k].y, c->loglogscale, c->semilog, chan, 1))
          * (my - to_log(tonecurve[k].y, c->loglogscale, c->semilog, chan, 1))
          + (mx - to_log(tonecurve[k].x, c->loglogscale, c->semilog, chan, 0))
          * (mx - to_log(tonecurve[k].x, c->loglogscale, c->semilog, chan, 0));

      if(dist < min)
      {
        min = dist;
        nearest = k;
      }
    }
    c->selected = nearest;
  }
finally:
  if(c->selected >= 0) gtk_widget_grab_focus(widget);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_tonecurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_params_t *d = (dt_iop_tonecurve_params_t *)self->default_params;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;

  int chan = c->channel;
  int autoscale_ab = p->tonecurve_autoscale_ab;
  int nodes = p->tonecurve_nodes[chan];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[chan];

  if(event->button == 1)
  {
    if(event->type == GDK_BUTTON_PRESS && (event->state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK
       && nodes < DT_IOP_TONECURVE_MAXNODES && c->selected == -1)
    {
      // if we are not on a node -> add a new node at the current x of the pointer and y of the curve at that x
      const int inset = DT_GUI_CURVE_EDITOR_INSET;
      GtkAllocation allocation;
      gtk_widget_get_allocation(widget, &allocation);
      int width = allocation.width - 2 * inset;
      c->mouse_x = event->x - inset;
      c->mouse_y = event->y - inset;

      const float mx = CLAMP(c->mouse_x, 0, width) / (float)width;
      const float linx = to_lin(mx, c->loglogscale, c->semilog, chan, 0);

      // don't add a node too close to others in x direction, it can crash dt
      int selected = -1;
      if(tonecurve[0].x > mx)
        selected = 0;
      else
      {
        for(int k = 1; k < nodes; k++)
        {
          if(tonecurve[k].x > mx)
          {
            selected = k;
            break;
          }
        }
      }
      if(selected == -1) selected = nodes;
      // > 0 -> check distance to left neighbour
      // < nodes -> check distance to right neighbour
      if(!((selected > 0 && linx - tonecurve[selected - 1].x <= 0.025) ||
           (selected < nodes && tonecurve[selected].x - linx <= 0.025)))
      {
        // evaluate the curve at the current x position
        const float y = dt_draw_curve_calc_value(c->minmax_curve[chan], linx);

        if(y >= 0.0 && y <= 1.0) // never add something outside the viewport, you couldn't change it afterwards
        {
          // create a new node
          selected = _add_node(tonecurve, &p->tonecurve_nodes[chan], linx, y);

          // maybe set the new one as being selected
          float min = .04f;
          min *= min; // comparing against square
          for(int k = 0; k < nodes; k++)
          {
            float other_y = to_log(tonecurve[k].y, c->loglogscale, c->semilog, chan, 1);
            float dist = (y - other_y) * (y - other_y);
            if(dist < min) c->selected = selected;
          }

          dt_dev_add_history_item(darktable.develop, self, TRUE);
          gtk_widget_queue_draw(self->widget);
        }
      }
      return TRUE;
    }
    else if(event->type == GDK_2BUTTON_PRESS)
    {
      // reset current curve
      // if autoscale_ab is on: allow only reset of L curve
      if((autoscale_ab == DT_S_SCALE_MANUAL) || chan == ch_L)
      {
        p->tonecurve_nodes[chan] = d->tonecurve_nodes[chan];
        p->tonecurve_type[chan] = d->tonecurve_type[chan];
        for(int k = 0; k < d->tonecurve_nodes[chan]; k++)
        {
          p->tonecurve[chan][k].x = d->tonecurve[chan][k].x;
          p->tonecurve[chan][k].y = d->tonecurve[chan][k].y;
        }
        c->selected = -2; // avoid motion notify re-inserting immediately.
        dt_bauhaus_combobox_set(c->interpolator, p->tonecurve_type[ch_L]);
        dt_dev_add_history_item(darktable.develop, self, TRUE);
        gtk_widget_queue_draw(self->widget);
      }
      else
      { // hence chan != Ch_L
        p->tonecurve_autoscale_ab = DT_S_SCALE_MANUAL;
        c->selected = -2; // avoid motion notify re-inserting immediately.
        dt_bauhaus_combobox_set(c->autoscale_ab, 1);
        dt_dev_add_history_item(darktable.develop, self, TRUE);
        gtk_widget_queue_draw(self->widget);
      }
      return TRUE;
    }
  }
  else if(event->button == 3 && c->selected >= 0)
  {
    if(c->selected == 0 || c->selected == nodes - 1)
    {
      float reset_value = c->selected == 0 ? 0 : 1;
      tonecurve[c->selected].y = tonecurve[c->selected].x = reset_value;
      gtk_widget_queue_draw(self->widget);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      return TRUE;
    }

    for(int k = c->selected; k < nodes - 1; k++)
    {
      tonecurve[k].x = tonecurve[k + 1].x;
      tonecurve[k].y = tonecurve[k + 1].y;
    }
    tonecurve[nodes - 1].x = tonecurve[nodes - 1].y = 0;
    c->selected = -2; // avoid re-insertion of that point immediately after this
    p->tonecurve_nodes[chan]--;
    gtk_widget_queue_draw(self->widget);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return TRUE;
  }
  return FALSE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
