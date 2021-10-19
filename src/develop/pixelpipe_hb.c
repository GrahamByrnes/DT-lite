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
#include "common/color_picker.h"
#include "common/colorspaces.h"
#include "common/histogram.h"
#include "common/imageio.h"
#include "common/iop_order.h"
#include "control/control.h"
#include "control/signal.h"
#include "develop/blend.h"
#include "develop/format.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe.h"
#include "develop/tiling.h"
#include "develop/masks.h"
#include "gui/gtk.h"
#include "libs/colorpicker.h"
#include "libs/lib.h"
#include "gui/color_picker_proxy.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

typedef enum dt_pixelpipe_flow_t
{
  PIXELPIPE_FLOW_NONE = 0,
  PIXELPIPE_FLOW_HISTOGRAM_NONE = 1 << 0,
  PIXELPIPE_FLOW_HISTOGRAM_ON_CPU = 1 << 1,
  PIXELPIPE_FLOW_PROCESSED_ON_CPU = 1 << 2,
  PIXELPIPE_FLOW_PROCESSED_WITH_TILING = 1 << 3,
  PIXELPIPE_FLOW_BLENDED_ON_CPU = 1 << 4,
} dt_pixelpipe_flow_t;

typedef enum dt_pixelpipe_picker_source_t
{
  PIXELPIPE_PICKER_INPUT = 0,
  PIXELPIPE_PICKER_OUTPUT = 1
} dt_pixelpipe_picker_source_t;

#include "develop/pixelpipe_cache.c"

static void get_output_format(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                              dt_develop_t *dev, dt_iop_buffer_dsc_t *dsc);

static char *_pipe_type_to_str(int pipe_type)
{
  const gboolean fast = (pipe_type & DT_DEV_PIXELPIPE_FAST) == DT_DEV_PIXELPIPE_FAST;
  char *r = NULL;

  switch(pipe_type & DT_DEV_PIXELPIPE_ANY)
  {
    case DT_DEV_PIXELPIPE_PREVIEW:
      if(fast)
        r = "preview/fast";
      else
        r = "preview";
      break;
    case DT_DEV_PIXELPIPE_PREVIEW2:
      if(fast)
        r = "preview2/fast";
      else
        r = "preview2";
      break;
    case DT_DEV_PIXELPIPE_FULL:
      r = "full";
      break;
    case DT_DEV_PIXELPIPE_THUMBNAIL:
      if(fast)
        r = "thumbnail/fast";
      else
        r = "thumbnail";
      break;
    case DT_DEV_PIXELPIPE_EXPORT:
      if(fast)
        r = "export/fast";
      else
        r = "export";
      break;
    default:
      r = "unknown";
  }
  return r;
}

int dt_dev_pixelpipe_init_export(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height, int levels)
{
  const int res = dt_dev_pixelpipe_init_cached(pipe, 4 * sizeof(float) * width * height, 2);
  pipe->type = DT_DEV_PIXELPIPE_EXPORT;
  pipe->levels = levels;
  return res;
}

int dt_dev_pixelpipe_init_thumbnail(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height)
{
  const int res = dt_dev_pixelpipe_init_cached(pipe, 4 * sizeof(float) * width * height, 2);
  pipe->type = DT_DEV_PIXELPIPE_THUMBNAIL;
  return res;
}

int dt_dev_pixelpipe_init_dummy(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height)
{
  const int res = dt_dev_pixelpipe_init_cached(pipe, 4 * sizeof(float) * width * height, 0);
  pipe->type = DT_DEV_PIXELPIPE_THUMBNAIL;
  return res;
}

int dt_dev_pixelpipe_init_preview(dt_dev_pixelpipe_t *pipe)
{
  // don't know which buffer size we're going to need, set to 0 (will be alloced on demand)
  const int res = dt_dev_pixelpipe_init_cached(pipe, 0, 8);
  pipe->type = DT_DEV_PIXELPIPE_PREVIEW;
  return res;
}

int dt_dev_pixelpipe_init_preview2(dt_dev_pixelpipe_t *pipe)
{
  // don't know which buffer size we're going to need, set to 0 (will be alloced on demand)
  const int res = dt_dev_pixelpipe_init_cached(pipe, 0, 5);
  pipe->type = DT_DEV_PIXELPIPE_PREVIEW2;
  return res;
}

int dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe)
{
  // don't know which buffer size we're going to need, set to 0 (will be alloced on demand)
  const int res = dt_dev_pixelpipe_init_cached(pipe, 0, 8);
  pipe->type = DT_DEV_PIXELPIPE_FULL;
  return res;
}

int dt_dev_pixelpipe_init_cached(dt_dev_pixelpipe_t *pipe, size_t size, int32_t entries)
{
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  pipe->processed_width = pipe->backbuf_width = pipe->iwidth = 0;
  pipe->processed_height = pipe->backbuf_height = pipe->iheight = 0;
  pipe->nodes = NULL;
  pipe->backbuf_size = size;
  if(!dt_dev_pixelpipe_cache_init(&(pipe->cache), entries, pipe->backbuf_size)) return 0;
  pipe->cache_obsolete = 0;
  pipe->backbuf = NULL;
  pipe->backbuf_scale = 0.0f;
  pipe->backbuf_zoom_x = 0.0f;
  pipe->backbuf_zoom_y = 0.0f;

  pipe->output_backbuf = NULL;
  pipe->output_backbuf_width = 0;
  pipe->output_backbuf_height = 0;
  pipe->output_imgid = 0;
  pipe->colors = (dt_image_is_raw(&pipe->image)) ? 1 : 4;
  pipe->processing = 0;
  dt_atomic_set_int(&pipe->shutdown,FALSE);
  pipe->tiling = 0;
  pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  pipe->bypass_blendif = 0;
  pipe->input_timestamp = 0;
  pipe->levels = IMAGEIO_RGB | IMAGEIO_INT8;
  dt_pthread_mutex_init(&(pipe->backbuf_mutex), NULL);
  dt_pthread_mutex_init(&(pipe->busy_mutex), NULL);
  pipe->icc_type = DT_COLORSPACE_NONE;
  pipe->icc_filename = NULL;
  pipe->icc_intent = DT_INTENT_LAST;
  pipe->iop = NULL;
  pipe->iop_order_list = NULL;
  pipe->forms = NULL;
  pipe->store_all_raster_masks = FALSE;

  return 1;
}

void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, 
                                float *input, int width, int height, float iscale)
{
  pipe->iwidth = width;
  pipe->iheight = height;
  pipe->iscale = iscale;
  pipe->input = input;
  pipe->image = dev->image_storage;
  get_output_format(NULL, pipe, NULL, dev, &pipe->dsc);
}

void dt_dev_pixelpipe_set_icc(dt_dev_pixelpipe_t *pipe, dt_colorspaces_color_profile_type_t icc_type,
                              const gchar *icc_filename, dt_iop_color_intent_t icc_intent)
{
  pipe->icc_type = icc_type;
  g_free(pipe->icc_filename);
  pipe->icc_filename = g_strdup(icc_filename ? icc_filename : "");
  pipe->icc_intent = icc_intent;
}

void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe)
{
  dt_pthread_mutex_lock(&pipe->backbuf_mutex);
  pipe->backbuf = NULL;
  // blocks while busy and sets shutdown bit:
  dt_dev_pixelpipe_cleanup_nodes(pipe);
  // so now it's safe to clean up cache:
  dt_dev_pixelpipe_cache_cleanup(&(pipe->cache));
  dt_pthread_mutex_unlock(&pipe->backbuf_mutex);
  dt_pthread_mutex_destroy(&(pipe->backbuf_mutex));
  dt_pthread_mutex_destroy(&(pipe->busy_mutex));
  pipe->icc_type = DT_COLORSPACE_NONE;
  g_free(pipe->icc_filename);
  pipe->icc_filename = NULL;

  g_free(pipe->output_backbuf);
  pipe->output_backbuf = NULL;
  pipe->output_backbuf_width = 0;
  pipe->output_backbuf_height = 0;
  pipe->output_imgid = 0;

  if(pipe->forms)
  {
    g_list_free_full(pipe->forms, (void (*)(void *))dt_masks_free_form);
    pipe->forms = NULL;
  }
}

void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe)
{
  dt_atomic_set_int(&pipe->shutdown,TRUE); // tell pipe that it should shut itself down if currently running
  dt_pthread_mutex_lock(&pipe->busy_mutex); // block until the pipe has shut down
  // destroy all nodes
  GList *nodes = pipe->nodes;
  while(nodes)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    // printf("cleanup module `%s'\n", piece->module->name());
    piece->module->cleanup_pipe(piece->module, pipe, piece);
    free(piece->blendop_data);
    piece->blendop_data = NULL;
    free(piece->histogram);
    piece->histogram = NULL;
    g_hash_table_destroy(piece->raster_masks);
    piece->raster_masks = NULL;
    free(piece);
    nodes = g_list_next(nodes);
  }
  g_list_free(pipe->nodes);
  pipe->nodes = NULL;
  // also cleanup iop here
  if(pipe->iop)
  {
    g_list_free(pipe->iop);
    pipe->iop = NULL;
  }
  // and iop order
  g_list_free_full(pipe->iop_order_list, free);
  pipe->iop_order_list = NULL;
  dt_pthread_mutex_unlock(&pipe->busy_mutex);	// safe for others to mess with the pipe now
}

void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex); // block until pipe is idle
  // clear any pending shutdown request
  dt_atomic_set_int(&pipe->shutdown,FALSE);
  // check that the pipe was actually properly cleaned up after the last run
  g_assert(pipe->nodes == NULL);
  g_assert(pipe->iop == NULL);
  g_assert(pipe->iop_order_list == NULL);
  pipe->iop_order_list = dt_ioppr_iop_order_copy_deep(dev->iop_order_list);
  // for all modules in dev:
  pipe->iop = g_list_copy(dev->iop);
  GList *modules = pipe->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)calloc(1, sizeof(dt_dev_pixelpipe_iop_t));
    piece->enabled = module->enabled;
    piece->request_histogram = DT_REQUEST_ONLY_IN_GUI;
    piece->histogram_params.roi = NULL;
    piece->histogram_params.bins_count = 256;
    piece->histogram_stats.bins_count = 0;
    piece->histogram_stats.pixels = 0;
    piece->colors = ((module->default_colorspace(module, pipe, NULL) == iop_cs_RAW) 
                                      && (dt_image_is_raw(&pipe->image))) ? 1 : 4;
    piece->iscale = pipe->iscale;
    piece->iwidth = pipe->iwidth;
    piece->iheight = pipe->iheight;
    piece->module = module;
    piece->pipe = pipe;
    piece->data = NULL;
    piece->hash = 0;
    piece->process_tiling_ready = 0;
    piece->raster_masks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, dt_free_align_ptr);
    memset(&piece->processed_roi_in, 0, sizeof(piece->processed_roi_in));
    memset(&piece->processed_roi_out, 0, sizeof(piece->processed_roi_out));
    dt_iop_init_pipe(piece->module, pipe, piece);
    pipe->nodes = g_list_append(pipe->nodes, piece);
    modules = g_list_next(modules);
  }
  dt_pthread_mutex_unlock(&pipe->busy_mutex); // safe for others to use/mess with the pipe now
}

// helper
void dt_dev_pixelpipe_synch(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, GList *history)
{
  dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
  // find piece in nodes list
  GList *nodes = pipe->nodes;
  dt_dev_pixelpipe_iop_t *piece = NULL;
  while(nodes)
  {
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    if(piece->module == hist->module)
    {
      piece->enabled = hist->enabled;
      dt_iop_commit_params(hist->module, hist->params, hist->blend_params, pipe, piece);
    }
    nodes = g_list_next(nodes);
  }
}

void dt_dev_pixelpipe_synch_all(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  // call reset_params on all pieces first.
  GList *nodes = pipe->nodes;
  while(nodes)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    piece->hash = 0;
    piece->enabled = piece->module->default_enabled;
    dt_iop_commit_params(piece->module, piece->module->default_params, piece->module->default_blendop_params,
                         pipe, piece);
    nodes = g_list_next(nodes);
  }
  // go through all history items and adjust params
  GList *history = dev->history;
  for(int k = 0; k < dev->history_end && history; k++)
  {
    dt_dev_pixelpipe_synch(pipe, dev, history);
    history = g_list_next(history);
  }
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
}

void dt_dev_pixelpipe_synch_top(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  GList *history = g_list_nth(dev->history, dev->history_end - 1);
  if(history)
    dt_dev_pixelpipe_synch(pipe, dev, history);
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
}

void dt_dev_pixelpipe_change(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&dev->history_mutex);
  // case DT_DEV_PIPE_UNCHANGED: case DT_DEV_PIPE_ZOOMED:
  if(pipe->changed & DT_DEV_PIPE_TOP_CHANGED)
    // only top history item changed.
    dt_dev_pixelpipe_synch_top(pipe, dev);
  if(pipe->changed & DT_DEV_PIPE_SYNCH)
    // pipeline topology remains intact, only change all params.
    dt_dev_pixelpipe_synch_all(pipe, dev);
    
  if(pipe->changed & DT_DEV_PIPE_REMOVE)
  {
    // modules have been added in between or removed. need to rebuild the whole pipeline.
    dt_dev_pixelpipe_cleanup_nodes(pipe);
    dt_dev_pixelpipe_create_nodes(pipe, dev);
    dt_dev_pixelpipe_synch_all(pipe, dev);
  }
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  dt_pthread_mutex_unlock(&dev->history_mutex);
  dt_dev_pixelpipe_get_dimensions(pipe, dev, pipe->iwidth, pipe->iheight,
                                  &pipe->processed_width, &pipe->processed_height);
}

static void get_output_format(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                              dt_develop_t *dev, dt_iop_buffer_dsc_t *dsc)
{
  if(module) 
    return module->output_format(module, pipe, piece, dsc);
  // first input.
  *dsc = pipe->image.buf_dsc;

  if(!(dt_image_is_raw(&pipe->image)))
    // image max is normalized before
    for(int k = 0; k < 4; k++)
      dsc->processed_maximum[k] = 1.0f;
}

// helper for color picking
static int pixelpipe_picker_helper(dt_iop_module_t *module, const dt_iop_roi_t *roi, float *picked_color,
                                   float *picked_color_min, float *picked_color_max,
                                   dt_pixelpipe_picker_source_t picker_source, int *box)
{
  const float wd = darktable.develop->preview_pipe->backbuf_width;
  const float ht = darktable.develop->preview_pipe->backbuf_height;
  const int width = roi->width;
  const int height = roi->height;
  const dt_image_t image = darktable.develop->image_storage;
  const int op_after_demosaic = dt_ioppr_is_iop_before(darktable.develop->preview_pipe->iop_order_list,
                                                       module->op, "demosaic", 0);
  // do not continue if one of the point coordinates is set to a negative value indicating a not yet defined
  // position
  if(module->color_picker_point[0] < 0 || module->color_picker_point[1] < 0) 
    return 1;

  float fbox[4] = { 0.0f };
  // get absolute pixel coordinates in final preview image
  if(darktable.lib->proxy.colorpicker.size)
  {
    for(int k = 0; k < 4; k += 2)
      fbox[k] = module->color_picker_box[k] * wd;
    for(int k = 1; k < 4; k += 2)
      fbox[k] = module->color_picker_box[k] * ht;
  }
  else
  {
    fbox[0] = fbox[2] = module->color_picker_point[0] * wd;
    fbox[1] = fbox[3] = module->color_picker_point[1] * ht;
  }
  // transform back to current module coordinates
  dt_dev_distort_backtransform_plus(darktable.develop, darktable.develop->preview_pipe, module->iop_order,
                               ((picker_source == PIXELPIPE_PICKER_INPUT) ? DT_DEV_TRANSFORM_DIR_FORW_INCL
                               : DT_DEV_TRANSFORM_DIR_FORW_EXCL),fbox, 2);

  if (op_after_demosaic || !dt_image_is_rawprepare_supported(&image))
    for(int idx = 0; idx < 4; idx++) 
      fbox[idx] *= darktable.develop->preview_downsampling;

  for(int k = 0; k < 4; k += 2)
  {
    fbox[k] -= roi->x;
    fbox[k + 1] -= roi->y;
  }
  // re-order edges of bounding box
  for(int k = 0; k < 2; k ++)
  {
    box[k] = fminf(fbox[k], fbox[k + 2]);
    box[k + 2] = fmaxf(fbox[k], fbox[k + 2]);
  }

  if(!darktable.lib->proxy.colorpicker.size)
    // if we are sampling one point, make sure that we actually sample it.
    for(int k = 2; k < 4; k++) 
      box[k] += 1;
  // do not continue if box is completely outside of roi
  if(box[0] >= width || box[1] >= height || box[2] < 0 || box[3] < 0) 
    return 1;
  // clamp bounding box to roi
  for(int k = 0; k < 4; k += 2) 
    box[k] = MIN(width - 1, MAX(0, box[k]));
  for(int k = 1; k < 4; k += 2) 
    box[k] = MIN(height - 1, MAX(0, box[k]));
  // safety check: area needs to have minimum 1 pixel width and height
  if(box[2] - box[0] < 1 || box[3] - box[1] < 1)
    return 1;

  return 0;
}

static void pixelpipe_picker(dt_iop_module_t *module, dt_iop_buffer_dsc_t *dsc, const float *pixel,
                             const dt_iop_roi_t *roi, float *picked_color, float *picked_color_min,
                             float *picked_color_max, const dt_iop_colorspace_type_t image_cst,
                             dt_pixelpipe_picker_source_t picker_source)
{
  int box[4];
  const int ch = dsc->channels;
  const int bch = ch < 4 ? ch : ch -1;
  
  if(pixelpipe_picker_helper(module, roi, picked_color, picked_color_min, picked_color_max, picker_source, box))
  {
    for(int k = 0; k < bch; k++)
    {
      picked_color_min[k] = INFINITY;
      picked_color_max[k] = -INFINITY;
      picked_color[k] = 0.0f;
    }
    return;
  }

  float * const min = malloc(bch * sizeof(float));
  float * const max = malloc(bch * sizeof(float));
  float * const avg = malloc(bch * sizeof(float));
  for(int k = 0; k < bch; k++)
  {
    *(min + k) = INFINITY;
    *(max + k) = -INFINITY;
    *(avg + k) = 0.0f;
  }
  dt_color_picker_helper(dsc, pixel, roi, box, avg, min, max, image_cst,
                         dt_iop_color_picker_get_active_cst(module));
  for(int k = 0; k < bch; k++)
  {
    picked_color_min[k] = min[k];
    picked_color_max[k] = max[k];
    picked_color[k] = avg[k];
  }
  free(min);
  free(max);
  free(avg);
}

static void _pixelpipe_pick_from_image(const float *const pixel, const dt_iop_roi_t *roi_in,
                                       cmsHTRANSFORM xform_rgb2lab, cmsHTRANSFORM xform_rgb2rgb,
                                       const float *const pick_box, const float *const pick_point,
                                       const int pick_size, float *pick_color_rgb_min, float *pick_color_rgb_max,
                                       float *pick_color_rgb_mean, float *pick_color_lab_min,
                                       float *pick_color_lab_max, float *pick_color_lab_mean, const int ch)
{
  const int bch = ch < 4 ? ch : ch - 1;
  
  float * const picked_color_rgb_min = malloc(bch * sizeof(float));
  float * const picked_color_rgb_max = malloc(bch * sizeof(float));
  float * const picked_color_rgb_mean = malloc(bch * sizeof(float));
  float * const rgb = malloc(bch * sizeof(float));

  for(int k = 0; k < bch; k++)
  {
    picked_color_rgb_min[k] = FLT_MAX;
    picked_color_rgb_max[k] = FLT_MIN;
    picked_color_rgb_mean[k] = 0.0f;
  }

  int box[4] = { 0 };
  int point[2] = { 0 };

  for(int k = 0; k < 2; k ++)
  {
    box[2 * k] = MIN(roi_in->width - 1, MAX(0, pick_box[2 * k] * roi_in->width));
    box[2 * k + 1] = MIN(roi_in->height - 1, MAX(0, pick_box[2 * k + 1] * roi_in->height));
  }
  point[0] = MIN(roi_in->width - 1, MAX(0, pick_point[0] * roi_in->width));
  point[1] = MIN(roi_in->height - 1, MAX(0, pick_point[1] * roi_in->height));
  const float w = 1.0 / ((box[3] - box[1] + 1) * (box[2] - box[0] + 1));

  if(pick_size == DT_COLORPICKER_SIZE_BOX)
  {
    for(int j = box[1]; j <= box[3]; j++)
      for(int i = box[0]; i <= box[2]; i++)
        for(int k = 0; k < bch; k++)
        {
          picked_color_rgb_min[k]
              = MIN(picked_color_rgb_min[k], pixel[4 * (roi_in->width * j + i) + k]);
          picked_color_rgb_max[k]
              = MAX(picked_color_rgb_max[k], pixel[4 * (roi_in->width * j + i) + k]);
          rgb[k] += w * pixel[4 * (roi_in->width * j + i) + k];
        }
    for(int k = 0; k < bch; k++)
      picked_color_rgb_mean[k] = rgb[k];
  }
  else
  {
    for(int k = 0; k < bch; k++)
      picked_color_rgb_mean[k] = picked_color_rgb_min[k]
          = picked_color_rgb_max[k] = pixel[4 * (roi_in->width * point[1] + point[0]) + k];
  }

  // Converting the display RGB values to histogram RGB
  if(xform_rgb2rgb)
  {
    // Preparing the data for transformation
    float rgb_ddata[9] = { 0.0f };
    for(int i = 0; i < bch; i++)
    {
      rgb_ddata[i] = picked_color_rgb_mean[i];
      rgb_ddata[i + 3] = picked_color_rgb_min[i];
      rgb_ddata[i + 6] = picked_color_rgb_max[i];
    }

    if(ch == 1)
      for(int j = 0; j < 3; j++)
        rgb_ddata[3 * j + 1] = rgb_ddata[3 * j + 2] = rgb_ddata[3 * j];
        
    float rgb_odata[9] = { 0.0f };
    cmsDoTransform(xform_rgb2rgb, rgb_ddata, rgb_odata, 3);

    for(int i = 0; i < bch; i++)
    {
      pick_color_rgb_mean[i] = rgb_odata[i];
      pick_color_rgb_min[i] = rgb_odata[i + 3];
      pick_color_rgb_max[i] = rgb_odata[i + 6];
    }
  }
  else
  {
    for(int i = 0; i < bch; i++)
    {
      pick_color_rgb_mean[i] = picked_color_rgb_mean[i];
      pick_color_rgb_min[i] = picked_color_rgb_min[i];
      pick_color_rgb_max[i] = picked_color_rgb_max[i];
    }
  }

  // Converting the RGB values to Lab
  if(xform_rgb2lab)
  {
    // Preparing the data for transformation
    float rgb_data[9] = { 0.0f };
    for(int i = 0; i < bch; i++)
    {
      rgb_data[i] = picked_color_rgb_mean[i];
      rgb_data[i + 3] = picked_color_rgb_min[i];
      rgb_data[i + 6] = picked_color_rgb_max[i];
    }
    
    if(ch == 1)
      for(int j = 0; j < 3; j++)
        rgb_data[3 * j + 1] = rgb_data[3 * j + 2] = rgb_data[3 * j];

    float Lab_data[9] = { 0.0f };
    cmsDoTransform(xform_rgb2lab, rgb_data, Lab_data, 3);

    for(int i = 0; i < bch; i++)
    {
      pick_color_lab_mean[i] = Lab_data[i];
      pick_color_lab_min[i] = Lab_data[i + 3];
      pick_color_lab_max[i] = Lab_data[i + 6];
    }
  }
  free(picked_color_rgb_min);
  free(picked_color_rgb_max);
  free(picked_color_rgb_mean);
  free(rgb);
}

static void _pixelpipe_pick_live_samples(const float *const input, const dt_iop_roi_t *roi_in, const int ch)
{
  cmsHPROFILE display_profile = NULL;
  cmsHPROFILE histogram_profile = NULL;
  cmsHPROFILE lab_profile = NULL;
  cmsHTRANSFORM xform_rgb2lab = NULL;
  cmsHTRANSFORM xform_rgb2rgb = NULL;
  dt_colorspaces_color_profile_type_t histogram_type = DT_COLORSPACE_SRGB;
  const gchar *histogram_filename = NULL;
  const gchar _histogram_filename[1] = { 0 };

  dt_ioppr_get_histogram_profile_type(&histogram_type, &histogram_filename);
  if(histogram_filename == NULL)
    histogram_filename = _histogram_filename;

  if(darktable.color_profiles->display_type == DT_COLORSPACE_DISPLAY || histogram_type == DT_COLORSPACE_DISPLAY)
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

  const dt_colorspaces_color_profile_t *d_profile = dt_colorspaces_get_profile(darktable.color_profiles->display_type,
                                                       darktable.color_profiles->display_filename,
                                                       DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY);
  if(d_profile)
    display_profile = d_profile->profile;

  if((histogram_type != darktable.color_profiles->display_type)
     || (histogram_type == DT_COLORSPACE_FILE
     && strcmp(histogram_filename, darktable.color_profiles->display_filename)))
  {
    const dt_colorspaces_color_profile_t *d_histogram = dt_colorspaces_get_profile(histogram_type,
                                                         histogram_filename,
                                                         DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY);                  
    if(d_histogram)
      histogram_profile = d_histogram->profile;
  }

  lab_profile = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;

  // display rgb --> lab
  if(display_profile && lab_profile)
    xform_rgb2lab = cmsCreateTransform(display_profile, TYPE_RGB_FLT, lab_profile, TYPE_Lab_FLT, INTENT_PERCEPTUAL, 0);

  // display rgb --> histogram rgb
  if(display_profile && histogram_profile)
    xform_rgb2rgb = cmsCreateTransform(display_profile, TYPE_RGB_FLT, histogram_profile, TYPE_RGB_FLT, INTENT_PERCEPTUAL, 0);

  if(darktable.color_profiles->display_type == DT_COLORSPACE_DISPLAY || histogram_type == DT_COLORSPACE_DISPLAY)
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

  dt_colorpicker_sample_t *sample = NULL;
  GSList *samples = darktable.lib->proxy.colorpicker.live_samples;

  while(samples)
  {
    sample = samples->data;

    if(sample->locked)
    {
      samples = g_slist_next(samples);
      continue;
    }

    _pixelpipe_pick_from_image(input, roi_in, xform_rgb2lab, xform_rgb2rgb,
        sample->box, sample->point, sample->size,
        sample->picked_color_rgb_min, sample->picked_color_rgb_max, sample->picked_color_rgb_mean,
        sample->picked_color_lab_min, sample->picked_color_lab_max, sample->picked_color_lab_mean, ch);

    samples = g_slist_next(samples);
  }

  if(xform_rgb2lab) cmsDeleteTransform(xform_rgb2lab);
  if(xform_rgb2rgb) cmsDeleteTransform(xform_rgb2rgb);
}

static void _pixelpipe_pick_primary_colorpicker(dt_develop_t *dev, const float *const input,
                                                const dt_iop_roi_t *roi_in, const int ch)
{
  cmsHPROFILE display_profile = NULL;
  cmsHPROFILE histogram_profile = NULL;
  cmsHPROFILE lab_profile = NULL;
  cmsHTRANSFORM xform_rgb2lab = NULL;
  cmsHTRANSFORM xform_rgb2rgb = NULL;
  dt_colorspaces_color_profile_type_t histogram_type = DT_COLORSPACE_SRGB;
  const gchar *histogram_filename = NULL;
  const gchar _histogram_filename[1] = { 0 };

  dt_ioppr_get_histogram_profile_type(&histogram_type, &histogram_filename);
  if(histogram_filename == NULL) histogram_filename = _histogram_filename;

  if(darktable.color_profiles->display_type == DT_COLORSPACE_DISPLAY || histogram_type == DT_COLORSPACE_DISPLAY)
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

  const dt_colorspaces_color_profile_t *d_profile = dt_colorspaces_get_profile(darktable.color_profiles->display_type,
                                                       darktable.color_profiles->display_filename,
                                                       DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY);
  if(d_profile) display_profile = d_profile->profile;

  if((histogram_type != darktable.color_profiles->display_type)
     || (histogram_type == DT_COLORSPACE_FILE
         && strcmp(histogram_filename, darktable.color_profiles->display_filename)))
  {
    const dt_colorspaces_color_profile_t *d_histogram = dt_colorspaces_get_profile(histogram_type,
                                                         histogram_filename,
                                                         DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY);
    if(d_histogram) histogram_profile = d_histogram->profile;
  }

  lab_profile = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;
  // display rgb --> lab
  if(display_profile && lab_profile)
    xform_rgb2lab = cmsCreateTransform(display_profile, TYPE_RGB_FLT, lab_profile, TYPE_Lab_FLT, INTENT_PERCEPTUAL, 0);
  // display rgb --> histogram rgb
  if(display_profile && histogram_profile)
    xform_rgb2rgb = cmsCreateTransform(display_profile, TYPE_RGB_FLT, histogram_profile, TYPE_RGB_FLT, INTENT_PERCEPTUAL, 0);

  if(darktable.color_profiles->display_type == DT_COLORSPACE_DISPLAY || histogram_type == DT_COLORSPACE_DISPLAY)
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

  _pixelpipe_pick_from_image(input, roi_in, xform_rgb2lab, xform_rgb2rgb,
      dev->gui_module->color_picker_box, dev->gui_module->color_picker_point, darktable.lib->proxy.colorpicker.size,
      darktable.lib->proxy.colorpicker.picked_color_rgb_min, darktable.lib->proxy.colorpicker.picked_color_rgb_max,
      darktable.lib->proxy.colorpicker.picked_color_rgb_mean, darktable.lib->proxy.colorpicker.picked_color_lab_min,
      darktable.lib->proxy.colorpicker.picked_color_lab_max, darktable.lib->proxy.colorpicker.picked_color_lab_mean, ch);

  if(xform_rgb2lab) cmsDeleteTransform(xform_rgb2lab);
  if(xform_rgb2rgb) cmsDeleteTransform(xform_rgb2rgb);
}

// returns 1 if blend process need the module default colorspace
static gboolean _transform_for_blend(const dt_iop_module_t *const self, const dt_dev_pixelpipe_iop_t *const piece)
{
 
  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *)piece->blendop_data;
  if(d) // check only if blend is active
    if((self->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && (d->mask_mode != DEVELOP_MASK_DISABLED))
      return TRUE;

  return FALSE;
}

static int pixelpipe_process_on_CPU(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                                    float *input, dt_iop_buffer_dsc_t *input_format, const dt_iop_roi_t *roi_in,
                                    void **output, dt_iop_buffer_dsc_t **out_format, const dt_iop_roi_t *roi_out,
                                    dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                    dt_develop_tiling_t *tiling, dt_pixelpipe_flow_t *pixelpipe_flow)
{
  // transform to module input colorspace
  int ch = piece->colors;
  dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height, input_format->cst,
                                      module->input_colorspace(module, pipe, piece), &input_format->cst,
                                      ch, dt_ioppr_get_pipe_work_profile_info(pipe));

  if(dt_atomic_get_int(&pipe->shutdown))
    return 1;

  const size_t in_bpp = dt_iop_buffer_dsc_to_bpp(input_format);
  const size_t bpp = dt_iop_buffer_dsc_to_bpp(*out_format);
  // process module on cpu. use tiling if needed and possible. 
  if(piece->process_tiling_ready
     && !dt_tiling_piece_fits_host_memory(MAX(roi_in->width, roi_out->width),
                                          MAX(roi_in->height, roi_out->height), MAX(in_bpp, bpp),
                                          tiling->factor, tiling->overhead))
  {
    module->process_tiling(module, piece, input, *output, roi_in, roi_out, in_bpp);
    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
  }
  else
  {
    module->process(module, piece, input, *output, roi_in, roi_out);
    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
  }
  // and save the output colorspace
  pipe->dsc.cst = module->output_colorspace(module, pipe, piece);
  ch = piece->colors;

  if(dt_atomic_get_int(&pipe->shutdown))
    return 1;
  // Lab color picking for module
  // pick from preview pipe to get pixels outside the viewport
  if(dev->gui_attached && pipe == dev->preview_pipe && module == dev->gui_module
     && module->request_color_pick != DT_REQUEST_COLORPICK_OFF && strcmp(module->op, "colorout"))
  {
    pixelpipe_picker(module, &piece->dsc_in, (float *)input, roi_in, module->picked_color,
                     module->picked_color_min, module->picked_color_max, input_format->cst, PIXELPIPE_PICKER_INPUT);
    pixelpipe_picker(module, &pipe->dsc, (float *)(*output), roi_out, module->picked_output_color,
                     module->picked_output_color_min, module->picked_output_color_max,
                     pipe->dsc.cst, PIXELPIPE_PICKER_OUTPUT);
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_PICKERDATA_READY, module, piece);
  }

  if(dt_atomic_get_int(&pipe->shutdown))
    return 1;
  // blend needs input/output images with default colorspace
  if(_transform_for_blend(module, piece))
  {
    dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height, input_format->cst,
                                        module->blend_colorspace(module, pipe, piece), &input_format->cst,
                                        ch, dt_ioppr_get_pipe_work_profile_info(pipe));

    dt_ioppr_transform_image_colorspace(module, *output, *output, roi_out->width, roi_out->height, pipe->dsc.cst,
                                        module->blend_colorspace(module, pipe, piece), &pipe->dsc.cst, 
                                        ch, dt_ioppr_get_pipe_work_profile_info(pipe));
  }

  if(dt_atomic_get_int(&pipe->shutdown))
    return 1;
  /* process blending on CPU */
  dt_develop_blend_process(module, piece, input, *output, roi_in, roi_out);
  *pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_CPU);

  return 0; //no errors
}

// recursive helper for process:
static int dt_dev_pixelpipe_process_rec(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void **output,
                                        dt_iop_buffer_dsc_t **out_format, const dt_iop_roi_t *roi_out, 
                                        GList *modules, GList *pieces, int pos, int *chan)
{
  dt_iop_roi_t roi_in = *roi_out;
  char module_name[256] = { 0 };
  void *input = NULL;
  dt_iop_module_t *module = NULL;
  dt_dev_pixelpipe_iop_t *piece = NULL;
  // if a module is active, check if this module allow a fast pipe run
  if(darktable.develop && dev->gui_module && dev->gui_module->flags() & IOP_FLAGS_ALLOW_FAST_PIPE)
    pipe->type |= DT_DEV_PIXELPIPE_FAST;
  else
    pipe->type &= ~DT_DEV_PIXELPIPE_FAST;

  if(modules)
  {
    module = (dt_iop_module_t *)modules->data;
    piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    piece->colors = *chan;
    piece->dsc_out.channels = *chan;
    piece->dsc_in.channels = *chan;
    // skip this module?
    if(!piece->enabled
       || (dev->gui_module && dev->gui_module->operation_tags_filter() & module->operation_tags()))
    {
      // recursion 1
      return dt_dev_pixelpipe_process_rec(pipe, dev, output, out_format, &roi_in, g_list_previous(modules),
                                          g_list_previous(pieces), pos - 1, chan);
    }
  }

  if(module)
    g_strlcpy(module_name, module->op, MIN(sizeof(module_name), sizeof(module->op)));
  get_output_format(module, pipe, piece, dev, *out_format);
  const size_t bpp = dt_iop_buffer_dsc_to_bpp(*out_format);
  const size_t bufsize = (size_t)bpp * roi_out->width * roi_out->height;
  // 1) if cached buffer is still available, return data
  if(dt_atomic_get_int(&pipe->shutdown))
    return 1;

  int cache_available = 0;
  uint64_t basichash = 0;
  uint64_t hash = 0;
  // do not get gamma from cache on preview pipe so we can compute the final histogram
  if((pipe->type & DT_DEV_PIXELPIPE_PREVIEW) != DT_DEV_PIXELPIPE_PREVIEW
                   || module == NULL || strcmp(module->op, "gamma") != 0)
  {
    dt_dev_pixelpipe_cache_fullhash(pipe->image.id, roi_out, pipe, pos, &basichash, &hash);
    cache_available = dt_dev_pixelpipe_cache_available(&(pipe->cache), hash);
  }
  if(cache_available)
  {
    (void)dt_dev_pixelpipe_cache_get(&(pipe->cache), basichash, hash, bufsize, output, out_format);
    if(!modules) 
      return 0;
    goto post_process_collect_info;
  }
  // 2) if history changed or exit event, abort processing?
  // preview pipe: abort on all but zoom events (same buffer anyways)
  if(dt_iop_breakpoint(dev, pipe))
    return 1;
  // if image has changed, stop now.
  if(pipe == dev->pipe && dev->image_force_reload)
    return 1;

  if(pipe == dev->preview_pipe && dev->preview_loading)
    return 1;

  if(pipe == dev->preview2_pipe && dev->preview2_loading)
    return 1;

  if(dev->gui_leaving)
    return 1;
  // 3) input -> output
  if(!modules)
  {
    // 3a) import input array with given scale and roi
    if(dt_atomic_get_int(&pipe->shutdown))
      return 1;

    dt_times_t start;
    dt_get_times(&start);
    // we're looking for the full buffer

    if(roi_out->scale == 1.0 && roi_out->x == 0 && roi_out->y == 0 && pipe->iwidth == roi_out->width
         && pipe->iheight == roi_out->height)
      *output = pipe->input;
    else if(dt_dev_pixelpipe_cache_get(&(pipe->cache), basichash, hash, bufsize, output, out_format))
    {
      memset(*output, 0, bufsize);
      
      if(roi_in.scale == 1.0f)
      {
        // fast branch for 1:1 pixel copies.
        // last minute clamping to catch potential out-of-bounds in roi_in and roi_out
        const int in_x = MAX(roi_in.x, 0);
        const int in_y = MAX(roi_in.y, 0);
        const int cp_width = MIN(roi_out->width, pipe->iwidth - in_x);
        const int cp_height = MIN(roi_out->height, pipe->iheight - in_y);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(bpp, cp_height, cp_width, in_x, in_y) \
        shared(pipe, roi_out, roi_in, output) \
        schedule(static)
#endif
        for(int j = 0; j < cp_height; j++)
          memcpy(((char *)*output) + (size_t)bpp * j * roi_out->width,
                   ((char *)pipe->input) + (size_t)bpp * (in_x + (in_y + j) * pipe->iwidth),
                   (size_t)bpp * cp_width);
      }
      else
      {
        roi_in.x /= roi_out->scale;
        roi_in.y /= roi_out->scale;
        roi_in.width = pipe->iwidth;
        roi_in.height = pipe->iheight;
        roi_in.scale = 1.0f;
        dt_iop_clip_and_zoom(*output, pipe->input, roi_out, &roi_in, roi_out->width, pipe->iwidth);
      }
    }
      // else found in cache.
    dt_show_times_f(&start, "[dev_pixelpipe]", "initing base buffer [%s]", _pipe_type_to_str(pipe->type));
  }
  else
  {
    // 3b) do recursion and obtain output array in &input
    if(dt_atomic_get_int(&pipe->shutdown))
           return 1;
    // get region of interest which is needed in input
    module->modify_roi_in(module, piece, roi_out, &roi_in);
    // recurse to get actual data of input buffer
    dt_iop_buffer_dsc_t _input_format = { 0 };
    dt_iop_buffer_dsc_t *input_format = &_input_format;

    piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    piece->processed_roi_in = roi_in;
    piece->processed_roi_out = *roi_out;

    // recursion 2, does not return result
    if(dt_dev_pixelpipe_process_rec(pipe, dev, &input, &input_format, &roi_in,
                        g_list_previous(modules), g_list_previous(pieces), pos - 1, chan))
      return 1;

    piece->colors = *chan;
    piece->dsc_out.channels = *chan;
    const size_t in_bpp = dt_iop_buffer_dsc_to_bpp(input_format);

    piece->dsc_out = piece->dsc_in = *input_format;
    module->output_format(module, pipe, piece, &piece->dsc_out);
    **out_format = pipe->dsc = piece->dsc_out;
    (**out_format).channels = piece->colors;
    pipe->colors = piece->colors;
    const size_t out_bpp = dt_iop_buffer_dsc_to_bpp(*out_format);

    if(dt_atomic_get_int(&pipe->shutdown))
      return 1;

    gboolean important = FALSE;

    if((pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW)
      important = (strcmp(module->op, "colorout") == 0);
    else
      important = (strcmp(module->op, "gamma") == 0);

    if(important)
      (void)dt_dev_pixelpipe_cache_get_important(&(pipe->cache), basichash, hash, bufsize, output, out_format);
    else
      (void)dt_dev_pixelpipe_cache_get(&(pipe->cache), basichash, hash, bufsize, output, out_format);

    if(dt_atomic_get_int(&pipe->shutdown))
      return 1;

    dt_times_t start;
    dt_get_times(&start);
    dt_pixelpipe_flow_t pixelpipe_flow = (PIXELPIPE_FLOW_NONE | PIXELPIPE_FLOW_HISTOGRAM_NONE);
    // special case: user requests to see channel data in the parametric mask of a module, or the blending
    // mask. In that case we skip all modules manipulating pixel content and only process image distorting
    // modules. Finally "gamma" is responsible for displaying channel/mask data accordingly.
    if(strcmp(module->op, "gamma") != 0
       && (pipe->mask_display & (DT_DEV_PIXELPIPE_DISPLAY_ANY | DT_DEV_PIXELPIPE_DISPLAY_MASK))
       && !(module->operation_tags() & IOP_TAG_DISTORT)
       && (in_bpp == out_bpp) && !memcmp(&roi_in, roi_out, sizeof(struct dt_iop_roi_t)))
    {
     // since we're not actually running the module, the output format is the same as the input format
      **out_format = pipe->dsc = piece->dsc_out = piece->dsc_in;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(in_bpp, out_bpp) \
      shared(roi_out, roi_in, output, input) \
      schedule(static)
#endif
      for(int j = 0; j < roi_out->height; j++)
            memcpy(((char *)*output) + (size_t)out_bpp * j * roi_out->width,
                   ((char *)input) + (size_t)in_bpp * j * roi_in.width,
                   (size_t)in_bpp * roi_in.width);

      return 0;
    }
    // get tiling requirement of module
    dt_develop_tiling_t tiling = { 0 };
    module->tiling_callback(module, piece, &roi_in, roi_out, &tiling);

    if(dt_atomic_get_int(&pipe->shutdown))
      return 1;
  
    if (pixelpipe_process_on_CPU(pipe, dev, input, input_format,  &roi_in, output, out_format,
                                 roi_out, module, piece, &tiling, &pixelpipe_flow))                                                                          
      return 1;

    char histogram_log[32] = "";
    *chan = piece->colors;
    piece->dsc_out.channels = piece->colors;
    
    if(!(pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_NONE))
      snprintf(histogram_log, sizeof(histogram_log), ", collected histogram on %s","CPU");

    gchar *module_label = dt_history_item_get_name(module);
    
    dt_show_times_f(
        &start, "[dev_pixelpipe]", "processed `%s' on %s%s%s, blended on %s", "via CPU", module_label, ", CPU",
        pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_WITH_TILING ? ", with tiling" : ", no tiling",
        (!(pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_NONE) && (piece->request_histogram & DT_REQUEST_ON))
            ? histogram_log : "");

    g_free(module_label);
    module_label = NULL;
    **out_format = piece->dsc_out = pipe->dsc;

    if(module == darktable.develop->gui_module)
    {
      // give the input buffer to the currently focused plugin more weight.
      // the user is likely to change that one soon, so keep it in cache.
      dt_dev_pixelpipe_cache_reweight(&(pipe->cache), input);
    }

post_process_collect_info:

    if(dt_atomic_get_int(&pipe->shutdown))
      return 1;

    const int ch = piece->colors;
    const int bch = ch < 4 ? ch : ch - 1;
    piece->dsc_out.channels = ch;
    // Picking RGB for the live samples and converting to Lab
    if(dev->gui_attached && pipe == dev->preview_pipe && (strcmp(module->op, "gamma") == 0)
       && darktable.lib->proxy.colorpicker.live_samples && input) // samples to pick
    {
      _pixelpipe_pick_live_samples((const float *const )input, &roi_in, ch);
    }

    if(dt_atomic_get_int(&pipe->shutdown))
      return 1;

    // Picking RGB for primary colorpicker output and converting to Lab
    if(dev->gui_attached && pipe == dev->preview_pipe
       && (strcmp(module->op, "gamma") == 0) // only gamma provides meaningful RGB data
       && dev->gui_module && !strcmp(dev->gui_module->op, "colorout")
       && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF
       && darktable.lib->proxy.colorpicker.picked_color_rgb_mean && input) // colorpicker module active
    {
      _pixelpipe_pick_primary_colorpicker(dev, (const float *const )input, &roi_in, ch);

      if(module->widget) 
        dt_control_queue_redraw_widget(module->widget);
    }

    // 4) final histogram:
    if(dt_atomic_get_int(&pipe->shutdown))
      return 1;

    if(dev->gui_attached && !dev->gui_leaving && pipe == dev->preview_pipe && (strcmp(module->op, "gamma") == 0))
    {
      if(input == NULL)
      {
        // input may not be available, so we use the output from gamma
        // this may lead to some rounding errors
        // FIXME: under what circumstances would input not be available? when this iop's result is pulled in from cache?
        float *const buf = dt_alloc_align(64, roi_out->width * roi_out->height * 4 * sizeof(float));
        if(buf)
        {
          const uint8_t *in = (uint8_t *)(*output);
          
          for(size_t k = 0; k < roi_out->width * roi_out->height * 4; k += 4)
            for(size_t c = 0; c < bch; c++)
              buf[k + c] = in[k + 2 - c] / 255.0f;

          darktable.lib->proxy.histogram.process(darktable.lib->proxy.histogram.module, buf,
                                                 roi_out->width, roi_out->height, DT_COLORSPACE_DISPLAY, "");
          dt_free_align(buf);
        }
      }
      else
        darktable.lib->proxy.histogram.process(darktable.lib->proxy.histogram.module, input,
                                               roi_in.width, roi_in.height, DT_COLORSPACE_DISPLAY, "");
    }
  }
  
  return 0;
}

int dt_dev_pixelpipe_process_no_gamma(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int x, int y, int width,
                                      int height, float scale)
{
  // temporarily disable gamma mapping.
  GList *gammap = g_list_last(pipe->nodes);
  dt_dev_pixelpipe_iop_t *gamma = (dt_dev_pixelpipe_iop_t *)gammap->data;
  while(strcmp(gamma->module->op, "gamma"))
  {
    gamma = NULL;
    gammap = g_list_previous(gammap);
    if(!gammap)
      break;

    gamma = (dt_dev_pixelpipe_iop_t *)gammap->data;
  }
  if(gamma)
    gamma->enabled = 0;

  const int ret = dt_dev_pixelpipe_process(pipe, dev, x, y, width, height, scale);
  if(gamma)
    gamma->enabled = 1;

  return ret;
}

void dt_dev_pixelpipe_disable_after(dt_dev_pixelpipe_t *pipe, const char *op)
{
  GList *nodes = g_list_last(pipe->nodes);
  dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  while(strcmp(piece->module->op, op))
  {
    piece->enabled = 0;
    piece = NULL;
    nodes = g_list_previous(nodes);
    if(!nodes) break;
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  }
}

void dt_dev_pixelpipe_disable_before(dt_dev_pixelpipe_t *pipe, const char *op)
{
  GList *nodes = pipe->nodes;
  dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  while(strcmp(piece->module->op, op))
  {
    piece->enabled = 0;
    piece = NULL;
    nodes = g_list_next(nodes);
    if(!nodes) break;
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  }
}

static int dt_dev_pixelpipe_process_rec_and_backcopy(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void **output,
                                                     dt_iop_buffer_dsc_t **out_format, const dt_iop_roi_t *roi_out, 
                                                     GList *modules, GList *pieces, int pos, int *chan)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  int ret = dt_dev_pixelpipe_process_rec(pipe, dev, output, out_format, roi_out, modules, pieces, pos, chan);
  dt_pthread_mutex_unlock(&pipe->busy_mutex);     
  return ret;
}

int dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int x, int y, int width, int height,
                             float scale)
{
  pipe->processing = 1;

  if(darktable.unmuted & DT_DEBUG_MEMORY)
  {
    fprintf(stderr, "[memory] before pixelpipe process\n");
    dt_print_mem_usage();
  }

  dt_iop_roi_t roi = (dt_iop_roi_t){ x, y, width, height, scale };
  // printf("pixelpipe homebrew process start\n");
  if(darktable.unmuted & DT_DEBUG_DEV)
    dt_dev_pixelpipe_cache_print(&pipe->cache);
  // get a snapshot of mask list
  if(pipe->forms)
    g_list_free_full(pipe->forms, (void (*)(void *))dt_masks_free_form);

  pipe->forms = dt_masks_dup_forms_deep(dev->forms, NULL);
  //  go through list of modules from the end:
  guint pos = g_list_length(pipe->iop);
  GList *modules = g_list_last(pipe->iop);
  GList *pieces = g_list_last(pipe->nodes);
  // check if we should obsolete caches
  if(pipe->cache_obsolete)
    dt_dev_pixelpipe_cache_flush(&(pipe->cache));

  pipe->cache_obsolete = 0;
  // mask display off as a starting point
  pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  // and blendif active
  pipe->bypass_blendif = 0;
  void *buf = NULL;
  dt_iop_buffer_dsc_t _out_format = { 0 };
  dt_iop_buffer_dsc_t *out_format = &_out_format;
  // run pixelpipe recursively and get error status
  int err = dt_dev_pixelpipe_process_rec_and_backcopy(pipe, dev, &buf, &out_format, &roi, modules,
                                                      pieces, pos, &(pipe->colors));
  {
    dt_pthread_mutex_lock(&pipe->busy_mutex);
    dt_pthread_mutex_unlock(&pipe->busy_mutex);

    dt_dev_pixelpipe_flush_caches(pipe);
    dt_dev_pixelpipe_change(pipe, dev);
  }
  // release resources:
  if(pipe->forms)
  {
    g_list_free_full(pipe->forms, (void (*)(void *))dt_masks_free_form);
    pipe->forms = NULL;
  }

  if(err)
  {
    pipe->processing = 0;
    return 1;
  }
  // terminate
  dt_pthread_mutex_lock(&pipe->backbuf_mutex);
  pipe->backbuf_hash = dt_dev_pixelpipe_cache_hash(pipe->image.id, &roi, pipe, 0);
  pipe->backbuf = buf;
  pipe->backbuf_width = width;
  pipe->backbuf_height = height;

  if((pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW
     || (pipe->type & DT_DEV_PIXELPIPE_FULL) == DT_DEV_PIXELPIPE_FULL
     || (pipe->type & DT_DEV_PIXELPIPE_PREVIEW2) == DT_DEV_PIXELPIPE_PREVIEW2)
  {
    if(pipe->output_backbuf == NULL || pipe->output_backbuf_width != pipe->backbuf_width
       || pipe->output_backbuf_height != pipe->backbuf_height)
    {
      g_free(pipe->output_backbuf);
      pipe->output_backbuf_width = pipe->backbuf_width;
      pipe->output_backbuf_height = pipe->backbuf_height;
      pipe->output_backbuf = g_malloc0((size_t)pipe->output_backbuf_width
                                       * pipe->output_backbuf_height * 4 * sizeof(uint8_t));
    }

    if(pipe->output_backbuf)
      memcpy(pipe->output_backbuf, pipe->backbuf,
             (size_t)pipe->output_backbuf_width * pipe->output_backbuf_height * 4 * sizeof(uint8_t));
    pipe->output_imgid = pipe->image.id;
  }
      
  dt_pthread_mutex_unlock(&pipe->backbuf_mutex);
  pipe->processing = 0;
  
  return 0;
}

void dt_dev_pixelpipe_flush_caches(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_flush(&pipe->cache);
}

void dt_dev_pixelpipe_get_dimensions(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, int width_in,
                                     int height_in, int *width, int *height)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  dt_iop_roi_t roi_in = (dt_iop_roi_t){ 0, 0, width_in, height_in, 1.0 };
  dt_iop_roi_t roi_out;
  GList *modules = pipe->iop;
  GList *pieces = pipe->nodes;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;

    piece->buf_in = roi_in;
    // skip this module?
    if(piece->enabled
       && !(dev->gui_module && dev->gui_module->operation_tags_filter() & module->operation_tags()))
       
      module->modify_roi_out(module, piece, &roi_out, &roi_in);
    else
      // pass through regions of interest for gui post expose events
      roi_out = roi_in;

    piece->buf_out = roi_out;
    roi_in = roi_out;

    modules = g_list_next(modules);
    pieces = g_list_next(pieces);
  }
  *width = roi_out.width;
  *height = roi_out.height;
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
}

float *dt_dev_get_raster_mask(const dt_dev_pixelpipe_t *pipe, const dt_iop_module_t *raster_mask_source,
                              const int raster_mask_id, const dt_iop_module_t *target_module,
                              gboolean *free_mask)
{
  if(!raster_mask_source)
    return NULL;

  *free_mask = FALSE;
  float *raster_mask = NULL;

  GList *source_iter;
  for(source_iter = pipe->nodes; source_iter; source_iter = g_list_next(source_iter))
  {
    const dt_dev_pixelpipe_iop_t *candidate = (dt_dev_pixelpipe_iop_t *)source_iter->data;
    if(candidate->module == raster_mask_source)
      break;
  }

  if(source_iter)
  {
    const dt_dev_pixelpipe_iop_t *source_piece = (dt_dev_pixelpipe_iop_t *)source_iter->data;
    if(source_piece && source_piece->enabled) // there might be stale masks from disabled modules left over. don't use those!
    {
      raster_mask = g_hash_table_lookup(source_piece->raster_masks, GINT_TO_POINTER(raster_mask_id));
      if(raster_mask)
      {
        for(GList *iter = g_list_next(source_iter); iter; iter = g_list_next(iter))
        {
          dt_dev_pixelpipe_iop_t *module = (dt_dev_pixelpipe_iop_t *)iter->data;

          if(module->enabled
            && !(module->module->dev->gui_module && module->module->dev->gui_module->operation_tags_filter()
                 & module->module->operation_tags()))
          {
            if(module->module->distort_mask
              && !(!strcmp(module->module->op, "finalscale") // hack against pipes not using finalscale
                    && module->processed_roi_in.width == 0
                    && module->processed_roi_in.height == 0))
            {
              float *transformed_mask = dt_alloc_align(64, sizeof(float)
                                                          * module->processed_roi_out.width
                                                          * module->processed_roi_out.height);
              module->module->distort_mask(module->module,
                                          module,
                                          raster_mask,
                                          transformed_mask,
                                          &module->processed_roi_in,
                                          &module->processed_roi_out);
              if(*free_mask) dt_free_align(raster_mask);
              *free_mask = TRUE;
              raster_mask = transformed_mask;
            }
            else if(!module->module->distort_mask &&
                    (module->processed_roi_in.width != module->processed_roi_out.width ||
                     module->processed_roi_in.height != module->processed_roi_out.height ||
                     module->processed_roi_in.x != module->processed_roi_out.x ||
                     module->processed_roi_in.y != module->processed_roi_out.y))
              printf("FIXME: module `%s' changed the roi from %d x %d @ %d / %d to %d x %d | %d / %d but doesn't have "
                     "distort_mask() implemented!\n", module->module->op, module->processed_roi_in.width,
                     module->processed_roi_in.height, module->processed_roi_in.x, module->processed_roi_in.y,
                     module->processed_roi_out.width, module->processed_roi_out.height, module->processed_roi_out.x,
                     module->processed_roi_out.y);
          }

          if(module->module == target_module)
            break;
        }
      }
    }
  }

  return raster_mask;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
