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
#include "common/colorlabels.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/undo.h"
#include "common/grouping.h"
#include "control/conf.h"
#include "control/control.h"
#include "gui/gtk.h"
#include <gdk/gdkkeysyms.h>

const char *dt_colorlabels_name[] = {
  "red", "yellow", "green", "blue", "purple",
  NULL // termination
};

typedef struct dt_undo_colorlabels_t
{
  int imgid;
  uint8_t before;
  uint8_t after;
} dt_undo_colorlabels_t;

int dt_colorlabels_get_labels(const int imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT color FROM main.color_labels WHERE imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  int colors = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
    colors |= (1<<sqlite3_column_int(stmt, 0));
  sqlite3_finalize(stmt);
  return colors;
}

static void _pop_undo_execute(const int imgid, const uint8_t before, const uint8_t after)
{
  for(int color=0; color<5; color++)
  {
    if(after & (1<<color))
    {
      if (!(before & (1<<color)))
        dt_colorlabels_set_label(imgid, color);
    }
    else if (before & (1<<color))
      dt_colorlabels_remove_label(imgid, color);
  }
}

static void _pop_undo(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data, dt_undo_action_t action, GList **imgs)
{
  if(type == DT_UNDO_COLORLABELS)
  {
    GList *list = (GList *)data;

    while(list)
    {
      dt_undo_colorlabels_t *undocolorlabels = (dt_undo_colorlabels_t *)list->data;

      const uint8_t before = (action == DT_ACTION_UNDO) ? undocolorlabels->after : undocolorlabels->before;
      const uint8_t after = (action == DT_ACTION_UNDO) ? undocolorlabels->before : undocolorlabels->after;
      _pop_undo_execute(undocolorlabels->imgid, before, after);
      *imgs = g_list_prepend(*imgs, GINT_TO_POINTER(undocolorlabels->imgid));
      list = g_list_next(list);
    }
    dt_collection_hint_message(darktable.collection);
  }
}

static void _colorlabels_undo_data_free(gpointer data)
{
  GList *l = (GList *)data;
  g_list_free(l);
}

void dt_colorlabels_remove_labels(const int imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.color_labels WHERE imgid=?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_colorlabels_set_label(const int imgid, const int color)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO main.color_labels (imgid, color) VALUES (?1, ?2)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, color);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_colorlabels_remove_label(const int imgid, const int color)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.color_labels WHERE imgid=?1 AND color=?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, color);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

typedef enum dt_colorlabels_actions_t
{
  DT_CA_SET = 0,
  DT_CA_ADD,
  DT_CA_TOGGLE
} dt_colorlabels_actions_t;



static void _colorlabels_execute(const GList *imgs, const int labels, GList **undo, const gboolean undo_on, const int action)
{
  GList *images = (GList *)imgs;
  while(images)
  {
    const int image_id = GPOINTER_TO_INT(images->data);
    const uint8_t before = dt_colorlabels_get_labels(image_id);
    uint8_t after = 0;
    switch(action)
    {
      case DT_CA_SET:
        after = labels;
        break;
      case DT_CA_ADD:
        after = before | labels;
        break;
      case DT_CA_TOGGLE:
        after = (before & labels) ? before & (~labels) : before | labels;
        break;
      default:
        after = before;
        break;
    }

    if(undo_on)
    {
      dt_undo_colorlabels_t *undocolorlabels = (dt_undo_colorlabels_t *)malloc(sizeof(dt_undo_colorlabels_t));
      undocolorlabels->imgid = image_id;
      undocolorlabels->before = before;
      undocolorlabels->after = after;
      *undo = g_list_append(*undo, undocolorlabels);
    }

    _pop_undo_execute(image_id, before, after);

    images = g_list_next(images);
  }
}

void dt_colorlabels_set_labels(const GList *img, const int labels, const gboolean clear_on,
                               const gboolean undo_on)
{
  GList *imgs = g_list_copy((GList *)img);
  if(imgs)
  {
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_COLORLABELS);

    _colorlabels_execute(imgs, labels, &undo, undo_on, clear_on ? DT_CA_SET : DT_CA_ADD);

    g_list_free(imgs);
    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_COLORLABELS, undo, _pop_undo, _colorlabels_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }
    dt_collection_hint_message(darktable.collection);
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  }
}

void dt_colorlabels_toggle_label_on_list(const GList *list, const int color, const gboolean undo_on)
{
  const int label = 1<<color;
  GList *undo = NULL;
  if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_COLORLABELS);

  if(color == 5)
  {
    _colorlabels_execute(list, 0, &undo, undo_on, DT_CA_SET);
  }
  else
  {
    _colorlabels_execute(list, label, &undo, undo_on, DT_CA_TOGGLE);
  }

  // synchronise xmp files
  GList *l = (GList *)list;
  while(l)
  {
    dt_image_synch_xmp(GPOINTER_TO_INT(l->data));
    l = g_list_next(l);
  }

  if(undo_on)
  {
    dt_undo_record(darktable.undo, NULL, DT_UNDO_COLORLABELS, undo, _pop_undo, _colorlabels_undo_data_free);
    dt_undo_end_group(darktable.undo);
  }
  dt_collection_hint_message(darktable.collection);
}

int dt_colorlabels_check_label(const int imgid, const int color)
{
  if(imgid <= 0) return 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT * FROM main.color_labels WHERE imgid=?1 AND color=?2 LIMIT 1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, color);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    return 1;
  }
  else
  {
    sqlite3_finalize(stmt);
    return 0;
  }
}

// FIXME: XMP uses Red, Green, ... while we use red, green, ... What should this function return?
const char *dt_colorlabels_to_string(int label)
{
  if(label < 0 || label >= DT_COLORLABELS_LAST) return ""; // shouldn't happen
  return dt_colorlabels_name[label];
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
