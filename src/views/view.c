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

#include "views/view.h"
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/module.h"
#include "common/selection.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "dtgtk/expander.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DECORATION_SIZE_LIMIT 40

static void dt_view_manager_load_modules(dt_view_manager_t *vm);
static int dt_view_load_module(void *v, const char *libname, const char *module_name);
static void dt_view_unload_module(dt_view_t *view);

void dt_view_manager_init(dt_view_manager_t *vm)
{
  /* prepare statements */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images "
                              "WHERE imgid = ?1", -1, &vm->statements.is_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.selected_images WHERE imgid = ?1",
                              -1, &vm->statements.delete_from_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT OR IGNORE INTO main.selected_images VALUES (?1)", -1,
                              &vm->statements.make_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT num FROM main.history WHERE imgid = ?1", -1,
                              &vm->statements.have_history, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT color FROM main.color_labels WHERE imgid=?1",
                              -1, &vm->statements.get_color, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "SELECT id FROM main.images WHERE group_id = (SELECT group_id FROM main.images WHERE id=?1) AND id != ?2",
      -1, &vm->statements.get_grouped, NULL);

  dt_view_manager_load_modules(vm);

  // Modules loaded, let's handle specific cases
  for(GList *iter = vm->views; iter; iter = g_list_next(iter))
  {
    dt_view_t *view = (dt_view_t *)iter->data;

    if(!strcmp(view->module_name, "darkroom"))
    {
      darktable.develop = (dt_develop_t *)view->data;
      break;
    }
  }
  vm->current_view = NULL;
}

void dt_view_manager_gui_init(dt_view_manager_t *vm)
{
  for(GList *iter = vm->views; iter; iter = g_list_next(iter))
  {
    dt_view_t *view = (dt_view_t *)iter->data;
    if(view->gui_init)
      view->gui_init(view);
  }
}

void dt_view_manager_cleanup(dt_view_manager_t *vm)
{
  for(GList *iter = vm->views; iter; iter = g_list_next(iter))
    dt_view_unload_module((dt_view_t *)iter->data);

  g_list_free_full(vm->views, free);
  vm->views = NULL;
}

const dt_view_t *dt_view_manager_get_current_view(dt_view_manager_t *vm)
{
  return vm->current_view;
}

// we want a stable order of views, for example for viewswitcher.
// anything not hardcoded will be put alphabetically wrt. localised names.
static gint sort_views(gconstpointer a, gconstpointer b)
{
  static const char *view_order[] = {"lighttable", "darkroom"};
  static const int n_view_order = G_N_ELEMENTS(view_order);

  dt_view_t *av = (dt_view_t *)a;
  dt_view_t *bv = (dt_view_t *)b;
  const char *aname = av->name(av);
  const char *bname = bv->name(bv);
  int apos = n_view_order;
  int bpos = n_view_order;

  for(int i = 0; i < n_view_order; i++)
  {
    if(!strcmp(av->module_name, view_order[i])) apos = i;
    if(!strcmp(bv->module_name, view_order[i])) bpos = i;
  }

  // order will be zero iff apos == bpos which can only happen when both views are not in view_order
  const int order = apos - bpos;
  return order ? order : strcmp(aname, bname);
}

static void dt_view_manager_load_modules(dt_view_manager_t *vm)
{
  vm->views = dt_module_load_modules("/views", sizeof(dt_view_t), dt_view_load_module, NULL, sort_views);
}

/* default flags for view which does not implement the flags() function */
static uint32_t default_flags()
{
  return 0;
}

/** load a view module */
static int dt_view_load_module(void *v, const char *libname, const char *module_name)
{
  dt_view_t *view = (dt_view_t *)v;
  view->data = NULL;
  view->vscroll_size = view->vscroll_viewport_size = 1.0;
  view->hscroll_size = view->hscroll_viewport_size = 1.0;
  view->vscroll_pos = view->hscroll_pos = 0.0;
  view->height = view->width = 100; // set to non-insane defaults before first expose/configure.
  g_strlcpy(view->module_name, module_name, sizeof(view->module_name));
  dt_print(DT_DEBUG_CONTROL, "[view_load_module] loading view `%s' from %s\n", module_name, libname);
  view->module = g_module_open(libname, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);

  if(!view->module)
  {
    fprintf(stderr, "[view_load_module] could not open %s (%s)!\n", libname, g_module_error());
    goto error;
  }
  
  int (*version)();

  if(!g_module_symbol(view->module, "dt_module_dt_version", (gpointer) & (version)))
    goto error;
    
  if(version() != dt_version())
  {
    fprintf(stderr, "[view_load_module] `%s' is compiled for another version of dt (module %d != dt %d) !\n",
            libname, version(), dt_version());
    goto error;
  }
  if(!g_module_symbol(view->module, "name", (gpointer) & (view->name)))
    view->name = NULL;
  if(!g_module_symbol(view->module, "view", (gpointer) & (view->view)))
    view->view = NULL;
  if(!g_module_symbol(view->module, "flags", (gpointer) & (view->flags)))
    view->flags = default_flags;
  if(!g_module_symbol(view->module, "init", (gpointer) & (view->init)))
    view->init = NULL;
  if(!g_module_symbol(view->module, "gui_init", (gpointer) & (view->gui_init)))
    view->gui_init = NULL;
  if(!g_module_symbol(view->module, "cleanup", (gpointer) & (view->cleanup)))
    view->cleanup = NULL;
  if(!g_module_symbol(view->module, "expose", (gpointer) & (view->expose)))
    view->expose = NULL;
  if(!g_module_symbol(view->module, "try_enter", (gpointer) & (view->try_enter)))
    view->try_enter = NULL;
  if(!g_module_symbol(view->module, "enter", (gpointer) & (view->enter)))
    view->enter = NULL;
  if(!g_module_symbol(view->module, "leave", (gpointer) & (view->leave)))
    view->leave = NULL;
  if(!g_module_symbol(view->module, "reset", (gpointer) & (view->reset)))
    view->reset = NULL;
  if(!g_module_symbol(view->module, "mouse_enter", (gpointer) & (view->mouse_enter)))
    view->mouse_enter = NULL;
  if(!g_module_symbol(view->module, "mouse_leave", (gpointer) & (view->mouse_leave)))
    view->mouse_leave = NULL;
  if(!g_module_symbol(view->module, "mouse_moved", (gpointer) & (view->mouse_moved)))
    view->mouse_moved = NULL;
  if(!g_module_symbol(view->module, "button_released", (gpointer) & (view->button_released)))
    view->button_released = NULL;
  if(!g_module_symbol(view->module, "button_pressed", (gpointer) & (view->button_pressed)))
    view->button_pressed = NULL;
  if(!g_module_symbol(view->module, "key_pressed", (gpointer) & (view->key_pressed)))
    view->key_pressed = NULL;
  if(!g_module_symbol(view->module, "key_released", (gpointer) & (view->key_released)))
    view->key_released = NULL;
  if(!g_module_symbol(view->module, "configure", (gpointer) & (view->configure)))
    view->configure = NULL;
  if(!g_module_symbol(view->module, "scrolled", (gpointer) & (view->scrolled)))
    view->scrolled = NULL;
  if(!g_module_symbol(view->module, "scrollbar_changed", (gpointer) & (view->scrollbar_changed)))
    view->scrollbar_changed = NULL;
  if(!g_module_symbol(view->module, "mouse_actions", (gpointer) & (view->mouse_actions)))
    view->mouse_actions = NULL;

  view->accel_closures = NULL;

  if(!strcmp(view->module_name, "darkroom"))
    darktable.develop = (dt_develop_t *)view->data;

  if(view->init)
    view->init(view);

  return 0;

error:
  if(view->module)
    g_module_close(view->module);
    
  return 1;
}

/** unload, cleanup */
static void dt_view_unload_module(dt_view_t *view)
{
  if(view->cleanup)
    view->cleanup(view);

  g_slist_free(view->accel_closures);

  if(view->module)
    g_module_close(view->module);
}

void dt_vm_remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove(GTK_CONTAINER(data), widget);
}

//When expanders get destroyed, they destroy the child
//so remove the child before that
static void _remove_child(GtkWidget *child,GtkContainer *container)
{
    if(DTGTK_IS_EXPANDER(child))
    {
      GtkWidget * evb = dtgtk_expander_get_body_event_box(DTGTK_EXPANDER(child));
      gtk_container_remove(GTK_CONTAINER(evb),dtgtk_expander_get_body(DTGTK_EXPANDER(child)));
      gtk_widget_destroy(child);
    }
    else
      gtk_container_remove(container,child);
}

int dt_view_manager_switch(dt_view_manager_t *vm, const char *view_name)
{
  gboolean switching_to_none = *view_name == '\0';
  dt_view_t *new_view = NULL;

  if(!switching_to_none)
  {
    for(GList *iter = vm->views; iter; iter = g_list_next(iter))
    {
      dt_view_t *v = (dt_view_t *)iter->data;
      if(!strcmp(v->module_name, view_name))
      {
        new_view = v;
        break;
      }
    }
    
    if(!new_view)
      return 1; // the requested view doesn't exist
  }

  return dt_view_manager_switch_by_view(vm, new_view);
}

int dt_view_manager_switch_by_view(dt_view_manager_t *vm, const dt_view_t *nv)
{
  dt_view_t *old_view = vm->current_view;
  dt_view_t *new_view = (dt_view_t *)nv; // views belong to us, we can de-const them :-)
  // Before switching views, restore accelerators if disabled
  if(!darktable.control->key_accelerators_on)
    dt_control_key_accelerators_on(darktable.control);
  // reset the cursor to the default one
  dt_control_change_cursor(GDK_LEFT_PTR);
  // also ignore what scrolling there was previously happening
  memset(darktable.gui->scroll_to, 0, sizeof(darktable.gui->scroll_to));
  // destroy old module list
  dt_undo_clear(darktable.undo, DT_UNDO_ALL);
  // Special case when entering nothing (just before leaving dt)
  if(!new_view)
  {
    if(old_view)
    {
      if(old_view->leave)
        old_view->leave(old_view);
      // iterator plugins and cleanup plugins in current view 
      for(GList *iter = darktable.lib->plugins; iter; iter = g_list_next(iter))
      {
        dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);
        // does this module belong to current view 
        if(dt_lib_is_visible_in_view(plugin, old_view))
        {
          if(plugin->view_leave)
            plugin->view_leave(plugin, old_view, NULL);

          plugin->gui_cleanup(plugin);
          plugin->data = NULL;
          dt_accel_disconnect_list(&plugin->accel_closures);
          plugin->widget = NULL;
        }
      }
    }
    // remove all widgets in all containers
    for(int l = 0; l < DT_UI_CONTAINER_SIZE; l++)
      dt_ui_container_destroy_children(darktable.gui->ui, l);

    vm->current_view = NULL;
    // remove sticky accels window
    if(vm->accels_window.window)
      dt_view_accels_hide(vm);

    return 0;
  }
  // invariant: new_view != NULL after this point
  assert(new_view != NULL);

  if(new_view->try_enter)
  {
    const int error = new_view->try_enter(new_view);
    if(error)
      return error;
  }
  // cleanup current view before initialization of new
  if(old_view)
  {
    if(old_view->leave)
      old_view->leave(old_view);

    dt_accel_disconnect_list(&old_view->accel_closures);
    // iterator plugins and cleanup plugins in current view
    for(GList *iter = darktable.lib->plugins; iter; iter = g_list_next(iter))
    {
      dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);
      // does this module belong to current view
      if(dt_lib_is_visible_in_view(plugin, old_view))
      {
        if(plugin->view_leave)
          plugin->view_leave(plugin, old_view, new_view);

        dt_accel_disconnect_list(&plugin->accel_closures);
      }
    }
    // remove all widets in all containers
    for(int l = 0; l < DT_UI_CONTAINER_SIZE; l++)
      dt_ui_container_foreach(darktable.gui->ui, l,(GtkCallback)_remove_child);
  }

  vm->current_view = new_view;
  dt_ui_restore_panels(darktable.gui->ui);
  // this has to be done in reverse order to have the lowest position at the bottom!
  for(GList *iter = g_list_last(darktable.lib->plugins); iter; iter = g_list_previous(iter))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);
    if(dt_lib_is_visible_in_view(plugin, new_view))
    {
      // try get the module expander
      GtkWidget *w = dt_lib_gui_get_expander(plugin);
      dt_lib_connect_common_accels(plugin);
      // if we didn't get an expander let's add the widget
      if(!w)
        w = plugin->widget;
      // add module to its container
      dt_ui_container_add_widget(darktable.gui->ui, plugin->container(plugin), w);
    }
  }
  for(GList *iter = darktable.lib->plugins; iter; iter = g_list_next(iter))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);

    if(dt_lib_is_visible_in_view(plugin, new_view))
    {
      // set expanded if last mode was that
      char var[1024];
      gboolean expanded = FALSE;
      gboolean visible = dt_lib_is_visible(plugin);

      if(plugin->expandable(plugin))
      {
        snprintf(var, sizeof(var), "plugins/%s/%s/expanded", new_view->module_name, plugin->plugin_name);
        expanded = dt_conf_get_bool(var);
        dt_lib_gui_set_expanded(plugin, expanded);
      }
      else
      {
        if(visible)
          gtk_widget_show_all(plugin->widget);
        else
          gtk_widget_hide(plugin->widget);
      }

      if(plugin->view_enter)
        plugin->view_enter(plugin, old_view, new_view);
    }
  }
  // enter view. crucially, do this before initing the plugins below
  if(new_view->enter)
    new_view->enter(new_view);

  dt_ui_update_scrollbars(darktable.gui->ui);

  if(vm->accels_window.window && vm->accels_window.sticky)
    dt_view_accels_refresh(vm);
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED, old_view, new_view);
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_LOG_REDRAW);
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_TOAST_REDRAW);
  return 0;
}

const char *dt_view_manager_name(dt_view_manager_t *vm)
{
  if(!vm->current_view)
    return "";
    
  if(vm->current_view->name)
    return vm->current_view->name(vm->current_view);
  else
    return vm->current_view->module_name;
}

void dt_view_manager_expose(dt_view_manager_t *vm, cairo_t *cr, int32_t width, int32_t height,
                            int32_t pointerx, int32_t pointery)
{
  if(!vm->current_view)
  {
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_BG);
    cairo_paint(cr);
    return;
  }

  vm->current_view->width = width;
  vm->current_view->height = height;

  if(vm->current_view->expose)
  {
    cairo_rectangle(cr, 0, 0, vm->current_view->width, vm->current_view->height);
    cairo_clip(cr);
    cairo_new_path(cr);
    cairo_save(cr);
    float px = pointerx, py = pointery;
    if(pointery > vm->current_view->height)
    {
      px = 10000.0;
      py = -1.0;
    }
    vm->current_view->expose(vm->current_view, cr, vm->current_view->width, vm->current_view->height, px, py);

    cairo_restore(cr);
    /* expose plugins */
    for(const GList *plugins = g_list_last(darktable.lib->plugins); plugins; plugins = g_list_previous(plugins))
    {
      dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

      if(plugin->gui_post_expose
         && dt_lib_is_visible_in_view(plugin, vm->current_view))
        plugin->gui_post_expose(plugin, cr, vm->current_view->width,
                                vm->current_view->height, px, py);
    }
  }
}

void dt_view_manager_reset(dt_view_manager_t *vm)
{
  if(!vm->current_view)
    return;
  if(vm->current_view->reset)
    vm->current_view->reset(vm->current_view);
}

void dt_view_manager_mouse_leave(dt_view_manager_t *vm)
{
  if(!vm->current_view)
    return;

  dt_view_t *v = vm->current_view;
  /* lets check if any plugins want to handle mouse move */
  gboolean handled = FALSE;
  GList *plugins = g_list_last(darktable.lib->plugins);
  while(plugins)
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);
    /* does this module belong to current view ?*/
    if(plugin->mouse_leave && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->mouse_leave(plugin)) handled = TRUE;
    /* get next plugin */
    plugins = g_list_previous(plugins);
  }
  /* if not handled by any plugin let pass to view handler*/
  if(!handled && v->mouse_leave)
    v->mouse_leave(v);
}

void dt_view_manager_mouse_enter(dt_view_manager_t *vm)
{
  if(!vm->current_view)
    return;
  if(vm->current_view->mouse_enter)
    vm->current_view->mouse_enter(vm->current_view);
}

void dt_view_manager_mouse_moved(dt_view_manager_t *vm, double x, double y, double pressure, int which)
{
  if(!vm->current_view) return;
  dt_view_t *v = vm->current_view;
  /* lets check if any plugins want to handle mouse move */
  gboolean handled = FALSE;
  for(const GList *plugins = g_list_last(darktable.lib->plugins);
      plugins; plugins = g_list_previous(plugins))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);
    // does this module belong to current view ?
    if(plugin->mouse_moved && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->mouse_moved(plugin, x, y, pressure, which))
        handled = TRUE;
  }
  // if not handled by any plugin let pass to view handler
  if(!handled && v->mouse_moved)
    v->mouse_moved(v, x, y, pressure, which);
}

int dt_view_manager_button_released(dt_view_manager_t *vm, double x, double y, int which, uint32_t state)
{
  if(!vm->current_view)
    return 0;

  dt_view_t *v = vm->current_view;
  /* lets check if any plugins want to handle button press */
  gboolean handled = FALSE;
  for(const GList *plugins = g_list_last(darktable.lib->plugins);
      plugins;
      plugins = g_list_previous(plugins))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);
    /* does this module belong to current view ?*/
    if(plugin->button_released && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->button_released(plugin, x, y, which, state)) handled = TRUE;                                     
  }

  if(handled)
    return 1;
  /* if not handled by any plugin let pass to view handler*/
  else if(v->button_released)
    v->button_released(v, x, y, which, state);

  return 0;
}

int dt_view_manager_button_pressed(dt_view_manager_t *vm, double x, double y, double pressure, int which,
                                   int type, uint32_t state)
{
  if(!vm->current_view)
    return 0;
    
  dt_view_t *v = vm->current_view;
  // lets check if any plugins want to handle button press 
  gboolean handled = FALSE;

  for(const GList *plugins = g_list_last(darktable.lib->plugins);
      plugins && !handled;
      plugins = g_list_previous(plugins))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);
    // does this module belong to current view ?
    if(plugin->button_pressed && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->button_pressed(plugin, x, y, pressure, which, type, state)) handled = TRUE;                                   
  }

  if(handled)
    return 1;
  /* if not handled by any plugin let pass to view handler*/
  else if(v->button_pressed)
    return v->button_pressed(v, x, y, pressure, which, type, state);

  return 0;
}

int dt_view_manager_key_pressed(dt_view_manager_t *vm, guint key, guint state)
{
  if(!vm->current_view)
    return 0;

  if(vm->current_view->key_pressed)
    return vm->current_view->key_pressed(vm->current_view, key, state);

  return 0;
}

int dt_view_manager_key_released(dt_view_manager_t *vm, guint key, guint state)
{
  if(!vm->current_view)
    return 0;
  if(vm->current_view->key_released)
    return vm->current_view->key_released(vm->current_view, key, state);

  return 0;
}

void dt_view_manager_configure(dt_view_manager_t *vm, int width, int height)
{
  for(GList *iter = vm->views; iter; iter = g_list_next(iter))
  {
    // this is necessary for all
    dt_view_t *v = (dt_view_t *)iter->data;
    v->width = width;
    v->height = height;

    if(v->configure)
      v->configure(v, width, height);
  }
}

void dt_view_manager_scrolled(dt_view_manager_t *vm, double x, double y, int up, int state)
{
  if(!vm->current_view)
    return;
  if(vm->current_view->scrolled)
    vm->current_view->scrolled(vm->current_view, x, y, up, state);
}

void dt_view_manager_scrollbar_changed(dt_view_manager_t *vm, double x, double y)
{
  if(!vm->current_view)
    return;

  if(vm->current_view->scrollbar_changed)
    vm->current_view->scrollbar_changed(vm->current_view, x, y);
}

void dt_view_set_scrollbar(dt_view_t *view, float hpos, float hlower, float hsize, float hwinsize, float vpos,
                           float vlower, float vsize, float vwinsize)
{
  if (view->vscroll_pos == vpos && view->vscroll_lower == vlower && view->vscroll_size == vsize &&
      view->vscroll_viewport_size == vwinsize && view->hscroll_pos == hpos && view->hscroll_lower == hlower &&
      view->hscroll_size == hsize && view->hscroll_viewport_size == hwinsize)
    return;

  view->vscroll_pos = vpos;
  view->vscroll_lower = vlower;
  view->vscroll_size = vsize;
  view->vscroll_viewport_size = vwinsize;
  view->hscroll_pos = hpos;
  view->hscroll_lower = hlower;
  view->hscroll_size = hsize;
  view->hscroll_viewport_size = hwinsize;

  GtkWidget *widget;
  widget = darktable.gui->widgets.left_border;
  gtk_widget_queue_draw(widget);
  widget = darktable.gui->widgets.right_border;
  gtk_widget_queue_draw(widget);
  widget = darktable.gui->widgets.bottom_border;
  gtk_widget_queue_draw(widget);
  widget = darktable.gui->widgets.top_border;
  gtk_widget_queue_draw(widget);

  if (!darktable.gui->scrollbars.dragging)
    dt_ui_update_scrollbars(darktable.gui->ui);
}

static int _images_to_act_on_find_custom(gconstpointer a, gconstpointer b)
{
  return (GPOINTER_TO_INT(a) != GPOINTER_TO_INT(b));
}

static void _images_to_act_on_insert_in_list(GList **list, const int imgid, gboolean only_visible)
{
  if(only_visible)
  {
    if(!g_list_find_custom(*list, GINT_TO_POINTER(imgid), _images_to_act_on_find_custom))
      *list = g_list_append(*list, GINT_TO_POINTER(imgid));
    return;
  }

  const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  if(image)
  {
    const int img_group_id = image->group_id;
    dt_image_cache_read_release(darktable.image_cache, image);

    if(!darktable.gui || !darktable.gui->grouping || darktable.gui->expanded_group_id == img_group_id
       || !dt_selection_get_collection(darktable.selection))
    {
      if(!g_list_find_custom(*list, GINT_TO_POINTER(imgid), _images_to_act_on_find_custom))
        *list = g_list_append(*list, GINT_TO_POINTER(imgid));
    }
    else
    {
      sqlite3_stmt *stmt;
      gchar *query = g_strdup_printf(
          NULL,
          "SELECT id"
          "  FROM main.images"
          "  WHERE group_id = %d AND id IN (%s)",
          img_group_id, dt_collection_get_query_no_group(dt_selection_get_collection(darktable.selection)));
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        const int imgidg = sqlite3_column_int(stmt, 0);
        if(!g_list_find_custom(*list, GINT_TO_POINTER(imgidg), _images_to_act_on_find_custom))
          *list = g_list_append(*list, GINT_TO_POINTER(imgidg));
      }
      sqlite3_finalize(stmt);
      g_free(query);
    }
  }
}

// get the list of images to act on during global changes (libs, accels)
const GList *dt_view_get_images_to_act_on(const gboolean only_visible, const gboolean force)
{
  /** Here's how it works
   *
   *             mouse over| x | x | x |   |   |
   *     mouse inside table| x | x |   |   |   |
   * mouse inside selection| x |   |   |   |   |
   *          active images| ? | ? | x |   | x |
   *                       |   |   |   |   |   |
   *                       | S | O | O | S | A |
   *  S = selection ; O = mouseover ; A = active images
   *  the mouse can be outside thumbtable in case of filmstrip + mouse in center widget
   *
   *  if only_visible is FALSE, then it will add also not visible images because of grouping
   **/
  const int mouseover = dt_control_get_mouse_over_id();

  // if possible, we return the cached list
  if(!force 
     && darktable.view_manager->act_on.ok
     && darktable.view_manager->act_on.image_over == mouseover
     && darktable.view_manager->act_on.inside_table == dt_ui_thumbtable(darktable.gui->ui)->mouse_inside
     && g_slist_length(darktable.view_manager->act_on.active_imgs)
            == g_slist_length(darktable.view_manager->active_images))
  {
    // we test active images if mouse outside table
    gboolean ok = TRUE;
    if(!dt_ui_thumbtable(darktable.gui->ui)->mouse_inside
       && g_slist_length(darktable.view_manager->act_on.active_imgs) > 0)
    {
      GSList *l1 = darktable.view_manager->act_on.active_imgs;
      GSList *l2 = darktable.view_manager->active_images;
      while(l1 && l2)
      {
        if(GPOINTER_TO_INT(l1->data) != GPOINTER_TO_INT(l2->data))
        {
          ok = FALSE;
          break;
        }
        l2 = g_slist_next(l2);
        l1 = g_slist_next(l1);
      }
    }

    if(ok)
      return darktable.view_manager->act_on.images;
  }

  GList *l = NULL;
  if(mouseover > 0)
  {
    // collumn 1,2,3
    if(dt_ui_thumbtable(darktable.gui->ui)->mouse_inside)
    {
      // column 1,2
      sqlite3_stmt *stmt;
      gboolean inside_sel = FALSE;
      gchar *query = g_strdup_printf("SELECT imgid FROM main.selected_images WHERE imgid =%d", mouseover);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      if(stmt != NULL && sqlite3_step(stmt) == SQLITE_ROW)
      {
        inside_sel = TRUE;
        sqlite3_finalize(stmt);
      }
      g_free(query);

      if(inside_sel)
      {
        // column 1
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT m.imgid FROM memory.collected_images as m, main.selected_images as s "
                                    "WHERE m.imgid=s.imgid "
                                    "ORDER BY m.rowid",
                                    -1, &stmt, NULL);
        while(stmt != NULL && sqlite3_step(stmt) == SQLITE_ROW)
          _images_to_act_on_insert_in_list(&l, sqlite3_column_int(stmt, 0), only_visible);

        if(stmt)
          sqlite3_finalize(stmt);
      }
      else
        // column 2
        _images_to_act_on_insert_in_list(&l, mouseover, only_visible);
    }
    else
      // column 3
      _images_to_act_on_insert_in_list(&l, mouseover, only_visible);
  }
  else
  {
    // column 4,5
    if(g_slist_length(darktable.view_manager->active_images) > 0)
    {
      // column 5
      GSList *ll = darktable.view_manager->active_images;
      while(ll)
      {
        const int id = GPOINTER_TO_INT(ll->data);
        _images_to_act_on_insert_in_list(&l, id, only_visible);
        ll = g_slist_next(ll);
      }
    }
    else
    {
      // column 4
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT m.imgid FROM memory.collected_images as m, main.selected_images as s "
                                  "WHERE m.imgid=s.imgid "
                                  "ORDER BY m.rowid",
                                  -1, &stmt, NULL);
      while(stmt != NULL && sqlite3_step(stmt) == SQLITE_ROW)
        _images_to_act_on_insert_in_list(&l, sqlite3_column_int(stmt, 0), only_visible);

      if(stmt)
        sqlite3_finalize(stmt);
    }
  }

  // let's register the new list as cached
  darktable.view_manager->act_on.image_over = mouseover;
  g_list_free(darktable.view_manager->act_on.images);
  darktable.view_manager->act_on.images = l;
  g_slist_free(darktable.view_manager->act_on.active_imgs);
  darktable.view_manager->act_on.active_imgs = g_slist_copy(darktable.view_manager->active_images);
  darktable.view_manager->act_on.inside_table = dt_ui_thumbtable(darktable.gui->ui)->mouse_inside;
  darktable.view_manager->act_on.ok = TRUE;
  return darktable.view_manager->act_on.images;
}

// get the main image to act on during global changes (libs, accels)
int dt_view_get_image_to_act_on()
{
  /** Here's how it works -- same as for list, except we don't care about mouse inside selection or table
   *
   *             mouse over| x |   |   |
   *          active images| ? |   | x |
   *                       |   |   |   |
   *                       | O | S | A |
   *  First image of ...
   *  S = selection ; O = mouseover ; A = active images
   **/

  int ret = -1;
  const int mouseover = dt_control_get_mouse_over_id();

  if(mouseover > 0)
    ret = mouseover;
  else
  {
    if(g_slist_length(darktable.view_manager->active_images) > 0)
      ret = GPOINTER_TO_INT(g_slist_nth_data(darktable.view_manager->active_images, 0));
    else
    {
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT s.imgid FROM main.selected_images as s, memory.collected_images as c "
                                  "WHERE s.imgid=c.imgid ORDER BY c.rowid LIMIT 1",
                                  -1, &stmt, NULL);
      if(stmt != NULL && sqlite3_step(stmt) == SQLITE_ROW)
        ret = sqlite3_column_int(stmt, 0);
      if(stmt)
        sqlite3_finalize(stmt);
    }
  }

  return ret;
}

int dt_view_image_get_surface(int imgid, int width, int height, cairo_surface_t **surface, const gboolean quality)
{
  if(*surface && cairo_surface_get_reference_count(*surface) > 0)
    cairo_surface_destroy(*surface);

  *surface = NULL;
  // get mipmap cahe image
  dt_mipmap_cache_t *cache = darktable.mipmap_cache;
  dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(cache, width * darktable.gui->ppd, height * darktable.gui->ppd);
  // if needed, we load the mimap buffer
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(cache, &buf, imgid, mip, DT_MIPMAP_BEST_EFFORT, 'r');
  const int buf_wd = buf.width;
  const int buf_ht = buf.height;

  if(!buf.buf)
  {
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    return 1;
  }
  // so we create a new image surface to return
  float scale = fminf(width / (float)buf_wd, height / (float)buf_ht) * darktable.gui->ppd_thb ;
  const int img_width = roundf(buf_wd * scale);
  const int img_height = roundf(buf_ht * scale);
  scale = fmaxf(img_width / (float)buf_wd, img_height / (float)buf_ht);                                                                 
  *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, img_width, img_height);

  // we transfer cached image on a cairo_surface (with colorspace transform if needed)
  cairo_surface_t *tmp_surface = NULL;
  uint8_t *rgbbuf = (uint8_t *)calloc(buf_wd * buf_ht * 4, sizeof(uint8_t));

  if(rgbbuf)
  {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(buf, rgbbuf)
#endif
    for(int i = 0; i < buf.height; i++)
    {
      const uint8_t *in = buf.buf + i * buf.width * 4;
      uint8_t *out = rgbbuf + i * buf.width * 4;
      for(int j = 0; j < buf.width; j++, in += 4, out += 4)
      {
        out[0] = in[2];
        out[1] = in[1];
        out[2] = in[0];
      }
    }

    const int32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, buf_wd);
    tmp_surface = cairo_image_surface_create_for_data(rgbbuf, CAIRO_FORMAT_RGB24, buf_wd, buf_ht, stride);
  }
  // draw the image scaled:
  if(tmp_surface)
  {
    cairo_t *cr = cairo_create(*surface);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, tmp_surface, 0, 0);
    // set filter to nearest:
    // for the right mip, we want to see exactly what the pipe gave us, 1:1 pixel for pixel.
    if((buf_wd <= 8 && buf_ht <= 8) || fabsf(scale - 1.0f) < 0.01f)
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    else
      cairo_pattern_set_filter(cairo_get_source(cr), ((darktable.gui->filter_image == CAIRO_FILTER_FAST) && quality)
        ? CAIRO_FILTER_GOOD : darktable.gui->filter_image) ;

    cairo_paint(cr);
    cairo_surface_destroy(tmp_surface);
    cairo_destroy(cr);
  }

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  if(rgbbuf) free(rgbbuf);

  return 0;
}

char* dt_view_extend_modes_str(const char * name, const gboolean is_hdr)
{
  char* upcase = g_ascii_strup(name, -1);  // extension in capital letters to avoid character descenders

  if(is_hdr)
  {
    gchar* fullname = g_strdup_printf("%s HDR", upcase);
    g_free(upcase);
    upcase = fullname;
  }

  return upcase;
}

/**
 * \brief Set the selection bit to a given value for the specified image
 * \param[in] imgid The image id
 * \param[in] value The boolean value for the bit
 */
void dt_view_set_selection(int imgid, int value)
{
  /* clear and reset statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.is_selected);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.is_selected);
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.is_selected, 1, imgid);

  if(sqlite3_step(darktable.view_manager->statements.is_selected) == SQLITE_ROW)
  {
    if(!value)
    {
      /* Value is set and should be unset; get rid of it */
      /* clear and reset statement */
      DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.delete_from_selected);
      DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.delete_from_selected);
      DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.delete_from_selected, 1, imgid);
      sqlite3_step(darktable.view_manager->statements.delete_from_selected);
    }
  }
  else if(value)
  {
    /* Select bit is unset and should be set; add it */
    /* clear and reset statement */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.make_selected);
    DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.make_selected);
    DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.make_selected, 1, imgid);
    sqlite3_step(darktable.view_manager->statements.make_selected);
  }
}

/**
 * \brief Toggle the selection bit in the database for the specified image
 * \param[in] imgid The image id
 */
void dt_view_toggle_selection(int imgid)
{
  /* clear and reset statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.is_selected);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.is_selected);
  /* setup statement and iterate over rows */
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.is_selected, 1, imgid);
  if(sqlite3_step(darktable.view_manager->statements.is_selected) == SQLITE_ROW)
  {
    /* clear and reset statement */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.delete_from_selected);
    DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.delete_from_selected);
    /* setup statement and execute */
    DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.delete_from_selected, 1, imgid);
    sqlite3_step(darktable.view_manager->statements.delete_from_selected);
  }
  else
  {
    /* clear and reset statement */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.make_selected);
    DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.make_selected);
    /* setup statement and execute */
    DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.make_selected, 1, imgid);
    sqlite3_step(darktable.view_manager->statements.make_selected);
  }
}

/**
 * \brief Reset filter
 */
void dt_view_filter_reset(const dt_view_manager_t *vm, gboolean smart_filter)
{
  if(vm->proxy.filter.module && vm->proxy.filter.reset_filter)
    vm->proxy.filter.reset_filter(vm->proxy.filter.module, smart_filter);
}

void dt_view_active_images_reset(gboolean raise)
{
  if(!darktable.view_manager->active_images)
    return;

  g_slist_free(darktable.view_manager->active_images);
  darktable.view_manager->active_images = NULL;

  if(raise)
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
}

void dt_view_active_images_add(int imgid, gboolean raise)
{
  darktable.view_manager->active_images
      = g_slist_append(darktable.view_manager->active_images, GINT_TO_POINTER(imgid));
  if(raise)
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
}

GSList *dt_view_active_images_get()
{
  return darktable.view_manager->active_images;
}

void dt_view_manager_view_toolbox_add(dt_view_manager_t *vm, GtkWidget *tool, dt_view_type_flags_t views)
{
  if(vm->proxy.view_toolbox.module)
    vm->proxy.view_toolbox.add(vm->proxy.view_toolbox.module, tool, views);
}

void dt_view_manager_module_toolbox_add(dt_view_manager_t *vm, GtkWidget *tool, dt_view_type_flags_t views)
{
  if(vm->proxy.module_toolbox.module)
    vm->proxy.module_toolbox.add(vm->proxy.module_toolbox.module, tool, views);
}

dt_darkroom_layout_t dt_view_darkroom_get_layout(dt_view_manager_t *vm)
{
  if(vm->proxy.darkroom.view)
    return vm->proxy.darkroom.get_layout(vm->proxy.darkroom.view);
  else
    return DT_DARKROOM_LAYOUT_EDITING;
}

void dt_view_lighttable_set_zoom(dt_view_manager_t *vm, gint zoom)
{
  if(vm->proxy.lighttable.module)
    vm->proxy.lighttable.set_zoom(vm->proxy.lighttable.module, zoom);
}

gint dt_view_lighttable_get_zoom(dt_view_manager_t *vm)
{
  if(vm->proxy.lighttable.module)
    return vm->proxy.lighttable.get_zoom(vm->proxy.lighttable.module);
  else
    return 10;
}

void dt_view_collection_update(const dt_view_manager_t *vm)
{
  if(vm->proxy.module_collect.module)
    vm->proxy.module_collect.update(vm->proxy.module_collect.module);
}

#ifdef HAVE_PRINT
void dt_view_print_settings(const dt_view_manager_t *vm, dt_print_info_t *pinfo)
{
  if (vm->proxy.print.view)
    vm->proxy.print.print_settings(vm->proxy.print.view, pinfo);
}
#endif

GSList *dt_mouse_action_create_simple(GSList *actions, dt_mouse_action_type_t type, GdkModifierType accel,
                                      const char *const description)
{
  dt_mouse_action_t *a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->action = type;
  a->key.accel_mods = accel;
  g_strlcpy(a->name, description, sizeof(a->name));
  return g_slist_append(actions, a);
}

GSList *dt_mouse_action_create_format(GSList *actions, dt_mouse_action_type_t type, GdkModifierType accel,
                                      const char *const format_string, const char *const replacement)
{
  dt_mouse_action_t *a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->action = type;
  a->key.accel_mods = accel;
  g_snprintf(a->name, sizeof(a->name), format_string, replacement);
  return g_slist_append(actions, a);
}


static gchar *_mouse_action_get_string(dt_mouse_action_t *ma)
{
  gchar *atxt = dt_util_dstrcat(NULL, "%s", gtk_accelerator_get_label(ma->key.accel_key, ma->key.accel_mods));
  if(strcmp(atxt, "")) atxt = dt_util_dstrcat(atxt, "+");
  switch(ma->action)
  {
    case DT_MOUSE_ACTION_LEFT:
      atxt = dt_util_dstrcat(atxt, _("Left click"));
      break;
    case DT_MOUSE_ACTION_RIGHT:
      atxt = dt_util_dstrcat(atxt, _("Right click"));
      break;
    case DT_MOUSE_ACTION_MIDDLE:
      atxt = dt_util_dstrcat(atxt, _("Middle click"));
      break;
    case DT_MOUSE_ACTION_SCROLL:
      atxt = dt_util_dstrcat(atxt, _("Scroll"));
      break;
    case DT_MOUSE_ACTION_DOUBLE_LEFT:
      atxt = dt_util_dstrcat(atxt, _("Left double-click"));
      break;
    case DT_MOUSE_ACTION_DOUBLE_RIGHT:
      atxt = dt_util_dstrcat(atxt, _("Right double-click"));
      break;
    case DT_MOUSE_ACTION_DRAG_DROP:
      atxt = dt_util_dstrcat(atxt, _("Drag and drop"));
      break;
    case DT_MOUSE_ACTION_LEFT_DRAG:
      atxt = dt_util_dstrcat(atxt, _("Left click+Drag"));
      break;
    case DT_MOUSE_ACTION_RIGHT_DRAG:
      atxt = dt_util_dstrcat(atxt, _("Right click+Drag"));
      break;
  }

  return atxt;
}

static void _accels_window_destroy(GtkWidget *widget, dt_view_manager_t *vm)
{
  // set to NULL so we can rely on it after
  vm->accels_window.window = NULL;
}

static void _accels_window_sticky(GtkWidget *widget, GdkEventButton *event, dt_view_manager_t *vm)
{
  if(!vm->accels_window.window) return;

  // creating new window
  GtkWindow *win = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(win));
  gtk_style_context_add_class(context, "accels_window");
  gtk_window_set_title(win, _("darktable - accels window"));
  GtkAllocation alloc;
  gtk_widget_get_allocation(dt_ui_main_window(darktable.gui->ui), &alloc);

  gtk_window_set_resizable(win, TRUE);
  gtk_window_set_icon_name(win, "darktable");
  gtk_window_set_default_size(win, alloc.width * 0.7, alloc.height * 0.7);
  g_signal_connect(win, "destroy", G_CALLBACK(_accels_window_destroy), vm);

  GtkWidget *sw
      = (GtkWidget *)g_list_first(gtk_container_get_children(GTK_CONTAINER(vm->accels_window.window)))->data;
  g_object_ref(sw);

  gtk_container_remove(GTK_CONTAINER(vm->accels_window.window), sw);
  gtk_container_add(GTK_CONTAINER(win), sw);
  g_object_unref(sw);
  gtk_widget_destroy(vm->accels_window.window);
  vm->accels_window.window = GTK_WIDGET(win);
  gtk_widget_show_all(vm->accels_window.window);
  gtk_widget_hide(vm->accels_window.sticky_btn);

  vm->accels_window.sticky = TRUE;
}

void dt_view_accels_show(dt_view_manager_t *vm)
{
  if(vm->accels_window.window) return;

  vm->accels_window.sticky = FALSE;
  vm->accels_window.prevent_refresh = FALSE;

  GtkStyleContext *context;
  vm->accels_window.window = gtk_window_new(GTK_WINDOW_POPUP);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(vm->accels_window.window);
#endif
  context = gtk_widget_get_style_context(vm->accels_window.window);
  gtk_style_context_add_class(context, "accels_window");

  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  context = gtk_widget_get_style_context(sw);
  gtk_style_context_add_class(context, "accels_window_scroll");

  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  vm->accels_window.flow_box = gtk_flow_box_new();
  context = gtk_widget_get_style_context(vm->accels_window.flow_box);
  gtk_style_context_add_class(context, "accels_window_box");
  gtk_orientable_set_orientation(GTK_ORIENTABLE(vm->accels_window.flow_box), GTK_ORIENTATION_HORIZONTAL);

  gtk_box_pack_start(GTK_BOX(hb), vm->accels_window.flow_box, TRUE, TRUE, 0);

  GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  vm->accels_window.sticky_btn
      = dtgtk_button_new(dtgtk_cairo_paint_multiinstance, CPF_STYLE_FLAT, NULL);
  g_object_set(G_OBJECT(vm->accels_window.sticky_btn), "tooltip-text",
               _("switch to a classic window which will stay open after key release."), (char *)NULL);
  g_signal_connect(G_OBJECT(vm->accels_window.sticky_btn), "button-press-event", G_CALLBACK(_accels_window_sticky), vm);
  context = gtk_widget_get_style_context(vm->accels_window.sticky_btn);
  gtk_style_context_add_class(context, "accels_window_stick");
  gtk_box_pack_start(GTK_BOX(vb), vm->accels_window.sticky_btn, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hb), vb, FALSE, FALSE, 0);

  dt_view_accels_refresh(vm);

  GtkAllocation alloc;
  gtk_widget_get_allocation(dt_ui_main_window(darktable.gui->ui), &alloc);
  // gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), alloc.height);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), alloc.height);
  gtk_scrolled_window_set_max_content_width(GTK_SCROLLED_WINDOW(sw), alloc.width);
  gtk_container_add(GTK_CONTAINER(sw), hb);
  gtk_container_add(GTK_CONTAINER(vm->accels_window.window), sw);

  gtk_window_set_resizable(GTK_WINDOW(vm->accels_window.window), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(vm->accels_window.window), alloc.width, alloc.height);
  gtk_window_set_transient_for(GTK_WINDOW(vm->accels_window.window),
                               GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_window_set_keep_above(GTK_WINDOW(vm->accels_window.window), TRUE);
  // needed on macOS to avoid fullscreening the popup with newer GTK
  gtk_window_set_type_hint(GTK_WINDOW(vm->accels_window.window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);

  gtk_window_set_gravity(GTK_WINDOW(vm->accels_window.window), GDK_GRAVITY_STATIC);
  gtk_window_set_position(GTK_WINDOW(vm->accels_window.window), GTK_WIN_POS_CENTER_ON_PARENT);
  gtk_widget_show_all(vm->accels_window.window);
}

void dt_view_accels_hide(dt_view_manager_t *vm)
{
  if(vm->accels_window.window && vm->accels_window.sticky) return;
  if(vm->accels_window.window) gtk_widget_destroy(vm->accels_window.window);
  vm->accels_window.window = NULL;
}

void dt_view_accels_refresh(dt_view_manager_t *vm)
{
  if(!vm->accels_window.window || vm->accels_window.prevent_refresh) return;

  // drop all existing tables
  GList *lw = gtk_container_get_children(GTK_CONTAINER(vm->accels_window.flow_box));

  for(const GList *lw_iter = lw; lw_iter; lw_iter = g_list_next(lw_iter))
  {
    GtkWidget *w = (GtkWidget *)lw_iter->data;
    gtk_widget_destroy(w);
  }
  g_list_free(lw);
  // get the list of valid accel for this view
  const dt_view_t *cv = dt_view_manager_get_current_view(vm);
  const dt_view_type_flags_t v = cv->view(cv);
  GtkStyleContext *context;

  typedef struct _bloc_t
  {
    gchar *base;
    gchar *title;
    GtkListStore *list_store;
  } _bloc_t;

  // go through all accels to populate categories with valid ones
  GList *blocs = NULL;
  GList *bl = NULL;
  GSList *l = darktable.control->accelerator_list;
  while(l)
  {
    dt_accel_t *da = (dt_accel_t *)l->data;
    if(da && (da->views & v) == v)
    {
      GtkAccelKey ak;
      if(gtk_accel_map_lookup_entry(da->path, &ak) && ak.accel_key > 0)
      {
        // we want the base path
        gchar **elems = g_strsplit(da->translated_path, "/", -1);
        if(elems[0] && elems[1] && elems[2])
        {
          // do we already have a category ?
          bl = blocs;
          _bloc_t *b = NULL;
          while(bl)
          {
            _bloc_t *bb = (_bloc_t *)bl->data;
            if(strcmp(elems[1], bb->base) == 0)
            {
              b = bb;
              break;
            }
            bl = g_list_next(bl);
          }
          // if not found, we create it
          if(!b)
          {
            b = (_bloc_t *)calloc(1, sizeof(_bloc_t));
            b->base = dt_util_dstrcat(NULL, "%s", elems[1]);

            if(g_str_has_prefix(da->path, "<Darktable>/views/"))
              b->title = dt_util_dstrcat(NULL, "%s", cv->name(cv));
            else
              b->title = dt_util_dstrcat(NULL, "%s", elems[1]);

            b->list_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
            blocs = g_list_prepend(blocs, b);
          }
          // we add the new line
          GtkTreeIter iter;
          gtk_list_store_prepend(b->list_store, &iter);
          gchar *txt;
          // for views accels, no need to specify the view name, it's in the category title
          if(g_str_has_prefix(da->path, "<Darktable>/views/"))
            txt = da->translated_path + strlen(elems[0]) + strlen(elems[1]) + strlen(elems[2]) + 3;
          else
            txt = da->translated_path + strlen(elems[0]) + strlen(elems[1]) + 2;
          // for dynamic accel, we need to add the "+scroll"
          gchar *atxt = dt_util_dstrcat(NULL, "%s", gtk_accelerator_get_label(ak.accel_key, ak.accel_mods));
          if(g_str_has_prefix(da->path, "<Darktable>/image operations/") && g_str_has_suffix(da->path, "/dynamic"))
            atxt = dt_util_dstrcat(atxt, _("+Scroll"));
          gtk_list_store_set(b->list_store, &iter, 0, atxt, 1, txt, -1);
          g_free(atxt);
          g_strfreev(elems);
        }
      }
    }
    l = g_slist_next(l);
  }

  // we add the mouse actions too
  if(cv->mouse_actions)
  {
    _bloc_t *bm = (_bloc_t *)calloc(1, sizeof(_bloc_t));
    bm->base = NULL;
    bm->title = dt_util_dstrcat(NULL, _("mouse actions"));
    bm->list_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    blocs = g_list_prepend(blocs, bm);

    GSList *lm = cv->mouse_actions(cv);
    while(lm)
    {
      dt_mouse_action_t *ma = (dt_mouse_action_t *)lm->data;
      if(ma)
      {
        GtkTreeIter iter;
        gtk_list_store_append(bm->list_store, &iter);
        gchar *atxt = _mouse_action_get_string(ma);
        gtk_list_store_set(bm->list_store, &iter, 0, atxt, 1, ma->name, -1);
        g_free(atxt);
      }
      lm = g_slist_next(lm);
    }
    g_slist_free_full(lm, free);
  }

  // now we create and insert the widget to display all accels by categories
  bl = blocs;
  while(bl)
  {
    const _bloc_t *bb = (_bloc_t *)bl->data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    // the title
    GtkWidget *lb = gtk_label_new(bb->title);
    context = gtk_widget_get_style_context(lb);
    gtk_style_context_add_class(context, "accels_window_cat_title");
    gtk_box_pack_start(GTK_BOX(box), lb, FALSE, FALSE, 0);

    // the list of accels
    GtkWidget *list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(bb->list_store));
    context = gtk_widget_get_style_context(list);
    gtk_style_context_add_class(context, "accels_window_list");
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(_("Accel"), renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(list), column);
    column = gtk_tree_view_column_new_with_attributes(_("Action"), renderer, "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(list), column);
    gtk_box_pack_start(GTK_BOX(box), list, FALSE, FALSE, 0);

    gtk_flow_box_insert(GTK_FLOW_BOX(vm->accels_window.flow_box), box, -1);
    g_free(bb->base);
    g_free(bb->title);
    bl = g_list_next(bl);
  }

  g_list_free_full(blocs, free);
  gtk_widget_show_all(vm->accels_window.flow_box);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
