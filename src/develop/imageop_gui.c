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

#include "develop/imageop_gui.h"
#include "develop/imageop.h"
#include "bauhaus/bauhaus.h"
#include "gui/color_picker_proxy.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <assert.h>
#include <gmodule.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include <time.h>

typedef struct dt_module_param_t
{
  dt_iop_module_t *module;
  void            *param;
} dt_module_param_t;

static inline void process_changed_value(dt_iop_module_t *self, GtkWidget *widget, void *data)
{
  if(!self) self = DT_BAUHAUS_WIDGET(widget)->module;

  if(self->gui_changed) self->gui_changed(self, widget, data);

  dt_iop_color_picker_reset(self, TRUE);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void dt_iop_slider_float_callback(GtkWidget *slider, float *field)
{
  if(darktable.gui->reset) return;

  float previous = *field;
  *field = dt_bauhaus_slider_get(slider);

  if (*field != previous) process_changed_value(NULL, slider, &previous);
}

void dt_iop_slider_int_callback(GtkWidget *slider, int *field)
{
  if(darktable.gui->reset) return;

  int previous = *field;
  *field = dt_bauhaus_slider_get(slider);

  if(*field != previous) process_changed_value(NULL, slider, &previous);
}

void dt_iop_combobox_enum_callback(GtkWidget *combobox, int *field)
{
  if(darktable.gui->reset) return;

  int previous = *field;

  *field = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(combobox));

  if(*field != previous) process_changed_value(NULL, combobox, &previous);
}

void dt_iop_combobox_int_callback(GtkWidget *combobox, int *field)
{
  if(darktable.gui->reset) return;

  int previous = *field;

  *field = dt_bauhaus_combobox_get(combobox);

  if(*field != previous) process_changed_value(NULL, combobox, &previous);
}

void dt_iop_combobox_bool_callback(GtkWidget *combobox, gboolean *field)
{
  if(darktable.gui->reset) return;

  gboolean previous = *field;
  *field = dt_bauhaus_combobox_get(combobox);

  if(*field != previous) process_changed_value(NULL, combobox, &previous);
}

static void _iop_toggle_callback(GtkWidget *togglebutton, dt_module_param_t *data)
{
  if(darktable.gui->reset) return;

  dt_iop_module_t *self = data->module;
  gboolean *field = (gboolean*)(data->param);

  gboolean previous = *field;
  *field = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(togglebutton));

  if(*field != previous) process_changed_value(self, togglebutton, &previous);
}

GtkWidget *dt_bauhaus_slider_from_params(dt_iop_module_t *self, const char *param)
{
  dt_iop_params_t *p = (dt_iop_params_t *)self->params;

  size_t param_index = 0;

  const size_t param_length = strlen(param) + 1;
  char *param_name = g_malloc(param_length);
  char *base_name = g_malloc(param_length);
  if(sscanf(param, "%[^[][%zu]", base_name, &param_index) == 2)
  {
    sprintf(param_name, "%s[0]", base_name);
  }
  else
  {
    memcpy(param_name, param, param_length);
  }
  g_free(base_name);

  const dt_introspection_field_t *f = self->so->get_f(param_name);

  GtkWidget *slider = NULL;
  gchar *str;

  if(f)
  {
    if(f->header.type == DT_INTROSPECTION_TYPE_FLOAT)
    {
      const float min = f->Float.Min;
      const float max = f->Float.Max;
      const float defval = *(float*)self->so->get_p(p, param_name);
      int digits = 2;
      float step = 0;

      const float top = fminf(max-min, fmaxf(fabsf(min),fabsf(max)));
      if (top>=100)
      {
        step = 1.f;
      }
      else
      {
        step = top / 100;
        const float log10step = log10f(step);
        const float fdigits = floorf(log10step+.1);
        step = powf(10.f,fdigits);
        if (log10step - fdigits > .5)
          step *= 5;
        if (fdigits < -2.f) 
          digits = -fdigits;
      }

      slider = dt_bauhaus_slider_new_with_range_and_feedback(self, min, max, step, defval, digits, 1);

      const char *post = ""; // set " %%", " EV" etc

      if (min < 0 || (post && *post))
      {
        str = g_strdup_printf("%%%s.0%df%s", (min < 0 ? "+" : ""), digits, post);

        dt_bauhaus_slider_set_format(slider, str);
      
        g_free(str);
      }

      g_signal_connect(G_OBJECT(slider), "value-changed", 
                       G_CALLBACK(dt_iop_slider_float_callback), 
                       p + f->header.offset + param_index * sizeof(float));
    }
    else if(f->header.type == DT_INTROSPECTION_TYPE_INT)
    {
      const int min = f->Int.Min;
      const int max = f->Int.Max;
      const int defval = *(float*)self->so->get_p(p, param_name);

      slider = dt_bauhaus_slider_new_with_range_and_feedback(self, min, max, 1, defval, 0, 1);

      g_signal_connect(G_OBJECT(slider), "value-changed", 
                       G_CALLBACK(dt_iop_slider_int_callback), 
                       p + f->header.offset + param_index * sizeof(int));
    }

    if (*f->header.description)
    {
      // we do not want to support a context as it break all translations see #5498
      // dt_bauhaus_widget_set_label(slider, NULL, g_dpgettext2(NULL, "introspection description", f->header.description));
      dt_bauhaus_widget_set_label(slider, NULL, gettext(f->header.description));
    }
    else
    {
      str = dt_util_str_replace(f->header.field_name, "_", " ");
    
      dt_bauhaus_widget_set_label(slider, NULL, _(str));

      g_free(str);
    }
  }
  else
  {
    str = g_strdup_printf("'%s' is not a float/int/slider parameter", param_name);

    slider = dt_bauhaus_slider_new(self);
    dt_bauhaus_widget_set_label(slider, NULL, str);

    g_free(str);
  }

  if(!self->widget) self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), slider, FALSE, FALSE, 0);

  g_free(param_name);

  return slider;
}

GtkWidget *dt_bauhaus_combobox_from_params(dt_iop_module_t *self, const char *param)
{
  dt_iop_params_t *p = (dt_iop_params_t *)self->params;
  dt_introspection_field_t *f = self->so->get_f(param);

  GtkWidget *combobox = dt_bauhaus_combobox_new(self);
  gchar *str = NULL;

  if (f && (f->header.type == DT_INTROSPECTION_TYPE_ENUM || 
            f->header.type == DT_INTROSPECTION_TYPE_INT  ||
            f->header.type == DT_INTROSPECTION_TYPE_UINT ||
            f->header.type == DT_INTROSPECTION_TYPE_BOOL ))
  {
    if (*f->header.description)
    {
      // we do not want to support a context as it break all translations see #5498
      // dt_bauhaus_widget_set_label(combobox, NULL, g_dpgettext2(NULL, "introspection description", f->header.description));
      dt_bauhaus_widget_set_label(combobox, NULL, gettext(f->header.description));
    }
    else
    {
      str = dt_util_str_replace(f->header.field_name, "_", " ");
    
      dt_bauhaus_widget_set_label(combobox, NULL, _(str));

      g_free(str);
    }

    if(f->header.type == DT_INTROSPECTION_TYPE_BOOL)
    {
      dt_bauhaus_combobox_add(combobox, _("no"));
      dt_bauhaus_combobox_add(combobox, _("yes"));

      g_signal_connect(G_OBJECT(combobox), "value-changed", G_CALLBACK(dt_iop_combobox_bool_callback), p + f->header.offset);
    }
    else
    {
      if(f->header.type == DT_INTROSPECTION_TYPE_ENUM)
      {
        for(dt_introspection_type_enum_tuple_t *iter = f->Enum.values; iter && iter->name; iter++)
        {
          // we do not want to support a context as it break all translations see #5498
          // dt_bauhaus_combobox_add_full(combobox, g_dpgettext2(NULL, "introspection description", iter->description), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GINT_TO_POINTER(iter->value), NULL, TRUE);
          dt_bauhaus_combobox_add_full(combobox, gettext(iter->description), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GINT_TO_POINTER(iter->value), NULL, TRUE);
        }

        g_signal_connect(G_OBJECT(combobox), "value-changed", G_CALLBACK(dt_iop_combobox_enum_callback), p + f->header.offset);
      }
      else
      {
        g_signal_connect(G_OBJECT(combobox), "value-changed", G_CALLBACK(dt_iop_combobox_int_callback), p + f->header.offset);
      }
    }
  }
  else
  {
    str = g_strdup_printf("'%s' is not an enum/int/bool/combobox parameter", param);

    dt_bauhaus_widget_set_label(combobox, NULL, str);

    g_free(str);
  }

  if(!self->widget) self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), combobox, FALSE, FALSE, 0);

  return combobox;
}

GtkWidget *dt_bauhaus_toggle_from_params(dt_iop_module_t *self, const char *param)
{
  dt_iop_params_t *p = (dt_iop_params_t *)self->params;
  dt_introspection_field_t *f = self->so->get_f(param);

  GtkWidget *button, *label;
  gchar *str;

  if(f && f->header.type == DT_INTROSPECTION_TYPE_BOOL)
  {
    if (*f->header.description)
    {
      // we do not want to support a context as it break all translations see #5498
      // button = gtk_check_button_new_with_label(g_dpgettext2(NULL, "introspection description", f->header.description));
      label = gtk_label_new(gettext(f->header.description));
    }
    else
    {
      str = dt_util_str_replace(f->header.field_name, "_", " ");
    
      label = gtk_label_new(_(str));

      g_free(str);
    }

    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    button = gtk_check_button_new();
    gtk_container_add(GTK_CONTAINER(button), label);
    dt_module_param_t *module_param = (dt_module_param_t *)g_malloc(sizeof(dt_module_param_t));
    module_param->module = self;
    module_param->param = p + f->header.offset;
    g_signal_connect_data(G_OBJECT(button), "toggled", G_CALLBACK(_iop_toggle_callback), module_param, (GClosureNotify)g_free, 0);
  }
  else
  {
    str = g_strdup_printf("'%s' is not a bool/togglebutton parameter", param);

    button = gtk_check_button_new_with_label(str);

    g_free(str);
  }

  if(!self->widget) self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), button, FALSE, FALSE, 0);

  return button;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
