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
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/history.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "gui/hist_dialog.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE(1)

typedef struct dt_lib_copy_history_t
{
  GtkWidget *discard_button;
  GtkButton *compress_button;
} dt_lib_copy_history_t;

const char *name(dt_lib_module_t *self)
{
  return _("history stack");
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

static void _update(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;
  const GList *imgs = dt_view_get_images_to_act_on(TRUE, FALSE);
  const guint act_on_cnt = g_list_length((GList *)imgs);

  gtk_widget_set_sensitive(GTK_WIDGET(d->discard_button), act_on_cnt > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(d->compress_button), act_on_cnt > 0);
}

static void compress_button_clicked(GtkWidget *widget, gpointer user_data)
{
  const GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  const GList *imgs = dt_view_get_images_to_act_on(TRUE, TRUE);

  if(!imgs) return;

  const int missing = dt_history_compress_on_list(imgs);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                             g_list_copy((GList *)imgs));
  dt_control_queue_redraw_center();

  if (missing)
  {
    GtkWidget *dialog = gtk_message_dialog_new(
    GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_CLOSE,
    ngettext("no history compression of 1 image.\nsee tag: darktable|problem|history-compress.",
             "no history compression of %d images.\nsee tag: darktable|problem|history-compress.", missing ), missing);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif

    gtk_window_set_title(GTK_WINDOW(dialog), _("history compression warning"));
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }
}

static void discard_button_clicked(GtkWidget *widget, gpointer user_data)
{
  gint res = GTK_RESPONSE_YES;
  const GList *imgs = dt_view_get_images_to_act_on(TRUE, TRUE);

  if(dt_conf_get_bool("ask_before_discard"))
  {
    const GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
    const int number = g_list_length((GList *)imgs);

    if (number == 0) return;

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        ngettext("do you really want to clear history of %d selected image?",
                 "do you really want to clear history of %d selected images?", number), number);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif
    gtk_window_set_title(GTK_WINDOW(dialog), _("delete images' history?"));
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }

  if(res == GTK_RESPONSE_YES)
  {
    dt_history_delete_on_list(imgs, TRUE);
    GList *imgs_copy = g_list_copy((GList *)imgs);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                               imgs_copy);
    dt_control_queue_redraw_center();
  }

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

static void _mouse_over_image_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_queue_postponed_update(self, _update);
}

void gui_reset(dt_lib_module_t *self)
{
  _update(self);
}

int position()
{
  return 600;
}

#define ellipsize_button(button) gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(button))), PANGO_ELLIPSIZE_END);
void gui_init(dt_lib_module_t *self)
{
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)malloc(sizeof(dt_lib_copy_history_t));
  self->data = (void *)d;
  self->timeout_handle = 0;

  self->widget = gtk_grid_new();
  GtkGrid *grid = GTK_GRID(self->widget);
  gtk_grid_set_column_homogeneous(grid, TRUE);
  int line = 0;

  d->compress_button = GTK_BUTTON(gtk_button_new_with_label(_("compress history")));
  ellipsize_button(d->compress_button);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->compress_button), _("compress history stack of\nall selected images"));
  gtk_grid_attach(grid, GTK_WIDGET(d->compress_button), 0, line, 3, 1);

  GtkWidget *discard = gtk_button_new_with_label(_("discard history"));
  ellipsize_button(discard);
  d->discard_button = discard;
  gtk_widget_set_tooltip_text(discard, _("discard history stack of\nall selected images"));
  gtk_grid_attach(grid, discard, 3, line++, 3, 1);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_image_selection_changed_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_collection_updated_callback), self);
  _update(self);

  g_signal_connect(G_OBJECT(d->compress_button), "clicked", G_CALLBACK(compress_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(discard), "clicked", G_CALLBACK(discard_button_clicked), (gpointer)self);
}
#undef ellipsize_button

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_image_selection_changed_callback), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_collection_updated_callback), self);

  free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
