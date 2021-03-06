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
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_tool_filter_t
{
  GtkWidget *filter;
  GtkWidget *comparator;
  GtkWidget *sort;
  GtkWidget *reverse;
} dt_lib_tool_filter_t;

#ifdef USE_LUA
typedef enum dt_collection_sort_order_t
{
  DT_COLLECTION_SORT_ORDER_ASCENDING = 0,
  DT_COLLECTION_SORT_ORDER_DESCENDING
} dt_collection_sort_order_t;
#endif

/* proxy function to intelligently reset filter */
static void _lib_filter_reset(dt_lib_module_t *self, gboolean smart_filter);

/* callback for filter combobox change */
static void _lib_filter_combobox_changed(GtkComboBox *widget, gpointer user_data);
/* callback for sort combobox change */
static void _lib_filter_sort_combobox_changed(GtkComboBox *widget, gpointer user_data);
/* callback for reverse sort check button change */
static void _lib_filter_reverse_button_changed(GtkDarktableToggleButton *widget, gpointer user_data);
/* callback for rating comparator combobox change */
static void _lib_filter_comparator_changed(GtkComboBox *widget, gpointer user_data);
/* updates the query and redraws the view */
static void _lib_filter_update_query(dt_lib_module_t *self);
/* make sure that the comparator button matches what is shown in the filter dropdown */
static gboolean _lib_filter_sync_combobox_and_comparator(dt_lib_module_t *self);
/* save the images order if the first collect filter is on tag*/
static void _lib_filter_set_tag_order(dt_lib_module_t *self);
/* images order change from outside */
static void _lib_filter_images_order_change(gpointer instance, int order, dt_lib_module_t *self);

const dt_collection_sort_t items[] =
{
  DT_COLLECTION_SORT_FILENAME,
  DT_COLLECTION_SORT_DATETIME,
  DT_COLLECTION_SORT_RATING,
  DT_COLLECTION_SORT_ID,
  DT_COLLECTION_SORT_COLOR,
  DT_COLLECTION_SORT_GROUP,
  DT_COLLECTION_SORT_PATH,
  DT_COLLECTION_SORT_CUSTOM_ORDER,
  DT_COLLECTION_SORT_TITLE,
  DT_COLLECTION_SORT_DESCRIPTION,
  DT_COLLECTION_SORT_ASPECT_RATIO,
  DT_COLLECTION_SORT_SHUFFLE,
};
#define NB_ITEMS (sizeof(items) / sizeof(dt_collection_sort_t))

static const char *_filter_get_label_at_pos(int pos)
{
  switch(pos)
  {
     case  0: return _("filename");
     case  1: return _("time");
     case  2: return _("rating");
     case  3: return _("id");
     case  4: return _("color label");
     case  5: return _("group");
     case  6: return _("full path");
     case  7: return _("custom sort");
     case  8: return _("title");
     case  9: return _("description");
     case 10: return _("aspect ratio");
     case 11: return _("shuffle");
     default: return "???";
  }
}

static int _filter_get_items(const dt_collection_sort_t sort)
{
  for(int i = 0; i < NB_ITEMS; i++)
  {
    if(sort == items[i])
    return i;
  }
  return 0;
}

const char *name(dt_lib_module_t *self)
{
  return _("filter");
}

const char **views(dt_lib_module_t *self)
{
  /* for now, show in all view due this affects filmroll too

     TODO: Consider to add flag for all views, which prevents
           unloading/loading a module while switching views.

   */
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 2001;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)g_malloc0(sizeof(dt_lib_tool_filter_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  /**/
  GtkWidget *widget;

  /* list label */
  widget = gtk_label_new(_("view"));
  gtk_box_pack_start(GTK_BOX(self->widget), widget, FALSE, FALSE, 0);

  d->comparator = widget = gtk_combo_box_text_new();
  gtk_box_pack_start(GTK_BOX(self->widget), widget, FALSE, FALSE, 0);
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "<"); // DT_COLLECTION_RATING_COMP_LT = 0,
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "???"); // DT_COLLECTION_RATING_COMP_LEQ,
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "="); // DT_COLLECTION_RATING_COMP_EQ,
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "???"); // DT_COLLECTION_RATING_COMP_GEQ,
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), ">"); // DT_COLLECTION_RATING_COMP_GT,
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "???"); // DT_COLLECTION_RATING_COMP_NE,

  gtk_combo_box_set_active(GTK_COMBO_BOX(widget), dt_collection_get_rating_comparator(darktable.collection));
  g_signal_connect(G_OBJECT(widget), "changed", G_CALLBACK(_lib_filter_comparator_changed), (gpointer)self);

  /* create the filter combobox */
  d->filter = widget = gtk_combo_box_text_new();
  gtk_box_pack_start(GTK_BOX(self->widget), widget, FALSE, FALSE, 0);
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _("all"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _("unstarred only"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "???");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "??? ???");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "??? ??? ???");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "??? ??? ??? ???");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "??? ??? ??? ??? ???");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _("rejected only"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _("all except rejected"));

  /* select the last selected value */
  gtk_combo_box_set_active(GTK_COMBO_BOX(widget), dt_collection_get_rating(darktable.collection));

  g_signal_connect(G_OBJECT(widget), "changed", G_CALLBACK(_lib_filter_combobox_changed), (gpointer)self);

  /* sort by label */
  widget = gtk_label_new(_("sort by"));
  gtk_box_pack_start(GTK_BOX(self->widget), widget, FALSE, FALSE, 0);

  /* sort combobox */
  d->sort = widget = gtk_combo_box_text_new();
  gtk_box_pack_start(GTK_BOX(self->widget), widget, FALSE, FALSE, 0);

  /* populate combobox */
  for (int idx = 0 ; idx < NB_ITEMS ; idx++)
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _filter_get_label_at_pos(idx));

  /* select the last selected value */

  const dt_collection_filter_t sort = dt_collection_get_sort_field(darktable.collection);
  gtk_combo_box_set_active(GTK_COMBO_BOX(widget),_filter_get_items(sort));

  g_signal_connect(G_OBJECT(widget), "changed", G_CALLBACK(_lib_filter_sort_combobox_changed), (gpointer)self);

  /* reverse order checkbutton */
  d->reverse = widget
      = dtgtk_togglebutton_new(dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_UP, NULL);
  gtk_widget_set_name(GTK_WIDGET(d->reverse), "control-button");
  if(darktable.collection->params.descending)
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(widget), dtgtk_cairo_paint_solid_arrow,
                                 CPF_DIRECTION_DOWN, NULL);

  gtk_box_pack_start(GTK_BOX(self->widget), widget, FALSE, FALSE, 0);

  /* select the last value and connect callback */
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget),
                               dt_collection_get_sort_descending(darktable.collection));

  g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(_lib_filter_reverse_button_changed),
                   (gpointer)self);

  /* initialize proxy */
  darktable.view_manager->proxy.filter.module = self;
  darktable.view_manager->proxy.filter.reset_filter = _lib_filter_reset;

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_IMAGES_ORDER_CHANGE,
                            G_CALLBACK(_lib_filter_images_order_change), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

// show/hide the comparator dropdown as required
static gboolean _lib_filter_sync_combobox_and_comparator(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  const int filter = gtk_combo_box_get_active(GTK_COMBO_BOX(d->filter));

  // 0 all
  // 1 unstarred only
  // 2 ???
  // 3 ??? ???
  // 4 ??? ??? ???
  // 5 ??? ??? ??? ???
  // 6 ??? ??? ??? ??? ???
  // 7 rejected only
  // 8 all except rejected

  if(filter > 1 && filter < 7)
    gtk_widget_show(d->comparator);
  else
    gtk_widget_hide(d->comparator);

  return FALSE;
}

static void _lib_filter_combobox_changed(GtkComboBox *widget, gpointer user_data)
{
  /* update last settings */
  const int i = gtk_combo_box_get_active(widget);

  uint32_t flags = dt_collection_get_filter_flags(darktable.collection)
    & ~(COLLECTION_FILTER_REJECTED | COLLECTION_FILTER_ALTERED | COLLECTION_FILTER_UNALTERED);

  /* update collection star filter flags */
  if(i == DT_COLLECTION_FILTER_ALL) // all
    flags &= ~(COLLECTION_FILTER_ATLEAST_RATING
               | COLLECTION_FILTER_EQUAL_RATING
               | COLLECTION_FILTER_CUSTOM_COMPARE);
  else if(i == DT_COLLECTION_FILTER_STAR_NO) // unstarred only
    flags = (flags | COLLECTION_FILTER_EQUAL_RATING) & ~(COLLECTION_FILTER_ATLEAST_RATING
                                                         | COLLECTION_FILTER_CUSTOM_COMPARE);
  else if(i == DT_COLLECTION_FILTER_REJECT) // rejected only
    flags = (flags & ~(COLLECTION_FILTER_ATLEAST_RATING
                       | COLLECTION_FILTER_EQUAL_RATING
                       | COLLECTION_FILTER_CUSTOM_COMPARE))
      | COLLECTION_FILTER_REJECTED;
  else if(i == DT_COLLECTION_FILTER_NOT_REJECT) // all except rejected
    flags = (flags | COLLECTION_FILTER_ATLEAST_RATING) & ~COLLECTION_FILTER_CUSTOM_COMPARE;
  else // explicit stars
    flags |= COLLECTION_FILTER_CUSTOM_COMPARE;

  dt_collection_set_filter_flags(darktable.collection, flags);

  /* set the star filter in collection */
  dt_collection_set_rating(darktable.collection, i);
  dt_control_set_mouse_over_id(-1); // maybe we are storing mouse_over_id (arrows)

  /* update the gui accordingly */
  _lib_filter_sync_combobox_and_comparator(user_data);

  /* update the query and view */
  _lib_filter_update_query(user_data);
}

/* save the images order if the first collect filter is on tag*/
static void _lib_filter_set_tag_order(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  if(darktable.collection->tagid)
  {
    const uint32_t sort = items[gtk_combo_box_get_active(GTK_COMBO_BOX(d->sort))];
    const gboolean descending = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->reverse));
    dt_tag_set_tag_order_by_id(darktable.collection->tagid, sort, descending);
  }
}

static void _lib_filter_images_order_change(gpointer instance, const int order, dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  gtk_combo_box_set_active(GTK_COMBO_BOX(d->sort), _filter_get_items(order & ~DT_COLLECTION_ORDER_FLAG));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->reverse), order & DT_COLLECTION_ORDER_FLAG);
}

static void _lib_filter_reverse_button_changed(GtkDarktableToggleButton *widget, gpointer user_data)
{
  const gboolean reverse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

  if(reverse)
    dtgtk_togglebutton_set_paint(widget, dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_DOWN, NULL);
  else
    dtgtk_togglebutton_set_paint(widget, dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_UP, NULL);
  gtk_widget_queue_draw(GTK_WIDGET(widget));

  /* update last settings */
  dt_collection_set_sort(darktable.collection, DT_COLLECTION_SORT_NONE, reverse);

  /* save the images order */
  _lib_filter_set_tag_order(user_data);

  /* update query and view */
  _lib_filter_update_query(user_data);
}

static void _lib_filter_comparator_changed(GtkComboBox *widget, gpointer user_data)
{
  dt_collection_set_rating_comparator(darktable.collection, gtk_combo_box_get_active(widget));

  _lib_filter_update_query(user_data);
}

static void _lib_filter_sort_combobox_changed(GtkComboBox *widget, gpointer user_data)
{
  /* update the ui last settings */
  dt_collection_set_sort(darktable.collection, items[gtk_combo_box_get_active(widget)], -1);

  /* save the images order */
  _lib_filter_set_tag_order(user_data);

  /* update the query and view */
  _lib_filter_update_query(user_data);
}

static void _lib_filter_update_query(dt_lib_module_t *self)
{
  /* sometimes changes */
  dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);

  /* updates query */
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, NULL);
}

static void _lib_filter_reset(dt_lib_module_t *self, gboolean smart_filter)
{
  dt_lib_tool_filter_t *dropdowns = (dt_lib_tool_filter_t *)self->data;

  if(smart_filter == TRUE)
  {
    /* initial import rating setting */
    const int initial_rating = dt_conf_get_int("ui_last/import_initial_rating");

    /* current selection in filter dropdown */
    const int current_filter = gtk_combo_box_get_active(GTK_COMBO_BOX(dropdowns->filter));

    /* convert filter dropdown to rating: 2-6 is 1-5 stars, for anything else, assume 0 stars */
    const int current_filter_rating = (current_filter >= 2 && current_filter <= 6) ? current_filter - 1 : 0;

    /* new filter is the lesser of the initial rating and the current filter rating */
    const int new_filter_rating = MIN(initial_rating, current_filter_rating);

    /* convert new filter rating to filter dropdown selector */
    const int new_filter = (new_filter_rating >= 1 && new_filter_rating <= 5) ? new_filter_rating + 1
                                                                              : new_filter_rating;

    /* Reset to new filter dropdown item */
    gtk_combo_box_set_active(GTK_COMBO_BOX(dropdowns->filter), new_filter);
  }
  else
  {
    /* Reset to topmost item, 'all' */
    gtk_combo_box_set_active(GTK_COMBO_BOX(dropdowns->filter), 0);
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
