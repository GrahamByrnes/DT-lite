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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/math.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(2, dt_iop_vibrance_params_t)

typedef struct dt_iop_vibrance_params_t
{
  float amount; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 25.0 $DESCRIPTION: "vibrance"
} dt_iop_vibrance_params_t;

typedef struct dt_iop_vibrance_gui_data_t
{
  GtkWidget *amount_scale;
} dt_iop_vibrance_gui_data_t;

typedef struct dt_iop_vibrance_data_t
{
  float amount;
} dt_iop_vibrance_data_t;

typedef struct dt_iop_vibrance_global_data_t
{
  int kernel_vibrance;
} dt_iop_vibrance_global_data_t;


const char *name()
{
  return _("vibrance");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_Lab;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_vibrance_data_t *d = (dt_iop_vibrance_data_t *)piece->data;
  const float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const size_t npixels = roi_out->width * roi_out->height;
  const int ch = piece->colors;
  const int bch = ch < 4 ? ch : ch - 1;
  const float amount = (d->amount * 0.01);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(amount, bch, in, out, npixels) \
  schedule(static)
#endif
  for(int k = 0; k < (size_t)4 * npixels; k += 4)
  {
    if(bch > 1)
    {/* saturation weight 0 - 1 */
      const float sw = sqrt((in[k + 1] * in[k + 1]) + (in[k + 2] * in[k + 2])) / 256.0;
      const float ls = 1.0 - ((amount * sw) * .25);
      const float ss = 1.0 + (amount * sw);
      out[k] = in[k] * ls;
      for(int j = 1; j < bch; j++) 
        out[k + j] = in[k+ j] * ss;
        
      out[k + 3] = in[k + 3];
    }
    else
      out[k] = in[k];
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_vibrance_params_t *p = (dt_iop_vibrance_params_t *)p1;
  dt_iop_vibrance_data_t *d = (dt_iop_vibrance_data_t *)piece->data;
  d->amount = p->amount;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_vibrance_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_vibrance_gui_data_t *g = (dt_iop_vibrance_gui_data_t *)self->gui_data;
  dt_iop_vibrance_params_t *p = (dt_iop_vibrance_params_t *)module->params;
  dt_bauhaus_slider_set(g->amount_scale, p->amount);
}
void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_vibrance_gui_data_t));
  dt_iop_vibrance_gui_data_t *g = (dt_iop_vibrance_gui_data_t *)self->gui_data;

  g->amount_scale = dt_bauhaus_slider_from_params(self, "amount");
  dt_bauhaus_slider_set_format(g->amount_scale, "%.0f%%");
  gtk_widget_set_tooltip_text(g->amount_scale, _("the amount of vibrance"));
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
