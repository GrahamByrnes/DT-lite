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
#include "common/darktable.h"
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
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
  const dt_lib_module_view_t *d = (dt_lib_module_view_t *)self->data;
  const gboolean choice = d->choice;
  gtk_widget_set_sensitive(GTK_WIDGET(d->all_button), choice);
  gtk_widget_set_sensitive(GTK_WIDGET(d->fav_button), !choice);
  fprintf(stderr, "in _update, choice = %d\n", choice);
}

void fav_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_view_t *d = (dt_lib_module_view_t *)user_data;
  gtk_widget_set_sensitive(GTK_WIDGET(d->all_button), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(d->fav_button), FALSE);
  d->choice = !(d->choice);
  user_data = d;
}

void all_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_view_t *d = (dt_lib_module_view_t *)user_data;
  gtk_widget_set_sensitive(GTK_WIDGET(d->all_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(d->fav_button), TRUE);
  d->choice = !(d->choice);
  user_data = d;
}

void gui_reset(dt_lib_module_t *self)
{
  _update(self);
}

#define ellipsize_button(button) gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(button))), PANGO_ELLIPSIZE_END);
void gui_init(dt_lib_module_t *self)
{
  dt_lib_module_view_t *d = (dt_lib_module_view_t *)malloc(sizeof(dt_lib_module_view_t));
  self->data = (void *)d;
  d->choice = TRUE;
  self->widget = gtk_grid_new();
  GtkGrid *grid = GTK_GRID(self->widget);
  gtk_grid_set_column_homogeneous(grid, TRUE);
  int line = 0;

  d->fav_button = GTK_BUTTON(gtk_button_new_with_label(_("view favorites")));
  ellipsize_button(d->fav_button);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->fav_button), _("show favorite modules"));
  gtk_grid_attach(grid, GTK_WIDGET(d->fav_button), 0, line, 3, 1);

  d->all_button = GTK_BUTTON(gtk_button_new_with_label(_("view all")));
  ellipsize_button(d->all_button);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->all_button), _("show all modules"));
  gtk_grid_attach(grid, GTK_WIDGET(d->all_button), 3, line++, 3, 1);
  
  g_signal_connect(G_OBJECT(d->fav_button), "clicked", G_CALLBACK(fav_button_clicked), (gpointer)d);
  g_signal_connect(G_OBJECT(d->all_button), "clicked", G_CALLBACK(all_button_clicked), (gpointer)d);
  _update(self);
  fprintf(stderr, "end of gui_init, choice = %d\n", d->choice);
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
