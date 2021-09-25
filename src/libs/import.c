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

#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/film.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_jpeg.h"
#include "common/mipmap_cache.h"
#include "common/metadata.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/import_metadata.h"
#include <gdk/gdkkeysyms.h>
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <strings.h>
#include <librsvg/rsvg.h>
// ugh, ugly hack. why do people break stuff all the time?
#ifndef RSVG_CAIRO_H
#include <librsvg/rsvg-cairo.h>
#endif

DT_MODULE(1)

typedef struct dt_lib_import_t
{
  GtkWidget *frame;
  GtkWidget *recursive;
  GtkWidget *ignore_jpeg;
  GtkWidget *expander;
  GtkButton *import_file;
  GtkButton *import_directory;
  GtkButton *import_camera;
  GtkButton *scan_devices;
  GtkButton *tethered_shoot;

  GtkBox *devices;
} dt_lib_import_t;

const char *name(dt_lib_module_t *self)
{
  return _("import");
}


const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 999;
}

static void _check_button_callback(GtkWidget *widget, gpointer data)
{
    dt_conf_set_bool("ui_last/import_ignore_jpegs",
                     gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
}

static GtkWidget *_lib_import_get_extra_widget(dt_lib_import_t *d, dt_import_metadata_t *metadata,
                                               gboolean import_folder)
{
  // add extra lines to 'extra'. don't forget to destroy the widgets later.
  GtkWidget *expander = gtk_expander_new(_("import options"));
  gtk_expander_set_expanded(GTK_EXPANDER(expander), dt_conf_get_bool("ui_last/import_options_expanded"));
  d->expander = expander;

  GtkWidget *frame = gtk_frame_new(NULL);
  gtk_widget_set_name(frame, "import_metadata");
  gtk_container_add(GTK_CONTAINER(frame), expander);
  d->frame = frame;

  GtkWidget *extra;
  extra = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(expander), extra);

  GtkWidget *recursive = NULL, *ignore_jpeg = NULL;
  if(import_folder == TRUE)
  {
    // recursive opening.
    recursive = gtk_check_button_new_with_label(_("import folders recursively"));
    gtk_widget_set_tooltip_text(recursive,
                                _("recursively import subfolders. Each folder goes into a new film roll."));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(recursive), dt_conf_get_bool("ui_last/import_recursive"));
    gtk_box_pack_start(GTK_BOX(extra), recursive, FALSE, FALSE, 0);

    // ignoring of jpegs. hack while we don't handle raw+jpeg in the same directories.
    ignore_jpeg = gtk_check_button_new_with_label(_("ignore JPEG files"));
    gtk_widget_set_tooltip_text(ignore_jpeg, _("do not load files with an extension of .jpg or .jpeg. this "
                                               "can be useful when there are raw+JPEG in a directory."));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ignore_jpeg),
                                 dt_conf_get_bool("ui_last/import_ignore_jpegs"));
    gtk_box_pack_start(GTK_BOX(extra), ignore_jpeg, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(ignore_jpeg), "clicked",
                   G_CALLBACK(_check_button_callback), ignore_jpeg);
  }
  d->recursive = recursive;
  d->ignore_jpeg = ignore_jpeg;

  metadata->box = extra;
  dt_import_metadata_dialog_new(metadata);
  gtk_widget_show_all(frame);
  return frame;
}

static void _lib_import_evaluate_extra_widget(dt_lib_import_t *d, dt_import_metadata_t *metadata,
                                              gboolean import_folder)
{
  if(import_folder == TRUE)
  {
    dt_conf_set_bool("ui_last/import_recursive",
                     gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->recursive)));
    dt_conf_set_bool("ui_last/import_ignore_jpegs",
                     gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->ignore_jpeg)));
  }

  dt_conf_set_bool("ui_last/import_options_expanded", gtk_expander_get_expanded(GTK_EXPANDER(d->expander)));
  dt_import_metadata_evaluate(metadata);
}

// maybe this should be (partly) in common/imageio.[c|h]?
static void _lib_import_update_preview(GtkFileChooser *file_chooser, gpointer data)
{
  GtkWidget *preview;
  char *filename;
  GdkPixbuf *pixbuf = NULL;
  gboolean have_preview = FALSE, no_preview_fallback = FALSE;
  preview = GTK_WIDGET(data);
  filename = gtk_file_chooser_get_preview_filename(file_chooser);

  if(filename && g_file_test(filename, G_FILE_TEST_IS_REGULAR))
  {
    // don't create dng thumbnails to avoid crashes in libtiff when these are hdr:
    char *c = filename + strlen(filename);
    while(c > filename && *c != '.') c--;
    if(!strcasecmp(c, ".dng")) no_preview_fallback = TRUE;
  }
  else
    no_preview_fallback = TRUE;

  // Step 1: try to check whether the picture contains embedded thumbnail
  // In case it has, we'll use that thumbnail to show on the dialog
  if(!have_preview && !no_preview_fallback)
  {
    uint8_t *buffer = NULL;
    size_t size;
    char *mime_type = NULL;

    if(!dt_exif_get_thumbnail(filename, &buffer, &size, &mime_type))
    {
      // Scale the image to the correct size
      GdkPixbuf *tmp;
      GdkPixbufLoader *loader = gdk_pixbuf_loader_new();

      if (!gdk_pixbuf_loader_write(loader, buffer, size, NULL))
        goto cleanup;
      // Calling gdk_pixbuf_loader_close forces the data to be parsed by the
      // loader. We must do this before calling gdk_pixbuf_loader_get_pixbuf.
      if(!gdk_pixbuf_loader_close(loader, NULL))
        goto cleanup;

      if (!(tmp = gdk_pixbuf_loader_get_pixbuf(loader)))
        goto cleanup;

      float ratio = 1.0 * gdk_pixbuf_get_height(tmp) / gdk_pixbuf_get_width(tmp);
      int width = 128, height = 128 * ratio;
      pixbuf = gdk_pixbuf_scale_simple(tmp, width, height, GDK_INTERP_BILINEAR);
      have_preview = TRUE;

    cleanup:
      gdk_pixbuf_loader_close(loader, NULL);
      free(mime_type);
      free(buffer);
      g_object_unref(loader); // This should clean up tmp as well
    }
  }
  // Step 2: if we were not able to get a thumbnail at step 1,
  // read the whole file to get a small size thumbnail
  // this will not try to read DNG files at all
  if(!have_preview && !no_preview_fallback)
  {
    pixbuf = gdk_pixbuf_new_from_file_at_size(filename, 128, 128, NULL);
    if(pixbuf != NULL) have_preview = TRUE;
  }
  // If we got a thumbnail (either embedded or reading the file directly)
  // we need to find out the rotation as well
  if(have_preview && !no_preview_fallback)
  {
    // get image orientation
    dt_image_t img = { 0 };
    (void)dt_exif_read(&img, filename);
    // Rotate the image to the correct orientation
    GdkPixbuf *tmp = pixbuf;

    if(img.orientation == ORIENTATION_ROTATE_CCW_90_DEG)
      tmp = gdk_pixbuf_rotate_simple(pixbuf, GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
    else if(img.orientation == ORIENTATION_ROTATE_CW_90_DEG)
      tmp = gdk_pixbuf_rotate_simple(pixbuf, GDK_PIXBUF_ROTATE_CLOCKWISE);
    else if(img.orientation == ORIENTATION_ROTATE_180_DEG)
      tmp = gdk_pixbuf_rotate_simple(pixbuf, GDK_PIXBUF_ROTATE_UPSIDEDOWN);

    if(pixbuf != tmp)
    {
      g_object_unref(pixbuf);
      pixbuf = tmp;
    }
  }
  // if no thumbanail found or read failed for whatever reason
  // or in case of DNG files
  // just display the default darktable logo
  if(!have_preview || no_preview_fallback)
  {
    /* load the dt logo as a background */
    cairo_surface_t *surface = dt_util_get_logo(128.0);

    if(surface)
    {
      guint8 *image_buffer = cairo_image_surface_get_data(surface);
      int image_width = cairo_image_surface_get_width(surface);
      int image_height = cairo_image_surface_get_height(surface);

      pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, image_width, image_height);

      cairo_surface_destroy(surface);
      free(image_buffer);
      have_preview = TRUE;
    }
  }
  if(have_preview)
    gtk_image_set_from_pixbuf(GTK_IMAGE(preview), pixbuf);

  if(pixbuf)
    g_object_unref(pixbuf);

  g_free(filename);
  gtk_file_chooser_set_preview_widget_active(file_chooser, have_preview);
}

static void _lib_import_single_image_callback(GtkWidget *widget, dt_lib_import_t* d)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("import image"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN, _("_cancel"), GTK_RESPONSE_CANCEL,
      _("_open"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), TRUE);
  char *last_directory = dt_conf_get_string("ui_last/import_last_directory");

  if(last_directory != NULL)
  {
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), last_directory);
    g_free(last_directory);
  }

  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());

  for(const char **i = dt_supported_extensions; *i != NULL; i++)
  {
    char *ext = g_strdup_printf("*.%s", *i);
    char *ext_upper = g_ascii_strup(ext, -1);
    gtk_file_filter_add_pattern(filter, ext);
    gtk_file_filter_add_pattern(filter, ext_upper);
    g_free(ext_upper);
    g_free(ext);
  }

  gtk_file_filter_set_name(filter, _("supported images"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  GtkWidget *preview = gtk_image_new();
  gtk_file_chooser_set_preview_widget(GTK_FILE_CHOOSER(filechooser), preview);
  g_signal_connect(filechooser, "update-preview", G_CALLBACK(_lib_import_update_preview), preview);

  dt_import_metadata_t metadata;
  gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(filechooser),
                                    _lib_import_get_extra_widget(d, &metadata, FALSE));

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(filechooser));
    dt_conf_set_string("ui_last/import_last_directory", folder);
    g_free(folder);
    _lib_import_evaluate_extra_widget(d, &metadata, FALSE);

    char *filename = NULL;
    dt_film_t film;
    GSList *list = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(filechooser));
    GSList *it = list;
    int id = 0;
    int filmid = 0;
    /* reset filter so that view isn't empty */
    dt_view_filter_reset(darktable.view_manager, TRUE);

    while(it)
    {
      filename = (char *)it->data;
      gchar *directory = g_path_get_dirname((const gchar *)filename);
      filmid = dt_film_new(&film, directory);
      id = dt_image_import(filmid, filename, TRUE);

      if(!id)
        dt_control_log(_("error loading file `%s'"), filename);

      g_free(filename);
      g_free(directory);
      it = g_slist_next(it);
    }

    if(id)
    {
      dt_film_open(filmid);
      // make sure buffers are loaded (load full for testing)
      dt_mipmap_buffer_t buf;
      dt_mipmap_cache_get(darktable.mipmap_cache, &buf, id, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
      gboolean loaded = (buf.buf != NULL);
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

      if(!loaded)
        dt_control_log(_("file has unknown format!"));
      else
      {
        dt_control_set_mouse_over_id(id);
        dt_ctl_switch_mode_to("darkroom");
      }
    }
  }

  gtk_widget_destroy(d->frame);
  gtk_widget_destroy(filechooser);
  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
}

static void _lib_import_folder_callback(GtkWidget *widget, dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("import folder"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_cancel"),
      GTK_RESPONSE_CANCEL, _("_open"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), TRUE);
  char *last_directory = dt_conf_get_string("ui_last/import_last_directory");

  if(last_directory != NULL)
  {
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), last_directory);
    g_free(last_directory);
  }

  dt_import_metadata_t metadata;
  gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(filechooser),
                                    _lib_import_get_extra_widget(d, &metadata, TRUE));
  // run the dialog
  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(filechooser));
    dt_conf_set_string("ui_last/import_last_directory", folder);
    g_free(folder);
    _lib_import_evaluate_extra_widget(d, &metadata, TRUE);

    char *filename = NULL, *first_filename = NULL;
    GSList *list = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(filechooser));
    GSList *it = list;
    /* reset filter so that view isn't empty */
    dt_view_filter_reset(darktable.view_manager, TRUE);
    /* for each selected folder add import job */
    while(it)
    {
      filename = (char *)it->data;
      dt_film_import(filename);

      if(!first_filename)
      {
        first_filename = g_strdup(filename);

        if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->recursive)))
          first_filename = dt_util_dstrcat(first_filename, "%%");
      }

      g_free(filename);
      it = g_slist_next(it);
    }
    /* update collection to view import */
    if(first_filename)
    {
      dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
      dt_conf_set_int("plugins/lighttable/collect/item0", 0);
      dt_conf_set_string("plugins/lighttable/collect/string0", first_filename);
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, NULL);
      g_free(first_filename);
    }

    g_slist_free(list);
  }

  gtk_widget_destroy(d->frame);
  gtk_widget_destroy(filechooser);
  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
}

void gui_init(dt_lib_module_t *self)
{
  // initialize ui widgets
  dt_lib_import_t *d = (dt_lib_import_t *)g_malloc0(sizeof(dt_lib_import_t));
  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  // add import single image buttons
  GtkWidget *widget = gtk_button_new_with_label(_("image..."));
  d->import_file = GTK_BUTTON(widget);
  gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(widget)), GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(widget, _("select one or more images to import"));
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_receives_default(widget, TRUE);
  gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(_lib_import_single_image_callback), d);

  // adding the import folder button
  widget = gtk_button_new_with_label(_("folder..."));
  d->import_directory = GTK_BUTTON(widget);
  gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(widget)), GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(widget, _("select a folder to import as film roll"));
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_receives_default(widget, TRUE);
  gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(_lib_import_folder_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* cleanup mem */
  g_free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
