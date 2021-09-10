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
#include "common/darktable.h"
#include "common/debug.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <assert.h>
#include <stdlib.h>

typedef struct dt_gui_presets_edit_dialog_t
{
  dt_iop_module_t *module;
  GtkEntry *name, *description;
  GtkCheckButton *autoapply, *filter;
  GtkWidget *details;
  GtkWidget *model, *maker, *lens;
  GtkWidget *iso_min, *iso_max;
  GtkWidget *exposure_min, *exposure_max;
  GtkWidget *aperture_min, *aperture_max;
  GtkWidget *focal_length_min, *focal_length_max;
  gchar *original_name;
  gint old_id;
  GtkWidget *format_btn[5];
} dt_gui_presets_edit_dialog_t;

// this is also called for non-gui applications linking to libdarktable!
// so beware, don't use any darktable.gui stuff here .. (or change this behaviour in darktable.c)
void dt_gui_presets_init()
{
  // remove auto generated presets from plugins, not the user included ones.
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM data.presets WHERE writeprotect = 1", NULL,
                        NULL, NULL);
}

void dt_gui_presets_add_generic(const char *name, dt_dev_operation_t op, const int32_t version,
                                const void *params, const int32_t params_size, const int32_t enabled)
{
  dt_develop_blend_params_t default_blendop_params
      = { DEVELOP_MASK_DISABLED,
          DEVELOP_BLEND_NORMAL2,
          100.0f,
          DEVELOP_COMBINE_NORM_EXCL,
          0,
          0,
          0.0f,
          DEVELOP_MASK_GUIDE_IN,
          0.0f,
          0.0f,
          0.0f,
          { 0, 0, 0, 0 },
          { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
            0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
            0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
            0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f },
          { 0 }, 0, 0, FALSE };
  dt_gui_presets_add_with_blendop(name, op, version, params, params_size,
                                  &default_blendop_params, enabled);
}

void dt_gui_presets_add_with_blendop(
    const char *name, dt_dev_operation_t op, const int32_t version,
    const void *params, const int32_t params_size,
    const void *blend_params, const int32_t enabled)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "INSERT OR REPLACE INTO data.presets (name, description, operation, op_version, op_params, enabled, "
      "blendop_params, blendop_version, multi_priority, multi_name, model, maker, lens, "
      "iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, focal_length_min, "
      "focal_length_max, "
      "writeprotect, autoapply, filter, def, format) "
      "VALUES (?1, '', ?2, ?3, ?4, ?5, ?6, ?7, 0, '', '%', '%', '%', 0, 340282346638528859812000000000000000000, "
      "0, 10000000, 0, 100000000, 0, "
      "1000, 1, 0, 0, 0, 0)",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, enabled);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, blend_params, sizeof(dt_develop_blend_params_t),
                             SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, dt_develop_blend_version());
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static void menuitem_pick_preset(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  gchar *name = g_object_get_data(G_OBJECT(menuitem), "dt-preset-name");
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT op_params, enabled, blendop_params, blendop_version, writeprotect FROM "
                              "data.presets WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, name, -1, SQLITE_TRANSIENT);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *op_params = sqlite3_column_blob(stmt, 0);
    int op_length = sqlite3_column_bytes(stmt, 0);
    int enabled = sqlite3_column_int(stmt, 1);
    const void *blendop_params = sqlite3_column_blob(stmt, 2);
    int bl_length = sqlite3_column_bytes(stmt, 2);
    int blendop_version = sqlite3_column_int(stmt, 3);
    int writeprotect = sqlite3_column_int(stmt, 4);

    if(op_params && (op_length == module->params_size))
    {
      memcpy(module->params, op_params, op_length);
      module->enabled = enabled;
    }

    if(blendop_params && (blendop_version == dt_develop_blend_version())
                      && (bl_length == sizeof(dt_develop_blend_params_t)))
      dt_iop_commit_blend_params(module, blendop_params);
    else if(blendop_params
            && dt_develop_blend_legacy_params(module, blendop_params, blendop_version, module->blend_params,
                                              dt_develop_blend_version(), bl_length) == 0)
    {
      // do nothing
    }
    else
      dt_iop_commit_blend_params(module, module->default_blendop_params);

    if(!writeprotect)
      dt_gui_store_last_preset(name);
  }
  sqlite3_finalize(stmt);
  dt_iop_request_focus(module);
  dt_iop_gui_update(module);
  dt_dev_add_history_item(darktable.develop, module, FALSE);
  gtk_widget_queue_draw(module->widget);
}

static gboolean menuitem_button_released_preset(GtkMenuItem *menuitem, GdkEventButton *event, dt_iop_module_t *module)
{
  if (event->button == 1 || (module->flags() & IOP_FLAGS_ONE_INSTANCE))
    menuitem_pick_preset(menuitem, module);
  else if (event->button == 2)
  {
    dt_iop_module_t *new_module = dt_iop_gui_duplicate(module, FALSE);
    if (new_module)
      menuitem_pick_preset(menuitem, new_module);
  }

  return FALSE;
}

void dt_gui_favorite_presets_menu_show()
{
  sqlite3_stmt *stmt;
  GtkMenu *menu = darktable.gui->presets_popup_menu;

  if(menu)
    gtk_widget_destroy(GTK_WIDGET(menu));

  darktable.gui->presets_popup_menu = GTK_MENU(gtk_menu_new());
  menu = darktable.gui->presets_popup_menu;
  gboolean presets = FALSE; /* TRUE if we have at least one menu entry */
  GList *modules = darktable.develop->iop;

  if(modules)
    do
    {
      dt_iop_module_t *iop = (dt_iop_module_t *)modules->data;
      /* check if module is favorite */
      if(iop->so->state == dt_iop_state_FAVORITE)
      {
        /* create submenu for module */
        GtkMenuItem *smi = (GtkMenuItem *)gtk_menu_item_new_with_label(iop->name());
        GtkMenu *sm = (GtkMenu *)gtk_menu_new();
        gtk_menu_item_set_submenu(smi, GTK_WIDGET(sm));

        /* query presets for module */
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT name, op_params, writeprotect,"
                                    "  description, blendop_params, op_version"
                                    " FROM data.presets"
                                    " WHERE operation=?1"
                                    " ORDER BY writeprotect DESC, LOWER(name), rowid",
                                    -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, iop->op, -1, SQLITE_TRANSIENT);

        while(sqlite3_step(stmt) == SQLITE_ROW)
        {
          char *name = (char *)sqlite3_column_text(stmt, 0);
          GtkMenuItem *mi = (GtkMenuItem *)gtk_menu_item_new_with_label(name);
          g_object_set_data_full(G_OBJECT(mi), "dt-preset-name", g_strdup(name), g_free);
          g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_pick_preset), iop);
          gtk_menu_shell_append(GTK_MENU_SHELL(sm), GTK_WIDGET(mi));
        }

        sqlite3_finalize(stmt);
        /* add submenu to main menu if we got any presets */
        GList *childs = gtk_container_get_children(GTK_CONTAINER(sm));

        if(childs)
        {
          gtk_menu_shell_append(GTK_MENU_SHELL(menu), GTK_WIDGET(smi));
          presets = TRUE;
          g_list_free(childs);
        }
      }

    } while((modules = g_list_next(modules)) != NULL);

  if(!presets)
  {
    gtk_widget_destroy(GTK_WIDGET(menu));
    darktable.gui->presets_popup_menu = NULL;
  }
}

static void dt_gui_presets_popup_menu_show_internal(dt_dev_operation_t op, int32_t version,
                                                    dt_iop_params_t *params, int32_t params_size,
                                                    dt_develop_blend_params_t *bl_params,
                                                    dt_iop_module_t *module, const dt_image_t *image,
                                                    void (*pick_callback)(GtkMenuItem *, void *),
                                                    void *callback_data)
{
  GtkMenu *menu = darktable.gui->presets_popup_menu;

  if(menu)
    gtk_widget_destroy(GTK_WIDGET(menu));

  darktable.gui->presets_popup_menu = GTK_MENU(gtk_menu_new());
  menu = darktable.gui->presets_popup_menu;
  const gboolean default_first = dt_conf_get_bool("plugins/darkroom/default_presets_first");
  gchar *query = NULL;
  GtkWidget *mi;
  int cnt = 0;
  sqlite3_stmt *stmt;
  // order: get shipped defaults first
  if(image)
  {
    // only matching if filter is on:
    int iformat = 0;

    if(dt_image_is_rawprepare_supported(image))
      iformat |= FOR_RAW;
    else iformat |= FOR_LDR;

    if(dt_image_is_hdr(image))
      iformat |= FOR_HDR;

    int excluded = 0;

    if(dt_image_monochrome_flags(image))
      excluded |= FOR_NOT_MONO;
    else excluded |= FOR_NOT_COLOR;
    
    query = g_strdup_printf
      ("SELECT name, op_params, writeprotect, description, blendop_params, "
       "  op_version, enabled"
       " FROM data.presets"
       " WHERE operation=?1"
       "   AND (filter=0"
       "          OR"
       "       (((?2 LIKE model AND ?3 LIKE maker) OR (?4 LIKE model AND ?5 LIKE maker))"
       "        AND ?6 LIKE lens"
       "        AND ?7 BETWEEN iso_min AND iso_max"
       "        AND ?8 BETWEEN exposure_min AND exposure_max"
       "        AND ?9 BETWEEN aperture_min AND aperture_max"
       "        AND ?10 BETWEEN focal_length_min AND focal_length_max"
       "        AND (format = 0 OR (format&?11 != 0 AND ~format&?12 != 0))))"
       " ORDER BY writeprotect %s, LOWER(name), rowid",
       default_first ? "DESC":"ASC");
       
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, op, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, image->exif_iso);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, image->exif_exposure);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, image->exif_aperture);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, image->exif_focal_length);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, iformat);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 12, excluded);
  }
  else
  {
    // don't know for which image. show all we got:
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT name, op_params, writeprotect, "
                                "  description, blendop_params, op_version, enabled"
                                " FROM data.presets"
                                " WHERE operation=?1"
                                " ORDER BY writeprotect DESC, LOWER(name), rowid",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, op, -1, SQLITE_TRANSIENT);
  }
  // collect all presets for op from db
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    void *op_params = (void *)sqlite3_column_blob(stmt, 1);
    int32_t op_params_size = sqlite3_column_bytes(stmt, 1);
    void *blendop_params = (void *)sqlite3_column_blob(stmt, 4);
    int32_t bl_params_size = sqlite3_column_bytes(stmt, 4);
    int32_t preset_version = sqlite3_column_int(stmt, 5);
    int32_t enabled = sqlite3_column_int(stmt, 6);
    int32_t isdefault = 0;
    int32_t isdisabled = (preset_version == version ? 0 : 1);
    const char *name = (char *)sqlite3_column_text(stmt, 0);

    if(module && !memcmp(module->default_params, op_params, MIN(op_params_size, module->params_size))
       && !memcmp(module->default_blendop_params, blendop_params,
                  MIN(bl_params_size, sizeof(dt_develop_blend_params_t))))
      isdefault = 1;

    if(module && !memcmp(params, op_params, MIN(op_params_size, params_size))
       && !memcmp(bl_params, blendop_params, MIN(bl_params_size, sizeof(dt_develop_blend_params_t)))
       && module->enabled == enabled)
    {
      char *markup;
      mi = gtk_menu_item_new_with_label("");
      
      if(isdefault)
        markup = g_markup_printf_escaped("<span weight=\"bold\">%s %s</span>", name, _("(default)"));
      else
        markup = g_markup_printf_escaped("<span weight=\"bold\">%s</span>", name);

      gtk_label_set_markup(GTK_LABEL(gtk_bin_get_child(GTK_BIN(mi))), markup);
      g_free(markup);
    }
    else
    {
      if(isdefault)
      {
        char *markup;
        mi = gtk_menu_item_new_with_label("");
        markup = g_markup_printf_escaped("%s %s", name, _("(default)"));
        gtk_label_set_markup(GTK_LABEL(gtk_bin_get_child(GTK_BIN(mi))), markup);
        g_free(markup);
      }
      else
        mi = gtk_menu_item_new_with_label((const char *)name);
    }

    if(isdisabled)
    {
      gtk_widget_set_sensitive(mi, 0);
      gtk_widget_set_tooltip_text(mi, _("disabled: wrong module version"));
    }
    else
    {
      g_object_set_data_full(G_OBJECT(mi), "dt-preset-name", g_strdup(name), g_free);

      if(module)
        g_signal_connect(G_OBJECT(mi), "button-release-event", G_CALLBACK(menuitem_button_released_preset), module);
      else if(pick_callback)
        g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(pick_callback), callback_data);

      gtk_widget_set_tooltip_text(mi, (const char *)sqlite3_column_text(stmt, 3));
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    cnt++;
  }

  sqlite3_finalize(stmt);

  if(cnt > 0)
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

void dt_gui_presets_popup_menu_show_for_module(dt_iop_module_t *module)
{
  dt_gui_presets_popup_menu_show_internal(module->op, module->version(), module->params, module->params_size,
                                          module->blend_params, module, &module->dev->image_storage, NULL,
                                          NULL);
}

void dt_gui_presets_update_mml(const char *name, dt_dev_operation_t op, const int32_t version,
                               const char *maker, const char *model, const char *lens)
{
  char tmp[1024];
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets"
      " SET maker=?1, model=?2, lens=?3"
      " WHERE operation=?4 AND op_version=?5 AND name=?6", -1,
      &stmt, NULL);
  snprintf(tmp, sizeof(tmp), "%%%s%%", maker);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, tmp, -1, SQLITE_TRANSIENT);
  snprintf(tmp, sizeof(tmp), "%%%s%%", model);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, tmp, -1, SQLITE_TRANSIENT);
  snprintf(tmp, sizeof(tmp), "%%%s%%", lens);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, tmp, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_iso(const char *name, dt_dev_operation_t op, const int32_t version,
                               const float min, const float max)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets"
      " SET iso_min=?1, iso_max=?2"
      " WHERE operation=?3 AND op_version=?4 AND name=?5", -1, &stmt,
      NULL);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, max);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_av(const char *name, dt_dev_operation_t op, const int32_t version, const float min,
                              const float max)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets"
      " SET aperture_min=?1, aperture_max=?2"
      " WHERE operation=?3 AND op_version=?4 AND name=?5",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, max);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_tv(const char *name, dt_dev_operation_t op, const int32_t version, const float min,
                              const float max)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets SET exposure_min=?1, exposure_max=?2 WHERE operation=?3 AND op_version=?4 AND name=?5",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, max);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_fl(const char *name, dt_dev_operation_t op, const int32_t version, const float min,
                              const float max)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET focal_length_min=?1, focal_length_max=?2"
                              " WHERE operation=?3 AND op_version=?4 AND name=?5",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, max);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_ldr(const char *name, dt_dev_operation_t op, const int32_t version,
                               const int ldrflag)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET format=?1"
                              " WHERE operation=?2 AND op_version=?3 AND name=?4",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, ldrflag);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_autoapply(const char *name, dt_dev_operation_t op, const int32_t version,
                                     const int autoapply)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets"
      " SET autoapply=?1"
      " WHERE operation=?2 AND op_version=?3 AND name=?4", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, autoapply);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_filter(const char *name, dt_dev_operation_t op, const int32_t version,
                                  const int filter)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET filter=?1"
                              " WHERE operation=?2 AND op_version=?3 AND name=?4",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filter);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
