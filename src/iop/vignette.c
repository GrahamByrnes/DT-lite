/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(4, dt_iop_vignette_params_t)
#define TEA_ROUNDS 8

typedef enum dt_iop_dither_t
{
  DITHER_OFF = 0,  // $DESCRIPTION: "off"
  DITHER_8BIT = 1, // $DESCRIPTION: "8-bit output"
  DITHER_16BIT = 2 // $DESCRIPTION: "16-bit output"
} dt_iop_dither_t;

typedef struct dt_iop_dvector_2d_t
{
  double x;
  double y;
} dt_iop_dvector_2d_t;

typedef struct dt_iop_fvector_2d_t
{
  float x; // $MIN: -1.0 $MAX: 1.0 $DESCRIPTION: "horizontal center"
  float y; // $MIN: -1.0 $MAX: 1.0 $DESCRIPTION: "vertical center"
} dt_iop_vector_2d_t;

typedef struct dt_iop_vignette_params_t
{
  float scale;               // $MIN: 0.0 $MAX: 200.0 $DEFAULT: 80.0 Inner radius, percent of largest image dimension
  float falloff_scale;       // $MIN: 0.0 $MAX: 200.0 $DEFAULT: 50.0 $DESCRIPTION: "fall-off strength" 0 - 100 Radius for falloff -- outer radius = inner radius + falloff_scale
  float brightness;          // $MIN: -1.0 $MAX: 1.0 $DEFAULT: -0.5 -1 - 1 Strength of brightness reduction
  float saturation;          // $MIN: -1.0 $MAX: 1.0 $DEFAULT: -0.5 -1 - 1 Strength of saturation reduction
  dt_iop_vector_2d_t center; // Center of vignette
  gboolean autoratio;        // $DEFAULT: FALSE
  float whratio;             // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "width/height ratio" 0-1 = width/height ratio, 1-2 = height/width ratio + 1
  float shape;               // $MIN: 0.0 $MAX: 5.0 $DEFAULT: 1.0 $DESCRIPTION: "shape"
  dt_iop_dither_t dithering; // $DEFAULT: DITHER_OFF if and how to perform dithering
  gboolean unbound;          // $DEFAULT: TRUE whether the values should be clipped
} dt_iop_vignette_params_t;


typedef struct dt_iop_vignette_gui_data_t
{
  GtkWidget *scale;
  GtkWidget *falloff_scale;
  GtkWidget *brightness;
  GtkWidget *saturation;
  GtkWidget *center_x;
  GtkWidget *center_y;
  GtkToggleButton *autoratio;
  GtkWidget *whratio;
  GtkWidget *shape;
  GtkWidget *dithering;
} dt_iop_vignette_gui_data_t;

typedef struct dt_iop_vignette_data_t
{
  float scale;
  float falloff_scale;
  float brightness;
  float saturation;
  dt_iop_vector_2d_t center; // Center of vignette
  gboolean autoratio;
  float whratio;
  float shape;
  int dithering;
  gboolean unbound;
} dt_iop_vignette_data_t;

typedef struct dt_iop_vignette_global_data_t
{
  int kernel_vignette;
} dt_iop_vignette_global_data_t;


const char *name()
{
  return _("vignetting");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING
         | IOP_FLAGS_TILING_FULL_ROI;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

static void encrypt_tea(unsigned int *arg)
{
  const unsigned int key[] = { 0xa341316c, 0xc8013ea4, 0xad90777d, 0x7e95761e };
  unsigned int v0 = arg[0], v1 = arg[1];
  unsigned int sum = 0;
  const unsigned int delta = 0x9e3779b9;
  for(int i = 0; i < TEA_ROUNDS; i++)
  {
    sum += delta;
    v0 += ((v1 << 4) + key[0]) ^ (v1 + sum) ^ ((v1 >> 5) + key[1]);
    v1 += ((v0 << 4) + key[2]) ^ (v0 + sum) ^ ((v0 >> 5) + key[3]);
  }
  arg[0] = v0;
  arg[1] = v1;
}

static float tpdf(unsigned int urandom)
{
  float frandom = (float)urandom / (float)0xFFFFFFFFu;

  return (frandom < 0.5f ? (sqrtf(2.0f * frandom) - 1.0f) : (1.0f - sqrtf(2.0f * (1.0f - frandom))));
}

static int get_grab(float pointerx, float pointery, float startx, float starty, float endx, float endy,
                    float zoom_scale)
{
  const float radius = 5.0 / zoom_scale;

  if(powf(pointerx - startx, 2) + powf(pointery, 2) <= powf(radius, 2)) return 2; // x size
  if(powf(pointerx, 2) + powf(pointery - starty, 2) <= powf(radius, 2)) return 4; // y size
  if(powf(pointerx, 2) + powf(pointery, 2) <= powf(radius, 2)) return 1;          // center
  if(powf(pointerx - endx, 2) + powf(pointery, 2) <= powf(radius, 2)) return 8;   // x falloff
  if(powf(pointerx, 2) + powf(pointery - endy, 2) <= powf(radius, 2)) return 16;  // y falloff

  return 0;
}

static void draw_overlay(cairo_t *cr, float x, float y, float fx, float fy, int grab, float zoom_scale)
{
  // half width/height of the crosshair
  const float crosshair_w = DT_PIXEL_APPLY_DPI(10.0) / zoom_scale;
  const float crosshair_h = DT_PIXEL_APPLY_DPI(10.0) / zoom_scale;

  // center crosshair
  cairo_move_to(cr, -crosshair_w, 0.0);
  cairo_line_to(cr, crosshair_w, 0.0);
  cairo_move_to(cr, 0.0, -crosshair_h);
  cairo_line_to(cr, 0.0, crosshair_h);
  cairo_stroke(cr);

  // inner border of the vignette
  cairo_save(cr);
  if(x <= y)
  {
    cairo_scale(cr, x / y, 1.0);
    cairo_arc(cr, 0.0, 0.0, y, 0.0, M_PI * 2.0);
  }
  else
  {
    cairo_scale(cr, 1.0, y / x);
    cairo_arc(cr, 0.0, 0.0, x, 0.0, M_PI * 2.0);
  }
  cairo_restore(cr);
  cairo_stroke(cr);

  // outer border of the vignette
  cairo_save(cr);
  if(fx <= fy)
  {
    cairo_scale(cr, fx / fy, 1.0);
    cairo_arc(cr, 0.0, 0.0, fy, 0.0, M_PI * 2.0);
  }
  else
  {
    cairo_scale(cr, 1.0, fy / fx);
    cairo_arc(cr, 0.0, 0.0, fx, 0.0, M_PI * 2.0);
  }
  cairo_restore(cr);
  cairo_stroke(cr);

  // the handles
  const float radius_sel = DT_PIXEL_APPLY_DPI(6.0) / zoom_scale;
  const float radius_reg = DT_PIXEL_APPLY_DPI(4.0) / zoom_scale;
  if(grab == 1)
    cairo_arc(cr, 0.0, 0.0, radius_sel, 0.0, M_PI * 2.0);
  else
    cairo_arc(cr, 0.0, 0.0, radius_reg, 0.0, M_PI * 2.0);
  cairo_stroke(cr);
  if(grab == 2)
    cairo_arc(cr, x, 0.0, radius_sel, 0.0, M_PI * 2.0);
  else
    cairo_arc(cr, x, 0.0, radius_reg, 0.0, M_PI * 2.0);
  cairo_stroke(cr);
  if(grab == 4)
    cairo_arc(cr, 0.0, -y, radius_sel, 0.0, M_PI * 2.0);
  else
    cairo_arc(cr, 0.0, -y, radius_reg, 0.0, M_PI * 2.0);
  cairo_stroke(cr);
  if(grab == 8)
    cairo_arc(cr, fx, 0.0, radius_sel, 0.0, M_PI * 2.0);
  else
    cairo_arc(cr, fx, 0.0, radius_reg, 0.0, M_PI * 2.0);
  cairo_stroke(cr);
  if(grab == 16)
    cairo_arc(cr, 0.0, -fy, radius_sel, 0.0, M_PI * 2.0);
  else
    cairo_arc(cr, 0.0, -fy, radius_reg, 0.0, M_PI * 2.0);
  cairo_stroke(cr);
}

// FIXME: For portrait images the overlay is a bit off. The coordinates in mouse_moved seem to be ok though.
// WTF?
void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = self->dev;
  //   dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;

  const float wd = dev->preview_pipe->backbuf_width;
  const float ht = dev->preview_pipe->backbuf_height;
  float bigger_side, smaller_side;
  if(wd >= ht)
  {
    bigger_side = wd;
    smaller_side = ht;
  }
  else
  {
    bigger_side = ht;
    smaller_side = wd;
  }
  const float zoom_y = dt_control_get_dev_zoom_y();
  const float zoom_x = dt_control_get_dev_zoom_x();
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  cairo_translate(cr, width / 2.0, height / 2.0);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

  float vignette_x = (p->center.x + 1.0) * 0.5 * wd;
  float vignette_y = (p->center.y + 1.0) * 0.5 * ht;

  cairo_translate(cr, vignette_x, vignette_y);

  float vignette_w = p->scale * 0.01 * 0.5 * wd; // start of falloff
  float vignette_h = p->scale * 0.01 * 0.5 * ht;
  float vignette_fx = vignette_w + p->falloff_scale * 0.01 * 0.5 * wd; // end of falloff
  float vignette_fy = vignette_h + p->falloff_scale * 0.01 * 0.5 * ht;

  if(p->autoratio == FALSE)
  {
    float factor1 = bigger_side / smaller_side;
    if(wd >= ht)
    {
      float factor2 = (2.0 - p->whratio) * factor1;

      if(p->whratio <= 1)
      {
        vignette_h *= factor1;
        vignette_w *= p->whratio;
        vignette_fx *= p->whratio;
        vignette_fy *= factor1;
      }
      else
      {
        vignette_h *= factor2;
        vignette_fy *= factor2;
      }
    }
    else
    {
      float factor2 = (p->whratio) * factor1;

      if(p->whratio <= 1)
      {
        vignette_w *= factor2;
        vignette_fx *= factor2;
      }
      else
      {
        vignette_w *= factor1;
        vignette_h *= (2.0 - p->whratio);
        vignette_fx *= factor1;
        vignette_fy *= (2.0 - p->whratio);
      }
    }
  }

  int grab = get_grab(pzx * wd - vignette_x, pzy * ht - vignette_y, vignette_w, -vignette_h, vignette_fx,
                      -vignette_fy, zoom_scale);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3.0) / zoom_scale);
  dt_draw_set_color_overlay(cr, 0.3, 0.8);
  draw_overlay(cr, vignette_w, vignette_h, vignette_fx, vignette_fy, grab, zoom_scale);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0) / zoom_scale);
  dt_draw_set_color_overlay(cr, 0.8, 0.8);
  draw_overlay(cr, vignette_w, vignette_h, vignette_fx, vignette_fy, grab, zoom_scale);
}

// FIXME: Pumping of the opposite direction when changing width/height. See two FIXMEs further down.
int mouse_moved(struct dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  const float wd = self->dev->preview_pipe->backbuf_width;
  const float ht = self->dev->preview_pipe->backbuf_height;
  float bigger_side, smaller_side;
  if(wd >= ht)
  {
    bigger_side = wd;
    smaller_side = ht;
  }
  else
  {
    bigger_side = ht;
    smaller_side = wd;
  }
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float zoom_scale = dt_dev_get_zoom_scale(self->dev, zoom, 1<<closeup, 1);
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;
  static int old_grab = -1;
  int grab = old_grab;

  float vignette_x = (p->center.x + 1.0) * 0.5 * wd;
  float vignette_y = (p->center.y + 1.0) * 0.5 * ht;

  float vignette_w = p->scale * 0.01 * 0.5 * wd; // start of falloff
  float vignette_h = p->scale * 0.01 * 0.5 * ht;
  float vignette_fx = vignette_w + p->falloff_scale * 0.01 * 0.5 * wd; // end of falloff
  float vignette_fy = vignette_h + p->falloff_scale * 0.01 * 0.5 * ht;

  if(p->autoratio == FALSE)
  {
    float factor1 = bigger_side / smaller_side;
    if(wd >= ht)
    {
      float factor2 = (2.0 - p->whratio) * factor1;

      if(p->whratio <= 1)
      {
        vignette_h *= factor1;
        vignette_w *= p->whratio;
        vignette_fx *= p->whratio;
        vignette_fy *= factor1;
      }
      else
      {
        vignette_h *= factor2;
        vignette_fy *= factor2;
      }
    }
    else
    {
      float factor2 = (p->whratio) * factor1;

      if(p->whratio <= 1)
      {
        vignette_w *= factor2;
        vignette_fx *= factor2;
      }
      else
      {
        vignette_w *= factor1;
        vignette_h *= (2.0 - p->whratio);
        vignette_fx *= factor1;
        vignette_fy *= (2.0 - p->whratio);
      }
    }
  }

  if(grab == 0 || !(darktable.control->button_down && darktable.control->button_down_which == 1))
    grab = get_grab(pzx * wd - vignette_x, pzy * ht - vignette_y, vignette_w, -vignette_h, vignette_fx,
                    -vignette_fy, zoom_scale);

  if(darktable.control->button_down && darktable.control->button_down_which == 1)
  {
    if(grab == 0) // pan the image
    {
      dt_control_change_cursor(GDK_HAND1);
      return 0;
    }
    else if(grab == 1) // move the center
    {
      dt_bauhaus_slider_set(g->center_x, pzx * 2.0 - 1.0);
      dt_bauhaus_slider_set(g->center_y, pzy * 2.0 - 1.0);
    }
    else if(grab == 2) // change the width
    {
      const float max = 0.5 * ((p->whratio <= 1.0) ? bigger_side * p->whratio : bigger_side);
      const float new_vignette_w = MIN(bigger_side, MAX(0.1, pzx * wd - vignette_x));
      const float ratio = new_vignette_w / vignette_h;
      const float new_scale = 100.0 * new_vignette_w / max;
      // FIXME: When going over the 1.0 boundary from wide to narrow (>1.0 -> <=1.0) the height slightly
      // changes, depending on speed.
      //        I guess we have to split the computation.
      if(ratio <= 1.0)
      {
        if(which == GDK_CONTROL_MASK)
          dt_bauhaus_slider_set(g->scale, new_scale);
        else
          dt_bauhaus_slider_set(g->whratio, ratio);
      }
      else
      {
        dt_bauhaus_slider_set(g->scale, new_scale);

        if(which != GDK_CONTROL_MASK)
        {
          float new_whratio = 2.0 - 1.0 / ratio;
          dt_bauhaus_slider_set(g->whratio, new_whratio);
        }
      }
    }
    else if(grab == 4) // change the height
    {
      const float new_vignette_h = MIN(bigger_side, MAX(0.1, vignette_y - pzy * ht));
      const float ratio = new_vignette_h / vignette_w;
      const float max = 0.5 * ((ratio <= 1.0) ? bigger_side * (2.0 - p->whratio) : bigger_side);
      // FIXME: When going over the 1.0 boundary from narrow to wide (>1.0 -> <=1.0) the width slightly
      // changes, depending on speed.
      //        I guess we have to split the computation.
      if(ratio <= 1.0)
      {
        if(which == GDK_CONTROL_MASK)
        {
          const float new_scale = 100.0 * new_vignette_h / max;
          dt_bauhaus_slider_set(g->scale, new_scale);
        }
        else
          dt_bauhaus_slider_set(g->whratio, 2.0 - ratio);
      }
      else
      {
        const float new_scale = 100.0 * new_vignette_h / max;
        dt_bauhaus_slider_set(g->scale, new_scale);

        if(which != GDK_CONTROL_MASK)
        {
          const float new_whratio = 1.0 / ratio;
          dt_bauhaus_slider_set(g->whratio, new_whratio);
        }
      }
    }
    else if(grab == 8) // change the falloff on the right
    {
      const float new_vignette_fx = pzx * wd - vignette_x;
      const float max = 0.5 * ((p->whratio <= 1.0) ? bigger_side * p->whratio : bigger_side);
      const float delta_x = MIN(2.0f * max, MAX(0.0, new_vignette_fx - vignette_w));
      const float new_falloff = 100.0 * delta_x / max;
      dt_bauhaus_slider_set(g->falloff_scale, new_falloff);
    }
    else if(grab == 16) // change the falloff on the top
    {
      const float new_vignette_fy = vignette_y - pzy * ht;
      const float max = 0.5 * ((p->whratio > 1.0) ? bigger_side * (2.0 - p->whratio) : bigger_side);
      const float delta_y = MIN(2.0f * max, MAX(0.0, new_vignette_fy - vignette_h));
      const float new_falloff = 100.0 * delta_y / max;
      dt_bauhaus_slider_set(g->falloff_scale, new_falloff);
    }
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(grab)
  {
    if(grab == 1)
      dt_control_change_cursor(GDK_FLEUR);
    else if(grab == 2)
      dt_control_change_cursor(GDK_SB_H_DOUBLE_ARROW);
    else if(grab == 4)
      dt_control_change_cursor(GDK_SB_V_DOUBLE_ARROW);
    else if(grab == 8)
      dt_control_change_cursor(GDK_SB_H_DOUBLE_ARROW);
    else if(grab == 16)
      dt_control_change_cursor(GDK_SB_V_DOUBLE_ARROW);
  }
  else
  {
    if(old_grab != grab) dt_control_change_cursor(GDK_LEFT_PTR);
  }
  old_grab = grab;
  dt_control_queue_redraw_center();
  return 0;
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  if(which == 1) return 1;
  return 0;
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  if(which == 1) return 1;
  return 0;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_vignette_data_t *data = (dt_iop_vignette_data_t *)piece->data;
  const dt_iop_roi_t *buf_in = &piece->buf_in;
  const gboolean unbound = data->unbound;

  /* Center coordinates of buf_in, these should not consider buf_in->{x,y}! */
  const dt_iop_vector_2d_t buf_center = { buf_in->width * .5f, buf_in->height * .5f };
  /* Center coordinates of vignette center */
  const dt_iop_vector_2d_t vignette_center = { buf_center.x + data->center.x * buf_in->width / 2.0,
                                               buf_center.y + data->center.y * buf_in->height / 2.0 };
  /* Coordinates of vignette_center in terms of roi_in */
  const dt_iop_vector_2d_t roi_center
      = { vignette_center.x * roi_in->scale - roi_in->x, vignette_center.y * roi_in->scale - roi_in->y };
  float xscale;
  float yscale;

  /* w/h ratio follows piece dimensions */
  if(data->autoratio)
  {
    xscale = 2.0 / (buf_in->width * roi_out->scale);
    yscale = 2.0 / (buf_in->height * roi_out->scale);
  }
  else /* specified w/h ratio, scale proportional to longest side */
  {
    const float basis = 2.0 / (MAX(buf_in->height, buf_in->width) * roi_out->scale);
    // w/h ratio from 0-1 use as-is
    if(data->whratio <= 1.0)
    {
      yscale = basis;
      xscale = yscale / data->whratio;
    }
    // w/h ratio from 1-2 interpret as 1-inf
    // that is, the h/w ratio + 1
    else
    {
      xscale = basis;
      yscale = xscale / (2.0 - data->whratio);
    }
  }
  const float dscale = data->scale / 100.0;
  // A minimum falloff is used, based on the image size, to smooth out aliasing artifacts
  const float min_falloff = 100.0 / MIN(buf_in->width, buf_in->height);
  const float fscale = MAX(data->falloff_scale, min_falloff) / 100.0;
  const float shape = MAX(data->shape, 0.001);
  const float exp1 = 2.0 / shape;
  const float exp2 = shape / 2.0;
  // Pre-scale the center offset
  const dt_iop_vector_2d_t roi_center_scaled = { roi_center.x * xscale, roi_center.y * yscale };

  float dither = 0.0f;

  switch(data->dithering)
  {
    case DITHER_8BIT:
      dither = 1.0f / 256;
      break;
    case DITHER_16BIT:
      dither = 1.0f / 65536;
      break;
    case DITHER_OFF:
    default:
      dither = 0.0f;
  }

  const int ch = piece->colors;
  const int bch = ch < 4 ? ch : ch - 1;
  unsigned int *const tea_states = calloc(2 * dt_get_num_threads(), sizeof(unsigned int));

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(dscale, exp1, exp2, fscale, ivoid, ovoid, bch, \
                      roi_center_scaled, roi_out, tea_states, unbound) \
  shared(data, yscale, xscale, dither) \
  schedule(static)
#endif

  for(int j = 0; j < roi_out->height; j++)
  {
    const size_t k = (size_t)4 * roi_out->width * j;
    const float *in = (const float *)ivoid + k;
    float *out = (float *)ovoid + k;
    unsigned int *tea_state = tea_states + 2 * dt_get_thread_num();
    tea_state[0] = j * roi_out->height + dt_get_thread_num();
    for(int i = 0; i < roi_out->width; i++, in += 4, out += 4)
    {
      // current pixel coord translated to local coord
      const dt_iop_vector_2d_t pv
          = { fabsf(i * xscale - roi_center_scaled.x), fabsf(j * yscale - roi_center_scaled.y) };

      // Calculate the pixel weight in vignette
      const float cplen = powf(powf(pv.x, exp1) + powf(pv.y, exp1), exp2); // Length from center to pv
      float weight = 0.0;
      float dith = 0.0;
      float *colm = calloc(bch, sizeof(float));

      if(cplen >= dscale) // pixel is outside the inner vignette circle, lets calculate weight of vignette
      {
        weight = ((cplen - dscale) / fscale);
        if(weight >= 1.0)
          weight = 1.0;
        else if(weight <= 0.0)
          weight = 0.0;
        else if(dither == 0.0f)
          dith = 0.0f;
        else
        {
          weight = 0.5 - cosf(M_PI * weight) / 2.0;
          encrypt_tea(tea_state);
          dith = dither * tpdf(tea_state[0]);
        }
      }
      // Let's apply weighted effect on brightness and desaturation
      for(int col = 0; col < bch; col++)
        colm[col] = in[col];
        
      if(weight > 0)
      {
        // Then apply falloff vignette
        float falloff = (data->brightness < 0) ? (1.0 + (weight * data->brightness))
                                               : (weight * data->brightness);
        float mv = 0;
        for(int col = 0; col < bch; col++)
        {
          colm[col] = data->brightness < 0 ? colm[col] * falloff + dith : colm[col] + falloff + dith;
          colm[col] = unbound ? colm[col] : CLIP(colm[col]);
          mv += colm[col];
        }
        mv /= bch;
        float wss = weight * data->saturation;
        for(int col = 0; col < bch; col++)
        {
          colm[col] = colm[col] - ((mv - colm[col]) * wss);
          colm[col] = unbound ? colm[col] : CLIP(colm[col]);
        }
      }
      for(int col = 0; col < bch; col++)
        out[col] = colm[col];

      out[3] = in[3];
      free(colm);
    }
  }
  free(tea_states);
}

static void autoratio_callback(GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->autoratio = gtk_toggle_button_get_active(button);
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  gtk_widget_set_sensitive(GTK_WIDGET(g->whratio), !p->autoratio);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)p1;
  dt_iop_vignette_data_t *d = (dt_iop_vignette_data_t *)piece->data;
  d->scale = p->scale;
  d->falloff_scale = p->falloff_scale;
  d->brightness = p->brightness;
  d->saturation = p->saturation;
  d->center = p->center;
  d->autoratio = p->autoratio;
  d->whratio = p->whratio;
  d->shape = p->shape;
  d->dithering = p->dithering;
  d->unbound = p->unbound;
}

void init_presets(dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "BEGIN", NULL, NULL, NULL);
  dt_iop_vignette_params_t p;
  p.scale = 40.0f;
  p.falloff_scale = 100.0f;
  p.brightness = -1.0f;
  p.saturation = 0.5f;
  p.center.x = 0.0f;
  p.center.y = 0.0f;
  p.autoratio = FALSE;
  p.whratio = 1.0f;
  p.shape = 1.0f;
  p.dithering = 0;
  p.unbound = TRUE;
  dt_gui_presets_add_generic(_("lomo"), self->op, self->version(), &p, sizeof(p), 1);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "COMMIT", NULL, NULL, NULL);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_vignette_data_t));
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
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)module->params;
  dt_bauhaus_slider_set(g->scale, p->scale);
  dt_bauhaus_slider_set(g->falloff_scale, p->falloff_scale);
  dt_bauhaus_slider_set(g->brightness, p->brightness);
  dt_bauhaus_slider_set(g->saturation, p->saturation);
  dt_bauhaus_slider_set(g->center_x, p->center.x);
  dt_bauhaus_slider_set(g->center_y, p->center.y);
  gtk_toggle_button_set_active(g->autoratio, p->autoratio);
  dt_bauhaus_slider_set(g->whratio, p->whratio);
  dt_bauhaus_slider_set(g->shape, p->shape);
  gtk_widget_set_sensitive(GTK_WIDGET(g->whratio), !p->autoratio);
  dt_bauhaus_combobox_set(g->dithering, p->dithering);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_vignette_gui_data_t));
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;

  g->scale = dt_bauhaus_slider_from_params(self, N_("scale"));
  g->falloff_scale = dt_bauhaus_slider_from_params(self, "falloff_scale");
  g->brightness = dt_bauhaus_slider_from_params(self, N_("brightness"));
  g->saturation = dt_bauhaus_slider_from_params(self, N_("saturation"));
  g->center_x = dt_bauhaus_slider_from_params(self, "center.x");
  g->center_y = dt_bauhaus_slider_from_params(self, "center.y");
  g->shape = dt_bauhaus_slider_from_params(self, N_("shape"));

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = dtgtk_reset_label_new(_("automatic ratio"), self, &p->autoratio, sizeof p->autoratio);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(label), TRUE, TRUE, 0);
  g->autoratio = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(_("automatic")));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->autoratio), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

  g->whratio = dt_bauhaus_slider_from_params(self, "whratio");
  g->dithering = dt_bauhaus_combobox_from_params(self, N_("dithering"));

  dt_bauhaus_slider_set_digits(g->brightness, 3);
  dt_bauhaus_slider_set_digits(g->saturation, 3);
  dt_bauhaus_slider_set_digits(g->center_x, 3);
  dt_bauhaus_slider_set_digits(g->center_y, 3);
  dt_bauhaus_slider_set_digits(g->whratio, 3);

  dt_bauhaus_slider_set_format(g->scale, "%.02f%%");
  dt_bauhaus_slider_set_format(g->falloff_scale, "%.02f%%");

  gtk_widget_set_sensitive(GTK_WIDGET(g->whratio), !p->autoratio);
  gtk_widget_set_tooltip_text(g->scale, _("the radii scale of vignette for start of fall-off"));
  gtk_widget_set_tooltip_text(g->falloff_scale, _("the radii scale of vignette for end of fall-off"));
  gtk_widget_set_tooltip_text(g->brightness, _("strength of effect on brightness"));
  gtk_widget_set_tooltip_text(g->saturation, _("strength of effect on saturation"));
  gtk_widget_set_tooltip_text(g->center_x, _("horizontal offset of center of the effect"));
  gtk_widget_set_tooltip_text(g->center_y, _("vertical offset of center of the effect"));
  gtk_widget_set_tooltip_text(g->shape, _("shape factor\n0 produces a rectangle\n1 produces a circle or ellipse\n"
                                          "2 produces a diamond"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->autoratio), _("enable to have the ratio automatically follow the image size"));
  gtk_widget_set_tooltip_text(g->whratio, _("width-to-height ratio"));
  gtk_widget_set_tooltip_text(g->dithering, _("add some level of random noise to prevent banding"));

  g_signal_connect(G_OBJECT(g->autoratio), "toggled", G_CALLBACK(autoratio_callback), self);
}

GSList *mouse_actions(struct dt_iop_module_t *self)
{
  GSList *lm = NULL;
  dt_mouse_action_t *a = NULL;

  a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->action = DT_MOUSE_ACTION_LEFT_DRAG;
  g_snprintf(a->name, sizeof(a->name), _("[%s on node] change vignette/feather size"), self->name());
  lm = g_slist_append(lm, a);

  a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->key.accel_mods = GDK_CONTROL_MASK;
  a->action = DT_MOUSE_ACTION_LEFT_DRAG;
  g_snprintf(a->name, sizeof(a->name), _("[%s on node] change vignette/feather size keeping ratio"),
             self->name());
  lm = g_slist_append(lm, a);

  a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->action = DT_MOUSE_ACTION_LEFT_DRAG;
  g_snprintf(a->name, sizeof(a->name), _("[%s on center] move vignette"), self->name());
  lm = g_slist_append(lm, a);

  return lm;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
