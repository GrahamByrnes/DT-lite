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
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/styles.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/styles.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE(1)

typedef struct dt_lib_styles_t
{
  GtkEntry *entry;
  GtkWidget *duplicate;
  GtkTreeView *tree;
  GtkWidget *create_button, *edit_button, *delete_button, *import_button, *export_button, *applymode, *apply_button;
} dt_lib_styles_t;

const char *name(dt_lib_module_t *self)
{
  return _("styles");
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

int position()
{
  return 599;
}

typedef enum _styles_columns_t
{
  DT_STYLES_COL_NAME = 0,
  DT_STYLES_COL_TOOLTIP,
  DT_STYLES_COL_FULLNAME,
  DT_STYLES_NUM_COLS
} _styles_columns_t;

static gboolean _get_node_for_name(GtkTreeModel *model, gboolean root, GtkTreeIter *iter, const gchar *parent_name)
{
  GtkTreeIter parent = *iter;

  if(root)
  {
    // iter is null, we are at the top level
    // if we have no nodes in this tree, let's create it now
    if(!gtk_tree_model_get_iter_first(model, iter))
    {
      gtk_tree_store_append(GTK_TREE_STORE(model), iter, NULL);
      return FALSE;
    }
  }
  else
  {
    // if we have no children, create one, this is our node
    if(!gtk_tree_model_iter_children(GTK_TREE_MODEL(model), iter, &parent))
    {
      gtk_tree_store_append(GTK_TREE_STORE(model), iter, &parent);
      return FALSE;
    }
  }

  // here we have iter to be on the right level, let's check if we can find parent_name
  do
  {
    gchar *name;
    gtk_tree_model_get(model, iter, DT_STYLES_COL_NAME, &name, -1);
    const gboolean match = !g_strcmp0(name, parent_name);
    g_free(name);

    if(match)
      return TRUE;
  }
  while(gtk_tree_model_iter_next(model, iter));

  // not found, create it under parent
  gtk_tree_store_append(GTK_TREE_STORE(model), iter, root ? NULL : &parent);
  return FALSE;
}

static void _gui_styles_update_view(dt_lib_styles_t *d)
{
  /* clear current list */
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->tree));
  g_object_ref(model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->tree), NULL);
  gtk_tree_store_clear(GTK_TREE_STORE(model));

  GList *result = dt_styles_get_list(gtk_entry_get_text(d->entry));
  if(result)
  {
    for(const GList *res_iter = result; res_iter; res_iter = g_list_next(res_iter))
    {
      dt_style_t *style = (dt_style_t *)res_iter->data;

      gchar *items_string = (gchar *)dt_styles_get_item_list_as_string(style->name);
      gchar *tooltip = NULL;

      if(style->description && *style->description)
        tooltip = g_strconcat("<b>", g_markup_escape_text(style->description, -1), "</b>\n", items_string, NULL);
      else
        tooltip = g_strdup(items_string);

      gchar **split = g_strsplit(style->name, "|", 0);
      int k = 0;

      while(split[k])
      {
        const gchar *s = split[k];
        const gboolean node_found = _get_node_for_name(model, k == 0, &iter, s);

        if(!node_found)
        {
          if(split[k+1])
            gtk_tree_store_set(GTK_TREE_STORE(model), &iter, DT_STYLES_COL_NAME, s, -1);
          else
            // a leaf
            gtk_tree_store_set(GTK_TREE_STORE(model), &iter,
                               DT_STYLES_COL_NAME, s, DT_STYLES_COL_TOOLTIP, tooltip, DT_STYLES_COL_FULLNAME, style->name, -1);
        }

        k++;
      }

      g_strfreev(split);
      g_free(items_string);
      g_free(tooltip);
    }

    g_list_free_full(result, dt_style_free);
  }

  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(d->tree), DT_STYLES_COL_TOOLTIP);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->tree), model);
  g_object_unref(model);
}

// get list of style names from selection
// free returned list with g_list_free_full(list, g_free)
GList* _get_selected_style_names(GList* selected_styles, GtkTreeModel *model)
{
  GtkTreeIter iter;
  GList *style_names = NULL;

  for (const GList *style = selected_styles; style; style = g_list_next(style))
  {
    GValue value = {0,};
    gtk_tree_model_get_iter(model, &iter, (GtkTreePath *)style->data);
    gtk_tree_model_get_value(model, &iter, DT_STYLES_COL_FULLNAME, &value);

    if(G_VALUE_HOLDS_STRING(&value))
      style_names = g_list_prepend(style_names, g_strdup(g_value_get_string(&value)));

    g_value_unset(&value);
  }
  return g_list_reverse(style_names);
}

static void apply_clicked(GtkWidget *w, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree));

  if(gtk_tree_selection_count_selected_rows(selection) == 0) return;

  GtkTreeModel *model= gtk_tree_view_get_model(d->tree);
  GList *selected_styles = gtk_tree_selection_get_selected_rows(selection, &model);
  GList *style_names = _get_selected_style_names(selected_styles, model);
  g_list_free_full(selected_styles, (GDestroyNotify) gtk_tree_path_free);

  if(style_names == NULL) return;

  const GList *list = dt_view_get_images_to_act_on(TRUE, TRUE);

  if(list)
    dt_multiple_styles_apply_to_list(style_names, list, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->duplicate)));

  g_list_free_full(style_names, g_free);
}

static void create_clicked(GtkWidget *w, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  const GList *list = dt_view_get_images_to_act_on(TRUE, TRUE);
  dt_styles_create_from_list(list);
  _gui_styles_update_view(d);
}

static void edit_clicked(GtkWidget *w, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree));

  if(gtk_tree_selection_count_selected_rows(selection) == 0) return;

  GtkTreeIter iter;
  GtkTreeModel *model= gtk_tree_view_get_model(d->tree);

  GList *styles = gtk_tree_selection_get_selected_rows(selection, &model);
  for (const GList *style = styles; style; style = g_list_next(style))
  {
    char *name = NULL;
    GValue value = {0,};
    gtk_tree_model_get_iter(model, &iter, (GtkTreePath *)style->data);
    gtk_tree_model_get_value(model, &iter, DT_STYLES_COL_FULLNAME, &value);

    if(G_VALUE_HOLDS_STRING(&value))
      name = g_strdup(g_value_get_string(&value));

    g_value_unset(&value);

    if(name)
    {
      dt_gui_styles_dialog_edit(name);
      _gui_styles_update_view(d);
      g_free(name);
    }
  }
  g_list_free_full (styles, (GDestroyNotify) gtk_tree_path_free);
}

static void delete_clicked(GtkWidget *w, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree));

  if(gtk_tree_selection_count_selected_rows(selection) == 0) return;

  GtkTreeModel *model= gtk_tree_view_get_model(d->tree);
  GList *selected_styles = gtk_tree_selection_get_selected_rows(selection, &model);
  GList *style_names = _get_selected_style_names(selected_styles, model);
  g_list_free_full(selected_styles, (GDestroyNotify) gtk_tree_path_free);

  if(style_names == NULL) return;

  const gint select_cnt = g_list_length(style_names);
  gint res = GTK_RESPONSE_YES;

  if(dt_conf_get_bool("plugins/lighttable/style/ask_before_delete_style"))
  {
    const GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
    GtkWidget *dialog = gtk_message_dialog_new
      (GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
       ngettext("do you really want to remove %d style?", "do you really want to remove %d styles?", select_cnt),
       select_cnt);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif

    gtk_window_set_title(GTK_WINDOW(dialog), ngettext("remove style?", "remove styles?", select_cnt));
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }

  if(res == GTK_RESPONSE_YES)
  {
    for (const GList *style = style_names; style; style = g_list_next(style))
      dt_styles_delete_by_name((char*)style->data);

    _gui_styles_update_view(d);
  }

  g_list_free_full(style_names, g_free);
}

static void export_clicked(GtkWidget *w, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree));

  if(gtk_tree_selection_count_selected_rows(selection) == 0) return;

  GtkTreeModel *model= gtk_tree_view_get_model(d->tree);
  GList *selected_styles = gtk_tree_selection_get_selected_rows(selection, &model);
  GList *style_names = _get_selected_style_names(selected_styles, model);
  g_list_free_full(selected_styles, (GDestroyNotify) gtk_tree_path_free);

  if(style_names == NULL) return;

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("select directory"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_cancel"),
      GTK_RESPONSE_CANCEL, _("_save"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), g_get_home_dir());
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    char *filedir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));

    for (const GList *style = style_names; style; style = g_list_next(style))
    {
      dt_styles_save_to_file((char*)style->data, filedir, FALSE);
      dt_control_log(_("style %s was successfully saved"), (char*)style->data);
    }

    g_free(filedir);
  }

  gtk_widget_destroy(filechooser);
  g_list_free_full(style_names, g_free);
}

static void import_clicked(GtkWidget *w, gpointer user_data)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("select style"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN, _("_cancel"), GTK_RESPONSE_CANCEL,
      _("_open"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), TRUE);
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), g_get_home_dir());

  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.dtstyle");
  gtk_file_filter_add_pattern(filter, "*.DTSTYLE");
  gtk_file_filter_set_name(filter, _("darktable style files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));

  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    GSList *filenames = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(filechooser));
    g_slist_foreach(filenames, (GFunc)dt_styles_import_from_file, NULL);
    g_slist_free_full(filenames, g_free);

    dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
    _gui_styles_update_view(d);
  }
  gtk_widget_destroy(filechooser);
}

static gboolean entry_callback(GtkEntry *entry, gpointer user_data)
{
  _gui_styles_update_view(user_data);
  return FALSE;
}

static gboolean entry_activated(GtkEntry *entry, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  const gchar *name = gtk_entry_get_text(d->entry);

  if(name)
  {
    const GList *imgs = dt_view_get_images_to_act_on(TRUE, TRUE);
    dt_styles_apply_to_list(name, imgs, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->duplicate)));
  }

  return FALSE;
}

static gboolean duplicate_callback(GtkEntry *entry, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  dt_conf_set_bool("ui_last/styles_create_duplicate",
                   gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->duplicate)));
  return FALSE;
}

static void applymode_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  const int mode = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/lighttable/style/applymode", mode);
}

static void _update(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  dt_lib_styles_t *d = (dt_lib_styles_t *)self->data;
  const GList *imgs = dt_view_get_images_to_act_on(TRUE, FALSE);
  const gboolean has_act_on = imgs != NULL;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree));
  const gint sel_styles_cnt = gtk_tree_selection_count_selected_rows(selection);

  gtk_widget_set_sensitive(GTK_WIDGET(d->create_button), has_act_on);
  gtk_widget_set_sensitive(GTK_WIDGET(d->edit_button), sel_styles_cnt > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(d->delete_button), sel_styles_cnt > 0);

  //import is ALWAYS enabled.
  gtk_widget_set_sensitive(GTK_WIDGET(d->export_button), sel_styles_cnt > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(d->apply_button), has_act_on && sel_styles_cnt > 0);
}

static void _styles_changed_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_styles_t *d = (dt_lib_styles_t *)self->data;
  _gui_styles_update_view(d);
  _update(self);
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

static void _tree_selection_changed(GtkTreeSelection *treeselection, gpointer data)
{
  _update((dt_lib_module_t *)data);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)malloc(sizeof(dt_lib_styles_t));
  self->data = (void *)d;
  self->timeout_handle = 0;
  d->edit_button = NULL;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *w;
  GtkWidget *scrolled;

  // tree
  d->tree = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_tree_view_set_headers_visible(d->tree, FALSE);
  GtkTreeStore *treestore = gtk_tree_store_new(DT_STYLES_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->tree), col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", DT_STYLES_COL_NAME);

  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree)), GTK_SELECTION_MULTIPLE);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->tree), GTK_TREE_MODEL(treestore));
  g_object_unref(treestore);
  g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree)), "changed", G_CALLBACK(_tree_selection_changed), self);

  // filter entry
  w = gtk_entry_new();
  d->entry = GTK_ENTRY(w);
  gtk_widget_set_tooltip_text(w, _("filter style names"));
  g_signal_connect(d->entry, "changed", G_CALLBACK(entry_callback), d);
  g_signal_connect(d->entry, "activate", G_CALLBACK(entry_activated), d);

  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->entry));

  scrolled = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), DT_PIXEL_APPLY_DPI(250));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->entry), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(scrolled), TRUE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(scrolled), GTK_WIDGET(d->tree));

  d->duplicate = gtk_check_button_new_with_label(_("create duplicate"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->duplicate), TRUE, FALSE, 0);
  g_signal_connect(d->duplicate, "toggled", G_CALLBACK(duplicate_callback), d);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->duplicate),
                               dt_conf_get_bool("ui_last/styles_create_duplicate"));
  gtk_widget_set_tooltip_text(d->duplicate, _("creates a duplicate of the image before applying style"));

  d->applymode = dt_bauhaus_combobox_new(NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->applymode), TRUE, FALSE, 0);
  dt_bauhaus_widget_set_label(d->applymode, NULL, _("mode"));
  dt_bauhaus_combobox_add(d->applymode, _("append"));
  dt_bauhaus_combobox_add(d->applymode, _("overwrite"));
  gtk_widget_set_tooltip_text(d->applymode, _("how to handle existing history"));
  dt_bauhaus_combobox_set(d->applymode, dt_conf_get_int("plugins/lighttable/style/applymode"));

  GtkWidget *hbox1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *hbox2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *hbox3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox1, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox2, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox3, TRUE, FALSE, 0);

  // create
  GtkWidget *widget = gtk_button_new_with_label(_("create..."));
  d->create_button = widget;
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(create_clicked), d);
  gtk_widget_set_tooltip_text(widget, _("create styles from history stack of selected images"));
  gtk_box_pack_start(GTK_BOX(hbox1), widget, TRUE, TRUE, 0);

  // edit
  widget = gtk_button_new_with_label(_("edit..."));
  d->edit_button = widget;
  g_signal_connect(widget, "clicked", G_CALLBACK(edit_clicked), d);
  gtk_widget_set_tooltip_text(widget, _("edit the selected styles in list above"));
  gtk_box_pack_start(GTK_BOX(hbox1), widget, TRUE, TRUE, 0);

  // delete
  widget = gtk_button_new_with_label(_("remove"));
  d->delete_button = widget;
  g_signal_connect(widget, "clicked", G_CALLBACK(delete_clicked), d);
  gtk_widget_set_tooltip_text(widget, _("removes the selected styles in list above"));
  gtk_box_pack_start(GTK_BOX(hbox1), widget, TRUE, TRUE, 0);

  // import button
  GtkWidget *importButton = gtk_button_new_with_label(C_("verb", "import..."));
  d->import_button = importButton;
  gtk_widget_set_tooltip_text(importButton, _("import styles from a style files"));
  g_signal_connect(importButton, "clicked", G_CALLBACK(import_clicked), d);
  gtk_box_pack_start(GTK_BOX(hbox2), importButton, TRUE, TRUE, 0);

  // export button
  GtkWidget *exportButton = gtk_button_new_with_label(_("export..."));
  d->export_button = exportButton;
  gtk_widget_set_tooltip_text(exportButton, _("export the selected styles into a style files"));
  g_signal_connect(exportButton, "clicked", G_CALLBACK(export_clicked), d);
  gtk_box_pack_start(GTK_BOX(hbox2), exportButton, TRUE, TRUE, 0);

  // apply button
  widget = gtk_button_new_with_label(_("apply"));
  d->apply_button = widget;
  g_signal_connect(widget, "clicked", G_CALLBACK(apply_clicked), d);
  gtk_widget_set_tooltip_text(widget, _("apply the selected styles in list above to selected images"));
  gtk_box_pack_start(GTK_BOX(hbox3), widget, TRUE, TRUE, 0);

  // add entry completion
  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(completion, gtk_tree_view_get_model(GTK_TREE_VIEW(d->tree)));
  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_set_completion(d->entry, completion);

  // update filtered list
  _gui_styles_update_view(d);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_STYLE_CHANGED, G_CALLBACK(_styles_changed_callback), self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_image_selection_changed_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_collection_updated_callback), self);

  g_signal_connect(G_OBJECT(d->applymode), "value-changed", G_CALLBACK(applymode_combobox_changed), (gpointer)self);

  _update(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  dt_lib_styles_t *d = (dt_lib_styles_t *)self->data;
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_styles_changed_callback), self);

  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_image_selection_changed_callback), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_collection_updated_callback), self);

  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(d->entry));
  free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
