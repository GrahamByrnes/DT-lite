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
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE(1)

typedef struct dt_lib_module_view_t
{
  GtkButton *all_button;
  GtkButton *fav_button;
  gboolean choice;
} dt_lib_module_view_t;

const char *name(dt_lib_module_t *self)
{
  return _("module view");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_BOTTOM;
}

int position()
{
  return 1;
}

void _update(dt_lib_module_t *self)
{
  dt_lib_module_view_t *d = (dt_lib_module_view_t *)self->data;
  gtk_widget_set_sensitive(GTK_WIDGET(d->all_button), d->choice);
  gtk_widget_set_sensitive(GTK_WIDGET(d->fav_button), !(d->choice));
}

void fav_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_module_view_t *d = (dt_lib_module_view_t *)self->data;
  d->choice = TRUE;
  gtk_widget_set_sensitive(GTK_WIDGET(d->all_button), d->choice);
  gtk_widget_set_sensitive(GTK_WIDGET(d->fav_button), !(d->choice));
}

void all_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_module_view_t *d = (dt_lib_module_view_t *)self->data;
  d->choice = FALSE;
  gtk_widget_set_sensitive(GTK_WIDGET(d->all_button), d->choice);
  gtk_widget_set_sensitive(GTK_WIDGET(d->fav_button), !(d->choice));
}

void gui_update(dt_lib_module_t *self)
{
  _update(self);
}
/*
static void _lib_module_view_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_module_view_t *d = (dt_lib_module_view_t *)self->data;
  dt_conf_set_bool("darkroom/ui/iop_view_default-1", d->choice);
  
  for(GList *iter = g_list_first(darktable.iop); iter; iter = g_list_next(iter))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)iter->data;
    dt_iop_gui_update(module);
  }
}*/

static void _lib_modulelist_gui_update(dt_lib_module_t *self)
{
  dt_lib_module_view_t *d = (dt_lib_module_view_t *)self->data;
  dt_conf_set_bool("darkroom/ui/iop_view_default-1", d->choice);
}

#define ellipsize_button(button) gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(button))), PANGO_ELLIPSIZE_END);
void gui_init(dt_lib_module_t *self)
{
  dt_lib_module_view_t *d = (dt_lib_module_view_t *)malloc(sizeof(dt_lib_module_view_t));
  self->data = (void *)d;
  d->choice = dt_conf_get_bool("darkroom/ui/iop_view_default");
//  dt_lib_module_view_favorite = d->choice;
  self->widget = gtk_grid_new();
  GtkGrid *grid = GTK_GRID(self->widget);
  gtk_grid_set_column_homogeneous(grid, TRUE);

  d->fav_button = GTK_BUTTON(gtk_button_new_with_label(_("view favorites")));
  ellipsize_button(d->fav_button);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->fav_button), _("show favorite modules"));
  gtk_grid_attach(grid, GTK_WIDGET(d->fav_button), 0, 0, 3, 1);

  d->all_button = GTK_BUTTON(gtk_button_new_with_label(_("view all")));
  ellipsize_button(d->all_button);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->all_button), _("show all modules"));
  gtk_grid_attach(grid, GTK_WIDGET(d->all_button), 3, 0, 3, 1);

  g_signal_connect(G_OBJECT(d->fav_button), "clicked", G_CALLBACK(fav_button_clicked), self);
  g_signal_connect(G_OBJECT(d->all_button), "clicked", G_CALLBACK(all_button_clicked), self);
  darktable.view_manager->proxy.module_view.module = self;
  darktable.view_manager->proxy.module_view.update = _lib_modulelist_gui_update;
//  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE,
//                            G_CALLBACK(_lib_module_view_callback), self);
  _update(self);
}
#undef ellipsize_button

void gui_cleanup(dt_lib_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_module_view_callback), self);
  free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
