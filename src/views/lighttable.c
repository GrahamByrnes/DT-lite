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
/** this is the view for the lighttable module.  */
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/grouping.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/settings.h"
#include "dtgtk/button.h"
#include "dtgtk/culling.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "views/view.h"
#include "views/view_api.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

DT_MODULE(1)

/**
 * this organises the whole library:
 * previously imported film rolls..
 */
typedef struct dt_library_t
{
  // culling and preview struct.
  dt_culling_t *culling;
  dt_culling_t *preview;

  dt_lighttable_layout_t current_layout;

  int preview_sticky;       // are we in sticky preview mode
  gboolean preview_state;   // are we in preview mode (always combined with another layout)
  gboolean already_started; // is it the first start of lighttable. Used by culling
  int thumbtable_offset;    // last thumbtable offset before entering culling

  GtkWidget *profile_floating_window;
} dt_library_t;

const char *name(const dt_view_t *self)
{
  return _("lighttable");
}


uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

// exit the full preview mode
static void _preview_quit(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  gtk_widget_hide(lib->preview->widget);
  if(lib->preview->selection_sync)
  {
    dt_selection_select_single(darktable.selection, lib->preview->offset_imgid);
  }
  lib->preview_state = FALSE;
  // restore panels
  dt_ui_restore_panels(darktable.gui->ui);

  // show/hide filmstrip & timeline when entering the view
  if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    // update thumbtable, to indicate if we navigate inside selection or not
    // this is needed as collection change is handle there
    dt_ui_thumbtable(darktable.gui->ui)->navigate_inside_selection = lib->culling->navigate_inside_selection;

    dt_lib_set_visible(darktable.view_manager->proxy.timeline.module, FALSE); // not available in this layouts
    dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module,
                       TRUE); // always on, visibility is driven by panel state

    dt_culling_update_active_images_list(lib->culling);
  }
  else
  {
    dt_ui_thumbtable(darktable.gui->ui)->navigate_inside_selection = FALSE;
    dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module, FALSE); // not available in this layouts
    dt_lib_set_visible(darktable.view_manager->proxy.timeline.module,
                       TRUE); // always on, visibility is driven by panel state

    // set offset back
    dt_thumbtable_set_offset(dt_ui_thumbtable(darktable.gui->ui), lib->thumbtable_offset, TRUE);

    // we need to show thumbtable
    if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
      dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), dt_ui_center_base(darktable.gui->ui),
                               DT_THUMBTABLE_MODE_FILEMANAGER);
    }
    else if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
    {
      dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), dt_ui_center_base(darktable.gui->ui),
                               DT_THUMBTABLE_MODE_ZOOM);
    }
    gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
    dt_thumbtable_full_redraw(dt_ui_thumbtable(darktable.gui->ui), TRUE);
  }
}

// check if we need to change the layout, and apply the change if needed
static void _lighttable_check_layout(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = dt_view_lighttable_get_layout(darktable.view_manager);
  const dt_lighttable_layout_t layout_old = lib->current_layout;

  if(lib->current_layout == layout) return;

  // if we are in full preview mode, we first need to exit this mode
  if(lib->preview_state) _preview_quit(self);

  lib->current_layout = layout;

  // layout has changed, let restore panels
  dt_ui_restore_panels(darktable.gui->ui);

  if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER || layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
  {
    dt_ui_thumbtable(darktable.gui->ui)->navigate_inside_selection = FALSE;
    gtk_widget_hide(lib->preview->widget);
    gtk_widget_hide(lib->culling->widget);

    // if we arrive from culling, we just need to ensure the offset is right
    if(layout_old == DT_LIGHTTABLE_LAYOUT_CULLING)
    {
      dt_thumbtable_set_offset(dt_ui_thumbtable(darktable.gui->ui), lib->thumbtable_offset, FALSE);
    }
    // we want to reacquire the thumbtable if needed
    if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
      dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), dt_ui_center_base(darktable.gui->ui),
                               DT_THUMBTABLE_MODE_FILEMANAGER);
    }
    else
    {
      dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), dt_ui_center_base(darktable.gui->ui),
                               DT_THUMBTABLE_MODE_ZOOM);
    }
    dt_thumbtable_full_redraw(dt_ui_thumbtable(darktable.gui->ui), TRUE);
    gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
  }
  else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    // record thumbtable offset
    lib->thumbtable_offset = dt_thumbtable_get_offset(dt_ui_thumbtable(darktable.gui->ui));

    if(!lib->already_started)
    {
      int id = lib->thumbtable_offset;
      sqlite3_stmt *stmt;
      gchar *query = dt_util_dstrcat(NULL, "SELECT rowid FROM memory.collected_images WHERE imgid=%d",
                                     dt_conf_get_int("plugins/lighttable/culling_last_id"));
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        id = sqlite3_column_int(stmt, 0);
      }
      g_free(query);
      sqlite3_finalize(stmt);

      dt_culling_init(lib->culling, id);
    }
    else
      dt_culling_init(lib->culling, -1);


    // ensure that thumbtable is not visible in the main view
    gtk_widget_hide(dt_ui_thumbtable(darktable.gui->ui)->widget);
    gtk_widget_hide(lib->preview->widget);
    gtk_widget_show(lib->culling->widget);

    dt_ui_thumbtable(darktable.gui->ui)->navigate_inside_selection = lib->culling->navigate_inside_selection;
  }

  lib->already_started = TRUE;

  if(layout == DT_LIGHTTABLE_LAYOUT_CULLING || lib->preview_state)
  {
    dt_lib_set_visible(darktable.view_manager->proxy.timeline.module, FALSE); // not available in this layouts
    dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module,
                       TRUE); // always on, visibility is driven by panel state
    dt_ui_scrollbars_show(darktable.gui->ui, FALSE);
    dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), lib->culling->offset_imgid, TRUE);
    dt_culling_update_active_images_list(lib->culling);
  }
  else
  {
    dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module, FALSE); // not available in this layouts
    dt_lib_set_visible(darktable.view_manager->proxy.timeline.module,
                       TRUE); // always on, visibility is driven by panel state
  }
}

static void _lighttable_change_offset(dt_view_t *self, gboolean reset, gint imgid)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  // full_preview change
  if(lib->preview_state)
    // we only do the change if the offset is different
    if(lib->culling->offset_imgid != imgid) dt_culling_change_offset_image(lib->preview, imgid);
  // culling change (note that full_preview can be combined with culling)
  if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    dt_culling_change_offset_image(lib->culling, imgid);
}

static void _culling_reinit(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  dt_culling_init(lib->culling, lib->culling->offset);
}

static void _culling_preview_reload_overlays(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  // change overlays if needed for culling and preview
  gchar *otxt = dt_util_dstrcat(NULL, "plugins/lighttable/overlays/culling/%d", DT_CULLING_MODE_CULLING);
  dt_thumbnail_overlay_t over = dt_conf_get_int(otxt);
  dt_culling_set_overlays_mode(lib->culling, over);
  g_free(otxt);
  otxt = dt_util_dstrcat(NULL, "plugins/lighttable/overlays/culling/%d", DT_CULLING_MODE_PREVIEW);
  over = dt_conf_get_int(otxt);
  dt_culling_set_overlays_mode(lib->preview, over);
  g_free(otxt);
}

static void _culling_preview_refresh(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  // change overlays if needed for culling and preview
  _culling_preview_reload_overlays(self);
  // full_preview change
  if(lib->preview_state)
    dt_culling_full_redraw(lib->preview, TRUE);
  // culling change (note that full_preview can be combined with culling)
  if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    dt_culling_full_redraw(lib->culling, TRUE);
}

static gboolean _preview_get_state(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  return lib->preview_state;
}

void init(dt_view_t *self)
{
  self->data = calloc(1, sizeof(dt_library_t));
  darktable.view_manager->proxy.lighttable.get_preview_state = _preview_get_state;
  darktable.view_manager->proxy.lighttable.view = self;
  darktable.view_manager->proxy.lighttable.change_offset = _lighttable_change_offset;
  darktable.view_manager->proxy.lighttable.culling_init_mode = _culling_reinit;
  darktable.view_manager->proxy.lighttable.culling_preview_refresh = _culling_preview_refresh;
  darktable.view_manager->proxy.lighttable.culling_preview_reload_overlays = _culling_preview_reload_overlays;
  // ensure the memory table is up to date
  dt_collection_memory_update();
}


void cleanup(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  free(lib->culling);
  free(lib->preview);
  free(self->data);
}

// display help text in the center view if there's no image to show
static int _lighttable_expose_empty(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                                    int32_t pointery)
{
  const float fs = DT_PIXEL_APPLY_DPI(15.0f);
  const float ls = 1.5f * fs;
  const float offy = height * 0.2f;
  const float offx = DT_PIXEL_APPLY_DPI(60);
  const float at = 0.3f;
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_BG);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  PangoLayout *layout;
  PangoRectangle ink;
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  pango_font_description_set_absolute_size(desc, fs * PANGO_SCALE);
  layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  cairo_set_font_size(cr, fs);
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_FONT);
  pango_layout_set_text(layout, _("there are no images in this collection"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, offx, offy - ink.height - ink.x);
  pango_cairo_show_layout(cr, layout);
  pango_layout_set_text(layout, _("if you have not imported any images yet"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, offx, offy + 2 * ls - ink.height - ink.x);
  pango_cairo_show_layout(cr, layout);
  pango_layout_set_text(layout, _("you can do so in the import module"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, offx, offy + 3 * ls - ink.height - ink.x);
  pango_cairo_show_layout(cr, layout);
  cairo_move_to(cr, offx - DT_PIXEL_APPLY_DPI(10.0f), offy + 3 * ls - ls * .25f);
  cairo_line_to(cr, 0.0f, 10.0f);
  dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_LIGHTTABLE_FONT, at);
  cairo_stroke(cr);
  pango_layout_set_text(layout, _("try to relax the filter settings in the top panel"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, offx, offy + 5 * ls - ink.height - ink.x);
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_FONT);
  pango_cairo_show_layout(cr, layout);
  cairo_rel_move_to(cr, 10.0f + ink.width, ink.height * 0.5f);
  cairo_line_to(cr, width * 0.5f, 0.0f);
  dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_LIGHTTABLE_FONT, at);
  cairo_stroke(cr);
  pango_layout_set_text(layout, _("or add images in the collection module in the left panel"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, offx, offy + 6 * ls - ink.height - ink.x);
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_FONT);
  pango_cairo_show_layout(cr, layout);
  cairo_move_to(cr, offx - DT_PIXEL_APPLY_DPI(10.0f), offy + 6 * ls - ls * 0.25f);
  cairo_rel_line_to(cr, -offx + 10.0f, 0.0f);
  dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_LIGHTTABLE_FONT, at);
  cairo_stroke(cr);

  pango_font_description_free(desc);
  g_object_unref(layout);
  return 0;
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const double start = dt_get_wtime();
  const dt_lighttable_layout_t layout = dt_view_lighttable_get_layout(darktable.view_manager);
  // Let's show full preview if in that state...
  _lighttable_check_layout(self);

  if(!darktable.collection || darktable.collection->count <= 0)
  {
    if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER || layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
      gtk_widget_hide(dt_ui_thumbtable(darktable.gui->ui)->widget);
    _lighttable_expose_empty(self, cr, width, height, pointerx, pointery);
  }
  else if(lib->preview_state)
  {
    if(!gtk_widget_get_visible(lib->preview->widget)) gtk_widget_show(lib->preview->widget);
    gtk_widget_hide(lib->culling->widget);
  }
  else // we do pass on expose to manager or zoomable
  {
    switch(layout)
    {
      case DT_LIGHTTABLE_LAYOUT_ZOOMABLE:
      case DT_LIGHTTABLE_LAYOUT_FILEMANAGER:
        if(!gtk_widget_get_visible(dt_ui_thumbtable(darktable.gui->ui)->widget))
          gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);

        break;
      case DT_LIGHTTABLE_LAYOUT_CULLING:
        if(!gtk_widget_get_visible(lib->culling->widget))
          gtk_widget_show(lib->culling->widget);

        gtk_widget_hide(lib->preview->widget);
        break;
      case DT_LIGHTTABLE_LAYOUT_FIRST:
      case DT_LIGHTTABLE_LAYOUT_LAST:
        break;
    }
  }

  // we have started the first expose
  lib->already_started = TRUE;
  const double end = dt_get_wtime();

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_LIGHTTABLE, "[lighttable] expose took %0.04f sec\n", end - start);
}

void enter(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = dt_view_lighttable_get_layout(darktable.view_manager);
  // we want to reacquire the thumbtable if needed
  if(!lib->preview_state)
  {
    if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
      dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), dt_ui_center_base(darktable.gui->ui),
                               DT_THUMBTABLE_MODE_FILEMANAGER);
      gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
    }
    else if(layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
    {
      dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), dt_ui_center_base(darktable.gui->ui),
                               DT_THUMBTABLE_MODE_ZOOM);
      gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
    }
  }

  // clean the undo list
  dt_undo_clear(darktable.undo, DT_UNDO_LIGHTTABLE);
  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
  dt_collection_hint_message(darktable.collection);
  // show/hide filmstrip & timeline when entering the view
  if(layout == DT_LIGHTTABLE_LAYOUT_CULLING || lib->preview_state)
  {
    dt_lib_set_visible(darktable.view_manager->proxy.timeline.module, FALSE); // not available in this layouts
    dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module,
                       TRUE); // always on, visibility is driven by panel state

    if(lib->preview_state)
      dt_culling_update_active_images_list(lib->preview);
    else
      dt_culling_update_active_images_list(lib->culling);
  }
  else
  {
    dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module, FALSE); // not available in this layouts
    dt_lib_set_visible(darktable.view_manager->proxy.timeline.module,
                       TRUE); // always on, visibility is driven by panel state
  }
  // restore panels
  dt_ui_restore_panels(darktable.gui->ui);
}

void leave(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  // ensure we have no active image remaining
  if(darktable.view_manager->active_images)
  {
    g_slist_free(darktable.view_manager->active_images);
    darktable.view_manager->active_images = NULL;
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
  }
  // we hide culling and preview too
  gtk_widget_hide(lib->culling->widget);
  gtk_widget_hide(lib->preview->widget);
  // exit preview mode if non-sticky
  if(lib->preview_state && lib->preview_sticky == 0)
    _preview_quit(self);
  // we remove the thumbtable from main view
  dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), NULL, DT_THUMBTABLE_MODE_FILMSTRIP);
  dt_ui_scrollbars_show(darktable.gui->ui, FALSE);
}

void reset(dt_view_t *self)
{
  dt_control_set_mouse_over_id(-1);
}


void scrollbar_changed(dt_view_t *self, double x, double y)
{
  const dt_lighttable_layout_t layout = dt_view_lighttable_get_layout(darktable.view_manager);

  switch(layout)
  {
    case DT_LIGHTTABLE_LAYOUT_FILEMANAGER:
    case DT_LIGHTTABLE_LAYOUT_ZOOMABLE:
    {
      dt_thumbtable_scrollbar_changed(dt_ui_thumbtable(darktable.gui->ui), x, y);
      break;
    }
    default:
      break;
  }
}

int key_released(dt_view_t *self, guint key, guint state)
{
  return 1;
}

int key_pressed(dt_view_t *self, guint key, guint state)
{
  return 0;
}

void init_key_accels(dt_view_t *self)
{
  // movement keys
  dt_accel_register_view(self, NC_("accel", "move page up"), GDK_KEY_Page_Up, 0);
  dt_accel_register_view(self, NC_("accel", "move page down"), GDK_KEY_Page_Down, 0);
  dt_accel_register_view(self, NC_("accel", "move up"), GDK_KEY_Up, 0);
  dt_accel_register_view(self, NC_("accel", "move down"), GDK_KEY_Down, 0);
  dt_accel_register_view(self, NC_("accel", "move left"), GDK_KEY_Left, 0);
  dt_accel_register_view(self, NC_("accel", "move right"), GDK_KEY_Right, 0);
  dt_accel_register_view(self, NC_("accel", "move start"), GDK_KEY_Home, 0);
  dt_accel_register_view(self, NC_("accel", "move end"), GDK_KEY_End, 0);

  // movement keys with selection
  dt_accel_register_view(self, NC_("accel", "move page up and select"), GDK_KEY_Page_Up, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "move page down and select"), GDK_KEY_Page_Down, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "move up and select"), GDK_KEY_Up, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "move down and select"), GDK_KEY_Down, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "move left and select"), GDK_KEY_Left, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "move right and select"), GDK_KEY_Right, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "move start and select"), GDK_KEY_Home, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "move end and select"), GDK_KEY_End, GDK_SHIFT_MASK);

  dt_accel_register_view(self, NC_("accel", "align images to grid"), 0, 0);
  dt_accel_register_view(self, NC_("accel", "reset first image offset"), 0, 0);
  dt_accel_register_view(self, NC_("accel", "select toggle image"), GDK_KEY_space, 0);
  dt_accel_register_view(self, NC_("accel", "select single image"), GDK_KEY_Return, 0);

  // Preview key
  dt_accel_register_view(self, NC_("accel", "preview"), GDK_KEY_w, 0);
  dt_accel_register_view(self, NC_("accel", "preview with focus detection"), GDK_KEY_w, GDK_CONTROL_MASK);
  dt_accel_register_view(self, NC_("accel", "sticky preview"), GDK_KEY_w, GDK_MOD1_MASK);
  dt_accel_register_view(self, NC_("accel", "sticky preview with focus detection"), GDK_KEY_w,
                         GDK_MOD1_MASK | GDK_CONTROL_MASK);

  // undo/redo
  dt_accel_register_view(self, NC_("accel", "undo"), GDK_KEY_z, GDK_CONTROL_MASK);
  dt_accel_register_view(self, NC_("accel", "redo"), GDK_KEY_y, GDK_CONTROL_MASK);

  // zoom for full culling & preview
  dt_accel_register_view(self, NC_("accel", "preview zoom 100%"), 0, 0);
  dt_accel_register_view(self, NC_("accel", "preview zoom fit"), 0, 0);

  // zoom in/out/min/max
  dt_accel_register_view(self, NC_("accel", "zoom in"), GDK_KEY_plus, GDK_CONTROL_MASK);
  dt_accel_register_view(self, NC_("accel", "zoom max"), GDK_KEY_plus, GDK_MOD1_MASK);
  dt_accel_register_view(self, NC_("accel", "zoom out"), GDK_KEY_minus, GDK_CONTROL_MASK);
  dt_accel_register_view(self, NC_("accel", "zoom min"), GDK_KEY_minus, GDK_MOD1_MASK);
}

GSList *mouse_actions(const dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  GSList *lm = NULL;
  dt_mouse_action_t *a = NULL;

  a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->action = DT_MOUSE_ACTION_DOUBLE_LEFT;
  g_strlcpy(a->name, _("open image in darkroom"), sizeof(a->name));
  lm = g_slist_append(lm, a);

  if(lib->preview_state)
  {
    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("switch to next/previous image"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->key.accel_mods = GDK_CONTROL_MASK;
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("zoom in the image"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->action = DT_MOUSE_ACTION_MIDDLE;
    /* xgettext:no-c-format */
    g_strlcpy(a->name, _("zoom to 100% and back"), sizeof(a->name));
    lm = g_slist_append(lm, a);
  }
  else if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
  {
    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("scroll the collection"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->key.accel_mods = GDK_CONTROL_MASK;
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("change number of images per row"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    if(darktable.collection->params.sort == DT_COLLECTION_SORT_CUSTOM_ORDER)
    {
      a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
      a->key.accel_mods = GDK_BUTTON1_MASK;
      a->action = DT_MOUSE_ACTION_DRAG_DROP;
      g_strlcpy(a->name, _("change image order"), sizeof(a->name));
      lm = g_slist_append(lm, a);
    }
  }
  else if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("scroll the collection"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->key.accel_mods = GDK_CONTROL_MASK;
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("zoom all the images"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->action = DT_MOUSE_ACTION_LEFT_DRAG;
    g_strlcpy(a->name, _("pan inside all the images"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->key.accel_mods = GDK_CONTROL_MASK | GDK_SHIFT_MASK;
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("zoom current image"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->key.accel_mods = GDK_SHIFT_MASK;
    a->action = DT_MOUSE_ACTION_LEFT_DRAG;
    g_strlcpy(a->name, _("pan inside current image"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->action = DT_MOUSE_ACTION_MIDDLE;
    /* xgettext:no-c-format */
    g_strlcpy(a->name, _("zoom to 100% and back"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->key.accel_mods = GDK_SHIFT_MASK;
    a->action = DT_MOUSE_ACTION_MIDDLE;
    /* xgettext:no-c-format */
    g_strlcpy(a->name, _("zoom current image to 100% and back"), sizeof(a->name));
    lm = g_slist_append(lm, a);
  }
  else if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
  {
    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("zoom the main view"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->action = DT_MOUSE_ACTION_LEFT_DRAG;
    g_strlcpy(a->name, _("pan inside the main view"), sizeof(a->name));
    lm = g_slist_append(lm, a);
  }

  return lm;
}

static void _profile_display_intent_callback(GtkWidget *combo, gpointer user_data)
{
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
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    dt_control_queue_redraw_center();
  }
}

static void _profile_display2_intent_callback(GtkWidget *combo, gpointer user_data)
{
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
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display2_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    dt_control_queue_redraw_center();
  }
}

static void _profile_display_profile_callback(GtkWidget *combo, gpointer user_data)
{
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
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            DT_COLORSPACES_PROFILE_TYPE_DISPLAY);
    dt_control_queue_redraw_center();
  }
}

static void _profile_display2_profile_callback(GtkWidget *combo, gpointer user_data)
{
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
    dt_control_queue_redraw_center();
  }
}

static void _profile_update_display_cmb(GtkWidget *cmb_display_profile)
{
  GList *l = darktable.color_profiles->profiles;
  while(l)
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->display_pos > -1)
    {
      if(prof->type == darktable.color_profiles->display_type
         && (prof->type != DT_COLORSPACE_FILE
             || !strcmp(prof->filename, darktable.color_profiles->display_filename)))
        if(dt_bauhaus_combobox_get(cmb_display_profile) != prof->display_pos)
        {
          dt_bauhaus_combobox_set(cmb_display_profile, prof->display_pos);
          break;
        }
    }
    l = g_list_next(l);
  }
}

static void _profile_update_display2_cmb(GtkWidget *cmb_display_profile)
{
  GList *l = darktable.color_profiles->profiles;
  while(l)
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->display2_pos > -1)
    {
      if(prof->type == darktable.color_profiles->display2_type
         && (prof->type != DT_COLORSPACE_FILE
             || !strcmp(prof->filename, darktable.color_profiles->display2_filename)))
        if(dt_bauhaus_combobox_get(cmb_display_profile) != prof->display2_pos)
        {
          dt_bauhaus_combobox_set(cmb_display_profile, prof->display2_pos);
          break;
        }
    }
    l = g_list_next(l);
  }
}

static void _profile_display_changed(gpointer instance, uint8_t profile_type, gpointer user_data)
{
  GtkWidget *cmb_display_profile = GTK_WIDGET(user_data);
  _profile_update_display_cmb(cmb_display_profile);
}

static void _profile_display2_changed(gpointer instance, uint8_t profile_type, gpointer user_data)
{
  GtkWidget *cmb_display_profile = GTK_WIDGET(user_data);
  _profile_update_display2_cmb(cmb_display_profile);
}

void gui_init(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  lib->culling = dt_culling_new(DT_CULLING_MODE_CULLING);
  lib->preview = dt_culling_new(DT_CULLING_MODE_PREVIEW);
  // add culling and preview to the center widget
  gtk_overlay_add_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)), lib->culling->widget);
  gtk_overlay_add_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)), lib->preview->widget);
  gtk_overlay_reorder_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)),
                              gtk_widget_get_parent(dt_ui_log_msg(darktable.gui->ui)), -1);
  gtk_overlay_reorder_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)),
                              gtk_widget_get_parent(dt_ui_toast_msg(darktable.gui->ui)), -1);
  // create display profile button
  GtkWidget *const profile_button = dtgtk_button_new(dtgtk_cairo_paint_display, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_tooltip_text(profile_button, _("set display profile"));
  dt_view_manager_module_toolbox_add(darktable.view_manager, profile_button, DT_VIEW_LIGHTTABLE);
  // and the popup window
  lib->profile_floating_window = gtk_popover_new(profile_button);
  gtk_widget_set_size_request(GTK_WIDGET(lib->profile_floating_window), 350, -1);
  g_object_set(G_OBJECT(lib->profile_floating_window), "transitions-enabled", FALSE, NULL);
  g_signal_connect_swapped(G_OBJECT(profile_button), "button-press-event",
                           G_CALLBACK(gtk_widget_show_all), lib->profile_floating_window);
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(lib->profile_floating_window), vbox);

  /** let's fill the encapsulating widgets */
  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));

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

  GtkWidget *display_profile = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(display_profile, NULL, _("display profile"));
  gtk_box_pack_start(GTK_BOX(vbox), display_profile, TRUE, TRUE, 0);

  GtkWidget *display2_profile = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(display2_profile, NULL, _("preview display profile"));
  gtk_box_pack_start(GTK_BOX(vbox), display2_profile, TRUE, TRUE, 0);

  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)profiles->data;

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
  }

  char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
  char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);
  char *tooltip = g_strdup_printf(_("display ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
  gtk_widget_set_tooltip_text(display_profile, tooltip);
  g_free(tooltip);
  tooltip = g_strdup_printf(_("preview display ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
  gtk_widget_set_tooltip_text(display2_profile, tooltip);
  g_free(tooltip);
  g_free(system_profile_dir);
  g_free(user_profile_dir);

  g_signal_connect(G_OBJECT(display_intent), "value-changed", G_CALLBACK(_profile_display_intent_callback), NULL);
  g_signal_connect(G_OBJECT(display_profile), "value-changed", G_CALLBACK(_profile_display_profile_callback), NULL);

  g_signal_connect(G_OBJECT(display2_intent), "value-changed", G_CALLBACK(_profile_display2_intent_callback), NULL);
  g_signal_connect(G_OBJECT(display2_profile), "value-changed", G_CALLBACK(_profile_display2_profile_callback),
                   NULL);

  // update the gui when profiles change
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            G_CALLBACK(_profile_display_changed), (gpointer)display_profile);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            G_CALLBACK(_profile_display2_changed), (gpointer)display2_profile);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
