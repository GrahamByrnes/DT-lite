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
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <inttypes.h>


DT_MODULE_INTROSPECTION(2, dt_iop_highlights_params_t)

typedef enum dt_iop_highlights_mode_t
{
  DT_IOP_HIGHLIGHTS_CLIP = 0,    // $DESCRIPTION: "clip highlights"
  DT_IOP_HIGHLIGHTS_LCH = 1,     // $DESCRIPTION: "reconstruct in LCh"
  DT_IOP_HIGHLIGHTS_INPAINT = 2, // $DESCRIPTION: "reconstruct color" 
} dt_iop_highlights_mode_t;

typedef struct dt_iop_highlights_params_t
{
  dt_iop_highlights_mode_t mode; // $DEFAULT: DT_IOP_HIGHLIGHTS_CLIP $DESCRIPTION: "method"
  float blendL; // unused $DEFAULT: 1.0
  float blendC; // unused $DEFAULT: 0.0
  float blendh; // unused $DEFAULT: 0.0
  float clip; // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "clipping threshold"
} dt_iop_highlights_params_t;

typedef struct dt_iop_highlights_gui_data_t
{
  GtkWidget *clip;
  GtkWidget *mode;
} dt_iop_highlights_gui_data_t;

typedef dt_iop_highlights_params_t dt_iop_highlights_data_t;

typedef struct dt_iop_highlights_global_data_t
{
  int kernel_highlights_1f_clip;
  int kernel_highlights_1f_lch_bayer;
  int kernel_highlights_1f_lch_xtrans;
  int kernel_highlights_4f_clip;
} dt_iop_highlights_global_data_t;


const char *name()
{
  return _("highlight reconstruction");
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_RAW;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  return 1;
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
              const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
              struct dt_develop_tiling_t *tiling)
{
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  const uint32_t filters = piece->pipe->dsc.filters;

  tiling->factor = 2.0f;  // in + out
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;

  if(filters == 9u)
  {
    // xtrans
    tiling->xalign = 6;
    tiling->yalign = 6;
    tiling->overlap = (d->mode == DT_IOP_HIGHLIGHTS_LCH) ? 2 : 0;
  }
  else if(filters)
  {
    // bayer
    tiling->xalign = 2;
    tiling->yalign = 2;
    tiling->overlap = (d->mode == DT_IOP_HIGHLIGHTS_LCH) ? 1 : 0;
  }
  else
  {
    // non-raw
    tiling->xalign = 1;
    tiling->yalign = 1;
    tiling->overlap = 0;
  }
}

/* interpolate value for a pixel, ideal via ratio to nearby pixel */
static inline float interp_pix_xtrans(const int ratio_next,
                                      const ssize_t offset_next,
                                      const float clip0, const float clip_next,
                                      const float *const in,
                                      const float *const ratios)
{
  assert(ratio_next != 0);
  // it's OK to exceed clipping of current pixel's color based on a
  // neighbor -- that is the purpose of interpolating highlight
  // colors
  const float clip_val = fmaxf(clip0, clip_next);
  if(in[offset_next] >= clip_next - 1e-5f)
  {
    // next pixel is also clipped
    return clip_val;
  }
  else
  {
    // set this pixel in ratio to the next
    assert(ratio_next != 0);
    if (ratio_next > 0)
      return fminf(in[offset_next] / ratios[ratio_next], clip_val);
    else
      return fminf(in[offset_next] * ratios[-ratio_next], clip_val);
  }
}

static inline void interpolate_color_xtrans(const void *const ivoid, void *const ovoid,
                                            const dt_iop_roi_t *const roi_in,
                                            const dt_iop_roi_t *const roi_out,
                                            int dim, int dir, int other,
                                            const float *const clip,
                                            const uint8_t (*const xtrans)[6],
                                            const int pass)
{
  // In Bayer each row/col has only green/red or green/blue
  // transitions, hence can reconstruct color by single ratio per
  // row. In x-trans there can be transitions between arbitrary colors
  // in a row/col (and 2x2 green blocks which provide no color
  // transition information). Hence calculate multiple color ratios
  // for each row/col.

  // Lookup for color ratios, e.g. red -> blue is roff[0][2] and blue
  // -> red is roff[2][0]. Returned value is an index into ratios. If
  // negative, then need to invert the ratio. Identity color
  // transitions aren't used.
  const int roff[3][3] = {{ 0, -1, -2},
                          { 1,  0, -3},
                          { 2,  3,  0}};
  // record ratios of color transitions 0:unused, 1:RG, 2:RB, and 3:GB
  float ratios[4] = {1.0f, 1.0f, 1.0f, 1.0f};

  // passes are 0:+x, 1:-x, 2:+y, 3:-y
  // dims are 0:traverse a row, 1:traverse a column
  // dir is 1:left to right, -1: right to left
  int i = (dim == 0) ? 0 : other;
  int j = (dim == 0) ? other : 0;
  const ssize_t offs = (dim ? roi_out->width : 1) * ((dir < 0) ? -1 : 1);
  const ssize_t offl = offs - (dim ? 1 : roi_out->width);
  const ssize_t offr = offs + (dim ? 1 : roi_out->width);
  int beg, end;
  if(dir == 1)
  {
    beg = 0;
    end = (dim == 0) ? roi_out->width : roi_out->height;
  }
  else
  {
    beg = ((dim == 0) ? roi_out->width : roi_out->height) - 1;
    end = -1;
  }

  float *in, *out;
  if(dim == 1)
  {
    out = (float *)ovoid + (size_t)i + (size_t)beg * roi_out->width;
    in = (float *)ivoid + (size_t)i + (size_t)beg * roi_in->width;
  }
  else
  {
    out = (float *)ovoid + (size_t)beg + (size_t)j * roi_out->width;
    in = (float *)ivoid + (size_t)beg + (size_t)j * roi_in->width;
  }

  for(int k = beg; k != end; k += dir)
  {
    if(dim == 1)
      j = k;
    else
      i = k;

    const uint8_t f0 = FCxtrans(j, i, roi_in, xtrans);
    const uint8_t f1 = FCxtrans(dim ? (j + dir) : j, dim ? i : (i + dir), roi_in, xtrans);
    const uint8_t fl = FCxtrans(dim ? (j + dir) : (j - 1), dim ? (i - 1) : (i + dir), roi_in, xtrans);
    const uint8_t fr = FCxtrans(dim ? (j + dir) : (j + 1), dim ? (i + 1) : (i + dir), roi_in, xtrans);
    const float clip0 = clip[f0];
    const float clip1 = clip[f1];
    const float clipl = clip[fl];
    const float clipr = clip[fr];
    const float clip_max = fmaxf(fmaxf(clip[0], clip[1]), clip[2]);

    if(i == 0 || i == roi_out->width - 1 || j == 0 || j == roi_out->height - 1)
    {
      if(pass == 3) out[0] = fminf(clip_max, in[0]);
    }
    else
    {
      // ratio to next pixel if this & next are unclamped and not in
      // 2x2 green block
      if ((f0 != f1) &&
          (in[0] < clip0 && in[0] > 1e-5f) &&
          (in[offs] < clip1 && in[offs] > 1e-5f))
      {
        const int r = roff[f0][f1];
        assert(r != 0);
        if (r > 0)
          ratios[r] = (3.f * ratios[r] + (in[offs] / in[0])) / 4.f;
        else
          ratios[-r] = (3.f * ratios[-r] + (in[0] / in[offs])) / 4.f;
      }

      if(in[0] >= clip0 - 1e-5f)
      {
        // interplate color for clipped pixel
        float add;
        if(f0 != f1)
          // next pixel is different color
          add =
            interp_pix_xtrans(roff[f0][f1], offs, clip0, clip1, in, ratios);
        else
          // at start of 2x2 green block, look diagonally
          add = (fl != f0) ?
            interp_pix_xtrans(roff[f0][fl], offl, clip0, clipl, in, ratios) :
            interp_pix_xtrans(roff[f0][fr], offr, clip0, clipr, in, ratios);

        if(pass == 0)
          out[0] = add;
        else if(pass == 3)
          out[0] = fminf(clip_max, (out[0] + add) / 4.0f);
        else
          out[0] += add;
      }
      else
      {
        // pixel is not clipped
        if(pass == 3) out[0] = in[0];
      }
    }
    out += offs;
    in += offs;
  }
}

static inline void interpolate_color(const void *const ivoid, void *const ovoid,
                                     const dt_iop_roi_t *const roi_out, int dim, int dir, int other,
                                     const float *clip, const uint32_t filters, const int pass)
{
  float ratio = 1.0f;
  float *in, *out;

  int i = 0, j = 0;
  if(dim == 0)
    j = other;
  else
    i = other;
  ssize_t offs = dim ? roi_out->width : 1;
  if(dir < 0) offs = -offs;
  int beg, end;
  if(dim == 0 && dir == 1)
  {
    beg = 0;
    end = roi_out->width;
  }
  else if(dim == 0 && dir == -1)
  {
    beg = roi_out->width - 1;
    end = -1;
  }
  else if(dim == 1 && dir == 1)
  {
    beg = 0;
    end = roi_out->height;
  }
  else if(dim == 1 && dir == -1)
  {
    beg = roi_out->height - 1;
    end = -1;
  }
  else
    return;

  if(dim == 1)
  {
    out = (float *)ovoid + i + (size_t)beg * roi_out->width;
    in = (float *)ivoid + i + (size_t)beg * roi_out->width;
  }
  else
  {
    out = (float *)ovoid + beg + (size_t)j * roi_out->width;
    in = (float *)ivoid + beg + (size_t)j * roi_out->width;
  }
  for(int k = beg; k != end; k += dir)
  {
    if(dim == 1)
      j = k;
    else
      i = k;
    const float clip0 = clip[FC(j, i, filters)];
    const float clip1 = clip[FC(dim ? (j + 1) : j, dim ? i : (i + 1), filters)];
    if(i == 0 || i == roi_out->width - 1 || j == 0 || j == roi_out->height - 1)
    {
      if(pass == 3) out[0] = in[0];
    }
    else
    {
      if(in[0] < clip0 && in[0] > 1e-5f)
      { // both are not clipped
        if(in[offs] < clip1 && in[offs] > 1e-5f)
        { // update ratio, exponential decay. ratio = in[odd]/in[even]
          if(k & 1)
            ratio = (3.0f * ratio + in[0] / in[offs]) / 4.0f;
          else
            ratio = (3.0f * ratio + in[offs] / in[0]) / 4.0f;
        }
      }

      if(in[0] >= clip0 - 1e-5f)
      { // in[0] is clipped, restore it as in[1] adjusted according to ratio
        float add = 0.0f;
        if(in[offs] >= clip1 - 1e-5f)
          add = fmaxf(clip0, clip1);
        else if(k & 1)
          add = in[offs] * ratio;
        else
          add = in[offs] / ratio;

        if(pass == 0)
          out[0] = add;
        else if(pass == 3)
          out[0] = (out[0] + add) / 4.0f;
        else
          out[0] += add;
      }
      else
      {
        if(pass == 3) out[0] = in[0];
      }
    }
    out += offs;
    in += offs;
  }
}

/*
 * these 2 constants were computed using following Sage code:
 *
 * sqrt3 = sqrt(3)
 * sqrt12 = sqrt(12) # 2*sqrt(3)
 *
 * print 'sqrt3 = ', sqrt3, ' ~= ', RealField(128)(sqrt3)
 * print 'sqrt12 = ', sqrt12, ' ~= ', RealField(128)(sqrt12)
 */
#define SQRT3 1.7320508075688772935274463415058723669L
#define SQRT12 3.4641016151377545870548926830117447339L // 2*SQRT3

static void process_lch_bayer(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                              void *const ovoid, const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out, const float clip)
{
  const uint32_t filters = piece->pipe->dsc.filters;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clip, filters, ivoid, ovoid, roi_out) \
  schedule(static) collapse(2)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    for(int i = 0; i < roi_out->width; i++)
    {
      float *const out = (float *)ovoid + (size_t)roi_out->width * j + i;
      const float *const in = (float *)ivoid + (size_t)roi_out->width * j + i;

      if(i == roi_out->width - 1 || j == roi_out->height - 1)
      {
        // fast path for border
        out[0] = MIN(clip, in[0]);
      }
      else
      {
        int clipped = 0;

        // sample 1 bayer block. thus we will have 2 green values.
        float R = 0.0f, Gmin = FLT_MAX, Gmax = -FLT_MAX, B = 0.0f;
        for(int jj = 0; jj <= 1; jj++)
        {
          for(int ii = 0; ii <= 1; ii++)
          {
            const float val = in[(size_t)jj * roi_out->width + ii];

            clipped = (clipped || (val > clip));

            const int c = FC(j + jj + roi_out->y, i + ii + roi_out->x, filters);
            switch(c)
            {
              case 0:
                R = val;
                break;
              case 1:
                Gmin = MIN(Gmin, val);
                Gmax = MAX(Gmax, val);
                break;
              case 2:
                B = val;
                break;
            }
          }
        }

        if(clipped)
        {
          const float Ro = MIN(R, clip);
          const float Go = MIN(Gmin, clip);
          const float Bo = MIN(B, clip);

          const float L = (R + Gmax + B) / 3.0f;

          float C = SQRT3 * (R - Gmax);
          float H = 2.0f * B - Gmax - R;

          const float Co = SQRT3 * (Ro - Go);
          const float Ho = 2.0f * Bo - Go - Ro;

          if(R != Gmax && Gmax != B)
          {
            const float ratio = sqrtf((Co * Co + Ho * Ho) / (C * C + H * H));
            C *= ratio;
            H *= ratio;
          }

          float RGB[3] = { 0.0f, 0.0f, 0.0f };

          /*
           * backtransform proof, sage:
           *
           * R,G,B,L,C,H = var('R,G,B,L,C,H')
           * solve([L==(R+G+B)/3, C==sqrt(3)*(R-G), H==2*B-G-R], R, G, B)
           *
           * result:
           * [[R == 1/6*sqrt(3)*C - 1/6*H + L, G == -1/6*sqrt(3)*C - 1/6*H + L, B == 1/3*H + L]]
           */
          RGB[0] = L - H / 6.0f + C / SQRT12;
          RGB[1] = L - H / 6.0f - C / SQRT12;
          RGB[2] = L + H / 3.0f;

          out[0] = RGB[FC(j + roi_out->y, i + roi_out->x, filters)];
        }
        else
        {
          out[0] = in[0];
        }
      }
    }
  }
}

static void process_lch_xtrans(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                               void *const ovoid, const dt_iop_roi_t *const roi_in,
                               const dt_iop_roi_t *const roi_out, const float clip)
{
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clip, ivoid, ovoid, roi_in, roi_out, xtrans) \
  schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    float *out = (float *)ovoid + (size_t)roi_out->width * j;
    float *in = (float *)ivoid + (size_t)roi_in->width * j;

    // bit vector used as ring buffer to remember clipping of current
    // and last two columns, checking current pixel and its vertical
    // neighbors
    int cl = 0;

    for(int i = 0; i < roi_out->width; i++)
    {
      // update clipping ring buffer
      cl = (cl << 1) & 6;
      if(j >= 2 && j <= roi_out->height - 3)
      {
        cl |= (in[-roi_in->width] > clip) | (in[0] > clip) | (in[roi_in->width] > clip);
      }

      if(i < 2 || i > roi_out->width - 3 || j < 2 || j > roi_out->height - 3)
      {
        // fast path for border
        out[0] = MIN(clip, in[0]);
      }
      else
      {
        // if current pixel is clipped, always reconstruct
        int clipped = (in[0] > clip);
        if(!clipped)
        {
          clipped = cl;
          if(clipped)
          {
            // If the ring buffer can't show we are in an obviously
            // unclipped region, this is the slow case: check if there
            // is any 3x3 block touching the current pixel which has
            // no clipping, as then don't need to reconstruct the
            // current pixel. This avoids zippering in edge
            // transitions from clipped to unclipped areas. The
            // X-Trans sensor seems prone to this, unlike Bayer, due
            // to its irregular pattern.
            for(int offset_j = -2; offset_j <= 0; offset_j++)
            {
              for(int offset_i = -2; offset_i <= 0; offset_i++)
              {
                if(clipped)
                {
                  clipped = 0;
                  for(int jj = offset_j; jj <= offset_j + 2; jj++)
                  {
                    for(int ii = offset_i; ii <= offset_i + 2; ii++)
                    {
                      const float val = in[(ssize_t)jj * roi_in->width + ii];
                      clipped = (clipped || (val > clip));
                    }
                  }
                }
              }
            }
          }
        }

        if(clipped)
        {
          float mean[3] = { 0.0f, 0.0f, 0.0f };
          int cnt[3] = { 0, 0, 0 };
          float RGBmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

          for(int jj = -1; jj <= 1; jj++)
          {
            for(int ii = -1; ii <= 1; ii++)
            {
              const float val = in[(ssize_t)jj * roi_in->width + ii];
              const int c = FCxtrans(j+jj, i+ii, roi_in, xtrans);
              mean[c] += val;
              cnt[c]++;
              RGBmax[c] = MAX(RGBmax[c], val);
            }
          }

          const float Ro = MIN(mean[0]/cnt[0], clip);
          const float Go = MIN(mean[1]/cnt[1], clip);
          const float Bo = MIN(mean[2]/cnt[2], clip);

          const float R = RGBmax[0];
          const float G = RGBmax[1];
          const float B = RGBmax[2];

          const float L = (R + G + B) / 3.0f;

          float C = SQRT3 * (R - G);
          float H = 2.0f * B - G - R;

          const float Co = SQRT3 * (Ro - Go);
          const float Ho = 2.0f * Bo - Go - Ro;

          if(R != G && G != B)
          {
            const float ratio = sqrtf((Co * Co + Ho * Ho) / (C * C + H * H));
            C *= ratio;
            H *= ratio;
          }

          float RGB[3] = { 0.0f, 0.0f, 0.0f };

          RGB[0] = L - H / 6.0f + C / SQRT12;
          RGB[1] = L - H / 6.0f - C / SQRT12;
          RGB[2] = L + H / 3.0f;

          out[0] = RGB[FCxtrans(j, i, roi_out, xtrans)];
        }
        else
          out[0] = in[0];
      }
      out++;
      in++;
    }
  }
}

#undef SQRT3
#undef SQRT12

static void process_clip_plain(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                               const float clip)
{
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  if(piece->pipe->dsc.filters)
  { // raw mosaic
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
    dt_omp_firstprivate(clip, in, out, roi_out) \
    schedule(static)
#endif
    for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
      out[k] = MIN(clip, in[k]);
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
    dt_omp_firstprivate(clip, in, out, roi_out) \
    schedule(static)
#endif
    for(size_t k = 0; k < (size_t)4 * roi_out->width * roi_out->height; k++)
      out[k] = MIN(clip, in[k]);
  }
}

static void process_clip(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         const float clip)
{
  if(darktable.codepath.OPENMP_SIMD)
    process_clip_plain(piece, ivoid, ovoid, roi_in, roi_out, clip);
  else
    dt_unreachable_codepath();
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const uint32_t filters = piece->pipe->dsc.filters;
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;

  const float clip = data->clip * fminf(piece->pipe->dsc.processed_maximum[0],
                           fminf(piece->pipe->dsc.processed_maximum[1], piece->pipe->dsc.processed_maximum[2]));

  if(!filters)
  {
    process_clip(piece, ivoid, ovoid, roi_in, roi_out, clip);
    for(int k = 0; k < 3; k++)
      piece->pipe->dsc.processed_maximum[k]
          = fminf(piece->pipe->dsc.processed_maximum[0],
                  fminf(piece->pipe->dsc.processed_maximum[1], piece->pipe->dsc.processed_maximum[2]));
    return;
  }

  switch(data->mode)
  {
    case DT_IOP_HIGHLIGHTS_INPAINT: // a1ex's (magiclantern) idea of color inpainting:
    {
      float clips[4] = { 0.0f };
      for(int j = 0; j < 3; j++)
        clips[j] = 0.987 * data->clip * piece->pipe->dsc.processed_maximum[j];

      if(filters == 9u)
      {
        const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(clips, filters, ivoid, ovoid, roi_in, roi_out, xtrans) \
        schedule(static)
#endif
        for(int j = 0; j < roi_out->height; j++)
        {
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 0, 1, j, clips, xtrans, 0);
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 0, -1, j, clips, xtrans, 1);
        }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(clips, filters, ivoid, ovoid, roi_in, roi_out, xtrans) \
        schedule(static)
#endif
        for(int i = 0; i < roi_out->width; i++)
        {
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 1, 1, i, clips, xtrans, 2);
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 1, -1, i, clips, xtrans, 3);
        }
      }
      else
      {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(clips, filters, ivoid, ovoid, roi_out) \
        shared(data, piece) \
        schedule(static)
#endif
        for(int j = 0; j < roi_out->height; j++)
        {
          interpolate_color(ivoid, ovoid, roi_out, 0, 1, j, clips, filters, 0);
          interpolate_color(ivoid, ovoid, roi_out, 0, -1, j, clips, filters, 1);
        }

// up/down directions
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(clips, filters, ivoid, ovoid, roi_out) \
        shared(data, piece) \
        schedule(static)
#endif
        for(int i = 0; i < roi_out->width; i++)
        {
          interpolate_color(ivoid, ovoid, roi_out, 1, 1, i, clips, filters, 2);
          interpolate_color(ivoid, ovoid, roi_out, 1, -1, i, clips, filters, 3);
        }
      }
      break;
    }
    case DT_IOP_HIGHLIGHTS_LCH:
      if(filters == 9u)
        process_lch_xtrans(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
      else
        process_lch_bayer(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
      break;
    default:
    case DT_IOP_HIGHLIGHTS_CLIP:
      process_clip(piece, ivoid, ovoid, roi_in, roi_out, clip);
      break;
  }

  // update processed maximum
  const float m = fmaxf(fmaxf(piece->pipe->dsc.processed_maximum[0], piece->pipe->dsc.processed_maximum[1]),
                        piece->pipe->dsc.processed_maximum[2]);
  for(int k = 0; k < 3; k++) piece->pipe->dsc.processed_maximum[k] = m;

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) 
      dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)p1;
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  memcpy(d, p, sizeof(*p));
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_highlights_data_t));
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
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)module->params;
  dt_bauhaus_slider_set(g->clip, p->clip);
  dt_bauhaus_combobox_set(g->mode, p->mode);
}

void reload_defaults(dt_iop_module_t *module)
{
  // enable this per default if raw or sraw, 
  module->default_enabled = dt_image_is_rawprepare_supported(&(module->dev->image_storage));
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_highlights_gui_data_t));
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;

  g->mode = dt_bauhaus_combobox_from_params(self, "mode");
  gtk_widget_set_tooltip_text(g->mode, _("highlight reconstruction method"));

  g->clip = dt_bauhaus_slider_from_params(self, "clip");
  dt_bauhaus_slider_set_digits(g->clip, 3);
  gtk_widget_set_tooltip_text(g->clip, _("manually adjust the clipping threshold against "
                                         "magenta highlights (you shouldn't ever need to touch this)"));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
