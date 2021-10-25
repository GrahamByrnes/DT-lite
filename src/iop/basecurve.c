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
#include "common/colorspaces_inline_conversions.h"
#include "common/debug.h"
#include "common/rgb_norms.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "dtgtk/drawingarea.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(5)
#define DT_GUI_CURVE_INFL .3f
#define DT_IOP_TONECURVE_RES 256
#define MAXNODES 20


DT_MODULE_INTROSPECTION(6, dt_iop_basecurve_params_t)

typedef struct dt_iop_basecurve_node_t
{
  float x; // $MIN: 0.0 $MAX: 1.0
  float y; // $MIN: 0.0 $MAX: 1.0
} dt_iop_basecurve_node_t;

typedef struct dt_iop_basecurve_params_t
{
  // three curves (c, ., .) with max number of nodes
  // the other two are reserved, maybe we'll have cam rgb at some point.
  dt_iop_basecurve_node_t basecurve[3][MAXNODES];
  int basecurve_nodes[3]; // $MIN: 0 $MAX: MAXNODES $DEFAULT: 0
  int basecurve_type[3];  // $MIN: 0 $MAX: MONOTONE_HERMITE $DEFAULT: MONOTONE_HERMITE
  int exposure_fusion;    /* number of exposure fusion steps
                             $DEFAULT: 0 $DESCRIPTION: "fusion" */
  float exposure_stops;   /* number of stops between fusion images
                             $MIN: 0.01 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "exposure shift" */
  float exposure_bias;    /* whether to do exposure-fusion with over or under-exposure
                             $MIN: -1.0 $MAX: 1.0 $DEFAULT: 1.0 $DESCRIPTION: "exposure bias" */
  int preserve_colors; /* $DEFAULT: DT_RGB_NORM_LUMINANCE $DESCRIPTION: "preserve colors" */
} dt_iop_basecurve_params_t;

typedef struct dt_iop_basecurve_gui_data_t
{
  dt_draw_curve_t *minmax_curve; // curve for gui to draw
  int minmax_curve_type, minmax_curve_nodes;
  GtkBox *hbox;
  GtkDrawingArea *area;
  GtkWidget *cmb_preserve_colors;
  double mouse_x, mouse_y;
  int selected;
  double selected_offset, selected_y, selected_min, selected_max;
  float draw_xs[DT_IOP_TONECURVE_RES], draw_ys[DT_IOP_TONECURVE_RES];
  float draw_min_xs[DT_IOP_TONECURVE_RES], draw_min_ys[DT_IOP_TONECURVE_RES];
  float draw_max_xs[DT_IOP_TONECURVE_RES], draw_max_ys[DT_IOP_TONECURVE_RES];
  float loglogscale;
  GtkWidget *logbase;
} dt_iop_basecurve_gui_data_t;

typedef struct basecurve_preset_t
{
  const char *name;
  const char *maker;
  const char *model;
  int iso_min;
  float iso_max;
  dt_iop_basecurve_params_t params;
  int autoapply;
  int filter;
} basecurve_preset_t;

static const basecurve_preset_t basecurve_presets[] = {
  // clang-format off
  // smoother cubic spline curve
  { N_("cubic spline"), "", "", 0, FLT_MAX,
    { { { { 0.0, 0.0}, { 1.0, 1.0 }, { 0., 0.}, { 0., 0.}, { 0., 0.}, { 0., 0.}, { 0., 0.}, { 0., 0.} } },
    { 2 }, { CUBIC_SPLINE }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  // clang-format on
};

static const int basecurve_presets_cnt = sizeof(basecurve_presets) / sizeof(basecurve_preset_t);

typedef struct dt_iop_basecurve_data_t
{
  dt_draw_curve_t *curve; // curve for pixelpipe piece and pixel processing
  int basecurve_type;
  int basecurve_nodes;
  float table[0x10000];      // precomputed look-up table for tone curve
  float unbounded_coeffs[3]; // approximation for extrapolation
  int exposure_fusion;
  float exposure_stops;
  float exposure_bias;
  int preserve_colors;
} dt_iop_basecurve_data_t;

const char *name()
{
  return _("base curve");
}

const char *description()
{
  return _("apply a view transform, works in RGB,\n"
           "takes preferably a linear RGB input,\n"
           "outputs non-linear RGB.");
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

static inline void apply_curve(
    const float *const in,
    float *const out,
    const int npix,
    const int preserve_colors,
    const float *const table,
    const float *const unbounded_coeffs,
    const int ch)
{
  const int bch = ch < 4 ? ch : ch - 1;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, out, npix, table, unbounded_coeffs, preserve_colors, bch) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)4 * npix; k += 4)
  {
    const float *inp = in + (size_t)k;
    float *outp = out + (size_t)k;
    float ratio = 1.0f;
    const float lum = bch == 1 ? in[0] : 0.21f * in[0] + 0.72f * in[1] + 0.07f * in[2];
    
    if(lum > 0.0f)
    {
      const float curve_lum = (lum < 1.0f)
        ? table[CLAMP((int)(lum * 0x10000ul), 0, 0xffff)]
        : dt_iop_eval_exp(unbounded_coeffs, lum);
      ratio = curve_lum / lum;
    }

    for(size_t c = 0; c < bch; c++)
      outp[c] = ratio * inp[c];
      
    outp[3] = inp[3];
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;
  const int ch = piece->colors;
  
  dt_iop_basecurve_data_t *const d = (dt_iop_basecurve_data_t *)(piece->data);
  const int npixels = roi_in->width * roi_in->height;
  apply_curve(in, out, npixels, d->preserve_colors, d->table, d->unbounded_coeffs, ch);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_basecurve_data_t *d = (dt_iop_basecurve_data_t *)(piece->data);
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)p1;

  d->exposure_fusion = p->exposure_fusion;
  d->exposure_stops = p->exposure_stops;
  d->exposure_bias = p->exposure_bias;
  d->preserve_colors = p->preserve_colors;

  const int c = 0;
  // take care of possible change of curve type or number of nodes (not yet implemented in UI)
  if(d->basecurve_type != p->basecurve_type[c] || d->basecurve_nodes != p->basecurve_nodes[c])
  {
    if(d->curve) // catch initial init_pipe case
      dt_draw_curve_destroy(d->curve);
    d->curve = dt_draw_curve_new(0.0, 1.0, p->basecurve_type[c]);
    d->basecurve_nodes = p->basecurve_nodes[c];
    d->basecurve_type = p->basecurve_type[c];
    for(int k = 0; k < p->basecurve_nodes[c]; k++)
      (void)dt_draw_curve_add_point(d->curve, p->basecurve[c][k].x, p->basecurve[c][k].y);
  }
  else
  {
    for(int k = 0; k < p->basecurve_nodes[c]; k++)
      dt_draw_curve_set_point(d->curve, k, p->basecurve[c][k].x, p->basecurve[c][k].y);
  }
  dt_draw_curve_calc_values(d->curve, 0.0f, 1.0f, 0x10000, NULL, d->table);

  // now the extrapolation stuff:
  const float xm = p->basecurve[0][p->basecurve_nodes[0] - 1].x;
  const float x[4] = { 0.7f * xm, 0.8f * xm, 0.9f * xm, 1.0f * xm };
  const float y[4] = { d->table[CLAMP((int)(x[0] * 0x10000ul), 0, 0xffff)],
                       d->table[CLAMP((int)(x[1] * 0x10000ul), 0, 0xffff)],
                       d->table[CLAMP((int)(x[2] * 0x10000ul), 0, 0xffff)],
                       d->table[CLAMP((int)(x[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the pixelpipe
  piece->data = calloc(1, sizeof(dt_iop_basecurve_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  dt_iop_basecurve_data_t *d = (dt_iop_basecurve_data_t *)(piece->data);
  dt_draw_curve_destroy(d->curve);
  free(piece->data);
  piece->data = NULL;
}

static float eval_grey(float x)
{  
  return x;
}

void gui_update(struct dt_iop_module_t *self)
{
  //dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  dt_iop_basecurve_gui_data_t *g = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  //dt_bauhaus_combobox_set(g->cmb_preserve_colors, p->preserve_colors);
  g->loglogscale = eval_grey(dt_bauhaus_slider_get(g->logbase));
  dt_iop_cancel_history_update(self); 
  // gui curve is read directly from params during expose event.
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);
  dt_iop_basecurve_params_t *d = module->default_params;
  d->basecurve[0][1].x = d->basecurve[0][1].y = 1.0;
  d->basecurve_nodes[0] = 2;
  memcpy(module->params, module->default_params, sizeof(dt_iop_basecurve_params_t));
}

static gboolean dt_iop_basecurve_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_basecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static float to_log(const float x, const float base)
{
  if(base > 0.0f)
    return logf(x * base + 1.0f) / logf(base + 1.0f);
  else
    return x;
}

static float to_lin(const float x, const float base)
{
  if(base > 0.0f)
    return (powf(base + 1.0f, x) - 1.0f) / base;
  else
    return x;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous);

static void logbase_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *g = (dt_iop_basecurve_gui_data_t *)self->gui_data;

  g->loglogscale = eval_grey(dt_bauhaus_slider_get(g->logbase));
  
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

static gboolean dt_iop_basecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;

  int nodes = p->basecurve_nodes[0];
  dt_iop_basecurve_node_t *basecurve = p->basecurve[0];
  if(c->minmax_curve_type != p->basecurve_type[0] || c->minmax_curve_nodes != p->basecurve_nodes[0])
  {
    dt_draw_curve_destroy(c->minmax_curve);
    c->minmax_curve = dt_draw_curve_new(0.0, 1.0, p->basecurve_type[0]);
    c->minmax_curve_nodes = p->basecurve_nodes[0];
    c->minmax_curve_type = p->basecurve_type[0];
    for(int k = 0; k < p->basecurve_nodes[0]; k++)
      (void)dt_draw_curve_add_point(c->minmax_curve, p->basecurve[0][k].x, p->basecurve[0][k].y);
  }
  else
  {
    for(int k = 0; k < p->basecurve_nodes[0]; k++)
      dt_draw_curve_set_point(c->minmax_curve, k, p->basecurve[0][k].x, p->basecurve[0][k].y);
  }
  dt_draw_curve_t *minmax_curve = c->minmax_curve;
  dt_draw_curve_calc_values(minmax_curve, 0.0, 1.0, DT_IOP_TONECURVE_RES, c->draw_xs, c->draw_ys);

  float unbounded_coeffs[3];
  const float xm = basecurve[nodes - 1].x;
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
  // clear bg
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;
/*
#if 0
  // draw shadow around
  float alpha = 1.0f;
  for(int k=0; k<inset; k++)
  {
    cairo_rectangle(cr, -k, -k, width + 2*k, height + 2*k);
    cairo_set_source_rgba(cr, 0, 0, 0, alpha);
    alpha *= 0.6f;
    cairo_fill(cr);
  }
#else */
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  cairo_set_source_rgb(cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);
//#endif

  cairo_set_source_rgb(cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  cairo_translate(cr, 0, height);
  if(c->selected >= 0)
  {
    char text[30];
    // draw information about current selected node
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, PANGO_SCALE);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);

    const float x_node_value = basecurve[c->selected].x * 100;
    const float y_node_value = basecurve[c->selected].y * 100;
    const float d_node_value = y_node_value - x_node_value;
    // scale conservatively to 100% of width:
    snprintf(text, sizeof(text), "100.00 / 100.00 ( +100.00)");
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    pango_font_description_set_absolute_size(desc, (double)width / ink.width * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);

    snprintf(text, sizeof(text), "%.2f / %.2f ( %+.2f)", x_node_value, y_node_value, d_node_value);

    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, 0.98f * width - ink.width - ink.x, -0.02 * height - ink.height - ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }
  cairo_scale(cr, 1.0f, -1.0f);

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  cairo_set_source_rgb(cr, .1, .1, .1);
  if(c->loglogscale)
    dt_draw_loglog_grid(cr, 4, 0, 0, width, height, c->loglogscale + 1.0f);
  else
    dt_draw_grid(cr, 4, 0, 0, width, height);

  // draw nodes positions
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  for(int k = 0; k < nodes; k++)
  {
    const float x = to_log(basecurve[k].x, c->loglogscale), y = to_log(basecurve[k].y, c->loglogscale);
    cairo_arc(cr, x * width, y * height, DT_PIXEL_APPLY_DPI(3), 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw selected cursor
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));

  if(c->selected >= 0)
  {
    cairo_set_source_rgb(cr, .9, .9, .9);
    const float x = to_log(basecurve[c->selected].x, c->loglogscale),
                y = to_log(basecurve[c->selected].y, c->loglogscale);
    cairo_arc(cr, x * width, y * height, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_set_source_rgb(cr, .9, .9, .9);
  // cairo_set_line_cap  (cr, CAIRO_LINE_CAP_SQUARE);
  cairo_move_to(cr, 0, height * to_log(c->draw_ys[0], c->loglogscale + 1.0f));
  for(int k = 1; k < DT_IOP_TONECURVE_RES; k++)
  {
    const float xx = k / (DT_IOP_TONECURVE_RES - 1.0f);
    if(xx > xm)
    {
      const float yy = dt_iop_eval_exp(unbounded_coeffs, xx);
      const float x = to_log(xx, c->loglogscale), y = to_log(yy, c->loglogscale);
      cairo_line_to(cr, x * width, height * y);
    }
    else
    {
      const float yy = c->draw_ys[k];
      const float x = to_log(xx, c->loglogscale), y = to_log(yy, c->loglogscale);
      cairo_line_to(cr, x * width, height * y);
    }
  }
  cairo_stroke(cr);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static inline int _add_node(dt_iop_basecurve_node_t *basecurve, int *nodes, float x, float y)
{
  int selected = -1;
  if(basecurve[0].x > x)
    selected = 0;
  else
  {
    for(int k = 1; k < *nodes; k++)
    {
      if(basecurve[k].x > x)
      {
        selected = k;
        break;
      }
    }
  }
  if(selected == -1) selected = *nodes;
  for(int i = *nodes; i > selected; i--)
  {
    basecurve[i].x = basecurve[i - 1].x;
    basecurve[i].y = basecurve[i - 1].y;
  }
  // found a new point
  basecurve[selected].x = x;
  basecurve[selected].y = y;
  (*nodes)++;
  return selected;
}

static void dt_iop_basecurve_sanity_check(dt_iop_module_t *self, GtkWidget *widget)
{
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  int ch = 0;
  int nodes = p->basecurve_nodes[ch];
  dt_iop_basecurve_node_t *basecurve = p->basecurve[ch];

  if(nodes <= 2)
    return;

  const float mx = basecurve[c->selected].x;
  // delete vertex if order has changed
  // for all points, x coordinate of point must be strictly larger than
  // the x coordinate of the previous point
  if((c->selected > 0 && (basecurve[c->selected - 1].x >= mx))
     || (c->selected < nodes - 1 && (basecurve[c->selected + 1].x <= mx)))
  {
    for(int k = c->selected; k < nodes - 1; k++)
    {
      basecurve[k].x = basecurve[k + 1].x;
      basecurve[k].y = basecurve[k + 1].y;
    }
    c->selected = -2; // avoid re-insertion of that point immediately after this
    p->basecurve_nodes[ch]--;
  }
}

static gboolean _move_point_internal(dt_iop_module_t *self, GtkWidget *widget, float dx, float dy, guint state);

static gboolean dt_iop_basecurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  int ch = 0;
  int nodes = p->basecurve_nodes[ch];
  dt_iop_basecurve_node_t *basecurve = p->basecurve[ch];

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
  const double old_m_x = c->mouse_x;
  const double old_m_y = c->mouse_y;
  c->mouse_x = event->x - inset;
  c->mouse_y = event->y - inset;

  const float mx = CLAMP(c->mouse_x, 0, width) / (float)width;
  const float my = 1.0f - CLAMP(c->mouse_y, 0, height) / (float)height;
  const float linx = to_lin(mx, c->loglogscale), liny = to_lin(my, c->loglogscale);

  if(event->state & GDK_BUTTON1_MASK)
  {
    // got a vertex selected:
    if(c->selected >= 0)
    {
      // this is used to translate mause position in loglogscale to make this behavior unified with linear scale.
      const float translate_mouse_x = old_m_x / width - to_log(basecurve[c->selected].x, c->loglogscale);
      const float translate_mouse_y = 1 - old_m_y / height - to_log(basecurve[c->selected].y, c->loglogscale);
      // dx & dy are in linear coordinates
      const float dx = to_lin(c->mouse_x / width - translate_mouse_x, c->loglogscale)
                       - to_lin(old_m_x / width - translate_mouse_x, c->loglogscale);
      const float dy = to_lin(1 - c->mouse_y / height - translate_mouse_y, c->loglogscale)
                       - to_lin(1 - old_m_y / height - translate_mouse_y, c->loglogscale);

      return _move_point_internal(self, widget, dx, dy, event->state);
    }
    else if(nodes < MAXNODES && c->selected >= -1)
    {
      // no vertex was close, create a new one!
      c->selected = _add_node(basecurve, &p->basecurve_nodes[ch], linx, liny);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
  }
  else
  {
    // minimum area around the node to select it:
    float min = 0.04f;
    min *= min; // comparing against square
    int nearest = -1;
    for(int k = 0; k < nodes; k++)
    {
      float dist
          = (my - to_log(basecurve[k].y, c->loglogscale)) * (my - to_log(basecurve[k].y, c->loglogscale))
            + (mx - to_log(basecurve[k].x, c->loglogscale)) * (mx - to_log(basecurve[k].x, c->loglogscale));
      if(dist < min)
      {
        min = dist;
        nearest = k;
      }
    }
    c->selected = nearest;
  }
  if(c->selected >= 0) gtk_widget_grab_focus(widget);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_basecurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  dt_iop_basecurve_params_t *d = (dt_iop_basecurve_params_t *)self->default_params;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;

  int ch = 0;
  int nodes = p->basecurve_nodes[ch];
  dt_iop_basecurve_node_t *basecurve = p->basecurve[ch];

  if(event->button == 1)
  {
    if(event->type == GDK_BUTTON_PRESS && (event->state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK
      && nodes < MAXNODES && c->selected == -1)
    {
      // if we are not on a node -> add a new node at the current x of the pointer and y of the curve at that x
      const int inset = DT_GUI_CURVE_EDITOR_INSET;
      GtkAllocation allocation;
      gtk_widget_get_allocation(widget, &allocation);
      int width = allocation.width - 2 * inset;
      c->mouse_x = event->x - inset;
      c->mouse_y = event->y - inset;

      const float mx = CLAMP(c->mouse_x, 0, width) / (float)width;
      const float linx = to_lin(mx, c->loglogscale);

      // don't add a node too close to others in x direction, it can crash dt
      int selected = -1;
      if(basecurve[0].x > linx)
        selected = 0;
      else
      {
        for(int k = 1; k < nodes; k++)
        {
          if(basecurve[k].x > linx)
          {
            selected = k;
            break;
          }
        }
      }
      if(selected == -1) selected = nodes;
      // > 0 -> check distance to left neighbour
      // < nodes -> check distance to right neighbour
      if(!((selected > 0 && linx - basecurve[selected - 1].x <= 0.025) ||
           (selected < nodes && basecurve[selected].x - linx <= 0.025)))
      {
        // evaluate the curve at the current x position
        const float y = dt_draw_curve_calc_value(c->minmax_curve, linx);

        if(y >= 0.0 && y <= 1.0) // never add something outside the viewport, you couldn't change it afterwards
        {
          // create a new node
          selected = _add_node(basecurve, &p->basecurve_nodes[ch], linx, y);

          // maybe set the new one as being selected
          float min = .04f;
          min *= min; // comparing against square
          for(int k = 0; k < nodes; k++)
          {
            float other_y = to_log(basecurve[k].y, c->loglogscale);
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
      p->basecurve_nodes[ch] = d->basecurve_nodes[ch];
      p->basecurve_type[ch] = d->basecurve_type[ch];
      for(int k = 0; k < d->basecurve_nodes[ch]; k++)
      {
        p->basecurve[ch][k].x = d->basecurve[ch][k].x;
        p->basecurve[ch][k].y = d->basecurve[ch][k].y;
      }
      c->selected = -2; // avoid motion notify re-inserting immediately.
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      gtk_widget_queue_draw(self->widget);
      return TRUE;
    }
  }
  else if(event->button == 3 && c->selected >= 0)
  {
    if(c->selected == 0 || c->selected == nodes - 1)
    {
      float reset_value = c->selected == 0 ? 0 : 1;
      basecurve[c->selected].y = basecurve[c->selected].x = reset_value;
      gtk_widget_queue_draw(self->widget);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      return TRUE;
    }

    for(int k = c->selected; k < nodes - 1; k++)
    {
      basecurve[k].x = basecurve[k + 1].x;
      basecurve[k].y = basecurve[k + 1].y;
    }
    basecurve[nodes - 1].x = basecurve[nodes - 1].y = 0;
    c->selected = -2; // avoid re-insertion of that point immediately after this
    p->basecurve_nodes[ch]--;
    gtk_widget_queue_draw(self->widget);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return TRUE;
  }
  return FALSE;
}

static gboolean area_resized(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  GtkRequisition r;
  r.width = allocation.width;
  r.height = allocation.width;
  gtk_widget_get_preferred_size(widget, &r, NULL);
  return TRUE;
}

static gboolean _move_point_internal(dt_iop_module_t *self, GtkWidget *widget, float dx, float dy, guint state)
{
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  float multiplier;
  int ch = 0;
  dt_iop_basecurve_node_t *basecurve = p->basecurve[ch];
  GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
  
  if((state & modifiers) == GDK_SHIFT_MASK)
    multiplier = dt_conf_get_float("darkroom/ui/scale_rough_step_multiplier");
  else if((state & modifiers) == GDK_CONTROL_MASK)
    multiplier = dt_conf_get_float("darkroom/ui/scale_precise_step_multiplier");
  else
    multiplier = dt_conf_get_float("darkroom/ui/scale_step_multiplier");

  dx *= multiplier;
  dy *= multiplier;

  basecurve[c->selected].x = CLAMP(basecurve[c->selected].x + dx, 0.0f, 1.0f);
  basecurve[c->selected].y = CLAMP(basecurve[c->selected].y + dy, 0.0f, 1.0f);

  dt_iop_basecurve_sanity_check(self, widget);

  gtk_widget_queue_draw(widget);
  dt_iop_queue_history_update(self, FALSE);
  return TRUE;
}

#define BASECURVE_DEFAULT_STEP (0.001f)

static gboolean _scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;

  if(dt_gui_ignore_scroll(event)) return FALSE;

  if(c->selected < 0) return TRUE;

  gdouble delta_y;
  if(dt_gui_get_scroll_delta(event, &delta_y))
  {
    delta_y *= -BASECURVE_DEFAULT_STEP;
    return _move_point_internal(self, widget, 0.0, delta_y, event->state);
  }

  return TRUE;
}

static gboolean dt_iop_basecurve_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;

  if(c->selected < 0) return TRUE;

  int handled = 0;
  float dx = 0.0f, dy = 0.0f;
  if(event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up)
  {
    handled = 1;
    dy = BASECURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down)
  {
    handled = 1;
    dy = -BASECURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Right || event->keyval == GDK_KEY_KP_Right)
  {
    handled = 1;
    dx = BASECURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Left || event->keyval == GDK_KEY_KP_Left)
  {
    handled = 1;
    dx = -BASECURVE_DEFAULT_STEP;
  }

  if(!handled) return TRUE;

  return _move_point_internal(self, widget, dx, dy, event->state);
}

#undef BASECURVE_DEFAULT_STEP

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_basecurve_gui_data_t));
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;

  c->minmax_curve = dt_draw_curve_new(0.0, 1.0, p->basecurve_type[0]);
  c->minmax_curve_type = p->basecurve_type[0];
  c->minmax_curve_nodes = p->basecurve_nodes[0];
  for(int k = 0; k < p->basecurve_nodes[0]; k++)
    (void)dt_draw_curve_add_point(c->minmax_curve, p->basecurve[0][k].x, p->basecurve[0][k].y);
  c->mouse_x = c->mouse_y = -1.0;
  c->selected = -1;
  c->loglogscale = 0;
  self->timeout_handle = 0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  c->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.0));
  gtk_widget_set_tooltip_text(GTK_WIDGET(c->area), _("abscissa: input, ordinate: output. works on RGB channels"));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->area), TRUE, TRUE, 0);
  
  c->logbase = dt_bauhaus_slider_new_with_range(self, 0.0f, 20.0f, 0.5f, 0.0f, 2);
  dt_bauhaus_widget_set_label(c->logbase, NULL, _("scale for graph"));  
  gtk_box_pack_start(GTK_BOX(self->widget), c->logbase , TRUE, TRUE, 0);  
  g_signal_connect(G_OBJECT(c->logbase), "value-changed", G_CALLBACK(logbase_callback), self);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                                 | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                 | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK
                                                 | darktable.gui->scroll_mask);
  gtk_widget_set_can_focus(GTK_WIDGET(c->area), TRUE);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(dt_iop_basecurve_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(dt_iop_basecurve_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(dt_iop_basecurve_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(dt_iop_basecurve_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "enter-notify-event", G_CALLBACK(dt_iop_basecurve_enter_notify), self);
  g_signal_connect(G_OBJECT(c->area), "configure-event", G_CALLBACK(area_resized), self);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(_scrolled), self);
  g_signal_connect(G_OBJECT(c->area), "key-press-event", G_CALLBACK(dt_iop_basecurve_key_press), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_draw_curve_destroy(c->minmax_curve);
  dt_iop_cancel_history_update(self);
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
