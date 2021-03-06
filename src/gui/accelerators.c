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

#include "gui/accelerators.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/utility.h"
#include "control/control.h"
#include "develop/blend.h"

#include "bauhaus/bauhaus.h"

#include <assert.h>
#include <gtk/gtk.h>

void dt_accel_path_global(char *s, size_t n, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s", NC_("accel", "global"), path);
}

void dt_accel_path_view(char *s, size_t n, char *module, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s", NC_("accel", "views"), module, path);
}

void dt_accel_path_iop(char *s, size_t n, char *module, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s", NC_("accel", "image operations"), module, path);
}

void dt_accel_path_lib(char *s, size_t n, char *module, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s", NC_("accel", "modules"), module, path);
}

static void dt_accel_path_global_translated(char *s, size_t n, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s", C_("accel", "global"), g_dpgettext2(NULL, "accel", path));
}

static void dt_accel_path_iop_translated(char *s, size_t n, dt_iop_module_so_t *module, const char *path)
{
  gchar *module_name_fixed = dt_util_str_replace(module->name(), "/", "-");
  snprintf(s, n, "<Darktable>/%s/%s/%s", C_("accel", "image operations"), module_name_fixed,
           g_dpgettext2(NULL, "accel", path));
  g_free(module_name_fixed);
}

static void dt_accel_path_lib_translated(char *s, size_t n, dt_lib_module_t *module, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s", C_("accel", "modules"), module->name(module),
           g_dpgettext2(NULL, "accel", path));
}

void dt_accel_register_global(const gchar *path, guint accel_key, GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_t *accel = (dt_accel_t *)g_malloc(sizeof(dt_accel_t));

  dt_accel_path_global(accel_path, sizeof(accel_path), path);
  gtk_accel_map_add_entry(accel_path, accel_key, mods);

  g_strlcpy(accel->path, accel_path, sizeof(accel->path));
  dt_accel_path_global_translated(accel_path, sizeof(accel_path), path);
  g_strlcpy(accel->translated_path, accel_path, sizeof(accel->translated_path));

  *(accel->module) = '\0';
  accel->local = FALSE;
  accel->views = DT_VIEW_DARKROOM | DT_VIEW_LIGHTTABLE | DT_VIEW_PRINT;
  darktable.control->accelerator_list = g_slist_prepend(darktable.control->accelerator_list, accel);
}

void dt_accel_register_iop(dt_iop_module_so_t *so, gboolean local, const gchar *path, guint accel_key,
                           GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_t *accel = (dt_accel_t *)g_malloc(sizeof(dt_accel_t));

  dt_accel_path_iop(accel_path, sizeof(accel_path), so->op, path);
  gtk_accel_map_add_entry(accel_path, accel_key, mods);

  g_strlcpy(accel->path, accel_path, sizeof(accel->path));
  dt_accel_path_iop_translated(accel_path, sizeof(accel_path), so, path);
  g_strlcpy(accel->translated_path, accel_path, sizeof(accel->translated_path));

  g_strlcpy(accel->module, so->op, sizeof(accel->module));
  accel->local = local;
  accel->views = DT_VIEW_DARKROOM;
  darktable.control->accelerator_list = g_slist_prepend(darktable.control->accelerator_list, accel);
}

void dt_accel_register_lib_for_views(dt_lib_module_t *self, dt_view_type_flags_t views, const gchar *path,
                                     guint accel_key, GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_path_lib(accel_path, sizeof(accel_path), self->plugin_name, path);

  if (dt_accel_find_by_path(accel_path)) return; // return if nothing to add, to avoid multiple entries

  dt_accel_t *accel = (dt_accel_t *)g_malloc(sizeof(dt_accel_t));

  gtk_accel_map_add_entry(accel_path, accel_key, mods);
  g_strlcpy(accel->path, accel_path, sizeof(accel->path));
  dt_accel_path_lib_translated(accel_path, sizeof(accel_path), self, path);
  g_strlcpy(accel->translated_path, accel_path, sizeof(accel->translated_path));

  g_strlcpy(accel->module, self->plugin_name, sizeof(accel->module));
  accel->local = FALSE;
  // we get the views in which the lib will be displayed
  accel->views = views;
  darktable.control->accelerator_list = g_slist_prepend(darktable.control->accelerator_list, accel);
}

void dt_accel_register_lib(dt_lib_module_t *self, const gchar *path, guint accel_key, GdkModifierType mods)
{
  dt_view_type_flags_t v = 0;
  int i=0;
  const gchar **views = self->views(self);

  while (views[i])
  {
    if(strcmp(views[i], "lighttable") == 0)
      v |= DT_VIEW_LIGHTTABLE;
    else if(strcmp(views[i], "darkroom") == 0)
      v |= DT_VIEW_DARKROOM;
    else if(strcmp(views[i], "print") == 0)
      v |= DT_VIEW_PRINT;
    else if(strcmp(views[i], "*") == 0)
      v |= DT_VIEW_DARKROOM | DT_VIEW_LIGHTTABLE | DT_VIEW_PRINT;

    i++;
  }

  dt_accel_register_lib_for_views(self, v, path, accel_key, mods);
}

static dt_accel_t *_lookup_accel(const gchar *path)
{
  for(const GSList *l = darktable.control->accelerator_list; l; l = g_slist_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strcmp(accel->path, path))
      return accel;
  }
  return NULL;
}

void dt_accel_connect_global(const gchar *path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_global(accel_path, sizeof(accel_path), path);
  dt_accel_t *laccel = _lookup_accel(accel_path);
  laccel->closure = closure;
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);
}

dt_accel_t *dt_accel_connect_lib_as_global(dt_lib_module_t *module, const gchar *path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_global(accel_path, sizeof(accel_path), path);
  dt_accel_t *accel = _lookup_accel(accel_path);
  
  if(!accel)
    return NULL; // this happens when the path doesn't match any accel (typos, ...)

  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);
  accel->closure = closure;
  module->accel_closures = g_slist_prepend(module->accel_closures, accel);
  return accel;
}

static void _connect_local_accel(dt_iop_module_t *module, dt_accel_t *accel)
{
  module->accel_closures_local = g_slist_prepend(module->accel_closures_local, accel);
}

dt_accel_t *dt_accel_connect_iop(dt_iop_module_t *module, const gchar *path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_iop(accel_path, sizeof(accel_path), module->op, path);
  // Looking up the entry in the global accelerators list
  dt_accel_t *accel = _lookup_accel(accel_path);

  if(accel)
    accel->closure = closure;

  if(accel && accel->local)
  {
    // Local accelerators don't actually get connected, just added to the list
    // They will be connected if/when the module gains focus
    _connect_local_accel(module, accel);
  }
  else
  {
    gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);
    module->accel_closures = g_slist_prepend(module->accel_closures, accel);
  }

  return accel;
}

dt_accel_t *dt_accel_connect_lib(dt_lib_module_t *module, const gchar *path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_lib(accel_path, sizeof(accel_path), module->plugin_name, path);
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);

  dt_accel_t *accel = _lookup_accel(accel_path);

  if(!accel) return NULL; // this happens when the path doesn't match any accel (typos, ...)

  accel->closure = closure;
  module->accel_closures = g_slist_prepend(module->accel_closures, accel);
  return accel;
}

static gboolean _press_button_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                       GdkModifierType modifier, gpointer data)
{
  if(!(GTK_IS_BUTTON(data))) return FALSE;

  gtk_button_clicked(GTK_BUTTON(data));
  return TRUE;
}

static gboolean _tooltip_callback(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                                  GtkTooltip *tooltip, gpointer user_data)
{
  char *text = gtk_widget_get_tooltip_text(widget);
  GtkAccelKey key;
  dt_accel_t *accel = g_object_get_data(G_OBJECT(widget), "dt-accel");

  if(accel && gtk_accel_map_lookup_entry(accel->path, &key))
  {
    gchar *key_name = gtk_accelerator_get_label(key.accel_key, key.accel_mods);

    if(key_name && *key_name)
    {
      char *tmp = g_strdup_printf("%s (%s)", text, key_name);
      g_free(text);
      text = tmp;
    }

    g_free(key_name);
  }

  gtk_tooltip_set_text(tooltip, text);
  g_free(text);
  return TRUE;
}

void dt_accel_connect_button_lib(dt_lib_module_t *module, const gchar *path, GtkWidget *button)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(_press_button_callback), button, NULL);
  dt_accel_t *accel = dt_accel_connect_lib(module, path, closure);
  g_object_set_data(G_OBJECT(button), "dt-accel", accel);

  if(gtk_widget_get_has_tooltip(button))
    g_signal_connect(G_OBJECT(button), "query-tooltip", G_CALLBACK(_tooltip_callback), NULL);
}

void dt_accel_connect_button_lib_as_global(dt_lib_module_t *module, const gchar *path, GtkWidget *button)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(_press_button_callback), button, NULL);
  dt_accel_t *accel = dt_accel_connect_lib_as_global(module, path, closure);
  g_object_set_data(G_OBJECT(button), "dt-accel", accel);

  if(gtk_widget_get_has_tooltip(button))
    g_signal_connect(G_OBJECT(button), "query-tooltip", G_CALLBACK(_tooltip_callback), NULL);
}

void dt_accel_widget_toast(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(!gtk_widget_is_visible(GTK_WIDGET(w)) && !darktable.gui->reset)
  {
    char *text = NULL;
    int show = 1;

    switch(w->type){
      case DT_BAUHAUS_SLIDER:
      {
        dt_bauhaus_slider_data_t *d = &w->data.slider;
        show = !d->is_dragging;
        text = dt_bauhaus_slider_get_text(widget);
        break;
      }
      case DT_BAUHAUS_COMBOBOX:
        text = g_strdup(dt_bauhaus_combobox_get_text(widget));
        break;
      default: //literally impossible but hey
        return;
        break;
    }

    if(show)
    {
      if(w->label[0] != '\0')
      { // label is not empty
        if(w->module && w->module->multi_name[0] != '\0')
          dt_toast_log(_("%s %s / %s: %s"), w->module->name(), w->module->multi_name, w->label, text);
        else if(w->module && !strstr(w->module->name(), w->label))
          dt_toast_log(_("%s / %s: %s"), w->module->name(), w->label, text);
        else
          dt_toast_log(_("%s: %s"), w->label, text);
      }
      else
      { //label is empty
        if(w->module && w->module->multi_name[0] != '\0')
          dt_toast_log(_("%s %s / %s"), w->module->name(), w->module->multi_name, text);
        else if(w->module)
          dt_toast_log(_("%s / %s"), w->module->name(), text);
        else
          dt_toast_log(_("%s"), text);
      }
    }

    g_free(text);
  }
}

float dt_accel_get_slider_scale_multiplier()
{
  const int slider_precision = dt_conf_get_int("accel/slider_precision");
  
  if(slider_precision == DT_IOP_PRECISION_COARSE)
    return dt_conf_get_float("darkroom/ui/scale_rough_step_multiplier");
  else if(slider_precision == DT_IOP_PRECISION_FINE)
    return dt_conf_get_float("darkroom/ui/scale_precise_step_multiplier");

  return dt_conf_get_float("darkroom/ui/scale_step_multiplier");
}

void dt_accel_connect_locals_iop(dt_iop_module_t *module)
{
  for(const GSList *l = module->accel_closures_local; l; l = g_slist_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel)
      gtk_accel_group_connect_by_path(darktable.control->accelerators, accel->path, accel->closure);
  }

  module->local_closures_connected = TRUE;
}

void dt_accel_disconnect_list(GSList **list_ptr)
{
  GSList *list = *list_ptr;
  while(list)
  {
    dt_accel_t *accel = (dt_accel_t *)list->data;
    if(accel)
      gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);

    list = g_slist_delete_link(list, list);
  }
  *list_ptr = NULL;
}

void dt_accel_disconnect_locals_iop(dt_iop_module_t *module)
{
  if(!module->local_closures_connected) return;

  for(const GSList *l = module->accel_closures_local; l; l = g_slist_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel)
    {
      g_closure_ref(accel->closure);
      gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
    }
  }

  module->local_closures_connected = FALSE;
}

void dt_accel_cleanup_locals_iop(dt_iop_module_t *module)
{
  GSList *l = module->accel_closures_local; 
  while(l)
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && module->local_closures_connected)
      gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
    l = g_slist_delete_link(l, l);
  }
  module->accel_closures_local = NULL;
}

typedef struct
{
  dt_iop_module_t *module;
  char *name;
} preset_iop_module_callback_description;

static void preset_iop_module_callback_destroyer(gpointer data, GClosure *closure)
{
  preset_iop_module_callback_description *callback_description
      = (preset_iop_module_callback_description *)data;
  g_free(callback_description->name);
  g_free(data);
}

static gboolean preset_iop_module_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  preset_iop_module_callback_description *callback_description
      = (preset_iop_module_callback_description *)data;
  dt_iop_module_t *module = callback_description->module;
  const char *name = callback_description->name;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT op_params, enabled, blendop_params, "
                                                             "blendop_version FROM data.presets "
                                                             "WHERE operation = ?1 AND name = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, name, -1, SQLITE_TRANSIENT);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *op_params = sqlite3_column_blob(stmt, 0);
    int op_length = sqlite3_column_bytes(stmt, 0);
    int enabled = sqlite3_column_int(stmt, 1);
    const void *blendop_params = sqlite3_column_blob(stmt, 2);
    int bl_length = sqlite3_column_bytes(stmt, 2);
    int blendop_version = sqlite3_column_int(stmt, 3);

    if(op_params && (op_length == module->params_size))
    {
      memcpy(module->params, op_params, op_length);
      module->enabled = enabled;
    }

    if(blendop_params && (blendop_version == dt_develop_blend_version())
       && (bl_length == sizeof(dt_develop_blend_params_t)))
      memcpy(module->blend_params, blendop_params, sizeof(dt_develop_blend_params_t));
    else if(blendop_params
            && dt_develop_blend_legacy_params(module, blendop_params, blendop_version, module->blend_params,
                                              dt_develop_blend_version(), bl_length) == 0)
    {
      // do nothing
    }
    else
      memcpy(module->blend_params, module->default_blendop_params, sizeof(dt_develop_blend_params_t));
  }

  sqlite3_finalize(stmt);
  dt_iop_gui_update(module);
  dt_dev_add_history_item(darktable.develop, module, FALSE);
  gtk_widget_queue_draw(module->widget);
  return TRUE;
}

void dt_accel_connect_preset_iop(dt_iop_module_t *module, const gchar *path)
{
  char build_path[1024];
  gchar *name = g_strdup(path);
  snprintf(build_path, sizeof(build_path), "%s/%s", _("preset"), name);
  preset_iop_module_callback_description *callback_description
      = g_malloc(sizeof(preset_iop_module_callback_description));
  callback_description->module = module;
  callback_description->name = name;

  GClosure *closure = g_cclosure_new(G_CALLBACK(preset_iop_module_callback), callback_description,
                                     preset_iop_module_callback_destroyer);
  dt_accel_connect_iop(module, build_path, closure);
}

typedef struct
{
  dt_lib_module_t *module;
  char *name;
} preset_lib_module_callback_description;

static void preset_lib_module_callback_destroyer(gpointer data, GClosure *closure)
{
  preset_lib_module_callback_description *callback_description
      = (preset_lib_module_callback_description *)data;
  g_free(callback_description->name);
  g_free(data);
}

static gboolean preset_lib_module_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  preset_lib_module_callback_description *callback_description
      = (preset_lib_module_callback_description *)data;
  dt_lib_module_t *module = callback_description->module;
  const char *pn = callback_description->name;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT op_params FROM data.presets WHERE operation = ?1 AND op_version = ?2 AND name = ?3", -1, &stmt,
      NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, pn, -1, SQLITE_TRANSIENT);

  int res = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *blob = sqlite3_column_blob(stmt, 0);
    int length = sqlite3_column_bytes(stmt, 0);
    if(blob)
    {
      for(const GList *it = darktable.lib->plugins; it; it = g_list_next(it))
      {
        dt_lib_module_t *search_module = (dt_lib_module_t *)it->data;

        if(!strncmp(search_module->plugin_name, module->plugin_name, 128))
        {
          res = module->set_params(module, blob, length);
          break;
        }
      }
    }
  }
  sqlite3_finalize(stmt);

  if(res)
  {
    dt_control_log(_("deleting preset for obsolete module"));
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM data.presets WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->plugin_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, pn, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  return TRUE;
}

void dt_accel_connect_preset_lib(dt_lib_module_t *module, const gchar *path)
{
  char build_path[1024];
  gchar *name = g_strdup(path);
  snprintf(build_path, sizeof(build_path), "%s/%s", _("preset"), name);
  preset_lib_module_callback_description *callback_description
      = g_malloc(sizeof(preset_lib_module_callback_description));
  callback_description->module = module;
  callback_description->name = name;

  GClosure *closure = g_cclosure_new(G_CALLBACK(preset_lib_module_callback), callback_description,
                                     preset_lib_module_callback_destroyer);
  dt_accel_connect_lib(module, build_path, closure);
}

void dt_accel_deregister_iop(dt_iop_module_t *module, const gchar *path)
{
  char build_path[1024];
  dt_accel_path_iop(build_path, sizeof(build_path), module->op, path);

  for(GSList *l = module->accel_closures_local; l; l = g_slist_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;

    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      module->accel_closures_local = g_slist_delete_link(module->accel_closures_local, l);
      l = NULL;
    }
  }

  for(GSList *l = module->accel_closures; l; l = g_slist_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      if(!accel->local || !module->local_closures_connected)
        gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);

      module->accel_closures = g_slist_delete_link(module->accel_closures, l);
      l = NULL;
    }
  }

  for(GSList *l = darktable.control->accelerator_list; l; l = g_slist_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;

    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      darktable.control->accelerator_list = g_slist_delete_link(darktable.control->accelerator_list, l);
      l = NULL;
      g_free(accel);
    }
  }

  for(GSList *l = darktable.control->dynamic_accelerator_list; l; l = g_slist_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;

    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      darktable.control->dynamic_accelerator_list
          = g_slist_delete_link(darktable.control->dynamic_accelerator_list, l);
      l = NULL;
      g_free(accel);
    }
  }
  dt_dynamic_accel_get_valid_list();
}

void dt_accel_deregister_lib(dt_lib_module_t *module, const gchar *path)
{
  char build_path[1024];
  dt_accel_path_lib(build_path, sizeof(build_path), module->plugin_name, path);
  for(GSList *l = module->accel_closures; l; l = g_slist_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      module->accel_closures = g_slist_delete_link(module->accel_closures, l);
      gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
      l = NULL;
    }
  }

  for(GSList *l = darktable.control->accelerator_list; l; l = g_slist_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      darktable.control->accelerator_list = g_slist_delete_link(darktable.control->accelerator_list, l);
      l = NULL;
      g_free(accel);
    }
  }
}

gboolean find_accel_internal(GtkAccelKey *key, GClosure *closure, gpointer data)
{
  return (closure == data);
}

void dt_accel_rename_preset_iop(dt_iop_module_t *module, const gchar *path, const gchar *new_path)
{
  char build_path[1024];
  dt_accel_path_iop(build_path, sizeof(build_path), module->op, path);

  for(GSList *l = module->accel_closures; l; l = g_slist_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      GtkAccelKey tmp_key
          = *(gtk_accel_group_find(darktable.control->accelerators, find_accel_internal, accel->closure));
      gboolean local = accel->local;
      dt_accel_deregister_iop(module, path);
      snprintf(build_path, sizeof(build_path), "%s/%s", _("preset"), new_path);
      dt_accel_register_iop(module->so, local, build_path, tmp_key.accel_key, tmp_key.accel_mods);
      dt_accel_connect_preset_iop(module, new_path);
      l = NULL;
    }
  }
}

void dt_accel_rename_preset_lib(dt_lib_module_t *module, const gchar *path, const gchar *new_path)
{
  char build_path[1024];
  dt_accel_path_lib(build_path, sizeof(build_path), module->plugin_name, path);
  for(GSList *l = module->accel_closures; l; l = g_slist_next(l))
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      GtkAccelKey tmp_key
          = *(gtk_accel_group_find(darktable.control->accelerators, find_accel_internal, accel->closure));
      dt_accel_deregister_lib(module, path);
      snprintf(build_path, sizeof(build_path), "%s/%s", _("preset"), new_path);
      dt_accel_register_lib(module, build_path, tmp_key.accel_key, tmp_key.accel_mods);
      dt_accel_connect_preset_lib(module, new_path);
      l = NULL;
    }
  }
}

void dt_dynamic_accel_get_valid_list()
{
  // remove all elements from the valid list (no need to free them, as they are in the norml list anyway)
  if (darktable.control->dynamic_accelerator_valid)
  {
    g_slist_free(darktable.control->dynamic_accelerator_valid);
    darktable.control->dynamic_accelerator_valid = NULL;
  }

  for(const GSList *l = darktable.control->dynamic_accelerator_list; l; l = g_slist_next(l))
  {
    dt_accel_dynamic_t *da = (dt_accel_dynamic_t *)l->data;
    if(da && da->mod_so->state != dt_iop_state_HIDDEN)
    {
      GtkAccelKey ak;
      if(gtk_accel_map_lookup_entry(da->path, &ak))
      {
        if(ak.accel_key > 0)
        {
          da->accel_key.accel_key = ak.accel_key;
          da->accel_key.accel_mods = ak.accel_mods;
          da->accel_key.accel_flags = ak.accel_flags;
          darktable.control->dynamic_accelerator_valid
              = g_slist_append(darktable.control->dynamic_accelerator_valid, da);
        }
      }
    }
  }
}

dt_accel_t *dt_accel_find_by_path(const gchar *path)
{
  return _lookup_accel(path);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
