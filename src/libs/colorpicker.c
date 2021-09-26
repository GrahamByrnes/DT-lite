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

#include "bauhaus/bauhaus.h"
#include "libs/colorpicker.h"
#include "common/darktable.h"
#include "common/iop_profile.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "dtgtk/togglebutton.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "common/colorspaces_inline_conversions.h"

DT_MODULE(1);

typedef struct dt_lib_colorpicker_t
{
  GtkWidget *large_color_patch;
  GtkWidget *color_mode_selector;
  GtkWidget *picker_button;
  GtkWidget *samples_container;
  GtkWidget *add_sample_button;
  GtkWidget *display_samples_check_box;
  dt_colorpicker_sample_t proxy_linked;
} dt_lib_colorpicker_t;

const char *name(dt_lib_module_t *self)
{
  return _("color picker");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int expandable(dt_lib_module_t *self)
{
  return 1;
}

int position()
{
  return 800;
}

// GUI callbacks

static gboolean _sample_draw_callback(GtkWidget *widget, cairo_t *cr, dt_colorpicker_sample_t *sample)
{
  const guint width = gtk_widget_get_allocated_width(widget);
  const guint height = gtk_widget_get_allocated_height(widget);
  gdk_cairo_set_source_rgba(cr, &sample->rgb);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill (cr);
  // if the sample is locked we want to add a lock
  if(sample->locked)
  {
    const int border = DT_PIXEL_APPLY_DPI(2);
    const int icon_width = width - 2 * border;
    const int icon_height = height - 2 * border;
    if(icon_width > 0 && icon_height > 0)
    {
      GdkRGBA fg_color;
      gtk_style_context_get_color(gtk_widget_get_style_context(widget), gtk_widget_get_state_flags(widget), &fg_color);
      gdk_cairo_set_source_rgba(cr, &fg_color);
      dtgtk_cairo_paint_lock(cr, border, border, icon_width, icon_height, 0, NULL);
    }
  }

  return FALSE;
}

static void _update_sample_label(dt_colorpicker_sample_t *sample)
{
  const int model = dt_conf_get_int("ui_last/colorpicker_model");
  float *rgb, *lab;
  rgb = sample->picked_color_rgb_mean;
  lab = sample->picked_color_lab_mean;
  // Setting the output button
  sample->rgb.red   = CLAMP(rgb[0], 0.f, 1.f);
  sample->rgb.green = CLAMP(rgb[1], 0.f, 1.f);
  sample->rgb.blue  = CLAMP(rgb[2], 0.f, 1.f);
  // Setting the output label
  char text[128] = { 0 };
  float alt[3] = { 0 };

  switch(model)
  {
    case 0:
      // RGB
      snprintf(text, sizeof(text), "%6d %6d %6d",
                (int)round(sample->rgb.red   * 255.f),
                (int)round(sample->rgb.green * 255.f),
                (int)round(sample->rgb.blue  * 255.f));
      break;
    case 1:
      // Lab
      snprintf(text, sizeof(text), "%6.02f %6.02f %6.02f", CLAMP(lab[0], .0f, 100.0f), lab[1], lab[2]);
      break;
    case 2:
      // LCh
      dt_Lab_2_LCH(lab, alt);

      if(alt[1] < 0.01)
        alt[2] = 0.0f;

      snprintf(text, sizeof(text), "%6.02f %6.02f %6.02f", CLAMP(alt[0], .0f, 100.0f), alt[1], alt[2] * 360);
      break;
  }

  gtk_label_set_text(GTK_LABEL(sample->output_label), text);
  gtk_widget_queue_draw(sample->color_patch);
}

static void _update_picker_output(dt_lib_module_t *self)
{
  dt_lib_colorpicker_t *data = self->data;
  dt_iop_module_t *module = dt_iop_get_colorout_module();

  if(module)
  {
    ++darktable.gui->reset;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->picker_button),
                                 module->request_color_pick != DT_REQUEST_COLORPICK_OFF);
    --darktable.gui->reset;
    _update_sample_label(&data->proxy_linked);
    gtk_widget_queue_draw(data->large_color_patch);
  }
}

static gboolean _large_patch_toggle(GtkWidget *widget, GdkEvent *event, dt_lib_colorpicker_t *data)
{
  const gboolean show_large_patch = !dt_conf_get_bool("ui_last/colorpicker_large");
  dt_conf_set_bool("ui_last/colorpicker_large", show_large_patch);
  gtk_widget_set_visible(gtk_widget_get_parent(data->large_color_patch), show_large_patch);

  return FALSE;
}

static void _picker_button_toggled(GtkToggleButton *button, dt_lib_colorpicker_t *data)
{
  gtk_widget_set_sensitive(GTK_WIDGET(data->add_sample_button), gtk_toggle_button_get_active(button));
  if(!gtk_toggle_button_get_active(button))
  {
    darktable.lib->proxy.colorpicker.display_samples = gtk_toggle_button_get_active(button);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->display_samples_check_box), FALSE);
  }
}

static void _update_size(dt_lib_module_t *self, int size)
{
  darktable.lib->proxy.colorpicker.size = size;
  _update_picker_output(self);
}

static void _update_samples_output(dt_lib_module_t *self)
{
  for(GSList *samples = darktable.lib->proxy.colorpicker.live_samples;
      samples; samples = g_slist_next(samples))
      
    _update_sample_label(samples->data);
}

static void _color_mode_changed(GtkWidget *widget, dt_lib_module_t *p)
{
  dt_conf_set_int("ui_last/colorpicker_model", dt_bauhaus_combobox_get(widget));
  _update_picker_output(p);
  _update_samples_output((dt_lib_module_t *)p);
}

static void _label_size_allocate_callback(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data)
{
  gint label_width;
  gtk_label_set_attributes(GTK_LABEL(widget), NULL);
  gtk_widget_get_preferred_width(widget, NULL, &label_width);

  double scale = (double)allocation->width / label_width;

  while(label_width > allocation->width && scale > .25)
  {
    PangoAttrList *attrlist = pango_attr_list_new();
    PangoAttribute *attr = pango_attr_scale_new(scale);
    pango_attr_list_insert(attrlist, attr);
    gtk_label_set_attributes(GTK_LABEL(widget), attrlist);
    pango_attr_list_unref(attrlist);

    gtk_widget_get_preferred_width(widget, NULL, &label_width);
    scale *= 0.95; // find first discrete font that's small enough
  }
}

static void _display_samples_changed(GtkToggleButton *button, gpointer data)
{
  dt_conf_set_int("ui_last/colorpicker_display_samples", gtk_toggle_button_get_active(button));
  darktable.lib->proxy.colorpicker.display_samples = gtk_toggle_button_get_active(button);
  dt_dev_invalidate_from_gui(darktable.develop);
}

static void _set_sample_point(dt_lib_module_t *self, float x, float y)
{
  if(darktable.develop->gui_module)
  {
    darktable.develop->gui_module->color_picker_point[0] = x;
    darktable.develop->gui_module->color_picker_point[1] = y;
  }

  _update_size(self, DT_COLORPICKER_SIZE_POINT);
}

void gui_init(dt_lib_module_t *self)
{
  // Initializing self data structure
  dt_lib_colorpicker_t *data = (dt_lib_colorpicker_t *)calloc(1, sizeof(dt_lib_colorpicker_t));
  self->data = (void *)data;

  data->proxy_linked.rgb.red = 0.7;
  data->proxy_linked.rgb.green = 0.7;
  data->proxy_linked.rgb.blue = 0.7;
  data->proxy_linked.rgb.alpha = 1.0;

  // Initializing proxy functions and data
  darktable.lib->proxy.colorpicker.module = self;
  darktable.lib->proxy.colorpicker.size = dt_conf_get_int("ui_last/colorpicker_size");
  darktable.lib->proxy.colorpicker.display_samples = dt_conf_get_int("ui_last/colorpicker_display_samples");
  darktable.lib->proxy.colorpicker.live_samples = NULL;
  darktable.lib->proxy.colorpicker.picked_color_rgb_mean = data->proxy_linked.picked_color_rgb_mean;
  darktable.lib->proxy.colorpicker.picked_color_rgb_min = data->proxy_linked.picked_color_rgb_min;
  darktable.lib->proxy.colorpicker.picked_color_rgb_max = data->proxy_linked.picked_color_rgb_max;
  darktable.lib->proxy.colorpicker.picked_color_lab_mean = data->proxy_linked.picked_color_lab_mean;
  darktable.lib->proxy.colorpicker.picked_color_lab_min = data->proxy_linked.picked_color_lab_min;
  darktable.lib->proxy.colorpicker.picked_color_lab_max = data->proxy_linked.picked_color_lab_max;
  darktable.lib->proxy.colorpicker.update_panel = _update_picker_output;
  darktable.lib->proxy.colorpicker.update_samples = _update_samples_output;
  darktable.lib->proxy.colorpicker.set_sample_point = _set_sample_point;

  // Setting up the GUI
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  
  GtkStyleContext *context = gtk_widget_get_style_context(self->widget);
  gtk_style_context_add_class(context, "picker-module");

  // The color patch
  GtkWidget *color_patch_wrapper = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(GTK_WIDGET(color_patch_wrapper), "color-picker-area");
  GtkWidget *color_patch = gtk_drawing_area_new();
  data->large_color_patch = color_patch;
  gtk_widget_set_tooltip_text(color_patch, _("click to (un)hide large color patch"));
  gtk_widget_set_events(color_patch, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(G_OBJECT(color_patch), "draw", G_CALLBACK(_sample_draw_callback), &data->proxy_linked);
  g_signal_connect(G_OBJECT(color_patch), "button-press-event", G_CALLBACK(_large_patch_toggle), data);
  gtk_box_pack_start(GTK_BOX(color_patch_wrapper), color_patch, TRUE, TRUE, 0);
  gtk_widget_show(color_patch);
  gtk_widget_set_no_show_all(color_patch_wrapper, dt_conf_get_bool("ui_last/colorpicker_large") == FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), color_patch_wrapper, FALSE, FALSE, 0);

  // The picker button, mode and statistic combo boxes
  GtkWidget *picker_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  data->color_mode_selector = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_combobox_add(data->color_mode_selector, _("RGB"));
  dt_bauhaus_combobox_add(data->color_mode_selector, _("Lab"));
  dt_bauhaus_combobox_add(data->color_mode_selector, _("LCh"));
  dt_bauhaus_combobox_set(data->color_mode_selector, dt_conf_get_int("ui_last/colorpicker_model"));
  dt_bauhaus_combobox_set_entries_ellipsis(data->color_mode_selector, PANGO_ELLIPSIZE_NONE);
  g_signal_connect(G_OBJECT(data->color_mode_selector), "value-changed", G_CALLBACK(_color_mode_changed), self);
  gtk_widget_set_valign(data->color_mode_selector, GTK_ALIGN_END);
  gtk_box_pack_start(GTK_BOX(picker_row), data->color_mode_selector, TRUE, TRUE, 0);

  data->picker_button = dt_color_picker_new(NULL, DT_COLOR_PICKER_POINT_AREA, picker_row);
  gtk_widget_set_tooltip_text(data->picker_button, _("turn on color picker"));
  gtk_widget_set_name(GTK_WIDGET(data->picker_button), "color-picker-button");
  g_signal_connect(G_OBJECT(data->picker_button), "toggled", G_CALLBACK(_picker_button_toggled), data);

  gtk_box_pack_start(GTK_BOX(self->widget), picker_row, TRUE, TRUE, 0);

  // The small sample, label and add button
  GtkWidget *sample_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  data->proxy_linked.color_patch = color_patch = gtk_drawing_area_new();
  gtk_widget_set_tooltip_text(color_patch, _("click to (un)hide large color patch"));
  gtk_widget_set_events(color_patch, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(G_OBJECT(color_patch), "button-press-event", G_CALLBACK(_large_patch_toggle), data);
  g_signal_connect(G_OBJECT(color_patch), "draw", G_CALLBACK(_sample_draw_callback), &data->proxy_linked);

  color_patch_wrapper = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(color_patch_wrapper), color_patch, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(sample_row), color_patch_wrapper, TRUE, TRUE, 0);

  GtkWidget *label = data->proxy_linked.output_label = gtk_label_new("");
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_START);
  gtk_widget_set_has_tooltip(label, TRUE);
  g_signal_connect(G_OBJECT(label), "size-allocate", G_CALLBACK(_label_size_allocate_callback), &data->proxy_linked);
  gtk_box_pack_start(GTK_BOX(sample_row), label, TRUE, TRUE, 0);

  gtk_box_pack_end(GTK_BOX(sample_row), data->add_sample_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), sample_row, TRUE, TRUE, 0);

  // Adding the live samples section
  label = dt_ui_section_label_new(_(""));
  context = gtk_widget_get_style_context(GTK_WIDGET(label));
  gtk_style_context_add_class(context, "section_label_top");
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);

  data->samples_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), data->samples_container, TRUE, TRUE, 0);

  data->display_samples_check_box = gtk_check_button_new_with_label(_("display sample on histogram"));
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(data->display_samples_check_box))),
                          PANGO_ELLIPSIZE_MIDDLE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->display_samples_check_box),
                               dt_conf_get_int("ui_last/colorpicker_display_samples"));
  g_signal_connect(G_OBJECT(data->display_samples_check_box), "toggled",
                   G_CALLBACK(_display_samples_changed), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), data->display_samples_check_box, TRUE, TRUE, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  // Clearing proxy functions
  darktable.lib->proxy.colorpicker.module = NULL;
  darktable.lib->proxy.colorpicker.update_panel = NULL;
  darktable.lib->proxy.colorpicker.update_samples = NULL;

  darktable.lib->proxy.colorpicker.set_sample_area = NULL;
  darktable.lib->proxy.colorpicker.set_sample_box_area = NULL;

  darktable.lib->proxy.colorpicker.picked_color_rgb_mean
      = darktable.lib->proxy.colorpicker.picked_color_rgb_min
      = darktable.lib->proxy.colorpicker.picked_color_rgb_max = NULL;
  darktable.lib->proxy.colorpicker.picked_color_lab_mean
      = darktable.lib->proxy.colorpicker.picked_color_lab_min
      = darktable.lib->proxy.colorpicker.picked_color_lab_max = NULL;

  free(self->data);
  self->data = NULL;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_colorpicker_t *data = self->data;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->picker_button), FALSE);
  dt_iop_color_picker_reset(NULL, FALSE);  // Resetting the picked colors

  for(int i = 0; i < 3; i++)
  {
    darktable.lib->proxy.colorpicker.picked_color_rgb_mean[i]
        = darktable.lib->proxy.colorpicker.picked_color_rgb_min[i]
        = darktable.lib->proxy.colorpicker.picked_color_rgb_max[i] = 0;

    darktable.lib->proxy.colorpicker.picked_color_lab_mean[i]
        = darktable.lib->proxy.colorpicker.picked_color_lab_min[i]
        = darktable.lib->proxy.colorpicker.picked_color_lab_max[i] = 0;
  }
  _update_picker_output(self);
  // Resetting GUI elements
  dt_bauhaus_combobox_set(data->color_mode_selector, 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->display_samples_check_box), FALSE);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
