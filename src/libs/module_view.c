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
  GtkToggleButton *fav_button;
  gboolean choice; // 0 all, 1 favourites
  char *button_title[2];
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
  dt_conf_set_bool("darkroom/ui/iop_view_default", d->choice);
}

void gui_update(dt_lib_module_t *self)
{
  _update(self);
}

static void _lib_module_view_gui_update(dt_lib_module_t *self)
{
  dt_lib_module_view_t *d = (dt_lib_module_view_t *)self->data;
  dt_conf_set_bool("darkroom/ui/iop_view_default", d->choice);
}

void _fav_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_module_view_t *d = (dt_lib_module_view_t *)self->data;
  d->choice = !d->choice;
  gtk_button_set_label(GTK_BUTTON(d->fav_button), d->button_title[!d->choice]);

  dt_view_manager_switch(darktable.view_manager, "lighttable");
  dt_view_manager_switch(darktable.view_manager, "darkroom"); /* *** there must be a more efficient way */
}

#define ellipsize_button(button) gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(button))), PANGO_ELLIPSIZE_END);
void gui_init(dt_lib_module_t *self)
{
  dt_lib_module_view_t *d = (dt_lib_module_view_t *)malloc(sizeof(dt_lib_module_view_t));
  self->data = (void *)d;
  d->choice = dt_conf_get_bool("darkroom/ui/iop_view_default");
  d->button_title[0]="show all";
  d->button_title[1]="only favourites";
  self->widget = gtk_grid_new();
  GtkGrid *grid = GTK_GRID(self->widget);
  gtk_grid_set_column_homogeneous(grid, TRUE);

  d->fav_button = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(d->button_title[!d->choice]));
  ellipsize_button(d->fav_button);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->fav_button), _("choose all modules or favourites"));
  gtk_grid_attach(grid, GTK_WIDGET(d->fav_button), 0, 0, 1, 1);

  g_signal_connect(G_OBJECT(d->fav_button), "toggled", G_CALLBACK(_fav_button_clicked), self);
  darktable.view_manager->proxy.module_view.module = self;
  darktable.view_manager->proxy.module_view.update = _lib_module_view_gui_update;
  
  _update(self);
}
#undef ellipsize_button

void gui_cleanup(dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
