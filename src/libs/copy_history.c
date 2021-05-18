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
  GtkWidget *pastemode;
  GtkButton *paste, *paste_parts;
  GtkWidget *copy_button, *discard_button, *load_button, *write_button;
  GtkWidget *copy_parts_button;
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
  const gboolean can_paste
      = darktable.view_manager->copy_paste.copied_imageid > 0
        && (act_on_cnt > 1
            || (act_on_cnt == 1
                && (darktable.view_manager->copy_paste.copied_imageid != dt_view_get_image_to_act_on())));

  gtk_widget_set_sensitive(GTK_WIDGET(d->discard_button), act_on_cnt > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(d->compress_button), act_on_cnt > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(d->load_button), act_on_cnt > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(d->write_button), act_on_cnt > 0);

  gtk_widget_set_sensitive(GTK_WIDGET(d->copy_button), act_on_cnt == 1);
  gtk_widget_set_sensitive(GTK_WIDGET(d->copy_parts_button), act_on_cnt == 1);

  gtk_widget_set_sensitive(GTK_WIDGET(d->paste), can_paste);
  gtk_widget_set_sensitive(GTK_WIDGET(d->paste_parts), can_paste);
}

static void write_button_clicked(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_control_write_sidecar_files();
}

static void load_button_clicked(GtkWidget *widget, dt_lib_module_t *self)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("open sidecar file"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN, _("_cancel"),
      GTK_RESPONSE_CANCEL, _("_open"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif

  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.xmp");
  gtk_file_filter_add_pattern(filter, "*.XMP");
  gtk_file_filter_set_name(filter, _("XMP sidecar files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    char *dtfilename;
    dtfilename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    const GList *imgs = dt_view_get_images_to_act_on(TRUE, TRUE);
    if(dt_history_load_and_apply_on_list(dtfilename, imgs) != 0)
    {
      GtkWidget *dialog
          = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
                                   GTK_BUTTONS_CLOSE, _("error loading file '%s'"), dtfilename);
#ifdef GDK_WINDOWING_QUARTZ
      dt_osx_disallow_fullscreen(dialog);
#endif
      gtk_dialog_run(GTK_DIALOG(dialog));
      gtk_widget_destroy(dialog);
    }
    else
    {
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                                 g_list_copy((GList *)imgs));
      dt_control_queue_redraw_center();
    }

    g_free(dtfilename);
  }
  gtk_widget_destroy(filechooser);
  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
}

static void compress_button_clicked(GtkWidget *widget, gpointer user_data)
{
  const GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  const GList *imgs = dt_view_get_images_to_act_on(TRUE, TRUE);
  if(g_list_length((GList *)imgs) < 1) return;

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


static void copy_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;

  const int id = dt_view_get_image_to_act_on();

  if(id > 0 && dt_history_copy(id))
  {
    _update(self);
  }
}

static void copy_parts_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;

  const int id = dt_view_get_image_to_act_on();

  if(id > 0 && dt_history_copy_parts(id))
  {
    _update(self);
  }
}

static void discard_button_clicked(GtkWidget *widget, gpointer user_data)
{
  gint res = GTK_RESPONSE_YES;

  const GList *imgs = dt_view_get_images_to_act_on(TRUE, TRUE);
  GList *imgs_copy = g_list_copy((GList *)imgs);

  if(dt_conf_get_bool("ask_before_discard"))
  {
    const GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

    const int number = g_list_length((GList *)imgs_copy);

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
    dt_history_delete_on_list(imgs_copy, TRUE);

    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                               g_list_copy((GList *)imgs_copy));
    dt_control_queue_redraw_center();
  }

  g_list_free(imgs_copy);
}

static void paste_button_clicked(GtkWidget *widget, gpointer user_data)
{

  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  /* get past mode and store, overwrite / merge */
  const int mode = dt_bauhaus_combobox_get(d->pastemode);
  dt_conf_set_int("plugins/lighttable/copy_history/pastemode", mode);

  /* copy history from previously copied image and past onto selection */
  const GList *imgs = dt_view_get_images_to_act_on(TRUE, TRUE);

  if(dt_history_paste_on_list(imgs, TRUE))
  {
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                               g_list_copy((GList *)imgs));
  }
}

static void paste_parts_button_clicked(GtkWidget *widget, gpointer user_data)
{
  /* copy history from previously copied image and past onto selection */
  const GList *imgs = dt_view_get_images_to_act_on(TRUE, TRUE);

  // at the time the dialog is started, some signals are sent and this in turn call
  // back dt_view_get_images_to_act_on() which free list and create a new one. So the
  // above imgs will be invalidated.

  GList *l_copy = g_list_copy((GList *)imgs);

  if(dt_history_paste_parts_on_list(imgs, TRUE))
  {
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                               g_list_copy((GList *)l_copy));
  }
  else
    g_list_free(l_copy);
}

static void pastemode_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  const int mode = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/lighttable/copy_history/pastemode", mode);

  _update((dt_lib_module_t *)user_data);
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
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));
  gtk_grid_set_column_homogeneous(grid, TRUE);
  int line = 0;

  GtkWidget *copy_parts = gtk_button_new_with_label(_("copy..."));
  ellipsize_button(copy_parts);
  d->copy_parts_button = copy_parts;
  gtk_widget_set_tooltip_text(copy_parts, _("copy part history stack of\nfirst selected image"));
  dt_gui_add_help_link(copy_parts, "history_stack.html#history_stack_usage");
  gtk_grid_attach(grid, copy_parts, 0, line, 3, 1);

  GtkWidget *copy = gtk_button_new_with_label(_("copy all"));
  ellipsize_button(copy);
  d->copy_button = copy;
  gtk_widget_set_tooltip_text(copy, _("copy history stack of\nfirst selected image"));
  dt_gui_add_help_link(copy, "history_stack.html#history_stack_usage");
  gtk_grid_attach(grid, copy, 3, line++, 3, 1);

  d->paste_parts = GTK_BUTTON(gtk_button_new_with_label(_("paste...")));
  ellipsize_button(d->paste_parts);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->paste_parts), _("paste part history stack to\nall selected images"));
  dt_gui_add_help_link(GTK_WIDGET(d->paste_parts), "history_stack.html#history_stack_usage");
  gtk_widget_set_sensitive(GTK_WIDGET(d->paste_parts), FALSE);
  gtk_grid_attach(grid, GTK_WIDGET(d->paste_parts), 0, line, 3, 1);

  d->paste = GTK_BUTTON(gtk_button_new_with_label(_("paste all")));
  ellipsize_button(d->paste);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->paste), _("paste history stack to\nall selected images"));
  dt_gui_add_help_link(GTK_WIDGET(d->paste), "history_stack.html#history_stack_usage");
  gtk_widget_set_sensitive(GTK_WIDGET(d->paste), FALSE);
  gtk_grid_attach(grid, GTK_WIDGET(d->paste), 3, line++, 3, 1);

  d->compress_button = GTK_BUTTON(gtk_button_new_with_label(_("compress history")));
  ellipsize_button(d->compress_button);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->compress_button), _("compress history stack of\nall selected images"));
  gtk_grid_attach(grid, GTK_WIDGET(d->compress_button), 0, line, 3, 1);

  GtkWidget *discard = gtk_button_new_with_label(_("discard history"));
  ellipsize_button(discard);
  d->discard_button = discard;
  gtk_widget_set_tooltip_text(discard, _("discard history stack of\nall selected images"));
  dt_gui_add_help_link(discard, "history_stack.html#history_stack_usage");
  gtk_grid_attach(grid, discard, 3, line++, 3, 1);

  d->pastemode = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->pastemode, NULL, _("mode"));
  dt_bauhaus_combobox_add(d->pastemode, _("append"));
  dt_bauhaus_combobox_add(d->pastemode, _("overwrite"));
  gtk_widget_set_tooltip_text(d->pastemode, _("how to handle existing history"));
  dt_gui_add_help_link(d->pastemode, "history_stack.html#history_stack_usage");
  gtk_grid_attach(grid, d->pastemode, 0, line++, 6, 1);
  dt_bauhaus_combobox_set(d->pastemode, dt_conf_get_int("plugins/lighttable/copy_history/pastemode"));

  GtkWidget *loadbutton = gtk_button_new_with_label(_("load sidecar file..."));
  ellipsize_button(loadbutton);
  d->load_button = loadbutton;
  gtk_widget_set_tooltip_text(loadbutton, _("open an XMP sidecar file\nand apply it to selected images"));
  dt_gui_add_help_link(loadbutton, "history_stack.html#history_stack_usage");
  gtk_grid_attach(grid, loadbutton, 0, line, 3, 1);

  GtkWidget *button = gtk_button_new_with_label(_("write sidecar files"));
  ellipsize_button(button);
  d->write_button = button;
  gtk_widget_set_tooltip_text(button, _("write history stack and tags to XMP sidecar files"));
  dt_gui_add_help_link(button, "history_stack.html#history_stack_usage");
  gtk_grid_attach(grid, button, 3, line, 3, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(write_button_clicked), (gpointer)self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_image_selection_changed_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_collection_updated_callback), self);

  _update(self);

  g_signal_connect(G_OBJECT(copy), "clicked", G_CALLBACK(copy_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(copy_parts), "clicked", G_CALLBACK(copy_parts_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(d->compress_button), "clicked", G_CALLBACK(compress_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(discard), "clicked", G_CALLBACK(discard_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(d->paste_parts), "clicked", G_CALLBACK(paste_parts_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(d->paste), "clicked", G_CALLBACK(paste_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(loadbutton), "clicked", G_CALLBACK(load_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(d->pastemode), "value-changed", G_CALLBACK(pastemode_combobox_changed), (gpointer)self);
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
