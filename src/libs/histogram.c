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
#include <stdint.h>

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/histogram.h"
#include "common/iop_profile.h"
#include "common/image_cache.h"
#include "common/math.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "libs/colorpicker.h"

#define HISTOGRAM_BINS 256

DT_MODULE(1)

typedef enum dt_lib_histogram_highlight_t
{
  DT_LIB_HISTOGRAM_HIGHLIGHT_OUTSIDE_WIDGET = 0,
  DT_LIB_HISTOGRAM_HIGHLIGHT_IN_WIDGET,
  DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT,
  DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE,
  DT_LIB_HISTOGRAM_HIGHLIGHT_TYPE,
  DT_LIB_HISTOGRAM_HIGHLIGHT_MODE,
  DT_LIB_HISTOGRAM_HIGHLIGHT_RED,
  DT_LIB_HISTOGRAM_HIGHLIGHT_GREEN,
  DT_LIB_HISTOGRAM_HIGHLIGHT_BLUE,
} dt_lib_histogram_highlight_t;

typedef enum dt_lib_histogram_scale_t
{
  DT_LIB_HISTOGRAM_LOGARITHMIC = 0,
  DT_LIB_HISTOGRAM_LINEAR,
  DT_LIB_HISTOGRAM_N // needs to be the last one
} dt_lib_histogram_scale_t;

const gchar *dt_lib_histogram_histogram_scale_names[DT_LIB_HISTOGRAM_N] = { "logarithmic", "linear" };

typedef struct dt_lib_histogram_t
{
  // histogram for display
  uint32_t *histogram;
  uint32_t histogram_max;
  dt_pthread_mutex_t lock;
  // exposure params on mouse down
  float exposure, black;
  // mouse state
  int32_t dragging;
  int32_t button_down_x, button_down_y;
  // depends on mouse positon
  dt_lib_histogram_highlight_t highlight;
  dt_lib_histogram_scale_t histogram_scale;
  gboolean red, green, blue;
  // button locations
  float type_x, mode_x, red_x, green_x, blue_x;
  float button_w, button_h, button_y, button_spacing;
  float picker_mean[3], picker_max[3], picker_min[3];
} dt_lib_histogram_t;

const char *name(dt_lib_module_t *self)
{
  return _("histogram");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", "tethering", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_TOP;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}

static void _lib_histogram_process_histogram(dt_lib_histogram_t *d, const float *const input, int width, int height)
{
  dt_dev_histogram_collection_params_t histogram_params = { 0 };
  const dt_iop_colorspace_type_t cst = iop_cs_rgb;
  dt_dev_histogram_stats_t histogram_stats = { .bins_count = HISTOGRAM_BINS, .ch = 4, .pixels = 0 };
  uint32_t histogram_max[4] = { 0 };
  dt_histogram_roi_t histogram_roi = { .width = width, .height = height,
                                      .crop_x = 0, .crop_y = 0, .crop_width = 0, .crop_height = 0 };
  // Constraining the area if the colorpicker is active in area mode
  dt_develop_t *dev = darktable.develop;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view(cv) == DT_VIEW_DARKROOM &&
     dev->gui_module && !strcmp(dev->gui_module->op, "colorout")
     && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF
     && darktable.lib->proxy.colorpicker.restrict_histogram)
  {
    if(darktable.lib->proxy.colorpicker.size == DT_COLORPICKER_SIZE_BOX)
    {
      histogram_roi.crop_x = MIN(width, MAX(0, dev->gui_module->color_picker_box[0] * width));
      histogram_roi.crop_y = MIN(height, MAX(0, dev->gui_module->color_picker_box[1] * height));
      histogram_roi.crop_width = width - MIN(width, MAX(0, dev->gui_module->color_picker_box[2] * width));
      histogram_roi.crop_height = height - MIN(height, MAX(0, dev->gui_module->color_picker_box[3] * height));
    }
    else
    {
      histogram_roi.crop_x = MIN(width, MAX(0, dev->gui_module->color_picker_point[0] * width));
      histogram_roi.crop_y = MIN(height, MAX(0, dev->gui_module->color_picker_point[1] * height));
      histogram_roi.crop_width = width - MIN(width, MAX(0, dev->gui_module->color_picker_point[0] * width));
      histogram_roi.crop_height = height - MIN(height, MAX(0, dev->gui_module->color_picker_point[1] * height));
    }
  }

  dt_times_t start_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_get_times(&start_time);

  d->histogram_max = 0;
  memset(d->histogram, 0, sizeof(uint32_t) * 4 * HISTOGRAM_BINS);

  histogram_params.roi = &histogram_roi;
  histogram_params.bins_count = HISTOGRAM_BINS;
  histogram_params.mul = histogram_params.bins_count - 1;

  dt_histogram_helper(&histogram_params, &histogram_stats, cst, iop_cs_NONE, input, &d->histogram, FALSE, NULL, 4);
  dt_histogram_max_helper(&histogram_stats, cst, iop_cs_NONE, &d->histogram, histogram_max);
  d->histogram_max = MAX(MAX(histogram_max[0], histogram_max[1]), histogram_max[2]);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_times_t end_time = { 0 };
    dt_get_times(&end_time);
    fprintf(stderr, "final histogram took %.3f secs (%.3f CPU)\n", end_time.clock - start_time.clock, end_time.user - start_time.user);
  }
}

static void dt_lib_histogram_process(struct dt_lib_module_t *self, const float *const input, int width, int height,
                                     dt_colorspaces_color_profile_type_t in_profile_type, const gchar *in_profile_filename)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_develop_t *dev = darktable.develop;
  float *img_display = NULL;

  // special case, clear the scopes
  if(!input)
  {
    dt_pthread_mutex_lock(&d->lock);
    memset(d->histogram, 0, sizeof(uint32_t) * 4 * HISTOGRAM_BINS);
    dt_pthread_mutex_unlock(&d->lock);
    return;
  }
  // If showing a selected image in tether view, then the image is
  // already in histogram profile. If the image is from live view and
  // histogram profile is DT_COLORSPACE_WORK or DT_COLORSPACE_EXPORT,
  // we just show the image as-is, as the image hasn't gone through
  // the pixelpipe. Otherwise, convert it to histogram profile.
  // FIXME: detect whether in live view (darktable.develop state should give a clue) and if so if 
  // histogram profile is DT_COLORSPACE_WORK use linear rec 2020 as a reasonable default, if profile is export skip this conversion
  if(in_profile_type != DT_COLORSPACE_NONE)
  {
    dt_colorspaces_color_profile_type_t out_profile_type;
    const char *out_profile_filename;
    const dt_iop_order_iccprofile_info_t *const profile_info_from
      = dt_ioppr_add_profile_info_to_list(dev, in_profile_type, in_profile_filename, INTENT_PERCEPTUAL);
    dt_ioppr_get_histogram_profile_type(&out_profile_type, &out_profile_filename);

    if(out_profile_type != DT_COLORSPACE_NONE)
    {
      const dt_iop_order_iccprofile_info_t *profile_info_to =
        dt_ioppr_add_profile_info_to_list(dev, out_profile_type, out_profile_filename, DT_INTENT_PERCEPTUAL);
      img_display = dt_alloc_align(64, width * height * 4 * sizeof(float));
      
      if(!img_display)
        return;
        
      dt_ioppr_transform_image_colorspace_rgb(input, img_display, width, height, profile_info_from,
                                              profile_info_to, "final histogram");
    }
  }

  dt_pthread_mutex_lock(&d->lock);
  _lib_histogram_process_histogram(d, img_display ? img_display : input, width, height);
  dt_pthread_mutex_unlock(&d->lock);

  if(img_display)
    dt_free_align(img_display);
}

static void _draw_color_toggle(cairo_t *cr, float x, float y, float width, float height, gboolean state)
{
  // FIXME: use dtgtk_cairo_paint_color()
  const float border = MIN(width * .05, height * .05);
  cairo_rectangle(cr, x + border, y + border, width - 2.0 * border, height - 2.0 * border);
  cairo_fill_preserve(cr);
  if(state)
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
  else
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_set_line_width(cr, border);
  cairo_stroke(cr);
}

static void _draw_histogram_scale_toggle(cairo_t *cr, float x, float y, float width, float height, int mode)
{
  cairo_save(cr);
  cairo_translate(cr, x, y);

  // border
  const float border = MIN(width * .05, height * .05);
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_rectangle(cr, border, border, width - 2.0 * border, height - 2.0 * border);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_set_line_width(cr, border);
  cairo_stroke(cr);

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
  cairo_move_to(cr, 2.0 * border, height - 2.0 * border);
  switch(mode)
  {
    case DT_LIB_HISTOGRAM_LINEAR:
      cairo_line_to(cr, width - 2.0 * border, 2.0 * border);
      cairo_stroke(cr);
      break;
    case DT_LIB_HISTOGRAM_LOGARITHMIC:
      cairo_curve_to(cr, 2.0 * border, 0.33 * height, 0.66 * width, 2.0 * border,
                     width - 2.0 * border, 2.0 * border);
      cairo_stroke(cr);
      break;
  }
  cairo_restore(cr);
}

static gboolean _lib_histogram_configure_callback(GtkWidget *widget, GdkEventConfigure *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  const int width = event->width;
  d->button_spacing = 0.02 * width;
  d->button_w = 0.06 * width;
  d->button_h = 0.06 * width;
  d->button_y = d->button_spacing;
  const float offset = d->button_w + d->button_spacing;
  d->blue_x = width - offset;
  d->green_x = d->blue_x - offset;
  d->red_x = d->green_x - offset;
  d->mode_x = d->red_x - offset;
  d->type_x = d->mode_x - offset;

  return TRUE;
}

static void _lib_histogram_draw_histogram(dt_lib_histogram_t *d, cairo_t *cr, int width, int height, const uint8_t mask[3])
{
  if(!d->histogram_max)
    return;
    
  const gboolean is_lin = d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR;
  const float hist_max = is_lin ? d->histogram_max : logf(1.0 + d->histogram_max);
  darktable.lib->proxy.histogram.is_linear = is_lin;
  const float lwidth = width / 255.0f, lheight = -height / hist_max;
  float pick_vals[9] = { 0.0f };

  if(darktable.lib->proxy.colorpicker.display_samples)
    for(int k = 0; k < 3; k++)
    {
      pick_vals[3 * k] = darktable.lib->proxy.colorpicker.picked_color_rgb_mean[k] * lwidth;
      pick_vals[3 * k + 1] = darktable.lib->proxy.colorpicker.picked_color_rgb_min[k] * lwidth;
      pick_vals[3 * k + 2] = darktable.lib->proxy.colorpicker.picked_color_rgb_max[k] * lwidth;
    }

  cairo_translate(cr, 0, height);
  cairo_scale(cr, lwidth, lheight + 10 / hist_max);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  
  for(int k = 0; k < 3; k++)
    if(mask[k])
    {
      cairo_set_source_rgba(cr, k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0, 0.5);
      dt_draw_histogram_8(cr, d->histogram, 4, k, is_lin, pick_vals + 3 * k, hist_max);
    }
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
}

static gboolean _lib_histogram_draw_callback(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_develop_t *dev = darktable.develop;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int width = allocation.width, height = allocation.height;

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0, width, height);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.5)); // borders width

  // Draw frame and background
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, width, height);
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_stroke_preserve(cr);
  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_fill(cr);
  cairo_restore(cr);

  // exposure change regions
  if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
  {
    cairo_set_source_rgb(cr, .5, .5, .5);
    cairo_rectangle(cr, 0, 0, 0.2 * width, height);
    cairo_fill(cr);
  }
  else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
  {
    cairo_set_source_rgb(cr, .5, .5, .5);
    cairo_rectangle(cr, 0.2 * width, 0, width, height);
    cairo_fill(cr);
  }
  
  // draw grid
  set_color(cr, darktable.bauhaus->graph_grid);
  dt_draw_grid(cr, 4, 0, 0, width, height);

  // darkroom view: draw scope so long as preview pipe is finished
  // tether view: draw whatever has come in from tether
  dt_pthread_mutex_lock(&d->lock);
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view(cv) == DT_VIEW_TETHERING || dev->image_storage.id == dev->preview_pipe->output_imgid)
  {
    cairo_save(cr);
    uint8_t mask[3] = { d->red, d->green, d->blue };
    _lib_histogram_draw_histogram(d, cr, width, height, mask);
    cairo_restore(cr);
  }
  dt_pthread_mutex_unlock(&d->lock);
  // buttons to control the display of the histogram: linear/log, r, g, b
  if(d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_OUTSIDE_WIDGET)
  {
    //_draw_type_toggle(cr, d->type_x, d->button_y, d->button_w, d->button_h, d->scope_type);
    _draw_histogram_scale_toggle(cr, d->mode_x, d->button_y, d->button_w, d->button_h, d->histogram_scale);
    cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 0.33);
    _draw_color_toggle(cr, d->red_x, d->button_y, d->button_w, d->button_h, d->red);
    cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 0.33);
    _draw_color_toggle(cr, d->green_x, d->button_y, d->button_w, d->button_h, d->green);
    cairo_set_source_rgba(cr, 0.0, 0.0, 1.0, 0.33);
    _draw_color_toggle(cr, d->blue_x, d->button_y, d->button_w, d->button_h, d->blue);
  }

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  return TRUE;
}

static gboolean _lib_histogram_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event,
                                                      gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_develop_t *dev = darktable.develop;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  const gboolean hooks_available = (cv->view(cv) == DT_VIEW_DARKROOM) && dt_dev_exposure_hooks_available(dev);
  // FIXME: as when dragging a bauhaus widget, delay processing the next event until the pixelpipe 
  // can update based on dev->preview_average_delay

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  if(d->dragging)
  {
    const float diff = event->x - d->button_down_x;
    const int range = allocation.width;
    
    if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
    {
      const float exposure = d->exposure + diff * 4.0f / (float)range;
      dt_dev_exposure_set_exposure(dev, exposure);
    }
    else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
    {
      const float black = d->black - diff * .1f / (float)range;
      dt_dev_exposure_set_black(dev, black);
    }
  }
  else
  {
    const float x = event->x;
    const float y = event->y;
    const float posx = x / (float)(allocation.width);
    const float posy = y / (float)(allocation.height);
    const dt_lib_histogram_highlight_t prior_highlight = d->highlight;

    // FIXME: rather than roll button code from scratch, take advantage of bauhaus/gtk button code?
    if(posx < 0.0f || posx > 1.0f || posy < 0.0f || posy > 1.0f)
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_OUTSIDE_WIDGET;
    // FIXME: simplify this, check for y position, and if it's in range, check for x, and set highlight, and depending on that draw tooltip
    // FIXME: or alternately use copy_path_flat(), append_path(p), in_fill() and keep around the rectangles for each button
    else if(x > d->mode_x && x < d->mode_x + d->button_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_MODE;
      
      switch(d->histogram_scale)
      {
        case DT_LIB_HISTOGRAM_LOGARITHMIC:
          gtk_widget_set_tooltip_text(widget, _("set scale to linear"));
          break;
        case DT_LIB_HISTOGRAM_LINEAR:
          gtk_widget_set_tooltip_text(widget, _("set scale to logarithmic"));
          break;
        default:
          g_assert_not_reached();
      }
    }
    else if(x > d->red_x && x < d->red_x + d->button_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_RED;
      gtk_widget_set_tooltip_text(widget, d->red ? _("click to hide red channel")
                                                 : _("click to show red channel"));
    }
    else if(x > d->green_x && x < d->green_x + d->button_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_GREEN;
      gtk_widget_set_tooltip_text(widget, d->green ? _("click to hide green channel")
                                                   : _("click to show green channel"));
    }
    else if(x > d->blue_x && x < d->blue_x + d->button_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_BLUE;
      gtk_widget_set_tooltip_text(widget, d->blue ? _("click to hide blue channel")
                                                  : _("click to show blue channel"));
    }
    else if(hooks_available)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE;
      gtk_widget_set_tooltip_text(widget,
                     _("drag to change exposure,\ndoubleclick resets\nctrl+scroll to change display height"));
    }
    else
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_IN_WIDGET;
      gtk_widget_set_tooltip_text(widget, _("ctrl+scroll to change display height"));
    }
    if(prior_highlight != d->highlight)
    {
      if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT ||
         d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
        dt_control_change_cursor(GDK_HAND1);
      else
        dt_control_change_cursor(GDK_LEFT_PTR);
      dt_control_queue_redraw_widget(widget);
    }
  }
  gint x, y; // notify gtk for motion_hint.
#if GTK_CHECK_VERSION(3, 20, 0)
  gdk_window_get_device_position(gtk_widget_get_window(widget),
                                 gdk_seat_get_pointer(gdk_display_get_default_seat(
                                   gdk_window_get_display(event->window))), &x, &y, 0);
#else
  gdk_window_get_device_position(event->window, gdk_device_manager_get_client_pointer(
                                     gdk_display_get_device_manager(gdk_window_get_display(event->window))),
                                     &x, &y, NULL);
#endif

  return TRUE;
}

static gboolean _lib_histogram_button_press_callback(GtkWidget *widget, GdkEventButton *event,
                                                     gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_develop_t *dev = darktable.develop;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  const dt_view_type_flags_t view_type = cv->view(cv);
  const gboolean hooks_available = (view_type == DT_VIEW_DARKROOM) && dt_dev_exposure_hooks_available(dev);

  if(event->type == GDK_2BUTTON_PRESS && hooks_available &&
     (d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT || d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE))
  {
    dt_dev_exposure_reset_defaults(dev);
  }
   else
  {
    if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_MODE)
    {
      d->histogram_scale = (d->histogram_scale + 1) % DT_LIB_HISTOGRAM_N;
      dt_conf_set_string("plugins/darkroom/histogram/histogram",
                         dt_lib_histogram_histogram_scale_names[d->histogram_scale]);
      // FIXME: this should really redraw current iop if its background is a histogram (check request_histogram)
      darktable.lib->proxy.histogram.is_linear = d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR;
    }
    else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_RED)
    {
      d->red = !d->red;
      dt_conf_set_bool("plugins/darkroom/histogram/show_red", d->red);
    }
    else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_GREEN)
    {
      d->green = !d->green;
      dt_conf_set_bool("plugins/darkroom/histogram/show_green", d->green);
    }
    else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLUE)
    {
      d->blue = !d->blue;
      dt_conf_set_bool("plugins/darkroom/histogram/show_blue", d->blue);
    }
    else if(hooks_available)
    {
      d->dragging = 1;
      if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
        d->exposure = dt_dev_exposure_get_exposure(dev);
      if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLUE)
        d->black = dt_dev_exposure_get_black(dev);
      d->button_down_x = event->x;
      d->button_down_y = event->y;
    }
  }
  // update for good measure
  dt_control_queue_redraw_widget(self->widget);

  return TRUE;
}

static gboolean _lib_histogram_scroll_callback(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  int delta_y;
  // note are using unit rather than smooth scroll events, as
  // exposure changes can get laggy if handling a multitude of smooth
  // scroll events
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    if(event->state & GDK_CONTROL_MASK && !darktable.gui->reset)
    {
      /* set size of navigation draw area */
      const float histheight = clamp_range_f(dt_conf_get_int("plugins/darkroom/histogram/height")
                                             * 1.0f + 10 * delta_y, 100.0f, 200.0f);
      dt_conf_set_int("plugins/darkroom/histogram/height", histheight);
      gtk_widget_set_size_request(self->widget, -1, DT_PIXEL_APPLY_DPI(histheight));
    }
    else
    {
      dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
      dt_develop_t *dev = darktable.develop;
      const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
      const dt_view_type_flags_t view_type = cv->view(cv);
      if(view_type == DT_VIEW_DARKROOM && dt_dev_exposure_hooks_available(dev))
      {
        // FIXME: as with bauhaus widget, delay processing the next event until the pixelpipe can update based on dev->preview_average_delay
        if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
        {
          const float ce = dt_dev_exposure_get_exposure(dev);
          dt_dev_exposure_set_exposure(dev, ce - 0.15f * delta_y);
        }
        else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
        {
          const float cb = dt_dev_exposure_get_black(dev);
          dt_dev_exposure_set_black(dev, cb + 0.001f * delta_y);
        }
      }
    }
  }

  return TRUE;
}

static gboolean _lib_histogram_button_release_callback(GtkWidget *widget, GdkEventButton *event,
                                                       gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  d->dragging = 0;
  return TRUE;
}

static gboolean _lib_histogram_enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                     gpointer user_data)
{
  dt_control_change_cursor(GDK_HAND1);
  return TRUE;
}

static gboolean _lib_histogram_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                     gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  d->dragging = 0;
  d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_OUTSIDE_WIDGET;
  dt_control_change_cursor(GDK_LEFT_PTR);
  dt_control_queue_redraw_widget(widget);
  return TRUE;
}

// this is only called in darkroom view
static void _lib_histogram_preview_updated_callback(gpointer instance, dt_lib_module_t *self)
{
  // preview pipe has already given process() the high quality
  // pre-gamma image. Now that preview pipe is complete, draw it
  dt_control_queue_redraw_widget(self->widget);
}

void view_enter(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  if(new_view->view(new_view) == DT_VIEW_DARKROOM)
  {
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                              G_CALLBACK(_lib_histogram_preview_updated_callback), self);
  }
  // FIXME: set histogram data to blank if enter tether with no active image
}

void view_leave(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  dt_control_signal_disconnect(darktable.signals,
                               G_CALLBACK(_lib_histogram_preview_updated_callback), self);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)g_malloc0(sizeof(dt_lib_histogram_t));
  self->data = (void *)d;

  dt_pthread_mutex_init(&d->lock, NULL);

  d->red = dt_conf_get_bool("plugins/darkroom/histogram/show_red");
  d->green = dt_conf_get_bool("plugins/darkroom/histogram/show_green");
  d->blue = dt_conf_get_bool("plugins/darkroom/histogram/show_blue");

  gchar *histogram_scale = dt_conf_get_string("plugins/darkroom/histogram/histogram");
  
  if(g_strcmp0(histogram_scale, "linear") == 0)
    d->histogram_scale = DT_LIB_HISTOGRAM_LINEAR;
  else if(g_strcmp0(histogram_scale, "logarithmic") == 0)
    d->histogram_scale = DT_LIB_HISTOGRAM_LOGARITHMIC;
    
  g_free(histogram_scale);
  d->histogram = (uint32_t *)calloc(4 * HISTOGRAM_BINS, sizeof(uint32_t));
  d->histogram_max = 0;
  // proxy functions and data so that pixelpipe or tether can
  // provide data for a histogram
  // FIXME: do need to pass self, or can wrap a callback as a lambda
  darktable.lib->proxy.histogram.module = self;
  darktable.lib->proxy.histogram.process = dt_lib_histogram_process;
  darktable.lib->proxy.histogram.is_linear = d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR;

  /* create drawingarea */
  self->widget = gtk_drawing_area_new();
  gtk_widget_set_name(self->widget, "main-histogram");
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

  gtk_widget_add_events(self->widget, GDK_LEAVE_NOTIFY_MASK | GDK_ENTER_NOTIFY_MASK | GDK_POINTER_MOTION_MASK
                                      | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                      darktable.gui->scroll_mask);
  /* connect callbacks */
  gtk_widget_set_tooltip_text(self->widget,
                              _("drag to change exposure,\ndoubleclick resets\nctrl+scroll to change display height"));
  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(_lib_histogram_draw_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "button-press-event",
                   G_CALLBACK(_lib_histogram_button_press_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "button-release-event",
                   G_CALLBACK(_lib_histogram_button_release_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "motion-notify-event",
                   G_CALLBACK(_lib_histogram_motion_notify_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "leave-notify-event",
                   G_CALLBACK(_lib_histogram_leave_notify_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "enter-notify-event",
                   G_CALLBACK(_lib_histogram_enter_notify_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "scroll-event", G_CALLBACK(_lib_histogram_scroll_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "configure-event",
                   G_CALLBACK(_lib_histogram_configure_callback), self);

  /* set size of navigation draw area */
  const float histheight = dt_conf_get_int("plugins/darkroom/histogram/height") * 1.0f;
  gtk_widget_set_size_request(self->widget, -1, DT_PIXEL_APPLY_DPI(histheight));
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  free(d->histogram);
  dt_pthread_mutex_destroy(&d->lock);
  g_free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
