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
// this is the view for the darkroom module.
// Second window starts at line 3175
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/selection.h"
#include "common/styles.h"
#include "common/tags.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "dtgtk/button.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "libs/colorpicker.h"
#include "views/view.h"
#include "views/view_api.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <gdk/gdkkeysyms.h>
#include <glib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifndef G_SOURCE_FUNC // Defined for glib >= 2.58
#define G_SOURCE_FUNC(f) ((GSourceFunc) (void (*)(void)) (f))
#endif

DT_MODULE(1)

static void _update_softproof_gamut_checking(dt_develop_t *d);
/* signal handler for filmstrip image switching */
static void _view_darkroom_filmstrip_activate_callback(gpointer instance, int imgid, gpointer user_data);

static void dt_dev_change_image(dt_develop_t *dev, const int32_t imgid);

static void _darkroom_display_second_window(dt_develop_t *dev);

static void _darkroom_ui_second_window_write_config(GtkWidget *widget);

const char *name(const dt_view_t *self)
{
  return _("darkroom");
}

void init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_develop_t));
  dt_dev_init((dt_develop_t *)self->data, 1);
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_DARKROOM;
}

void cleanup(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;

  if(dev->second_window.second_wnd)
  {
    if(gtk_widget_is_visible(dev->second_window.second_wnd))
    {
      dt_conf_set_bool("second_window/last_visible", TRUE);
      _darkroom_ui_second_window_write_config(dev->second_window.second_wnd);
    }
    else
      dt_conf_set_bool("second_window/last_visible", FALSE);

    gtk_widget_destroy(dev->second_window.second_wnd);
    dev->second_window.second_wnd = NULL;
    dev->second_window.widget = NULL;
  }
  else
    dt_conf_set_bool("second_window/last_visible", FALSE);

  dt_dev_cleanup(dev);
  free(dev);
}

static cairo_status_t write_snapshot_data(void *closure, const unsigned char *data, unsigned int length)
{
  const int fd = GPOINTER_TO_INT(closure);
  ssize_t res = write(fd, data, length);

  if(res != length)
    return CAIRO_STATUS_WRITE_ERROR;

  return CAIRO_STATUS_SUCCESS;
}

static dt_darkroom_layout_t _lib_darkroom_get_layout(dt_view_t *self)
{
  return DT_DARKROOM_LAYOUT_EDITING;
}

void expose(dt_view_t *self, cairo_t *cri, int32_t width, int32_t height,int32_t pointerx,int32_t pointery)
{
  cairo_set_source_rgb(cri, .2, .2, .2);
  cairo_save(cri);
  dt_develop_t *dev = (dt_develop_t *)self->data;
  const int32_t tb = dev->border_size;
  // account for border, make it transparent for other modules called below:
  pointerx -= tb;
  pointery -= tb;

  if(dev->gui_synch && !dev->image_loading)
  {
    // synch module guis from gtk thread:
    ++darktable.gui->reset;
    GList *modules = dev->iop;
    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      dt_iop_gui_update(module);
      modules = g_list_next(modules);
    }
    --darktable.gui->reset;
    dev->gui_synch = 0;
  }

  if(dev->image_status == DT_DEV_PIXELPIPE_DIRTY || dev->image_status == DT_DEV_PIXELPIPE_INVALID
     || dev->pipe->input_timestamp < dev->preview_pipe->input_timestamp)
    dt_dev_process_image(dev);

  if(dev->preview_status == DT_DEV_PIXELPIPE_DIRTY || dev->preview_status == DT_DEV_PIXELPIPE_INVALID
     || dev->pipe->input_timestamp > dev->preview_pipe->input_timestamp)
    dt_dev_process_preview(dev);

  if(dev->preview2_status == DT_DEV_PIXELPIPE_DIRTY || dev->preview2_status == DT_DEV_PIXELPIPE_INVALID
     || dev->pipe->input_timestamp > dev->preview2_pipe->input_timestamp)
    dt_dev_process_preview2(dev);

  dt_pthread_mutex_t *mutex = NULL;
  int stride;
  const float zoom_y = dt_control_get_dev_zoom_y();
  const float zoom_x = dt_control_get_dev_zoom_x();
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float backbuf_scale = dt_dev_get_zoom_scale(dev, zoom, 1.0f, 0) * darktable.gui->ppd;

  static cairo_surface_t *image_surface = NULL;
  static int image_surface_width = 0, image_surface_height = 0, image_surface_imgid = -1;

  if(image_surface_width != width || image_surface_height != height || image_surface == NULL)
  {
    // create double-buffered image to draw on, to make modules draw more fluently.
    image_surface_width = width;
    image_surface_height = height;
    if(image_surface)
      cairo_surface_destroy(image_surface);

    image_surface = dt_cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    image_surface_imgid = -1; // invalidate old stuff
  }

  cairo_surface_t *surface;
  cairo_t *cr = cairo_create(image_surface);
  // adjust scroll bars
  float zx = zoom_x, zy = zoom_y, boxw = 1.0f, boxh = 1.0f;
  dt_dev_check_zoom_bounds(dev, &zx, &zy, zoom, closeup, &boxw, &boxh);

  if(boxw > 0.95f)
  {
    zx = .0f;
    boxw = 1.01f;
  }
  if(boxh > 0.95f)
  {
    zy = .0f;
    boxh = 1.01f;
  }

  dt_view_set_scrollbar(self, zx, -0.5 + boxw/2, 0.5, boxw/2, zy, -0.5+ boxh/2, 0.5, boxh/2);

  if(dev->pipe->output_backbuf && // do we have an image?
    dev->pipe->output_imgid == dev->image_storage.id && // is the right image?
    dev->pipe->backbuf_scale == backbuf_scale && // is this the zoom scale we want to display?
    dev->pipe->backbuf_zoom_x == zoom_x && dev->pipe->backbuf_zoom_y == zoom_y)
  {
    // draw image
    mutex = &dev->pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);
    float wd = dev->pipe->output_backbuf_width;
    float ht = dev->pipe->output_backbuf_height;
    stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, wd);
    surface = dt_cairo_image_surface_create_for_data(dev->pipe->output_backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride);
    wd /= darktable.gui->ppd;
    ht /= darktable.gui->ppd;

    if(dev->iso_12646.enabled) // force middle grey in background
      cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    else
    {
      if(dev->full_preview)
        dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_PREVIEW_BG);
      else
        dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);
    }

    cairo_paint(cr);
    cairo_translate(cr, ceilf(.5f * (width - wd)), ceilf(.5f * (height - ht)));

    if(closeup)
    {
      const double scale = 1<<closeup;
      cairo_scale(cr, scale, scale);
      cairo_translate(cr, -(.5 - 0.5/scale) * wd, -(.5 - 0.5/scale) * ht);
    }

    if(dev->iso_12646.enabled)
    {
      // draw the white frame around picture
      cairo_rectangle(cr, -tb / 3., -tb / 3.0, wd + 2. * tb / 3., ht + 2. * tb / 3.);
      cairo_set_source_rgb(cr, 1., 1., 1.);
      cairo_fill(cr);
    }

    cairo_rectangle(cr, 0, 0, wd, ht);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), darktable.gui->filter_image);
    cairo_paint(cr);

    cairo_surface_destroy(surface);
    dt_pthread_mutex_unlock(mutex);
    image_surface_imgid = dev->image_storage.id;
  }
  else if(dev->preview_pipe->output_backbuf && dev->preview_pipe->output_imgid == dev->image_storage.id)
  {
    // draw preview
    mutex = &dev->preview_pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);

    const float wd = dev->preview_pipe->output_backbuf_width;
    const float ht = dev->preview_pipe->output_backbuf_height;
    const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);

    if(dev->iso_12646.enabled)
      // force middle grey in background
      cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    else
      dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);

    cairo_paint(cr);

    if(dev->iso_12646.enabled)
    {
      // draw the white frame around picture
      cairo_rectangle(cr, 2 * tb / 3., 2 * tb / 3.0, width - 4. * tb / 3., height - 4. * tb / 3.);
      cairo_set_source_rgb(cr, 1., 1., 1.);
      cairo_fill(cr);
    }

    cairo_rectangle(cr, tb, tb, width-2*tb, height-2*tb);
    cairo_clip(cr);
    stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, wd);
    surface = cairo_image_surface_create_for_data(dev->preview_pipe->output_backbuf,
                                                  CAIRO_FORMAT_RGB24, wd, ht, stride);
    cairo_translate(cr, width / 2.0, height / 2.0f);
    cairo_scale(cr, zoom_scale, zoom_scale);
    cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

    cairo_rectangle(cr, 0, 0, wd, ht);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), darktable.gui->filter_image);
    cairo_fill(cr);
    cairo_surface_destroy(surface);
    dt_pthread_mutex_unlock(mutex);
    image_surface_imgid = dev->image_storage.id;
  }
  else if(dev->preview_pipe->output_imgid != dev->image_storage.id)
  {
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);
    cairo_paint(cr);
    // waiting message
    PangoRectangle ink;
    PangoLayout *layout;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    float fontsize;
    gchar *load_txt;

    if(dev->image_invalid_cnt)
    {
      fontsize = DT_PIXEL_APPLY_DPI(20);
      load_txt = dt_util_dstrcat(NULL, "%s `%s' %s\n\n%s\n%s",
          "darktable could not load image",
          dev->image_storage.filename,
          ", switch to lighttable now.",
          "Please check image (use exiv2 or exiftool) for corrupted data.",
          "seems to be intact concider to open an issue at https://github.com/darktable-org/darktable." );
      if(dev->image_invalid_cnt > 400)
      {
        dev->image_invalid_cnt = 0;
        dt_view_manager_switch(darktable.view_manager, "lighttable");
        return;
      }
    }
    else
    {
      fontsize = DT_PIXEL_APPLY_DPI(14);
      load_txt = dt_util_dstrcat(NULL, "%s %s ...", _("loading image"), dev->image_storage.filename);
    }

    pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, load_txt, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    const float xc = width / 2.0, yc = height * 0.85 - DT_PIXEL_APPLY_DPI(10), wd = ink.width * .5f;
    cairo_move_to(cr, xc - wd, yc + 1. / 3. * fontsize - fontsize);
    pango_cairo_layout_path(cr, layout);
    cairo_set_line_width(cr, 2.0);
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LOG_BG);
    cairo_stroke_preserve(cr);
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LOG_FG);
    cairo_fill(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);
    g_free(load_txt);
    image_surface_imgid = dev->image_storage.id;
  }

  cairo_restore(cri);

  if(image_surface_imgid == dev->image_storage.id)
  {
    cairo_destroy(cr);
    cairo_set_source_surface(cri, image_surface, 0, 0);
    cairo_paint(cri);
  }
  /* if we are in full preview mode, we don"t want anything else than the image */
  if(dev->full_preview)
    return;
  /* check if we should create a snapshot of view */
  if(darktable.develop->proxy.snapshot.request && !darktable.develop->image_loading)
  {
    /* reset the request */
    darktable.develop->proxy.snapshot.request = FALSE;
    /* validation of snapshot filename */
    g_assert(darktable.develop->proxy.snapshot.filename != NULL);
    // Store current image surface to snapshot file.
    //  FIXME: add checks so that we don't make snapshots of preview pipe image surface.
    int fd = g_open(darktable.develop->proxy.snapshot.filename, O_CREAT | O_WRONLY | O_BINARY, 0600);
    cairo_surface_write_to_png_stream(image_surface, write_snapshot_data, GINT_TO_POINTER(fd));
    close(fd);
  }
  // Displaying sample areas if enabled
  if(darktable.lib->proxy.colorpicker.live_samples
     && (darktable.lib->proxy.colorpicker.display_samples
         || darktable.lib->proxy.colorpicker.selected_sample))
  {
    GSList *samples = darktable.lib->proxy.colorpicker.live_samples;
    dt_colorpicker_sample_t *sample = NULL;
    
    const gboolean only_selected_sample =
      darktable.lib->proxy.colorpicker.selected_sample
      && !darktable.lib->proxy.colorpicker.display_samples;
      
    cairo_save(cri);
    // The colorpicker samples bounding rectangle should only be displayed inside the visible image
    const int pwidth = (dev->pipe->output_backbuf_width<<closeup) / darktable.gui->ppd;
    const int pheight = (dev->pipe->output_backbuf_height<<closeup) / darktable.gui->ppd;

    const float hbar = (self->width - pwidth) * .5f;
    const float tbar = (self->height - pheight) * .5f;
    cairo_rectangle(cri, hbar, tbar, (double)pwidth, (double)pheight);
    cairo_clip(cri);
    const float wd = dev->preview_pipe->backbuf_width;
    const float ht = dev->preview_pipe->backbuf_height;
    const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);
    const float lw = 1.0 / zoom_scale;

    cairo_translate(cri, width / 2.0, height / 2.0f);
    cairo_scale(cri, zoom_scale, zoom_scale);
    cairo_translate(cri, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

    while(samples)
    {
      sample = samples->data;
      // only dislay selected sample, skip if not the selected sample
      if(only_selected_sample
         && sample != darktable.lib->proxy.colorpicker.selected_sample)
      {
        samples = g_slist_next(samples);
        continue;
      }
      
      cairo_set_line_width(cri, lw);

      if(sample == darktable.lib->proxy.colorpicker.selected_sample)
        cairo_set_source_rgb(cri, .2, 0, 0);
      else
        cairo_set_source_rgb(cri, 0, 0, .2);

      const float *box = sample->box;
      const float *point = sample->point;

      if(sample->size == DT_COLORPICKER_SIZE_BOX)
      {
        cairo_rectangle(cri, box[0] * wd + lw, box[1] * ht + lw, (box[2] - box[0]) * wd, (box[3] - box[1]) * ht);
        cairo_stroke(cri);

        if(sample == darktable.lib->proxy.colorpicker.selected_sample)
          cairo_set_source_rgb(cri, .8, 0, 0);
        else
          cairo_set_source_rgb(cri, 0, 0, .8);
        cairo_rectangle(cri, box[0] * wd + 2. * lw, box[1] * ht + 2. * lw,
                        (box[2] - box[0]) * wd - 2. * lw, (box[3] - box[1]) * ht - 2. * lw);
        cairo_stroke(cri);
      }
      else
      {
        cairo_rectangle(cri, point[0] * wd - .01 * wd, point[1] * ht - .01 * wd, .02 * wd, .02 * wd);
        cairo_stroke(cri);

        if(sample == darktable.lib->proxy.colorpicker.selected_sample)
          cairo_set_source_rgb(cri, .8, 0, 0);
        else
          cairo_set_source_rgb(cri, 0, 0, .8);
        cairo_rectangle(cri, (point[0] - 0.01) * wd + lw,
                        point[1] * ht - 0.01 * wd + lw, .02 * wd - 2. * lw,
                        .02 * wd - 2. * lw);
        cairo_move_to(cri, point[0] * wd, point[1] * ht - .01 * wd + lw);
        cairo_line_to(cri, point[0] * wd, point[1] * ht + .01 * wd - lw);
        cairo_move_to(cri, point[0] * wd - .01 * wd + lw, point[1] * ht);
        cairo_line_to(cri, point[0] * wd + .01 * wd - lw, point[1] * ht);
        cairo_stroke(cri);
      }

      samples = g_slist_next(samples);
    }

    cairo_restore(cri);
  }

  // display mask if we have a current module activated or if the masks manager module is expanded
  const gboolean display_masks =
    (dev->gui_module && dev->gui_module->enabled)
    || dt_lib_gui_get_expanded(dt_lib_get_module("masks"));
  // execute module callback hook.
  if(dev->gui_module && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF && display_masks)
  {
    // The colorpicker bounding rectangle should only be displayed inside the visible image
    const int pwidth = (dev->pipe->output_backbuf_width<<closeup) / darktable.gui->ppd;
    const int pheight = (dev->pipe->output_backbuf_height<<closeup) / darktable.gui->ppd;
    const float hbar = (self->width - pwidth) * .5f;
    const float tbar = (self->height - pheight) * .5f;
    cairo_save(cri);
    cairo_rectangle(cri, hbar, tbar, (double)pwidth, (double)pheight);
    cairo_clip(cri);

    const float wd = dev->preview_pipe->backbuf_width;
    const float ht = dev->preview_pipe->backbuf_height;
    const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);

    cairo_translate(cri, width / 2.0, height / 2.0f);
    cairo_scale(cri, zoom_scale, zoom_scale);
    cairo_translate(cri, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);
    cairo_set_line_width(cri, 1.0 / zoom_scale);
    cairo_set_source_rgb(cri, .2, .2, .2);
    const float *box = dev->gui_module->color_picker_box;
    const float *point = dev->gui_module->color_picker_point;

    if(darktable.lib->proxy.colorpicker.size)
    {
      cairo_translate(cri, 1.0 / zoom_scale, 1.0 / zoom_scale);

      double x = box[0] * wd, y = box[1] * ht;

      double d = 1. / zoom_scale;
      cairo_set_source_rgb(cri, .0, .0, .0);

      for(int blackwhite = 2; blackwhite; blackwhite--)
      {
        double w = 5. / zoom_scale - d;

        cairo_rectangle(cri, x + d, y + d, (box[2] - box[0]) * wd - 2. * d, (box[3] - box[1]) * ht - 2. * d);

        cairo_rectangle(cri, x - w, y - w, 2. * w, 2. * w);
        cairo_rectangle(cri, x - w, box[3] * ht - w, 2. * w, 2. * w);
        cairo_rectangle(cri, box[2] * wd - w, y - w, 2. * w, 2. * w);
        cairo_rectangle(cri, box[2] * wd - w, box[3] * ht - w, 2. * w, 2. *w);
        cairo_stroke(cri);

        d = 0;
        cairo_set_source_rgb(cri, .8, .8, .8);
      }
    }
    else if(point[0] >= 0.0f && point[0] <= 1.0f && point[1] >= 0.0f && point[1] <= 1.0f)
    {
      const float size = (wd + ht) / 2.0;
      cairo_rectangle(cri,
                      point[0] * wd - .01 * size,
                      point[1] * ht - .01 * size,
                      .02 * size, .02 * size);
      cairo_stroke(cri);

      cairo_set_source_rgb(cri, .8, .8, .8);
      cairo_rectangle(cri,
                      point[0] * wd - .01 * size + 1.0 / zoom_scale,
                      point[1] * ht - .01 * size + 1.0 / zoom_scale,
                      .02 * size - 2. / zoom_scale,
                      .02 * size - 2. / zoom_scale);
      cairo_move_to(cri, point[0] * wd, point[1] * ht - .01 * size + 1. / zoom_scale);
      cairo_line_to(cri, point[0] * wd, point[1] * ht + .01 * size - 1. / zoom_scale);
      cairo_move_to(cri, point[0] * wd - .01 * size + 1. / zoom_scale, point[1] * ht);
      cairo_line_to(cri, point[0] * wd + .01 * size - 1. / zoom_scale, point[1] * ht);
      cairo_stroke(cri);
    }
    cairo_restore(cri);
  }
  else
  {
    if(dev->form_visible && display_masks)
      dt_masks_events_post_expose(dev->gui_module, cri, width, height, pointerx, pointery);
    // module
    if(dev->gui_module && dev->gui_module->gui_post_expose)
      dev->gui_module->gui_post_expose(dev->gui_module, cri, width, height, pointerx, pointery);
  }
  // indicate if we are in gamut check or softproof mode
  if(darktable.color_profiles->mode != DT_PROFILE_NORMAL)
  {
    gchar *label = darktable.color_profiles->mode == DT_PROFILE_GAMUTCHECK ? _("gamut check") : _("soft proof");
    cairo_set_source_rgba(cri, 0.5, 0.5, 0.5, 0.5);
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    layout = pango_cairo_create_layout(cri);
    pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(20) * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, label, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cri, ink.height * 2, height - (ink.height * 3));
    pango_cairo_layout_path(cri, layout);
    cairo_set_source_rgb(cri, 0.7, 0.7, 0.7);
    cairo_fill_preserve(cri);
    cairo_set_line_width(cri, 0.7);
    cairo_set_source_rgb(cri, 0.3, 0.3, 0.3);
    cairo_stroke(cri);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }
}

void reset(dt_view_t *self)
{
  dt_control_set_dev_zoom(DT_ZOOM_FIT);
  dt_control_set_dev_zoom_x(0);
  dt_control_set_dev_zoom_y(0);
  dt_control_set_dev_closeup(0);
}

int try_enter(dt_view_t *self)
{
  int imgid = dt_view_get_image_to_act_on();

  if(imgid < 0)
  {
    // fail :(
    dt_control_log(_("no image to open !"));
    return 1;
  }
  // this loads the image from db if needed:
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  // get image and check if it has been deleted from disk first!
  char imgfilename[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(img->id, imgfilename, sizeof(imgfilename), &from_cache);

  if(!g_file_test(imgfilename, G_FILE_TEST_IS_REGULAR))
  {
    dt_control_log(_("image `%s' is currently unavailable"), img->filename);
    dt_image_cache_read_release(darktable.image_cache, img);
    return 1;
  }
  // and drop the lock again.
  dt_image_cache_read_release(darktable.image_cache, img);
  darktable.develop->image_storage.id = imgid;
  return 0;
}

static void dt_dev_cleanup_module_accels(dt_iop_module_t *module)
{
  dt_accel_disconnect_list(&module->accel_closures);
  dt_accel_cleanup_locals_iop(module);
}

static void dt_dev_change_image(dt_develop_t *dev, const int32_t imgid)
{
  // stop crazy users from sleeping on key-repeat spacebar:
  if(dev->image_loading)
    return;
  // change active image
  g_slist_free(darktable.view_manager->active_images);
  darktable.view_manager->active_images = NULL;
  darktable.view_manager->active_images
      = g_slist_append(darktable.view_manager->active_images, GINT_TO_POINTER(imgid));
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);

  // if the previous shown image is selected and the selection is unique
  // then we change the selected image to the new one
  if(dev->image_storage.id > 0)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT m.imgid FROM memory.collected_images as m, main.selected_images as s "
                                "WHERE m.imgid=s.imgid",
                                -1, &stmt, NULL);
    gboolean follow = FALSE;
    
    if(sqlite3_step(stmt) == SQLITE_ROW)
      if(sqlite3_column_int(stmt, 0) == dev->image_storage.id && sqlite3_step(stmt) != SQLITE_ROW)
        follow = TRUE;
        
    sqlite3_finalize(stmt);
    
    if(follow)
      dt_selection_select_single(darktable.selection, imgid);
  }
  // disable color picker when changing image
  if(darktable.lib->proxy.colorpicker.picker_proxy)
    dt_iop_color_picker_reset(darktable.lib->proxy.colorpicker.picker_proxy->module, FALSE);
  // update aspect ratio
  if(dev->preview_pipe->backbuf && dev->preview_status == DT_DEV_PIXELPIPE_VALID)
  {
    double aspect_ratio = (double)dev->preview_pipe->backbuf_width / (double)dev->preview_pipe->backbuf_height;
    dt_image_set_aspect_ratio_to(dev->preview_pipe->image.id, aspect_ratio, TRUE);
  }
  else
    dt_image_set_aspect_ratio(dev->image_storage.id, TRUE);

  // clean the undo list
  dt_undo_clear(darktable.undo, DT_UNDO_DEVELOP);
  // prevent accels_window to refresh
  darktable.view_manager->accels_window.prevent_refresh = TRUE;
  // make sure we can destroy and re-setup the pixel pipes.
  // we acquire the pipe locks, which will block the processing threads
  // in darkroom mode before they touch the pipes (init buffers etc).
  // we don't block here, since we hold the gdk lock, which will
  // result in circular locking when background threads emit signals
  // which in turn try to acquire the gdk lock.
  //
  // worst case, it'll drop some change image events. sorry.
  if(dt_pthread_mutex_BAD_trylock(&dev->preview_pipe_mutex))
    return;

  if(dt_pthread_mutex_BAD_trylock(&dev->pipe_mutex))
  {
    dt_pthread_mutex_BAD_unlock(&dev->preview_pipe_mutex);
    return;
  }

  if(dt_pthread_mutex_BAD_trylock(&dev->preview2_pipe_mutex))
  {
    dt_pthread_mutex_BAD_unlock(&dev->pipe_mutex);
    dt_pthread_mutex_BAD_unlock(&dev->preview_pipe_mutex);
    return;
  }
  // get current plugin in focus before defocus
  gchar *active_plugin = NULL;

  if(darktable.develop->gui_module)
    active_plugin = g_strdup(darktable.develop->gui_module->op);

  dt_iop_request_focus(NULL);
  g_assert(dev->gui_attached);
  // commit image ops to db
  dt_dev_write_history(dev);
  // be sure light table will update the thumbnail
  if (!dt_history_hash_is_mipmap_synced(dev->image_storage.id))
  {
    dt_mipmap_cache_remove(darktable.mipmap_cache, dev->image_storage.id);
    dt_image_reset_final_size(dev->image_storage.id);
    dt_image_synch_xmp(dev->image_storage.id);
    dt_history_hash_set_mipmap(dev->image_storage.id);
  }
  // cleanup visible masks
  if(!dev->form_gui)
  {
    dev->form_gui = (dt_masks_form_gui_t *)calloc(1, sizeof(dt_masks_form_gui_t));
    dt_masks_init_form_gui(dev->form_gui);
  }

  dt_masks_change_form_gui(NULL);

  while(dev->history)
  {
    // clear history of old image
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(dev->history->data);
    dt_dev_free_history_item(hist);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }
  // get new image:
  dt_dev_reload_image(dev, imgid);
  // make sure no signals propagate here:
  ++darktable.gui->reset;

  const guint nb_iop = g_list_length(dev->iop);
  dt_dev_pixelpipe_cleanup_nodes(dev->pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview2_pipe);

  for(int i = nb_iop - 1; i >= 0; i--)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(g_list_nth_data(dev->iop, i));
    // the base module is the one with the lowest multi_priority
    const guint clen = g_list_length(dev->iop);
    int base_multi_priority = 0;

    for(int k = 0; k < clen; k++)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)(g_list_nth_data(dev->iop, k));
      if(strcmp(module->op, mod->op) == 0) base_multi_priority = MIN(base_multi_priority, mod->multi_priority);
    }

    if(module->multi_priority == base_multi_priority) // if the module is the "base" instance, we keep it
    {
      module->iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, module->op, module->multi_priority);
      module->multi_priority = 0;
      module->multi_name[0] = '\0';
      dt_iop_reload_defaults(module);
      dt_iop_gui_update(module);
    }
    else // else we delete it and remove it from the panel
    {
      if(!dt_iop_is_hidden(module))
      {
        gtk_widget_destroy(module->expander);
        dt_iop_gui_cleanup_module(module);
      }

      // we remove the module from the list
      dev->iop = g_list_remove_link(dev->iop, g_list_nth(dev->iop, i));

      // we cleanup the module
      dt_accel_disconnect_list(&module->accel_closures);
      dt_accel_cleanup_locals_iop(module);
      dt_iop_cleanup_module(module);
      free(module);
    }
  }

  dev->iop = g_list_sort(dev->iop, dt_sort_iop_by_order);
  // we also clear the saved modules
  while(dev->alliop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->alliop->data);
    free(dev->alliop->data);
    dev->alliop = g_list_delete_link(dev->alliop, dev->alliop);
  }
  // and masks
  g_list_free_full(dev->forms, (void (*)(void *))dt_masks_free_form);
  dev->forms = NULL;
  g_list_free_full(dev->allforms, (void (*)(void *))dt_masks_free_form);
  dev->allforms = NULL;
  dt_dev_pixelpipe_create_nodes(dev->pipe, dev);
  dt_dev_pixelpipe_create_nodes(dev->preview_pipe, dev);

  if(dev->second_window.widget && GTK_IS_WIDGET(dev->second_window.widget))
    dt_dev_pixelpipe_create_nodes(dev->preview2_pipe, dev);

  dt_dev_read_history(dev);
  // we have to init all module instances other than "base" instance
  GList *modules = g_list_last(dev->iop);

  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    if(module->multi_priority > 0)
    {
      if(!dt_iop_is_hidden(module))
      {
        module->gui_init(module);
        dt_iop_reload_defaults(module);
        /* add module to right panel */
        GtkWidget *expander = dt_iop_gui_get_expander(module);
        dt_ui_container_add_widget(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER, expander);
        dt_iop_gui_set_expanded(module, FALSE, dt_conf_get_bool("darkroom/ui/single_module"));
        dt_iop_gui_update_blending(module);
      }
    }
    else
      //  update the module header to ensure proper multi-name display
      if(!dt_iop_is_hidden(module))
      {
        if(module->change_image)
          module->change_image(module);
          
        dt_iop_gui_update_header(module);
      }

    modules = g_list_previous(modules);
  }

  dt_dev_pop_history_items(dev, dev->history_end);
  // set the module list order
  dt_dev_reorder_gui_module_list(dev);
  dt_dev_masks_list_change(dev);
  // cleanup histograms
  g_list_foreach(dev->iop, (GFunc)dt_iop_cleanup_histogram, (gpointer)NULL);
  // make signals work again, we can't restore the active_plugin while signals
  //   are blocked due to implementation of dt_iop_request_focus so we do it now
  //  A double history entry is not generated.
  --darktable.gui->reset;
  // Now we can request focus again and write a safe plugins/darkroom/active
  if(active_plugin)
  {
    gboolean valid = FALSE;
    modules = dev->iop;

    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

      if(!strcmp(module->op, active_plugin))
      {
        valid = TRUE;
        dt_conf_set_string("plugins/darkroom/active", active_plugin);
        dt_iop_request_focus(module);
      }

      modules = g_list_next(modules);
    }

    if(!valid)
      dt_conf_set_string("plugins/darkroom/active", "");

    g_free(active_plugin);
  }
  // Signal develop initialize
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED);
  // release pixel pipe mutices
  dt_pthread_mutex_BAD_unlock(&dev->preview2_pipe_mutex);
  dt_pthread_mutex_BAD_unlock(&dev->preview_pipe_mutex);
  dt_pthread_mutex_BAD_unlock(&dev->pipe_mutex);
  // update hint message
  dt_collection_hint_message(darktable.collection);
  // update accels_window
  darktable.view_manager->accels_window.prevent_refresh = FALSE;

  if(darktable.view_manager->accels_window.window && darktable.view_manager->accels_window.sticky)
    dt_view_accels_refresh(darktable.view_manager);
  // just make sure at this stage we have only history info into the undo, all automatic
  // tagging should be ignored.
  dt_undo_clear(darktable.undo, DT_UNDO_TAGS);
}

static void _view_darkroom_filmstrip_activate_callback(gpointer instance, int32_t imgid, gpointer user_data)
{
  if(imgid > 0)
  {
    // switch images in darkroom mode:
    const dt_view_t *self = (dt_view_t *)user_data;
    dt_develop_t *dev = (dt_develop_t *)self->data;
    dt_dev_change_image(dev, imgid);
    // move filmstrip
    dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), imgid, TRUE);
    // force redraw
    dt_control_queue_redraw();
  }
}

static void _darkroom_ui_pipe_finish_signal_callback(gpointer instance, gpointer data)
{
  dt_control_queue_redraw_center();
}

static void _darkroom_ui_preview2_pipe_finish_signal_callback(gpointer instance, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_develop_t *dev = (dt_develop_t *)self->data;

  if(dev->second_window.widget)
    gtk_widget_queue_draw(dev->second_window.widget);
}

static void _darkroom_ui_favorite_presets_popupmenu(GtkWidget *w, gpointer user_data)
{
  // create favorites menu and popup 
  dt_gui_favorite_presets_menu_show();
  // if we got any styles, lets popup menu for selection 
  if(darktable.gui->presets_popup_menu)
  {
    gtk_widget_show_all(GTK_WIDGET(darktable.gui->presets_popup_menu));
    gtk_menu_popup_at_pointer(darktable.gui->presets_popup_menu, NULL);
  }
  else
    dt_control_log(_("no userdefined presets for favorite modules were found"));
}

static void _darkroom_ui_apply_style_activate_callback(gchar *name)
{
  dt_control_log(_("applied style `%s' on current image"), name);

  // write current history changes so nothing gets lost
  dt_dev_write_history(darktable.develop);
  // apply style on image and reload
  dt_styles_apply_to_image(name, FALSE, darktable.develop->image_storage.id);
  dt_dev_reload_image(darktable.develop, darktable.develop->image_storage.id);
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);
}

static void _darkroom_ui_apply_style_popupmenu(GtkWidget *w, gpointer user_data)
{
  // show styles popup menu 
  GList *styles = dt_styles_get_list("");
  GtkMenuShell *menu = NULL;

  if(styles)
  {
    menu = GTK_MENU_SHELL(gtk_menu_new());

    do
    {
      dt_style_t *style = (dt_style_t *)styles->data;
      char *items_string = dt_styles_get_item_list_as_string(style->name);
      gchar *tooltip = NULL;

      if(style->description && *style->description)
        tooltip = g_strconcat("<b>", g_markup_escape_text(style->description, -1), "</b>\n", items_string, NULL);
      else
        tooltip = g_strdup(items_string);

      gchar **split = g_strsplit(style->name, "|", 0);
      // if sub-menu, do not put leading group in final name
      gchar *mi_name = NULL;

      if(split[1])
      {
        mi_name = g_strdup(split[1]);
        for(int i=2; split[i]; i++)
          mi_name = g_strconcat(mi_name, " | ", split[i], NULL);
      }
      else
        mi_name = g_strdup(split[0]);

      GtkWidget *mi = gtk_menu_item_new_with_label(mi_name);
      gtk_widget_set_tooltip_markup(mi, tooltip);
      g_free(mi_name);
      // check if we already have a sub-menu with this name
      GtkMenu *sm = NULL;
      GList *childs = gtk_container_get_children(GTK_CONTAINER(menu));

      while(childs)
      {
        GtkMenuItem *smi = (GtkMenuItem *)childs->data;
        if(!g_strcmp0(split[0],gtk_menu_item_get_label(smi)))
        {
          sm = (GtkMenu *)gtk_menu_item_get_submenu(smi);
          g_list_free(childs);
          break;
        }
        childs = g_list_next(childs);
      }

      GtkMenuItem *smi = NULL;
      // no sub-menu, but we need one
      if(!sm && split[1])
      {
        smi = (GtkMenuItem *)gtk_menu_item_new_with_label(split[0]);
        sm = (GtkMenu *)gtk_menu_new();
        gtk_menu_item_set_submenu(smi, GTK_WIDGET(sm));
      }

      if(sm)
        gtk_menu_shell_append(GTK_MENU_SHELL(sm), mi);
      else
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

      if(smi)
      {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), GTK_WIDGET(smi));
        gtk_widget_show(GTK_WIDGET(smi));
      }

      g_signal_connect_swapped(G_OBJECT(mi), "activate",
                               G_CALLBACK(_darkroom_ui_apply_style_activate_callback),
                               (gpointer)g_strdup(style->name));
      gtk_widget_show(mi);
      g_free(items_string);
      g_free(tooltip);
      g_strfreev(split);
    } while((styles = g_list_next(styles)) != NULL);

    g_list_free_full(styles, dt_style_free);
  }
  // if we got any styles, lets popup menu for selection
  if(menu)
    gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
  else
    dt_control_log(_("no styles have been created yet"));
}

static void _second_window_quickbutton_clicked(GtkWidget *w, dt_develop_t *dev)
{
  if(dev->second_window.second_wnd && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)))
  {
    _darkroom_ui_second_window_write_config(dev->second_window.second_wnd);
    gtk_widget_destroy(dev->second_window.second_wnd);
    dev->second_window.second_wnd = NULL;
    dev->second_window.widget = NULL;
  }
  else if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)))
    _darkroom_display_second_window(dev);
}

/** toolbar buttons */

static gboolean _toolbar_show_popup(gpointer user_data)
{
  gtk_widget_show_all(GTK_WIDGET(user_data));
  // cancel glib timeout if invoked by long button press
  return FALSE;
}

// colour assessment
static void _iso_12646_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;

  if (!d->gui_attached) return;

  d->iso_12646.enabled = !d->iso_12646.enabled;
  d->width = d->orig_width;
  d->height = d->orig_height;

  if(d->iso_12646.enabled)
    d->border_size = 0.125 * d->width;
  else
    // Reset border size from config
    d->border_size = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));

  dt_dev_configure(d, d->width, d->height);
  dt_ui_restore_panels(darktable.gui->ui);
  dt_dev_reprocess_center(d);
}

// overlay color
static void _overlay_color_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overlay_color.enabled = !d->overlay_color.enabled;
  dt_dev_reprocess_center(d);
}

static gboolean _overlay_color_quickbutton_pressed(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  _toolbar_show_popup(d->overlay_color.floating_window);
  return TRUE;
}

static gboolean _overlay_color_quickbutton_released(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;

  if(d->overlay_color.timeout > 0)
    g_source_remove(d->overlay_color.timeout);

  d->overlay_color.timeout = 0;
  return FALSE;
}

static void overlay_colors_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overlay_color.color = dt_bauhaus_combobox_get(combo);
  dt_conf_set_int("darkroom/ui/overlay_color", d->overlay_color.color);
  dt_dev_reprocess_center(d);
}

// overexposed
static void _overexposed_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.enabled = !d->overexposed.enabled;
  dt_dev_reprocess_center(d);
}

static gboolean _overexposed_quickbutton_pressed(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  const GdkEventButton *e = (GdkEventButton *)event;

  if(e->button == 3)
  {
    _toolbar_show_popup(d->overexposed.floating_window);
    return TRUE;
  }
  else
  {
    d->overexposed.timeout = g_timeout_add_seconds(1, _toolbar_show_popup, d->overexposed.floating_window);
    return FALSE;
  }
}

static gboolean _overexposed_quickbutton_released(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;

  if(d->overexposed.timeout > 0)
    g_source_remove(d->overexposed.timeout);

  d->overexposed.timeout = 0;
  return FALSE;
}

static void colorscheme_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.colorscheme = dt_bauhaus_combobox_get(combo);

  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void lower_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.lower = dt_bauhaus_slider_get(slider);

  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void upper_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.upper = dt_bauhaus_slider_get(slider);

  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    dt_dev_reprocess_center(d);
}

// rawoverexposed //
static void _rawoverexposed_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.enabled = !d->rawoverexposed.enabled;
  dt_dev_reprocess_center(d);
}

static gboolean _rawoverexposed_quickbutton_pressed(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  const GdkEventButton *e = (GdkEventButton *)event;

  if(e->button == 3)
  {
    _toolbar_show_popup(d->rawoverexposed.floating_window);
    return TRUE;
  }
  else
  {
    d->rawoverexposed.timeout = g_timeout_add_seconds(1, _toolbar_show_popup, d->rawoverexposed.floating_window);
    return FALSE;
  }
}

static gboolean _rawoverexposed_quickbutton_released(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;

  if(d->rawoverexposed.timeout > 0)
    g_source_remove(d->rawoverexposed.timeout);

  d->rawoverexposed.timeout = 0;
  return FALSE;
}

static void rawoverexposed_mode_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.mode = dt_bauhaus_combobox_get(combo);

  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void rawoverexposed_colorscheme_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.colorscheme = dt_bauhaus_combobox_get(combo);

  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void rawoverexposed_threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.threshold = dt_bauhaus_slider_get(slider);

  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));
  else
    dt_dev_reprocess_center(d);
}

// softproof
static void _softproof_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;

  if(darktable.color_profiles->mode == DT_PROFILE_SOFTPROOF)
    darktable.color_profiles->mode = DT_PROFILE_NORMAL;
  else
    darktable.color_profiles->mode = DT_PROFILE_SOFTPROOF;

  _update_softproof_gamut_checking(d);
  dt_dev_reprocess_center(d);
}

static gboolean _softproof_quickbutton_pressed(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  GdkEventButton *e = (GdkEventButton *)event;
  gtk_popover_set_relative_to(GTK_POPOVER(d->profile.floating_window), d->profile.softproof_button);

  if(e->button == 3)
  {
    _toolbar_show_popup(d->profile.floating_window);
    return TRUE;
  }
  else
  {
    d->profile.timeout = g_timeout_add_seconds(1, _toolbar_show_popup, d->profile.floating_window);
    return FALSE;
  }
}

static gboolean _second_window_quickbutton_pressed(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  GdkEventButton *e = (GdkEventButton *)event;
  gtk_popover_set_relative_to(GTK_POPOVER(d->profile.floating_window), d->second_window.button);

  if(e->button == 3)
  {
    _toolbar_show_popup(d->profile.floating_window);
    return TRUE;
  }
  else
  {
    d->profile.timeout = g_timeout_add_seconds(1, _toolbar_show_popup, d->profile.floating_window);
    return FALSE;
  }
}

static gboolean _profile_quickbutton_released(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;

  if(d->profile.timeout > 0)
    g_source_remove(d->profile.timeout);

  d->profile.timeout = 0;
  return FALSE;
}

// gamut
static void _gamut_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;

  if(darktable.color_profiles->mode == DT_PROFILE_GAMUTCHECK)
    darktable.color_profiles->mode = DT_PROFILE_NORMAL;
  else
    darktable.color_profiles->mode = DT_PROFILE_GAMUTCHECK;

  _update_softproof_gamut_checking(d);
  dt_dev_reprocess_center(d);
}

static gboolean _gamut_quickbutton_pressed(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  GdkEventButton *e = (GdkEventButton *)event;
  gtk_popover_set_relative_to(GTK_POPOVER(d->profile.floating_window), d->profile.gamut_button);

  if(e->button == 3)
  {
    _toolbar_show_popup(d->profile.floating_window);
    return TRUE;
  }
  else
  {
    d->profile.timeout = g_timeout_add_seconds(1, _toolbar_show_popup, d->profile.floating_window);
    return FALSE;
  }
}

/* set the gui state for both softproof and gamut checking */
static void _update_softproof_gamut_checking(dt_develop_t *d)
{
  g_signal_handlers_block_by_func(d->profile.softproof_button, _softproof_quickbutton_clicked, d);
  g_signal_handlers_block_by_func(d->profile.gamut_button, _gamut_quickbutton_clicked, d);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->profile.softproof_button),
                                                 darktable.color_profiles->mode == DT_PROFILE_SOFTPROOF);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->profile.gamut_button),
                                                 darktable.color_profiles->mode == DT_PROFILE_GAMUTCHECK);
  g_signal_handlers_unblock_by_func(d->profile.softproof_button, _softproof_quickbutton_clicked, d);
  g_signal_handlers_unblock_by_func(d->profile.gamut_button, _gamut_quickbutton_clicked, d);
}

static void display_intent_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  const int pos = dt_bauhaus_combobox_get(combo);
  dt_iop_color_intent_t new_intent = darktable.color_profiles->display_intent;
  // we are not using the int value directly so it's robust against changes on lcms' side
  switch(pos)
  {
    case 0:
      new_intent = DT_INTENT_PERCEPTUAL;
      break;
    case 1:
      new_intent = DT_INTENT_RELATIVE_COLORIMETRIC;
      break;
    case 2:
      new_intent = DT_INTENT_SATURATION;
      break;
    case 3:
      new_intent = DT_INTENT_ABSOLUTE_COLORIMETRIC;
      break;
  }

  if(new_intent != darktable.color_profiles->display_intent)
  {
    darktable.color_profiles->display_intent = new_intent;
    dt_dev_reprocess_all(d);
  }
}

static void display2_intent_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  const int pos = dt_bauhaus_combobox_get(combo);
  dt_iop_color_intent_t new_intent = darktable.color_profiles->display2_intent;
  // we are not using the int value directly so it's robust against changes on lcms' side
  switch(pos)
  {
    case 0:
      new_intent = DT_INTENT_PERCEPTUAL;
      break;
    case 1:
      new_intent = DT_INTENT_RELATIVE_COLORIMETRIC;
      break;
    case 2:
      new_intent = DT_INTENT_SATURATION;
      break;
    case 3:
      new_intent = DT_INTENT_ABSOLUTE_COLORIMETRIC;
      break;
  }

  if(new_intent != darktable.color_profiles->display2_intent)
  {
    darktable.color_profiles->display2_intent = new_intent;
    dt_dev_reprocess_all(d);
  }
}

static void softproof_profile_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);

  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;

    if(pp->out_pos == pos)
    {
      if(darktable.color_profiles->softproof_type != pp->type
        || (darktable.color_profiles->softproof_type == DT_COLORSPACE_FILE
            && strcmp(darktable.color_profiles->softproof_filename, pp->filename)))
      {
        darktable.color_profiles->softproof_type = pp->type;
        g_strlcpy(darktable.color_profiles->softproof_filename, pp->filename,
                  sizeof(darktable.color_profiles->softproof_filename));
        profile_changed = TRUE;
      }

      goto end;
    }
  }

  // profile not found, fall back to sRGB. shouldn't happen
  fprintf(stderr, "can't find softproof profile `%s', using sRGB instead\n", dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->softproof_type != DT_COLORSPACE_SRGB;
  darktable.color_profiles->softproof_type = DT_COLORSPACE_SRGB;
  darktable.color_profiles->softproof_filename[0] = '\0';

end:
  if(profile_changed)
  {
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            DT_COLORSPACES_PROFILE_TYPE_SOFTPROOF);
    dt_dev_reprocess_all(d);
  }
}

static void display_profile_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);

  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;

    if(pp->display_pos == pos)
    {
      if(darktable.color_profiles->display_type != pp->type
        || (darktable.color_profiles->display_type == DT_COLORSPACE_FILE
            && strcmp(darktable.color_profiles->display_filename, pp->filename)))
      {
        darktable.color_profiles->display_type = pp->type;
        g_strlcpy(darktable.color_profiles->display_filename, pp->filename,
                  sizeof(darktable.color_profiles->display_filename));
        profile_changed = TRUE;
      }

      goto end;
    }
  }

  // profile not found, fall back to system display profile. shouldn't happen
  fprintf(stderr, "can't find display profile `%s', using system display profile instead\n", dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->display_type != DT_COLORSPACE_DISPLAY;
  darktable.color_profiles->display_type = DT_COLORSPACE_DISPLAY;
  darktable.color_profiles->display_filename[0] = '\0';

end:
  if(profile_changed)
  {
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_DISPLAY);
    dt_dev_reprocess_all(d);
  }
}

static void display2_profile_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);

  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;

    if(pp->display2_pos == pos)
    {
      if(darktable.color_profiles->display2_type != pp->type
         || (darktable.color_profiles->display2_type == DT_COLORSPACE_FILE
             && strcmp(darktable.color_profiles->display2_filename, pp->filename)))
      {
        darktable.color_profiles->display2_type = pp->type;
        g_strlcpy(darktable.color_profiles->display2_filename, pp->filename,
                  sizeof(darktable.color_profiles->display2_filename));
        profile_changed = TRUE;
      }

      goto end;
    }
  }

  // profile not found, fall back to system display2 profile. shouldn't happen
  fprintf(stderr, "can't find preview display profile `%s', using system display profile instead\n",
          dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->display2_type != DT_COLORSPACE_DISPLAY2;
  darktable.color_profiles->display2_type = DT_COLORSPACE_DISPLAY2;
  darktable.color_profiles->display2_filename[0] = '\0';

end:
  if(profile_changed)
  {
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display2_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            DT_COLORSPACES_PROFILE_TYPE_DISPLAY2);
    dt_dev_reprocess_all(d);
  }
}

static void histogram_profile_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);

  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
    if(pp->category_pos == pos)
    {
      if(darktable.color_profiles->histogram_type != pp->type
        || (darktable.color_profiles->histogram_type == DT_COLORSPACE_FILE
            && strcmp(darktable.color_profiles->histogram_filename, pp->filename)))
      {
        darktable.color_profiles->histogram_type = pp->type;
        g_strlcpy(darktable.color_profiles->histogram_filename, pp->filename,
                  sizeof(darktable.color_profiles->histogram_filename));
        profile_changed = TRUE;
      }

      goto end;
    }
  }

  // profile not found, fall back to export profile. shouldn't happen
  fprintf(stderr, "can't find histogram profile `%s', using export profile instead\n", dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->histogram_type != DT_COLORSPACE_WORK;
  darktable.color_profiles->histogram_type = DT_COLORSPACE_WORK;
  darktable.color_profiles->histogram_filename[0] = '\0';

end:
  if(profile_changed)
  {
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_HISTOGRAM);
    dt_dev_reprocess_all(d);
  }
}

// FIXME: turning off lcms2 in prefs hides the widget but leaves the window sized like before -> ugly-ish
static void _preference_changed(gpointer instance, gpointer user_data)
{
  GtkWidget *display_intent = GTK_WIDGET(user_data);
  const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");

  if(force_lcms2)
  {
    gtk_widget_set_no_show_all(display_intent, FALSE);
    gtk_widget_set_visible(display_intent, TRUE);
  }
  else
  {
    gtk_widget_set_no_show_all(display_intent, TRUE);
    gtk_widget_set_visible(display_intent, FALSE);
  }
  // reconstruct dynamic accels list
  dt_dynamic_accel_get_valid_list();
}

static void _preference_prev_downsample_change(gpointer instance, gpointer user_data)
{
  if(user_data != NULL)
  {
    float *ds_value = user_data;
    *ds_value = dt_dev_get_preview_downsampling();
  }
}

static void _update_display_profile_cmb(GtkWidget *cmb_display_profile)
{
  GList *l = darktable.color_profiles->profiles;

  while(l)
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;

    if(prof->display_pos > -1)
      if(prof->type == darktable.color_profiles->display_type
         && (prof->type != DT_COLORSPACE_FILE
             || !strcmp(prof->filename, darktable.color_profiles->display_filename)))

        if(dt_bauhaus_combobox_get(cmb_display_profile) != prof->display_pos)
        {
          dt_bauhaus_combobox_set(cmb_display_profile, prof->display_pos);
          break;
        }

    l = g_list_next(l);
  }
}

static void _update_display2_profile_cmb(GtkWidget *cmb_display_profile)
{
  GList *l = darktable.color_profiles->profiles;

  while(l)
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;

    if(prof->display2_pos > -1)
      if(prof->type == darktable.color_profiles->display2_type
         && (prof->type != DT_COLORSPACE_FILE
             || !strcmp(prof->filename, darktable.color_profiles->display2_filename)))
        if(dt_bauhaus_combobox_get(cmb_display_profile) != prof->display2_pos)
        {
          dt_bauhaus_combobox_set(cmb_display_profile, prof->display2_pos);
          break;
        }

    l = g_list_next(l);
  }
}

static void _display_profile_changed(gpointer instance, uint8_t profile_type, gpointer user_data)
{
  GtkWidget *cmb_display_profile = GTK_WIDGET(user_data);
  _update_display_profile_cmb(cmb_display_profile);
}

static void _display2_profile_changed(gpointer instance, uint8_t profile_type, gpointer user_data)
{
  GtkWidget *cmb_display_profile = GTK_WIDGET(user_data);
  _update_display2_profile_cmb(cmb_display_profile);
}
// end of toolbox //

void gui_init(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
   // Add view specific tool buttons
  // create favorite plugin preset popup tool
  GtkWidget *favorite_presets
      = dtgtk_button_new(dtgtk_cairo_paint_presets, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_tooltip_text(favorite_presets, _("quick access to presets of your favorites"));
  g_signal_connect(G_OBJECT(favorite_presets), "clicked", G_CALLBACK(_darkroom_ui_favorite_presets_popupmenu),
                   NULL);
  dt_view_manager_view_toolbox_add(darktable.view_manager, favorite_presets, DT_VIEW_DARKROOM);

  // create quick styles popup menu tool 
  GtkWidget *styles = dtgtk_button_new(dtgtk_cairo_paint_styles, CPF_STYLE_FLAT, NULL);
  g_signal_connect(G_OBJECT(styles), "clicked", G_CALLBACK(_darkroom_ui_apply_style_popupmenu), NULL);
  gtk_widget_set_tooltip_text(styles, _("quick access for applying any of your styles"));
  dt_view_manager_view_toolbox_add(darktable.view_manager, styles, DT_VIEW_DARKROOM);

  // create second window display button 
  dev->second_window.button
      = dtgtk_togglebutton_new(dtgtk_cairo_paint_display2, CPF_STYLE_FLAT, NULL);
  g_signal_connect(G_OBJECT(dev->second_window.button), "clicked", G_CALLBACK(_second_window_quickbutton_clicked),
                   dev);
  g_signal_connect(G_OBJECT(dev->second_window.button), "button-press-event",
                   G_CALLBACK(_second_window_quickbutton_pressed), dev);
  g_signal_connect(G_OBJECT(dev->second_window.button), "button-release-event",
                   G_CALLBACK(_profile_quickbutton_released), dev);
  gtk_widget_set_tooltip_text(dev->second_window.button, _("display a second darkroom image window"));
  dt_view_manager_view_toolbox_add(darktable.view_manager, dev->second_window.button, DT_VIEW_DARKROOM);

  const int dialog_width = 350;

  // Enable ISO 12646-compliant colour assessment conditions
  dev->iso_12646.button
      = dtgtk_togglebutton_new(dtgtk_cairo_paint_bulb, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_tooltip_text(dev->iso_12646.button,
                              _("toggle ISO 12646 color assessment conditions"));
  g_signal_connect(G_OBJECT(dev->iso_12646.button), "clicked", G_CALLBACK(_iso_12646_quickbutton_clicked), dev);
  dt_view_manager_module_toolbox_add(darktable.view_manager, dev->iso_12646.button, DT_VIEW_DARKROOM);
  // create rawoverexposed popup tool
  {
    // the button
    dev->rawoverexposed.button
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_rawoverexposed, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(dev->rawoverexposed.button,
                                _("toggle raw over exposed indication\nright click for options"));
    g_signal_connect(G_OBJECT(dev->rawoverexposed.button), "clicked",
                     G_CALLBACK(_rawoverexposed_quickbutton_clicked), dev);
    g_signal_connect(G_OBJECT(dev->rawoverexposed.button), "button-press-event",
                     G_CALLBACK(_rawoverexposed_quickbutton_pressed), dev);
    g_signal_connect(G_OBJECT(dev->rawoverexposed.button), "button-release-event",
                     G_CALLBACK(_rawoverexposed_quickbutton_released), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->rawoverexposed.button, DT_VIEW_DARKROOM);

    // and the popup window
    dev->rawoverexposed.floating_window = gtk_popover_new(dev->rawoverexposed.button);
    gtk_widget_set_size_request(GTK_WIDGET(dev->rawoverexposed.floating_window), dialog_width, -1);
    g_object_set(G_OBJECT(dev->rawoverexposed.floating_window), "transitions-enabled", FALSE, NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(dev->rawoverexposed.floating_window), vbox);
    /** let's fill the encapsulating widgets */
    /* mode of operation */
    GtkWidget *mode = dt_bauhaus_combobox_new(NULL);
    dt_bauhaus_widget_set_label(mode, NULL, _("mode"));
    dt_bauhaus_combobox_add(mode, _("mark with CFA color"));
    dt_bauhaus_combobox_add(mode, _("mark with solid color"));
    dt_bauhaus_combobox_add(mode, _("false color"));
    dt_bauhaus_combobox_set(mode, dev->rawoverexposed.mode);
    gtk_widget_set_tooltip_text(mode, _("select how to mark the clipped pixels"));
    g_signal_connect(G_OBJECT(mode), "value-changed", G_CALLBACK(rawoverexposed_mode_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(mode), TRUE, TRUE, 0);
    gtk_widget_set_state_flags(mode, GTK_STATE_FLAG_SELECTED, TRUE);

    /* color scheme */
    GtkWidget *colorscheme = dt_bauhaus_combobox_new(NULL);
    dt_bauhaus_widget_set_label(colorscheme, NULL, _("color scheme"));
    dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "red"));
    dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "green"));
    dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "blue"));
    dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "black"));
    dt_bauhaus_combobox_set(colorscheme, dev->rawoverexposed.colorscheme);
    gtk_widget_set_tooltip_text(
        colorscheme,
        _("select the solid color to indicate over exposure.\nwill only be used if mode = mark with solid color"));
    g_signal_connect(G_OBJECT(colorscheme), "value-changed", G_CALLBACK(rawoverexposed_colorscheme_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(colorscheme), TRUE, TRUE, 0);
    gtk_widget_set_state_flags(colorscheme, GTK_STATE_FLAG_SELECTED, TRUE);

    /* threshold */
    GtkWidget *threshold = dt_bauhaus_slider_new_with_range(NULL, 0.0, 2.0, 0.01, 1.0, 3);
    dt_bauhaus_slider_set(threshold, dev->rawoverexposed.threshold);
    dt_bauhaus_widget_set_label(threshold, NULL, _("clipping threshold"));
    gtk_widget_set_tooltip_text(
        threshold, _("threshold of what shall be considered overexposed\n1.0 - white level\n0.0 - black level"));
    g_signal_connect(G_OBJECT(threshold), "value-changed", G_CALLBACK(rawoverexposed_threshold_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(threshold), TRUE, TRUE, 0);
  }
  // create overexposed popup tool
  {
    // the button
    dev->overexposed.button
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_overexposed, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(dev->overexposed.button,
                                _("toggle over/under exposed indication\nright click for options"));
    g_signal_connect(G_OBJECT(dev->overexposed.button), "clicked",
                     G_CALLBACK(_overexposed_quickbutton_clicked), dev);
    g_signal_connect(G_OBJECT(dev->overexposed.button), "button-press-event",
                     G_CALLBACK(_overexposed_quickbutton_pressed), dev);
    g_signal_connect(G_OBJECT(dev->overexposed.button), "button-release-event",
                     G_CALLBACK(_overexposed_quickbutton_released), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->overexposed.button, DT_VIEW_DARKROOM);

    // and the popup window
    dev->overexposed.floating_window = gtk_popover_new(dev->overexposed.button);
    gtk_widget_set_size_request(GTK_WIDGET(dev->overexposed.floating_window), dialog_width, -1);
    g_object_set(G_OBJECT(dev->overexposed.floating_window), "transitions-enabled", FALSE, NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->overexposed.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    /* color scheme */
    GtkWidget *colorscheme = dt_bauhaus_combobox_new(NULL);
    dt_bauhaus_widget_set_label(colorscheme, NULL, _("color scheme"));
    dt_bauhaus_combobox_add(colorscheme, _("black & white"));
    dt_bauhaus_combobox_add(colorscheme, _("red & blue"));
    dt_bauhaus_combobox_add(colorscheme, _("purple & green"));
    dt_bauhaus_combobox_set(colorscheme, dev->overexposed.colorscheme);
    gtk_widget_set_tooltip_text(colorscheme, _("select colors to indicate over/under exposure"));
    g_signal_connect(G_OBJECT(colorscheme), "value-changed", G_CALLBACK(colorscheme_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(colorscheme), TRUE, TRUE, 0);
    gtk_widget_set_state_flags(colorscheme, GTK_STATE_FLAG_SELECTED, TRUE);

    /* lower */
    GtkWidget *lower = dt_bauhaus_slider_new_with_range(NULL, 0.0, 100.0, 0.1, 2.0, 2);
    dt_bauhaus_slider_set(lower, dev->overexposed.lower);
    dt_bauhaus_slider_set_format(lower, "%.0f%%");
    dt_bauhaus_widget_set_label(lower, NULL, _("lower threshold"));
    gtk_widget_set_tooltip_text(lower, _("threshold of what shall be considered underexposed"));
    g_signal_connect(G_OBJECT(lower), "value-changed", G_CALLBACK(lower_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(lower), TRUE, TRUE, 0);

    /* upper */
    GtkWidget *upper = dt_bauhaus_slider_new_with_range(NULL, 0.0, 100.0, 0.1, 98.0, 2);
    dt_bauhaus_slider_set(upper, dev->overexposed.upper);
    dt_bauhaus_slider_set_format(upper, "%.0f%%");
    dt_bauhaus_widget_set_label(upper, NULL, _("upper threshold"));
    gtk_widget_set_tooltip_text(upper, _("threshold of what shall be considered overexposed"));
    g_signal_connect(G_OBJECT(upper), "value-changed", G_CALLBACK(upper_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(upper), TRUE, TRUE, 0);
  }

  // create profile popup tool & buttons (softproof + gamut)
  {
    // the softproof button
    dev->profile.softproof_button =
      dtgtk_togglebutton_new(dtgtk_cairo_paint_softproof, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(dev->profile.softproof_button,
                                _("toggle softproofing\nright click for profile options"));
    g_signal_connect(G_OBJECT(dev->profile.softproof_button), "clicked",
                     G_CALLBACK(_softproof_quickbutton_clicked), dev);
    g_signal_connect(G_OBJECT(dev->profile.softproof_button), "button-press-event",
                     G_CALLBACK(_softproof_quickbutton_pressed), dev);
    g_signal_connect(G_OBJECT(dev->profile.softproof_button), "button-release-event",
                     G_CALLBACK(_profile_quickbutton_released), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->profile.softproof_button, DT_VIEW_DARKROOM);

    // the gamut check button
    dev->profile.gamut_button =
      dtgtk_togglebutton_new(dtgtk_cairo_paint_gamut_check, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(dev->profile.gamut_button,
                 _("toggle gamut checking\nright click for profile options"));
    g_signal_connect(G_OBJECT(dev->profile.gamut_button), "clicked",
                     G_CALLBACK(_gamut_quickbutton_clicked), dev);
    g_signal_connect(G_OBJECT(dev->profile.gamut_button), "button-press-event",
                     G_CALLBACK(_gamut_quickbutton_pressed), dev);
    g_signal_connect(G_OBJECT(dev->profile.gamut_button), "button-release-event",
                     G_CALLBACK(_profile_quickbutton_released), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->profile.gamut_button, DT_VIEW_DARKROOM);
    // and the popup window, which is shared between the two profile buttons
    dev->profile.floating_window = gtk_popover_new(NULL);
    gtk_widget_set_size_request(GTK_WIDGET(dev->profile.floating_window), dialog_width, -1);
    g_object_set(G_OBJECT(dev->profile.floating_window), "transitions-enabled", FALSE, NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->profile.floating_window), vbox);
    /** let's fill the encapsulating widgets */
    char datadir[PATH_MAX] = { 0 };
    char confdir[PATH_MAX] = { 0 };
    dt_loc_get_user_config_dir(confdir, sizeof(confdir));
    dt_loc_get_datadir(datadir, sizeof(datadir));
    const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");

    GtkWidget *display_intent = dt_bauhaus_combobox_new(NULL);
    dt_bauhaus_widget_set_label(display_intent, NULL, _("display intent"));
    gtk_box_pack_start(GTK_BOX(vbox), display_intent, TRUE, TRUE, 0);
    dt_bauhaus_combobox_add(display_intent, _("perceptual"));
    dt_bauhaus_combobox_add(display_intent, _("relative colorimetric"));
    dt_bauhaus_combobox_add(display_intent, C_("rendering intent", "saturation"));
    dt_bauhaus_combobox_add(display_intent, _("absolute colorimetric"));

    GtkWidget *display2_intent = dt_bauhaus_combobox_new(NULL);
    dt_bauhaus_widget_set_label(display2_intent, NULL, _("preview display intent"));
    gtk_box_pack_start(GTK_BOX(vbox), display2_intent, TRUE, TRUE, 0);
    dt_bauhaus_combobox_add(display2_intent, _("perceptual"));
    dt_bauhaus_combobox_add(display2_intent, _("relative colorimetric"));
    dt_bauhaus_combobox_add(display2_intent, C_("rendering intent", "saturation"));
    dt_bauhaus_combobox_add(display2_intent, _("absolute colorimetric"));

    if(!force_lcms2)
    {
      gtk_widget_set_no_show_all(display_intent, TRUE);
      gtk_widget_set_visible(display_intent, FALSE);
      gtk_widget_set_no_show_all(display2_intent, TRUE);
      gtk_widget_set_visible(display2_intent, FALSE);
    }

    GtkWidget *display_profile = dt_bauhaus_combobox_new(NULL);
    GtkWidget *display2_profile = dt_bauhaus_combobox_new(NULL);
    GtkWidget *softproof_profile = dt_bauhaus_combobox_new(NULL);
    GtkWidget *histogram_profile = dt_bauhaus_combobox_new(NULL);
    dt_bauhaus_widget_set_label(softproof_profile, NULL, _("softproof profile"));
    dt_bauhaus_widget_set_label(display_profile, NULL, _("display profile"));
    dt_bauhaus_widget_set_label(display2_profile, NULL, _("preview display profile"));
    dt_bauhaus_widget_set_label(histogram_profile, NULL, _("histogram profile"));
    gtk_box_pack_start(GTK_BOX(vbox), softproof_profile, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), display_profile, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), display2_profile, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), histogram_profile, TRUE, TRUE, 0);
    GList *l = darktable.color_profiles->profiles;

    while(l)
    {
      dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;

      if(prof->display_pos > -1)
      {
        dt_bauhaus_combobox_add(display_profile, prof->name);

        if(prof->type == darktable.color_profiles->display_type
          && (prof->type != DT_COLORSPACE_FILE
              || !strcmp(prof->filename, darktable.color_profiles->display_filename)))
          dt_bauhaus_combobox_set(display_profile, prof->display_pos);
      }

      if(prof->display2_pos > -1)
      {
        dt_bauhaus_combobox_add(display2_profile, prof->name);

        if(prof->type == darktable.color_profiles->display2_type
           && (prof->type != DT_COLORSPACE_FILE
               || !strcmp(prof->filename, darktable.color_profiles->display2_filename)))
          dt_bauhaus_combobox_set(display2_profile, prof->display2_pos);
      }
      // the system display profile is only suitable for display purposes
      if(prof->out_pos > -1)
      {
        dt_bauhaus_combobox_add(softproof_profile, prof->name);

        if(prof->type == darktable.color_profiles->softproof_type
          && (prof->type != DT_COLORSPACE_FILE
              || !strcmp(prof->filename, darktable.color_profiles->softproof_filename)))
          dt_bauhaus_combobox_set(softproof_profile, prof->out_pos);
      }

      if(prof->category_pos > -1)
      {
        dt_bauhaus_combobox_add(histogram_profile, prof->name);

        if(prof->type == darktable.color_profiles->histogram_type
          && (prof->type != DT_COLORSPACE_FILE
              || !strcmp(prof->filename, darktable.color_profiles->histogram_filename)))
          dt_bauhaus_combobox_set(histogram_profile, prof->category_pos);
      }

      l = g_list_next(l);
    }

    char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
    char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);
    char *tooltip = g_strdup_printf(_("display ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(display_profile, tooltip);
    g_free(tooltip);
    tooltip = g_strdup_printf(_("preview display ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(display2_profile, tooltip);
    g_free(tooltip);
    tooltip = g_strdup_printf(_("softproof ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(softproof_profile, tooltip);
    g_free(tooltip);
    tooltip = g_strdup_printf(_("histogram and color picker ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(histogram_profile, tooltip);
    g_free(tooltip);
    g_free(system_profile_dir);
    g_free(user_profile_dir);

    g_signal_connect(G_OBJECT(display_intent), "value-changed", G_CALLBACK(display_intent_callback), dev);
    g_signal_connect(G_OBJECT(display_profile), "value-changed", G_CALLBACK(display_profile_callback), dev);
    g_signal_connect(G_OBJECT(display2_intent), "value-changed", G_CALLBACK(display2_intent_callback), dev);
    g_signal_connect(G_OBJECT(display2_profile), "value-changed", G_CALLBACK(display2_profile_callback), dev);
    g_signal_connect(G_OBJECT(softproof_profile), "value-changed", G_CALLBACK(softproof_profile_callback), dev);
    g_signal_connect(G_OBJECT(histogram_profile), "value-changed", G_CALLBACK(histogram_profile_callback), dev);

    _update_softproof_gamut_checking(dev);

    dt_control_signal_connect(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE,
                              G_CALLBACK(_preference_prev_downsample_change), &(dev->preview_downsampling));
    // update the gui when the preferences changed (i.e. show intent when using lcms2)
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE,
                              G_CALLBACK(_preference_changed), (gpointer)display_intent);
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE, G_CALLBACK(_preference_changed),
                              (gpointer)display2_intent);
    // and when profiles change
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                              G_CALLBACK(_display_profile_changed), (gpointer)display_profile);
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                              G_CALLBACK(_display2_profile_changed), (gpointer)display2_profile);
  }

  // create overlay color changer popup tool
  {
    // the button
    dev->overlay_color.button
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_grid, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(dev->overlay_color.button,
                                _("set the color of lines that overlay the image (drawn masks, crop and rotate guides etc.)"));
    g_signal_connect(G_OBJECT(dev->overlay_color.button), "clicked",
                     G_CALLBACK(_overlay_color_quickbutton_clicked), dev);
    g_signal_connect(G_OBJECT(dev->overlay_color.button), "button-press-event",
                     G_CALLBACK(_overlay_color_quickbutton_pressed), dev);
    g_signal_connect(G_OBJECT(dev->overlay_color.button), "button-release-event",
                     G_CALLBACK(_overlay_color_quickbutton_released), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->overlay_color.button, DT_VIEW_DARKROOM);

    // and the popup window
    dev->overlay_color.floating_window = gtk_popover_new(dev->overlay_color.button);
    gtk_widget_set_size_request(GTK_WIDGET(dev->overlay_color.floating_window), dialog_width, -1);
    g_object_set(G_OBJECT(dev->overlay_color.floating_window), "transitions-enabled", FALSE, NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->overlay_color.floating_window), vbox);

    // let's fill the encapsulating widget
    GtkWidget *overlay_colors = dev->overlay_color.colors = dt_bauhaus_combobox_new(NULL);
    dt_bauhaus_widget_set_label(overlay_colors, NULL, _("overlay color"));
    dt_bauhaus_combobox_add(overlay_colors, _("gray"));
    dt_bauhaus_combobox_add(overlay_colors, _("red"));
    dt_bauhaus_combobox_add(overlay_colors, _("green"));
    dt_bauhaus_combobox_add(overlay_colors, _("yellow"));
    dt_bauhaus_combobox_add(overlay_colors, _("cyan"));
    dt_bauhaus_combobox_add(overlay_colors, _("magenta"));
    dt_bauhaus_combobox_set(overlay_colors, dev->overlay_color.color);
    gtk_widget_set_tooltip_text(overlay_colors, _("set overlay color"));
    g_signal_connect(G_OBJECT(overlay_colors), "value-changed", G_CALLBACK(overlay_colors_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(overlay_colors), TRUE, TRUE, 0);
    gtk_widget_set_state_flags(overlay_colors, GTK_STATE_FLAG_SELECTED, TRUE);
  }

  darktable.view_manager->proxy.darkroom.view = self;
  darktable.view_manager->proxy.darkroom.get_layout = _lib_darkroom_get_layout;
  dev->border_size = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
}

void enter(dt_view_t *self)
{
  // prevent accels_window to refresh
  darktable.view_manager->accels_window.prevent_refresh = TRUE;
  // clean the undo list
  dt_undo_clear(darktable.undo, DT_UNDO_DEVELOP);
  // connect to ui pipe finished signal for redraw
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                            G_CALLBACK(_darkroom_ui_pipe_finish_signal_callback), (gpointer)self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW2_PIPE_FINISHED,
                            G_CALLBACK(_darkroom_ui_preview2_pipe_finish_signal_callback), (gpointer)self);
  dt_print(DT_DEBUG_CONTROL, "[run_job+] 11 %f in darkroom mode\n", dt_get_wtime());
  dt_develop_t *dev = (dt_develop_t *)self->data;

  if(!dev->form_gui)
  {
    dev->form_gui = (dt_masks_form_gui_t *)calloc(1, sizeof(dt_masks_form_gui_t));
    dt_masks_init_form_gui(dev->form_gui);
  }

  dt_masks_change_form_gui(NULL);
  dev->form_gui->pipe_hash = 0;
  dev->form_gui->formid = 0;
  dev->gui_leaving = 0;
  dev->gui_module = NULL;
  // change active image
  dt_view_active_images_reset(FALSE);
  dt_view_active_images_add(dev->image_storage.id, TRUE);

  dt_control_set_dev_zoom(DT_ZOOM_FIT);
  dt_control_set_dev_zoom_x(0);
  dt_control_set_dev_zoom_y(0);
  dt_control_set_dev_closeup(0);
  // take a copy of the image struct for convenience.
  dt_dev_load_image(darktable.develop, dev->image_storage.id);
  // add IOP modules to plugin list
  // avoid triggering of events before plugin is ready:
  ++darktable.gui->reset;
  char option[1024];
  GList *modules = g_list_last(dev->iop);

  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    /* initialize gui if iop have one defined */
    if(!dt_iop_is_hidden(module))
    {
      module->gui_init(module);
      dt_iop_reload_defaults(module);
      /* add module to right panel */
      GtkWidget *expander = dt_iop_gui_get_expander(module);
      dt_ui_container_add_widget(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER, expander);
      snprintf(option, sizeof(option), "plugins/darkroom/%s/expanded", module->op);
      
      if(dt_conf_get_bool(option))
        dt_iop_gui_set_expanded(module, TRUE, dt_conf_get_bool("darkroom/ui/single_module"));
      else
        dt_iop_gui_set_expanded(module, FALSE, FALSE);
    }

    modules = g_list_previous(modules);
  }
  // make signals work again:
  --darktable.gui->reset;
  // signal that darktable.develop is initialized and ready to be used
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE);
  // synch gui and flag pipe as dirty
  // this is done here and not in dt_read_history, as it would else be triggered before module->gui_init.
  dt_dev_pop_history_items(dev, dev->history_end);
  // ensure that filmstrip shows current image
  dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), dev->image_storage.id, TRUE);
  // get last active plugin:
  gchar *active_plugin = dt_conf_get_string("plugins/darkroom/active");

  if(active_plugin)
  {
    modules = dev->iop;

    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

      if(!strcmp(module->op, active_plugin))
        dt_iop_request_focus(module);

      modules = g_list_next(modules);
    }

    g_free(active_plugin);
  }
  // update module multishow state now modules are loaded
  dt_dev_modules_update_multishow(dev);
  // image should be there now.
  float zoom_x, zoom_y;
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, DT_ZOOM_FIT, 0, NULL, NULL);
  dt_control_set_dev_zoom_x(zoom_x);
  dt_control_set_dev_zoom_y(zoom_y);
  // connect signal for filmstrip image activate
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                            G_CALLBACK(_view_darkroom_filmstrip_activate_callback), self);
  dt_collection_hint_message(darktable.collection);
  dt_ui_scrollbars_show(darktable.gui->ui, dt_conf_get_bool("darkroom/ui/scrollbars"));

  if(dt_conf_get_bool("second_window/last_visible"))
  {
    _darkroom_display_second_window(dev);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->second_window.button), TRUE);
  }
  // just make sure at this stage we have only history info into the undo, all automatic
  // tagging should be ignored.
  dt_undo_clear(darktable.undo, DT_UNDO_TAGS);
  // update accels_window
  darktable.view_manager->accels_window.prevent_refresh = FALSE;
}

void leave(dt_view_t *self)
{
  dt_iop_color_picker_cleanup();

  if(darktable.lib->proxy.colorpicker.picker_proxy)
    dt_iop_color_picker_reset(darktable.lib->proxy.colorpicker.picker_proxy->module, FALSE);

  /* disconnect from filmstrip image activate */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_view_darkroom_filmstrip_activate_callback),
                               (gpointer)self);
  /* disconnect from pipe finish signal */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_darkroom_ui_pipe_finish_signal_callback),
                               (gpointer)self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_darkroom_ui_preview2_pipe_finish_signal_callback),
                               (gpointer)self);
  // store last active plugin:
  if(darktable.develop->gui_module)
    dt_conf_set_string("plugins/darkroom/active", darktable.develop->gui_module->op);
  else
    dt_conf_set_string("plugins/darkroom/active", "");

  dt_develop_t *dev = (dt_develop_t *)self->data;
  // reset color assesment mode
  if(dev->iso_12646.enabled)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->iso_12646.button), FALSE);
    dev->iso_12646.enabled = FALSE;
    dev->width = dev->orig_width;
    dev->height = dev->orig_height;
    dev->border_size = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  }
  // commit image ops to db
  dt_dev_write_history(dev);
  // update aspect ratio
  if(dev->preview_pipe->backbuf && dev->preview_status == DT_DEV_PIXELPIPE_VALID)
  {
    double aspect_ratio = (double)dev->preview_pipe->backbuf_width / (double)dev->preview_pipe->backbuf_height;
    dt_image_set_aspect_ratio_to(dev->preview_pipe->image.id, aspect_ratio, FALSE);
  }
  else
    dt_image_set_aspect_ratio(dev->image_storage.id, FALSE);
  // be sure light table will regenerate the thumbnail:
  if (!dt_history_hash_is_mipmap_synced(dev->image_storage.id))
  {
    dt_mipmap_cache_remove(darktable.mipmap_cache, dev->image_storage.id);
    dt_image_reset_final_size(dev->image_storage.id);
    // dump new xmp data
    dt_image_synch_xmp(dev->image_storage.id);
    dt_history_hash_set_mipmap(dev->image_storage.id);
  }
  // clear gui.
  dt_pthread_mutex_lock(&dev->preview_pipe_mutex);
  dt_pthread_mutex_lock(&dev->preview2_pipe_mutex);
  dt_pthread_mutex_lock(&dev->pipe_mutex);
  dev->gui_leaving = 1;

  dt_dev_pixelpipe_cleanup_nodes(dev->pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview2_pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);
  dt_pthread_mutex_lock(&dev->history_mutex);

  while(dev->history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(dev->history->data);
    // printf("removing history item %d - %s, data %f %f\n", hist->module->instance, hist->module->op, *(float
    // *)hist->params, *((float *)hist->params+1));
    dt_dev_free_history_item(hist);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }

  while(dev->iop)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(dev->iop->data);
    if(!dt_iop_is_hidden(module)) dt_iop_gui_cleanup_module(module);

    dt_dev_cleanup_module_accels(module);
    module->accel_closures = NULL;
    dt_iop_cleanup_module(module);
    free(module);
    dev->iop = g_list_delete_link(dev->iop, dev->iop);
  }

  while(dev->alliop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->alliop->data);
    free(dev->alliop->data);
    dev->alliop = g_list_delete_link(dev->alliop, dev->alliop);
  }

  dt_pthread_mutex_unlock(&dev->history_mutex);
  dt_pthread_mutex_unlock(&dev->pipe_mutex);
  dt_pthread_mutex_unlock(&dev->preview2_pipe_mutex);
  dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);
  // cleanup visible masks
  if(dev->form_gui)
  {
    dev->gui_module = NULL; // modules have already been free()
    dt_masks_clear_form_gui(dev);
    free(dev->form_gui);
    dev->form_gui = NULL;
    dt_masks_change_form_gui(NULL);
  }
  // clear masks
  g_list_free_full(dev->forms, (void (*)(void *))dt_masks_free_form);
  dev->forms = NULL;
  g_list_free_full(dev->allforms, (void (*)(void *))dt_masks_free_form);
  dev->allforms = NULL;
  // take care of the overexposed window
  if(dev->overexposed.timeout > 0)
    g_source_remove(dev->overexposed.timeout);

  gtk_widget_hide(dev->overexposed.floating_window);
  gtk_widget_hide(dev->profile.floating_window);
  dt_ui_scrollbars_show(darktable.gui->ui, FALSE);
  // darkroom development could have changed a collection, so update that before being back in lightroom
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                             g_list_append(NULL, GINT_TO_POINTER(darktable.develop->image_storage.id)));
  darktable.develop->image_storage.id = -1;
  dt_print(DT_DEBUG_CONTROL, "[run_job-] 11 %f in darkroom mode\n", dt_get_wtime());
}

void mouse_leave(dt_view_t *self)
{
  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of opened image.
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_control_set_mouse_over_id(dev->image_storage.id);
  // masks
  int handled = dt_masks_events_mouse_leave(dev->gui_module);

  if(handled) return;
  // module
  if(dev->gui_module && dev->gui_module->mouse_leave)
    handled = dev->gui_module->mouse_leave(dev->gui_module);

  // reset any changes the selected plugin might have made.
  dt_control_change_cursor(GDK_LEFT_PTR);
}

/* This helper function tests for a position to be within the displayed area
   of an image. To avoid "border cases" we accept values to be slighly out of area too.
*/
static int mouse_in_imagearea(dt_view_t *self, double x, double y)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  const int closeup = dt_control_get_dev_closeup();
  const int pwidth = (dev->pipe->output_backbuf_width<<closeup) / darktable.gui->ppd;
  const int pheight = (dev->pipe->output_backbuf_height<<closeup) / darktable.gui->ppd;

  x -= (self->width - pwidth) / 2;
  y -= (self->height - pheight) / 2;

  if((x < -3) || (x > (pwidth + 6)) || (y < -3) || (y > (pheight + 6)))
    return FALSE;

  return TRUE;
}

void mouse_enter(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_masks_events_mouse_enter(dev->gui_module);
}

void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  const int32_t tb = dev->border_size;
  const int32_t capwd = self->width  - 2*tb;
  const int32_t capht = self->height - 2*tb;
  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of opened image.
  int32_t mouse_over_id = dt_control_get_mouse_over_id();

  if(mouse_over_id == -1)
  {
    mouse_over_id = dev->image_storage.id;
    dt_control_set_mouse_over_id(mouse_over_id);
  }

  dt_control_t *ctl = darktable.control;
  const int32_t width_i = self->width;
  const int32_t height_i = self->height;
  float offx = 0.0f, offy = 0.0f;

  if(width_i > capwd)
    offx = (capwd - width_i) * .5f;

  if(height_i > capht)
    offy = (capht - height_i) * .5f;

  int handled = 0;

  if(dev->gui_module && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF
                     && ctl->button_down && ctl->button_down_which == 1)
  {
    // module requested a color box
    if(mouse_in_imagearea(self, x, y))
    {
      // Make sure a minimal width/height
      float delta_x = 1 / (float) dev->pipe->processed_width;
      float delta_y = 1 / (float) dev->pipe->processed_height;

      float zoom_x, zoom_y;
      dt_dev_get_pointer_zoom_pos(dev, x + offx, y + offy, &zoom_x, &zoom_y);

      if(darktable.lib->proxy.colorpicker.size)
      {
        dev->gui_module->color_picker_box[0] = fmaxf(0.0, fminf(dev->gui_module->color_picker_point[0], .5f + zoom_x) - delta_x);
        dev->gui_module->color_picker_box[1] = fmaxf(0.0, fminf(dev->gui_module->color_picker_point[1], .5f + zoom_y) - delta_y);
        dev->gui_module->color_picker_box[2] = fminf(1.0, fmaxf(dev->gui_module->color_picker_point[0], .5f + zoom_x) + delta_x);
        dev->gui_module->color_picker_box[3] = fminf(1.0, fmaxf(dev->gui_module->color_picker_point[1], .5f + zoom_y) + delta_y);
      }
      else
      {
        dev->gui_module->color_picker_point[0] = .5f + zoom_x;
        dev->gui_module->color_picker_point[1] = .5f + zoom_y;

        dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
      }
    }

    dt_control_queue_redraw();
    return;
  }

  x += offx;
  y += offy;
  // masks
  handled = dt_masks_events_mouse_moved(dev->gui_module, x, y, pressure, which);

  if(handled) return;
  // module
  if(dev->gui_module && dev->gui_module->mouse_moved)
    handled = dev->gui_module->mouse_moved(dev->gui_module, x, y, pressure, which);

  if(handled) return;

  if(darktable.control->button_down && darktable.control->button_down_which == 1)
  {
    // depending on dev_zoom, adjust dev_zoom_x/y.
    const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    const int closeup = dt_control_get_dev_closeup();
    int procw, proch;
    dt_dev_get_processed_size(dev, &procw, &proch);
    const float scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
    float old_zoom_x, old_zoom_y;
    old_zoom_x = dt_control_get_dev_zoom_x();
    old_zoom_y = dt_control_get_dev_zoom_y();
    float zx = old_zoom_x - (1.0 / scale) * (x - ctl->button_x - offx) / procw;
    float zy = old_zoom_y - (1.0 / scale) * (y - ctl->button_y - offy) / proch;
    dt_dev_check_zoom_bounds(dev, &zx, &zy, zoom, closeup, NULL, NULL);
    dt_control_set_dev_zoom_x(zx);
    dt_control_set_dev_zoom_y(zy);
    ctl->button_x = x - offx;
    ctl->button_y = y - offy;
    dt_dev_invalidate(dev);
    dt_control_queue_redraw_center();
    dt_control_navigation_redraw();
  }
}

int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_develop_t *dev = darktable.develop;
  const int32_t tb = dev->border_size;
  const int32_t capwd = self->width  - 2*tb;
  const int32_t capht = self->height - 2*tb;
  const int32_t width_i = self->width;
  const int32_t height_i = self->height;

  if(width_i > capwd)
    x += (capwd - width_i) * .5f;

  if(height_i > capht)
    y += (capht - height_i) * .5f;

  int handled = 0;

  if(dev->gui_module && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF && which == 1)
  {
    dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
    dt_control_queue_redraw();
    return 1;
  }
  // masks
  if(dev->form_visible)
    handled = dt_masks_events_button_released(dev->gui_module, x, y, which, state);

  if(handled)
    return handled;
  // module
  if(dev->gui_module && dev->gui_module->button_released)
    handled = dev->gui_module->button_released(dev->gui_module, x, y, which, state);

  if(handled)
    return handled;

  if(which == 1)
    dt_control_change_cursor(GDK_LEFT_PTR);

  return 1;
}

int button_pressed(dt_view_t *self, double x, double y, double pressure, int which, int type, uint32_t state)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  const int32_t tb = dev->border_size;
  const int32_t capwd = self->width  - 2*tb;
  const int32_t capht = self->height - 2*tb;
  const int32_t width_i = self->width;
  const int32_t height_i = self->height;
  float offx = 0.0f, offy = 0.0f;

  if(width_i > capwd)
    offx = (capwd - width_i) * .5f;

  if(height_i > capht)
    offy = (capht - height_i) * .5f;

  int handled = 0;

  if(dev->gui_module && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF && which == 1)
  {
    float zoom_x = 0.0f, zoom_y = 0.0f;
    dt_dev_get_pointer_zoom_pos(dev, x + offx, y + offy, &zoom_x, &zoom_y);

    if(mouse_in_imagearea(self, x, y))
    {
      // The default box will be a square with 1% of the image width
      const float delta_x = 0.01f;
      const float delta_y = delta_x * (float)dev->pipe->processed_width / (float)dev->pipe->processed_height;
      zoom_x += 0.5f;
      zoom_y += 0.5f;
      dev->gui_module->color_picker_point[0] = zoom_x;
      dev->gui_module->color_picker_point[1] = zoom_y;

      if(darktable.lib->proxy.colorpicker.size)
      {
        gboolean on_corner_prev_box = TRUE;
        float opposite_x, opposite_y;

        if(fabsf(zoom_x - dev->gui_module->color_picker_box[0]) < .005f)
          opposite_x = dev->gui_module->color_picker_box[2];
        else if(fabsf(zoom_x - dev->gui_module->color_picker_box[2]) < .005f)
          opposite_x = dev->gui_module->color_picker_box[0];
        else
          on_corner_prev_box = FALSE;

        if(fabsf(zoom_y - dev->gui_module->color_picker_box[1]) < .005f)
          opposite_y = dev->gui_module->color_picker_box[3];
        else if(fabsf(zoom_y - dev->gui_module->color_picker_box[3]) < .005f)
          opposite_y = dev->gui_module->color_picker_box[1];
        else
          on_corner_prev_box = FALSE;

        if(on_corner_prev_box)
        {
          dev->gui_module->color_picker_point[0] = opposite_x;
          dev->gui_module->color_picker_point[1] = opposite_y;
        }
        else
        {
          dev->gui_module->color_picker_box[0] = fmaxf(0.0, zoom_x - delta_x);
          dev->gui_module->color_picker_box[1] = fmaxf(0.0, zoom_y - delta_y);
          dev->gui_module->color_picker_box[2] = fminf(1.0, zoom_x + delta_x);
          dev->gui_module->color_picker_box[3] = fminf(1.0, zoom_y + delta_y);
        }
      }
      else
        dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
    }

    dt_control_queue_redraw();
    return 1;
  }

  if(dev->gui_module && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF && which == 3)
  {
    // default is hardcoded this way
    dev->gui_module->color_picker_box[0] = dev->gui_module->color_picker_box[1] = .01f;
    dev->gui_module->color_picker_box[2] = dev->gui_module->color_picker_box[3] = .99f;
    dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
    dt_control_queue_redraw();
    return 1;
  }

  x += offx;
  y += offy;
  // masks
  if(dev->form_visible)
    handled = dt_masks_events_button_pressed(dev->gui_module, x, y, pressure, which, type, state);

  if(handled)
    return handled;
  // module
  if(dev->gui_module && dev->gui_module->button_pressed)
    handled = dev->gui_module->button_pressed(dev->gui_module, x, y, pressure, which, type, state);

  if(handled)
    return handled;

  if(which == 1 && type == GDK_2BUTTON_PRESS)
    return 0;

  if(which == 1)
  {
    dt_control_change_cursor(GDK_HAND1);
    return 1;
  }

  if(which == 2)
  {
    // zoom to 1:1 2:1 and back
    dt_dev_zoom_t zoom;
    int closeup, procw, proch;
    float zoom_x, zoom_y;
    zoom = dt_control_get_dev_zoom();
    closeup = dt_control_get_dev_closeup();
    zoom_x = dt_control_get_dev_zoom_x();
    zoom_y = dt_control_get_dev_zoom_y();
    dt_dev_get_processed_size(dev, &procw, &proch);
    const float scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
    zoom_x += (1.0 / scale) * (x - .5f * dev->width) / procw;
    zoom_y += (1.0 / scale) * (y - .5f * dev->height) / proch;

    if(zoom == DT_ZOOM_1)
    {
      if(!closeup)
        closeup = 1;
      else
      {
        zoom = DT_ZOOM_FIT;
        zoom_x = zoom_y = 0.0f;
        closeup = 0;
      }
    }
    else
      zoom = DT_ZOOM_1;

    dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
    dt_control_set_dev_zoom(zoom);
    dt_control_set_dev_closeup(closeup);
    dt_control_set_dev_zoom_x(zoom_x);
    dt_control_set_dev_zoom_y(zoom_y);
    dt_dev_invalidate(dev);
    dt_control_queue_redraw_center();
    dt_control_navigation_redraw();
    return 1;
  }

  return 0;
}

void scrollbar_changed(dt_view_t *self, double x, double y)
{
  dt_control_set_dev_zoom_x(x);
  dt_control_set_dev_zoom_y(y);
  /* redraw pipe */
  dt_dev_invalidate(darktable.develop);
  dt_control_queue_redraw_center();
  dt_control_navigation_redraw();
}

void scrolled(dt_view_t *self, double x, double y, int up, int state)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  const int32_t tb = dev->border_size;
  const int32_t capwd = self->width  - 2*tb;
  const int32_t capht = self->height - 2*tb;
  const int32_t width_i = self->width;
  const int32_t height_i = self->height;

  if(width_i > capwd)
    x += (capwd - width_i) * .5f;

  if(height_i > capht)
    y += (capht - height_i) * .5f;

  int handled = 0;
  // dynamic accels
  if(self->dynamic_accel_current && self->dynamic_accel_current->widget)
  {
    GtkWidget *widget = self->dynamic_accel_current->widget;
    dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

    if(w->type == DT_BAUHAUS_SLIDER)
    {
      float value = dt_bauhaus_slider_get(widget);
      float step = dt_bauhaus_slider_get_step(widget);
      float multiplier = dt_accel_get_slider_scale_multiplier();
      const float min_visible = powf(10.0f, -dt_bauhaus_slider_get_digits(widget));

      if(fabsf(step*multiplier) < min_visible)
        multiplier = min_visible / fabsf(step);

      if(up)
        dt_bauhaus_slider_set(self->dynamic_accel_current->widget, value + step * multiplier);
      else
        dt_bauhaus_slider_set(self->dynamic_accel_current->widget, value - step * multiplier);
    }
    else
    {
      const int currentval = dt_bauhaus_combobox_get(widget);

      if(up)
      {
        const int nextval = currentval + 1 >= dt_bauhaus_combobox_length(widget) ? 0 : currentval + 1;
        dt_bauhaus_combobox_set(widget, nextval);
      }
      else
      {
        const int prevval = currentval - 1 < 0 ? dt_bauhaus_combobox_length(widget) : currentval - 1;
        dt_bauhaus_combobox_set(widget, prevval);
      }

    }

    g_signal_emit_by_name(G_OBJECT(self->dynamic_accel_current->widget), "value-changed");
    dt_accel_widget_toast(self->dynamic_accel_current->widget);
    return;
  }
  // masks
  if(dev->form_visible)
    handled = dt_masks_events_mouse_scrolled(dev->gui_module, x, y, up, state);

  if(handled) return;
  // module
  if(dev->gui_module && dev->gui_module->scrolled)
    handled = dev->gui_module->scrolled(dev->gui_module, x, y, up, state);

  if(handled) return;
  // free zoom
  dt_dev_zoom_t zoom;
  int closeup, procw, proch;
  float zoom_x, zoom_y;
  zoom = dt_control_get_dev_zoom();
  closeup = dt_control_get_dev_closeup();
  zoom_x = dt_control_get_dev_zoom_x();
  zoom_y = dt_control_get_dev_zoom_y();
  dt_dev_get_processed_size(dev, &procw, &proch);
  float scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
  const float fitscale = dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, 1.0, 0);
  float oldscale = scale;
  // offset from center now (current zoom_{x,y} points there)
  float mouse_off_x = x - .5 * dev->width, mouse_off_y = y - .5 * dev->height;
  zoom_x += mouse_off_x / (procw * scale);
  zoom_y += mouse_off_y / (proch * scale);
  zoom = DT_ZOOM_FREE;
  closeup = 0;

  if(up)
  {
    if((scale == 1.0f || scale == 2.0f) && !((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK))
      return;

    if(scale >= 16.0f)
      return;
    else if(scale >= 8.0f)
      scale = 16.0;
    else if(scale >= 4.0f)
      scale = 8.0;
    else if(scale >= 2.0f)
      scale = 4.0;
    else if(scale < fitscale)
      scale += .05f * (1.0f - fitscale);
    else
      scale += .1f * (1.0f - fitscale);
  }
  else
  {
    if(scale == fitscale && !((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK))
      return;
    else if(scale < 0.5 * fitscale)
      return;
    else if(scale <= fitscale)
      scale -= .05f * (1.0f - fitscale);
    else if(scale <= 2.0f)
      scale -= .1f * (1.0f - fitscale);
    else if(scale <= 4.0f)
      scale = 2.0f;
    else if(scale <= 8.0f)
      scale = 4.0f;
    else
      scale = 8.0f;
  }
  // we want to be sure to stop at 1:1 and FIT levels
  if((scale - 1.0) * (oldscale - 1.0) < 0)
    scale = 1.0f;

  if((scale - fitscale) * (oldscale - fitscale) < 0)
    scale = fitscale;

  scale = fmaxf(fminf(scale, 16.0f), 0.5 * fitscale);
  // for 200% zoom we want pixel doubling instead of interpolation
  if(scale > 15.9999f)
  {
    scale = 1.0f; // don't interpolate
    closeup = 4;  // enable closeup mode (pixel doubling)
  }
  else if(scale > 7.9999f)
  {
    scale = 1.0f; // don't interpolate
    closeup = 3;  // enable closeup mode (pixel doubling)
  }
  else if(scale > 3.9999f)
  {
    scale = 1.0f; // don't interpolate
    closeup = 2;  // enable closeup mode (pixel doubling)
  }
  else if(scale > 1.9999f)
  {
    scale = 1.0f; // don't interpolate
    closeup = 1;  // enable closeup mode (pixel doubling)
  }

  if(fabsf(scale - 1.0f) < 0.001f)
    zoom = DT_ZOOM_1;

  if(fabsf(scale - fitscale) < 0.001f)
    zoom = DT_ZOOM_FIT;

  dt_control_set_dev_zoom_scale(scale);
  dt_control_set_dev_closeup(closeup);
  scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);

  zoom_x -= mouse_off_x / (procw * scale);
  zoom_y -= mouse_off_y / (proch * scale);
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
  dt_control_set_dev_zoom(zoom);
  dt_control_set_dev_zoom_x(zoom_x);
  dt_control_set_dev_zoom_y(zoom_y);
  dt_dev_invalidate(dev);
  dt_control_queue_redraw_center();
  dt_control_navigation_redraw();
}

int key_released(dt_view_t *self, guint key, guint state)
{
  return 1;
}

int key_pressed(dt_view_t *self, guint key, guint state)
{
  return 1;
}

void configure(dt_view_t *self, int wd, int ht)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dev->orig_width = wd;
  dev->orig_height = ht;
  dt_dev_configure(dev, wd, ht);
}

GSList *mouse_actions(const dt_view_t *self)
{
  GSList *lm = NULL;
  GSList *lm2 = NULL;
  dt_mouse_action_t *a = NULL;

  a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->action = DT_MOUSE_ACTION_DOUBLE_LEFT;
  g_strlcpy(a->name, _("switch to lighttable"), sizeof(a->name));
  lm = g_slist_append(lm, a);

  a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->action = DT_MOUSE_ACTION_SCROLL;
  g_strlcpy(a->name, _("zoom in the image"), sizeof(a->name));
  lm = g_slist_append(lm, a);

  a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->key.accel_mods = GDK_CONTROL_MASK;
  a->action = DT_MOUSE_ACTION_SCROLL;
  g_strlcpy(a->name, _("unbounded zoom in the image"), sizeof(a->name));
  lm = g_slist_append(lm, a);

  a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->action = DT_MOUSE_ACTION_MIDDLE;
  g_strlcpy(a->name, _("zoom to 100% 200% and back"), sizeof(a->name));
  lm = g_slist_append(lm, a);

  a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->key.accel_mods = GDK_SHIFT_MASK;
  a->action = DT_MOUSE_ACTION_SCROLL;
  g_strlcpy(a->name, _("[modules] expand module without closing others"), sizeof(a->name));
  lm = g_slist_append(lm, a);

  a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->key.accel_mods = GDK_SHIFT_MASK | GDK_CONTROL_MASK;
  a->action = DT_MOUSE_ACTION_DRAG_DROP;
  g_strlcpy(a->name, _("[modules] change module position in pipe"), sizeof(a->name));
  lm = g_slist_append(lm, a);
  const dt_develop_t *dev = (dt_develop_t *)self->data;

  if(dev->form_visible)// masks
    lm2 = dt_masks_mouse_actions(dev->form_visible);
  else if(dev->gui_module && dev->gui_module->mouse_actions)
    // modules with on canvas actions
    lm2 = dev->gui_module->mouse_actions(dev->gui_module);
  // we concatenate the 2 lists
  GSList *l = lm2;

  while(l)
  {
    a = (dt_mouse_action_t *)l->data;
    if(a) lm = g_slist_append(lm, a);
    l = g_slist_next(l);
  }

  g_slist_free(lm2);

  return lm;
}

//-----------------------------------------------------------
// second darkroom window
//-----------------------------------------------------------

/* helper macro that applies the DPI transformation to fixed pixel values. input should be defaulting to 96
 * DPI */
#define DT_PIXEL_APPLY_DPI_2ND_WND(dev, value) ((value) * dev->second_window.dpi_factor)

static void second_window_expose(GtkWidget *widget, dt_develop_t *dev, cairo_t *cri, int32_t width, int32_t height,
                                 int32_t pointerx, int32_t pointery)
{
  cairo_set_source_rgb(cri, .2, .2, .2);
  cairo_save(cri);

  if(dev->preview2_status == DT_DEV_PIXELPIPE_DIRTY || dev->preview2_status == DT_DEV_PIXELPIPE_INVALID
     || dev->pipe->input_timestamp > dev->preview2_pipe->input_timestamp)
    dt_dev_process_preview2(dev);

  dt_pthread_mutex_t *mutex = NULL;
  const float zoom_y = dt_second_window_get_dev_zoom_y(dev);
  const float zoom_x = dt_second_window_get_dev_zoom_x(dev);
  const dt_dev_zoom_t zoom = dt_second_window_get_dev_zoom(dev);
  const int closeup = dt_second_window_get_dev_closeup(dev);
  const float backbuf_scale = dt_second_window_get_zoom_scale(dev, zoom, 1.0f, 0) * dev->second_window.ppd;

  static cairo_surface_t *image_surface = NULL;
  static int image_surface_width = 0, image_surface_height = 0, image_surface_imgid = -1;

  if(image_surface_width != width || image_surface_height != height || image_surface == NULL)
  {
    // create double-buffered image to draw on, to make modules draw more fluently.
    image_surface_width = width;
    image_surface_height = height;
    if(image_surface) cairo_surface_destroy(image_surface);
    image_surface = dt_cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    image_surface_imgid = -1; // invalidate old stuff
  }
  cairo_surface_t *surface;
  cairo_t *cr = cairo_create(image_surface);

  if(dev->preview2_pipe->output_backbuf && // do we have an image?
    dev->preview2_pipe->backbuf_scale == backbuf_scale && // is this the zoom scale we want to display?
    dev->preview2_pipe->backbuf_zoom_x == zoom_x && dev->preview2_pipe->backbuf_zoom_y == zoom_y)
  {
    // draw image
    mutex = &dev->preview2_pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);
    float wd = dev->preview2_pipe->output_backbuf_width;
    float ht = dev->preview2_pipe->output_backbuf_height;
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, wd);
    surface = dt_cairo_image_surface_create_for_data(dev->preview2_pipe->output_backbuf,
                                                     CAIRO_FORMAT_RGB24, wd, ht, stride);
    wd /= dev->second_window.ppd;
    ht /= dev->second_window.ppd;
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);
    cairo_paint(cr);
    cairo_translate(cr, ceilf(0.5 * (width - wd)), ceilf(0.5 * (height - ht)));

    if(closeup)
    {
      const double scale = 1<<closeup;
      cairo_scale(cr, scale, scale);
      cairo_translate(cr, -(.5 - 0.5/scale) * wd, -(.5 - 0.5/scale) * ht);
    }

    cairo_rectangle(cr, 0, 0, wd, ht);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), darktable.gui->filter_image);
    cairo_fill(cr);

    cairo_surface_destroy(surface);
    dt_pthread_mutex_unlock(mutex);
    image_surface_imgid = dev->image_storage.id;
  }
  else if(dev->preview_pipe->output_backbuf)
  {
    // draw preview
    mutex = &dev->preview_pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);

    const float wd = dev->preview_pipe->output_backbuf_width;
    const float ht = dev->preview_pipe->output_backbuf_height;
    const float zoom_scale = dt_second_window_get_zoom_scale(dev, zoom, 1 << closeup, 1);
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);
    cairo_paint(cr);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, wd);
    surface = cairo_image_surface_create_for_data(dev->preview_pipe->output_backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride);
    cairo_translate(cr, width / 2.0, height / 2.0f);
    cairo_scale(cr, zoom_scale, zoom_scale);
    cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);
    // avoid to draw the 1px garbage that sometimes shows up in the preview :(
    cairo_rectangle(cr, 0, 0, wd - 1, ht - 1);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), darktable.gui->filter_image);
    cairo_fill(cr);
    cairo_surface_destroy(surface);
    dt_pthread_mutex_unlock(mutex);
    image_surface_imgid = dev->image_storage.id;
  }

  cairo_restore(cri);

  if(image_surface_imgid == dev->image_storage.id)
  {
    cairo_destroy(cr);
    cairo_set_source_surface(cri, image_surface, 0, 0);
    cairo_paint(cri);
  }
}

static void second_window_scrolled(GtkWidget *widget, dt_develop_t *dev, double x, double y, const int up,
                                   const int state)
{
  int procw, proch;

  dt_dev_zoom_t zoom = dt_second_window_get_dev_zoom(dev);
  int closeup = dt_second_window_get_dev_closeup(dev);
  float zoom_x = dt_second_window_get_dev_zoom_x(dev);
  float zoom_y = dt_second_window_get_dev_zoom_y(dev);
  dt_second_window_get_processed_size(dev, &procw, &proch);
  float scale = dt_second_window_get_zoom_scale(dev, zoom, 1 << closeup, 0);
  const float fitscale = dt_second_window_get_zoom_scale(dev, DT_ZOOM_FIT, 1.0, 0);
  float oldscale = scale;
  // offset from center now (current zoom_{x,y} points there)
  float mouse_off_x = x - .5 * dev->second_window.width, mouse_off_y = y - .5 * dev->second_window.height;
  zoom_x += mouse_off_x / (procw * scale);
  zoom_y += mouse_off_y / (proch * scale);
  zoom = DT_ZOOM_FREE;
  closeup = 0;

  if(up)
  {
    if((scale == 1.0f || scale == 2.0f) && !((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK))
      return;

    if(scale >= 16.0f)
      return;
    else if(scale >= 8.0f)
      scale = 16.0;
    else if(scale >= 4.0f)
      scale = 8.0;
    else if(scale >= 2.0f)
      scale = 4.0;
    else if(scale < fitscale)
      scale += .05f * (1.0f - fitscale);
    else
      scale += .1f * (1.0f - fitscale);
  }
  else
  {
    if(scale == fitscale && !((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK))
      return;
    else if(scale < 0.5 * fitscale)
      return;
    else if(scale <= fitscale)
      scale -= .05f * (1.0f - fitscale);
    else if(scale <= 2.0f)
      scale -= .1f * (1.0f - fitscale);
    else if(scale <= 4.0f)
      scale = 2.0f;
    else if(scale <= 8.0f)
      scale = 4.0f;
    else
      scale = 8.0f;
  }
  // we want to be sure to stop at 1:1 and FIT levels
  if((scale - 1.0) * (oldscale - 1.0) < 0)
    scale = 1.0f;

  if((scale - fitscale) * (oldscale - fitscale) < 0)
    scale = fitscale;
    
  scale = fmaxf(fminf(scale, 16.0f), 0.5 * fitscale);
  // for 200% zoom we want pixel doubling instead of interpolation
  if(scale > 15.9999f)
  {
    scale = 1.0f; // don't interpolate
    closeup = 4;  // enable closeup mode (pixel doubling)
  }
  else if(scale > 7.9999f)
  {
    scale = 1.0f; // don't interpolate
    closeup = 3;  // enable closeup mode (pixel doubling)
  }
  else if(scale > 3.9999f)
  {
    scale = 1.0f; // don't interpolate
    closeup = 2;  // enable closeup mode (pixel doubling)
  }
  else if(scale > 1.9999f)
  {
    scale = 1.0f; // don't interpolate
    closeup = 1;  // enable closeup mode (pixel doubling)
  }

  if(fabsf(scale - 1.0f) < 0.001f)
    zoom = DT_ZOOM_1;

  if(fabsf(scale - fitscale) < 0.001f)
    zoom = DT_ZOOM_FIT;

  dt_second_window_set_zoom_scale(dev, scale);
  dt_second_window_set_dev_closeup(dev, closeup);
  scale = dt_second_window_get_zoom_scale(dev, zoom, 1 << closeup, 0);

  zoom_x -= mouse_off_x / (procw * scale);
  zoom_y -= mouse_off_y / (proch * scale);
  dt_second_window_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
  dt_second_window_set_dev_zoom(dev, zoom);
  dt_second_window_set_dev_zoom_x(dev, zoom_x);
  dt_second_window_set_dev_zoom_y(dev, zoom_y);
  // pipe needs to be reconstructed
  dev->preview2_status = DT_DEV_PIXELPIPE_DIRTY;
  gtk_widget_queue_draw(widget);
}

static void _second_window_configure_ppd_dpi(dt_develop_t *dev)
{
  GtkWidget *widget = dev->second_window.second_wnd;
  dev->second_window.ppd = dev->second_window.ppd_thb = dt_get_system_gui_ppd(widget);

  if(dt_conf_get_bool("ui/performance"))
    dev->second_window.ppd_thb *= DT_GUI_THUMBSIZE_REDUCE;
  // get the screen resolution
  float screen_dpi_overwrite = dt_conf_get_float("screen_dpi_overwrite");

  if(screen_dpi_overwrite > 0.0)
  {
    dev->second_window.dpi = screen_dpi_overwrite;
    gdk_screen_set_resolution(gtk_widget_get_screen(widget), screen_dpi_overwrite);
    dt_print(DT_DEBUG_CONTROL, "[screen resolution] setting the screen resolution to %f dpi as specified in "
                               "the configuration file\n", screen_dpi_overwrite);
  }
  else
  {
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_autoset_dpi(widget);
#endif
    dev->second_window.dpi = gdk_screen_get_resolution(gtk_widget_get_screen(widget));

    if(dev->second_window.dpi < 0.0)
    {
      dev->second_window.dpi = 96.0;
      gdk_screen_set_resolution(gtk_widget_get_screen(widget), 96.0);
      dt_print(DT_DEBUG_CONTROL, "[screen resolution] setting the screen resolution to the default 96 dpi\n");
    }
    else
      dt_print(DT_DEBUG_CONTROL, "[screen resolution] setting the screen resolution to %f dpi\n", dev->second_window.dpi);
  }

  dev->second_window.dpi_factor = dev->second_window.dpi / 96;
  // according to man xrandr and the docs of gdk_screen_set_resolution 96 is the default
}

static gboolean _second_window_draw_callback(GtkWidget *widget, cairo_t *crf, dt_develop_t *dev)
{
  int pointerx, pointery;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int32_t width = allocation.width;
  int32_t height = allocation.height;
  dev->second_window.width = width;
  dev->second_window.height = height;

  gdk_window_get_device_position(gtk_widget_get_window(widget),
                                 gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(widget))),
                                 &pointerx, &pointery, NULL);
  second_window_expose(widget, dev, crf, width, height, pointerx, pointery);
  return TRUE;
}

static gboolean dt_gui_get_second_window_scroll_unit_deltas(const GdkEventScroll *event, int *delta_x, int *delta_y)
{
  // accumulates scrolling regardless of source or the widget being scrolled
  static gdouble acc_x = 0.0, acc_y = 0.0;
  gboolean handled = FALSE;

  switch(event->direction)
  {
    // is one-unit cardinal, e.g. from a mouse scroll wheel
    case GDK_SCROLL_LEFT:
      if(delta_x) *delta_x = -1;
      if(delta_y) *delta_y = 0;
      handled = TRUE;
      break;
    case GDK_SCROLL_RIGHT:
      if(delta_x) *delta_x = 1;
      if(delta_y) *delta_y = 0;
      handled = TRUE;
      break;
    case GDK_SCROLL_UP:
      if(delta_x) *delta_x = 0;
      if(delta_y) *delta_y = -1;
      handled = TRUE;
      break;
    case GDK_SCROLL_DOWN:
      if(delta_x) *delta_x = 0;
      if(delta_y) *delta_y = 1;
      handled = TRUE;
      break;
    // is trackpad (or touch) scroll
    case GDK_SCROLL_SMOOTH:
      // stop events reset accumulated delta
      if(event->is_stop)
      {
        acc_x = acc_y = 0.0;
        break;
      }
      // accumulate trackpad/touch scrolls until they make a unit
      // scroll, and only then tell caller that there is a scroll to
      // handle
      acc_x += event->delta_x;
      acc_y += event->delta_y;

      if(fabs(acc_x) >= 1.0)
      {
        gdouble amt = trunc(acc_x);
        acc_x -= amt;
        if(delta_x) *delta_x = (int)amt;
        if(delta_y) *delta_y = 0;
        handled = TRUE;
      }

      if(fabs(acc_y) >= 1.0)
      {
        gdouble amt = trunc(acc_y);
        acc_y -= amt;
        if(delta_x && !handled) *delta_x = 0;
        if(delta_y) *delta_y = (int)amt;
        handled = TRUE;
      }

      break;
    default:
      break;
  }
  return handled;
}

static gboolean _second_window_scrolled_callback(GtkWidget *widget, GdkEventScroll *event, dt_develop_t *dev)
{
  int delta_y;
  
  if(dt_gui_get_second_window_scroll_unit_deltas(event, NULL, &delta_y))
  {
    second_window_scrolled(widget, dev, event->x, event->y, delta_y < 0, event->state & 0xf);
    gtk_widget_queue_draw(widget);
  }

  return TRUE;
}

static gboolean _second_window_configure_callback(GtkWidget *da, GdkEventConfigure *event, dt_develop_t *dev)
{
  static int oldw = 0;
  static int oldh = 0;

  if(oldw != event->width || oldh != event->height)
  {
    dev->second_window.width = event->width;
    dev->second_window.height = event->height;
    // pipe needs to be reconstructed
    dev->preview2_status = DT_DEV_PIXELPIPE_DIRTY;
    dev->preview2_pipe->changed |= DT_DEV_PIPE_REMOVE;
    dev->preview2_pipe->cache_obsolete = 1;
  }

  oldw = event->width;
  oldh = event->height;
  dt_colorspaces_set_display_profile(DT_COLORSPACE_DISPLAY2);
#ifndef GDK_WINDOWING_QUARTZ
  _second_window_configure_ppd_dpi(dev);
#endif
  return TRUE;
}

static void _darkroom_ui_second_window_init(GtkWidget *widget, dt_develop_t *dev)
{
  const int width = MAX(10, dt_conf_get_int("second_window/window_w"));
  const int height = MAX(10, dt_conf_get_int("second_window/window_h"));
  dev->second_window.width = width;
  dev->second_window.height = height;

  const gint x = MAX(0, dt_conf_get_int("second_window/window_x"));
  const gint y = MAX(0, dt_conf_get_int("second_window/window_y"));
  gtk_window_set_default_size(GTK_WINDOW(widget), width, height);
  gtk_widget_show_all(widget);
  gtk_window_move(GTK_WINDOW(widget), x, y);
  gtk_window_resize(GTK_WINDOW(widget), width, height);
  const int fullscreen = dt_conf_get_bool("second_window/fullscreen");
  
  if(fullscreen)
    gtk_window_fullscreen(GTK_WINDOW(widget));
  else
  {
    gtk_window_unfullscreen(GTK_WINDOW(widget));
    const int maximized = dt_conf_get_bool("second_window/maximized");
    
    if(maximized)
      gtk_window_maximize(GTK_WINDOW(widget));
    else
      gtk_window_unmaximize(GTK_WINDOW(widget));
  }
}

static void _darkroom_ui_second_window_write_config(GtkWidget *widget)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  gint x, y;
  gtk_window_get_position(GTK_WINDOW(widget), &x, &y);
  dt_conf_set_int("second_window/window_x", x);
  dt_conf_set_int("second_window/window_y", y);
  dt_conf_set_int("second_window/window_w", allocation.width);
  dt_conf_set_int("second_window/window_h", allocation.height);
  dt_conf_set_bool("second_window/maximized",
                   (gdk_window_get_state(gtk_widget_get_window(widget)) & GDK_WINDOW_STATE_MAXIMIZED));
  dt_conf_set_bool("second_window/fullscreen",
                   (gdk_window_get_state(gtk_widget_get_window(widget)) & GDK_WINDOW_STATE_FULLSCREEN));
}

static gboolean _second_window_delete_callback(GtkWidget *widget, GdkEvent *event, dt_develop_t *dev)
{
  _darkroom_ui_second_window_write_config(dev->second_window.second_wnd);
  dev->second_window.second_wnd = NULL;
  dev->second_window.widget = NULL;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->second_window.button), FALSE);

  return FALSE;
}

static gboolean _second_window_key_pressed_callback(GtkWidget *widget, GdkEventKey *event, dt_develop_t *dev)
{
  return TRUE;
}

static void _darkroom_display_second_window(dt_develop_t *dev)
{
  if(dev->second_window.second_wnd == NULL)
  {
    dev->second_window.width = -1;
    dev->second_window.height = -1;
    dev->second_window.second_wnd = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(dev->second_window.second_wnd, "second_window");
    _second_window_configure_ppd_dpi(dev);

    gtk_window_set_icon_name(GTK_WINDOW(dev->second_window.second_wnd), "darktable");
    gtk_window_set_title(GTK_WINDOW(dev->second_window.second_wnd), _("darktable - darkroom preview"));
    GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->second_window.second_wnd), container);
    GtkWidget *widget = gtk_grid_new();
    gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);

    dev->second_window.widget = gtk_drawing_area_new();
    gtk_widget_set_size_request(dev->second_window.widget, DT_PIXEL_APPLY_DPI_2ND_WND(dev, 50),
                                DT_PIXEL_APPLY_DPI_2ND_WND(dev, 200));
    gtk_widget_set_hexpand(dev->second_window.widget, TRUE);
    gtk_widget_set_vexpand(dev->second_window.widget, TRUE);
    gtk_widget_set_app_paintable(dev->second_window.widget, TRUE);

    gtk_grid_attach(GTK_GRID(widget), dev->second_window.widget, 0, 0, 1, 1);

    gtk_widget_set_events(dev->second_window.widget, GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                                         | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                         | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                                                         | darktable.gui->scroll_mask);
    /* connect callbacks */
    g_signal_connect(G_OBJECT(dev->second_window.widget), "draw", G_CALLBACK(_second_window_draw_callback), dev);
    g_signal_connect(G_OBJECT(dev->second_window.widget), "scroll-event",
                     G_CALLBACK(_second_window_scrolled_callback), dev);
    g_signal_connect(G_OBJECT(dev->second_window.widget), "configure-event",
                     G_CALLBACK(_second_window_configure_callback), dev);
    g_signal_connect(G_OBJECT(dev->second_window.second_wnd), "delete-event",
                     G_CALLBACK(_second_window_delete_callback), dev);
    g_signal_connect(G_OBJECT(dev->second_window.second_wnd), "key-press-event",
                     G_CALLBACK(_second_window_key_pressed_callback), dev);
    _darkroom_ui_second_window_init(dev->second_window.second_wnd, dev);
  }

  gtk_widget_show_all(dev->second_window.second_wnd);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
