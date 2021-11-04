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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/histogram.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/pixelpipe.h"
#include "dtgtk/paint.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"

#define exposure2white(x) exp2f(-(x))
#define white2exposure(x) -logf(fmaxf(1e-20f, x))/logf(2.0f)

DT_MODULE_INTROSPECTION(6, dt_iop_exposure_params_t)

typedef enum dt_iop_exposure_mode_t
{
  EXPOSURE_MODE_MANUAL,   
  EXPOSURE_MODE_DEFLICKER 
} dt_iop_exposure_mode_t;

typedef struct dt_iop_exposure_params_t
{
  dt_iop_exposure_mode_t mode; // $DEFAULT: EXPOSURE_MODE_MANUAL
  float black;    // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "black level correction"
  float exposure; // $MIN: -18.0 $MAX: 18.0 $DEFAULT: 0.0
  float deflicker_percentile;
  float deflicker_target_level;
  int compensate_exposure_bias;
} dt_iop_exposure_params_t;


typedef struct dt_iop_exposure_gui_data_t
{
  GtkWidget *mode;
  GtkWidget *black;
  GtkStack *mode_stack;
  GtkWidget *exposure;
  GtkWidget *autoexpp;
  GtkWidget *deflicker_percentile;
  GtkWidget *deflicker_target_level;
  uint32_t *deflicker_histogram; // used to cache histogram of source file
  dt_dev_histogram_stats_t deflicker_histogram_stats;
  GtkLabel *deflicker_used_EC;
  GtkWidget *compensate_exposure_bias;
  float deflicker_computed_exposure;
  dt_pthread_mutex_t lock;
} dt_iop_exposure_gui_data_t;

typedef struct dt_iop_exposure_data_t
{
  dt_iop_exposure_params_t params;
  int deflicker;
  float black;
  float scale;
} dt_iop_exposure_data_t;

typedef struct dt_iop_exposure_global_data_t
{
  int kernel_exposure;
} dt_iop_exposure_global_data_t;


const char *name()
{
  return _("exposure");
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

void init_presets (dt_iop_module_so_t *self)
{
  dt_gui_presets_update_ldr(_("scene-referred default"), self->op, self->version(), FOR_RAW);
  dt_gui_presets_update_autoapply(_("scene-referred default"), self->op, self->version(),
                 (strcmp(dt_conf_get_string("plugins/darkroom/workflow"), "scene-referred") == 0));
}

static void process_common_setup(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_data_t *d = piece->data;
  d->black = d->params.black;
  float exposure = d->params.exposure;
  const float white = exposure2white(exposure);
  d->scale = 1.0 / (white - d->black);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_exposure_data_t *const d = (const dt_iop_exposure_data_t *const)piece->data;
  process_common_setup(self, piece);
  const size_t npixels = roi_out->width * roi_out->height;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(d, i, o, npixels) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)4 * npixels; k += 4)
  {
    for(int j = 0; j < 3; j++)
      ((float *)o)[k + j] = (((float *)i)[k + j] - d->black) * d->scale;
      
    ((float *)o)[k + 3] = ((float *)i)[k + 3];
  }
  for(int k = 0; k < 3; k++)
    piece->pipe->dsc.processed_maximum[k] *= d->scale;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)p1;
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;

  d->params.black = p->black;
  d->params.exposure = p->exposure;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_exposure_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  dt_iop_color_picker_reset(self, TRUE);
  dt_bauhaus_slider_set_soft(g->black, p->black);
  dt_bauhaus_slider_set_soft(g->exposure, p->exposure);
}

static void exposure_set_black(struct dt_iop_module_t *self, const float black);

static void exposure_set_white(struct dt_iop_module_t *self, const float white)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  const float exposure = white2exposure(white);

  if(p->exposure == exposure)
    return;

  p->exposure = exposure;
  
  if(p->black >= white)
    exposure_set_black(self, white - 0.01);

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  ++darktable.gui->reset;
  dt_bauhaus_slider_set_soft(g->exposure, p->exposure);
  --darktable.gui->reset;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void exposure_set_black(struct dt_iop_module_t *self, const float black)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(p->black == black)
    return;

  p->black = black;
  
  if(p->black >= exposure2white(p->exposure))
    exposure_set_white(self, p->black + 0.01);

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  ++darktable.gui->reset;
  dt_bauhaus_slider_set_soft(g->black, p->black);
  --darktable.gui->reset;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  if(darktable.gui->reset) return;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  const float white = fmaxf(fmaxf(self->picked_color_max[0], self->picked_color_max[1]),
                            self->picked_color_max[2]) * (1.0 - dt_bauhaus_slider_get(g->autoexpp));
  exposure_set_white(self, white);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(w == g->exposure)
  {
    const float white = exposure2white(p->exposure);
    if(p->black >= white)
      exposure_set_black(self, white - 0.01);
  }
  else if(w == g->black)
  {
    const float white = exposure2white(p->exposure);
    if(p->black >= white)
      exposure_set_white(self, p->black + 0.01);
  }
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_exposure_gui_data_t));
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  g->deflicker_histogram = NULL;////
  dt_pthread_mutex_init(&g->lock, NULL);

  const float low_lim = dt_conf_get_float("exposure_lower_limit");
  const float up_lim = dt_conf_get_float("exposure_upper_limit");
  const float black_lim = dt_conf_get_float("exposure_black_limit");
  
  g->exposure = dt_bauhaus_slider_from_params(self, N_("exposure"));
  gtk_widget_set_tooltip_text(g->exposure, _("adjust the exposure correction"));
  dt_bauhaus_slider_set_step(g->exposure, 0.02);
  dt_bauhaus_slider_set_digits(g->exposure, 3);
  dt_bauhaus_slider_set_format(g->exposure, _("%.2f EV"));
  dt_bauhaus_slider_set_soft_range(g->exposure, low_lim, up_lim);

  g->black = dt_bauhaus_slider_from_params(self, "black");
  dt_bauhaus_slider_set_step(g->black, 0.0001);
  dt_bauhaus_slider_set_digits(g->black, 4);
  dt_bauhaus_slider_set_soft_range(g->black, -black_lim, black_lim);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  GList *instances = darktable.develop->proxy.exposure;
  while(instances != NULL)
  {
    GList *next = g_list_next(instances);
    dt_dev_proxy_exposure_t *instance = (dt_dev_proxy_exposure_t *)instances->data;
    if(instance->module == self)
    {
      g_free(instance);
      darktable.develop->proxy.exposure = g_list_delete_link(darktable.develop->proxy.exposure, instances);
    }
    instances = next;
  }
  
  dt_pthread_mutex_destroy(&g->lock);

  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
