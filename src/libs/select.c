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
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include <gdk/gdkkeysyms.h>
#ifdef USE_LUA
#include "lua/call.h"
#include "lua/image.h"
#endif
#include "libs/lib_api.h"

DT_MODULE(1)

const char *name(dt_lib_module_t *self)
{
  return _("select");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

typedef struct dt_lib_select_t
{
  GtkWidget *select_all_button, *select_none_button, *select_invert_button, *select_film_roll_button,
      *select_untouched_button;
} dt_lib_select_t;

static void _update(dt_lib_module_t *self)
{
  dt_lib_select_t *d = (dt_lib_select_t *)self->data;

  const uint32_t collection_cnt =  dt_collection_get_count_no_group(darktable.collection);
  const uint32_t selected_cnt = dt_collection_get_selected_count(darktable.collection);

  gtk_widget_set_sensitive(GTK_WIDGET(d->select_all_button), selected_cnt < collection_cnt);
  gtk_widget_set_sensitive(GTK_WIDGET(d->select_none_button), selected_cnt > 0);

  gtk_widget_set_sensitive(GTK_WIDGET(d->select_invert_button), collection_cnt > 0);

  //theoretically can count if there are unaltered in collection but no need to waste CPU cycles on that.
  gtk_widget_set_sensitive(GTK_WIDGET(d->select_untouched_button), collection_cnt > 0);

  gtk_widget_set_sensitive(GTK_WIDGET(d->select_film_roll_button), selected_cnt > 0);
}

static void _image_selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  _update(self);
}

static void _collection_updated_callback(gpointer instance, dt_collection_change_t query_change, gpointer imgs,
                                        int next, dt_lib_module_t *self)
{
  _update(self);
}

static void button_clicked(GtkWidget *widget, gpointer user_data)
{
  switch(GPOINTER_TO_INT(user_data))
  {
    case 0: // all
      dt_selection_select_all(darktable.selection);
      break;
    case 1: // none
      dt_selection_clear(darktable.selection);
      break;
    case 2: // invert
      dt_selection_invert(darktable.selection);
      break;
    case 4: // untouched
      dt_selection_select_unaltered(darktable.selection);
      break;
    default: // case 3: same film roll
      dt_selection_select_filmroll(darktable.selection);
  }

  dt_control_queue_redraw_center();
}

int position()
{
  return 800;
}

#define ellipsize_button(button) gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(button))), PANGO_ELLIPSIZE_END);
void gui_init(dt_lib_module_t *self)
{
  dt_lib_select_t *d = (dt_lib_select_t *)malloc(sizeof(dt_lib_select_t));
  self->data = d;
  self->widget = gtk_grid_new();
  dt_gui_add_help_link(self->widget, "select.html#select_usage");

  GtkGrid *grid = GTK_GRID(self->widget);
  gtk_grid_set_column_homogeneous(grid, TRUE);
  int line = 0;
  GtkWidget *button;

  button = gtk_button_new_with_label(_("select all"));
  ellipsize_button(button);
  d->select_all_button = button;
  gtk_widget_set_tooltip_text(button, _("select all images in current collection"));
  gtk_grid_attach(grid, button, 0, line, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(0));

  button = gtk_button_new_with_label(_("select none"));
  ellipsize_button(button);
  d->select_none_button = button;
  gtk_widget_set_tooltip_text(button, _("clear selection"));
  gtk_grid_attach(grid, button, 1, line++, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(1));


  button = gtk_button_new_with_label(_("invert selection"));
  ellipsize_button(button);
  gtk_widget_set_tooltip_text(button, _("select unselected images\nin current collection"));
  d->select_invert_button = button;
  gtk_grid_attach(grid, button, 0, line, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(2));

  button = gtk_button_new_with_label(_("select film roll"));
  ellipsize_button(button);
  d->select_film_roll_button = button;
  gtk_widget_set_tooltip_text(button, _("select all images which are in the same\nfilm roll as the selected images"));
  gtk_grid_attach(grid, button, 1, line++, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(3));


  button = gtk_button_new_with_label(_("select untouched"));
  ellipsize_button(button);
  d->select_untouched_button = button;
  gtk_widget_set_tooltip_text(button, _("select untouched images in\ncurrent collection"));
  gtk_grid_attach(grid, button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(4));

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_image_selection_changed_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_collection_updated_callback), self);

  _update(self);
}
#undef ellipsize_button

void gui_cleanup(dt_lib_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_image_selection_changed_callback), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_collection_updated_callback), self);
  free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
