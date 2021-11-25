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
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
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
  dt_lighttable_layout_t current_layout;

  int preview_sticky;       // are we in sticky preview mode
  gboolean preview_state;   // are we in preview mode (always combined with another layout)
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
  lib->preview_state = FALSE;
  // restore panels
  dt_ui_restore_panels(darktable.gui->ui);

  dt_ui_thumbtable(darktable.gui->ui)->navigate_inside_selection = FALSE;
  dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module, FALSE); // not available in this layouts
  dt_lib_set_visible(darktable.view_manager->proxy.timeline.module,
                     TRUE); // always on, visibility is driven by panel state

  dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), dt_ui_center_base(darktable.gui->ui),
                           DT_THUMBTABLE_MODE_FILEMANAGER);
  gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
  dt_thumbtable_full_redraw(dt_ui_thumbtable(darktable.gui->ui), TRUE);
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
  dt_collection_memory_update();
}

void cleanup(dt_view_t *self)
{
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
//  dt_library_t *lib = (dt_library_t *)self->data;
  const double start = dt_get_wtime();

  if(!darktable.collection || darktable.collection->count <= 0)
  {
    gtk_widget_hide(dt_ui_thumbtable(darktable.gui->ui)->widget);
    _lighttable_expose_empty(self, cr, width, height, pointerx, pointery);
  }
  else if(!gtk_widget_get_visible(dt_ui_thumbtable(darktable.gui->ui)->widget))
    gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);

  const double end = dt_get_wtime();

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_LIGHTTABLE, "[lighttable] expose took %0.04f sec\n", end - start);
}

void enter(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  // we want to reacquire the thumbtable if needed
  if(!lib->preview_state)
  {
    dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), dt_ui_center_base(darktable.gui->ui),
                             DT_THUMBTABLE_MODE_FILEMANAGER);
    gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
  }

  // clean the undo list
  dt_undo_clear(darktable.undo, DT_UNDO_LIGHTTABLE);
  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
  dt_collection_hint_message(darktable.collection);

  dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module, FALSE); // not available in this layouts
  dt_lib_set_visible(darktable.view_manager->proxy.timeline.module,
                     TRUE); // always on, visibility is driven by panel state
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

int key_released(dt_view_t *self, guint key, guint state)
{
  return 1;
}

int key_pressed(dt_view_t *self, guint key, guint state)
{
  return 0;
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
  for(const GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
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
  }
}

static void _profile_update_display2_cmb(GtkWidget *cmb_display_profile)
{
  for(const GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
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
