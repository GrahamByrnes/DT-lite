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
#include "control/jobs/control_jobs.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/film.h"
#include "common/history.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_dng.h"
#include "common/imageio_module.h"
#include "common/mipmap_cache.h"
#include "common/tags.h"
#include "common/undo.h"
#include "control/conf.h"
#include "develop/imageop_math.h"

#include "gui/gtk.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#ifndef _WIN32
#include <glob.h>
#endif
#ifdef __APPLE__
#include "osx/osx.h"
#endif
#ifdef _WIN32
#include "win/dtwin.h"
#endif

typedef struct dt_control_time_offset_t
{
  long int offset;
} dt_control_time_offset_t;

typedef struct dt_control_gpx_apply_t
{
  gchar *filename;
  gchar *tz;
} dt_control_gpx_apply_t;

typedef struct dt_control_export_t
{
  int max_width, max_height, format_index, storage_index;
  dt_imageio_module_data_t *sdata; // needed since the gui thread resets things like overwrite once the export
  // is dispatched, but we have to keep that information
  gboolean high_quality, upscale, export_masks;
  char style[128];
  gboolean style_append;
  dt_colorspaces_color_profile_type_t icc_type;
  gchar *icc_filename;
  dt_iop_color_intent_t icc_intent;
  gchar *metadata_export;
} dt_control_export_t;

typedef struct dt_control_image_enumerator_t
{
  GList *index;
  int flag;
  gpointer data;
} dt_control_image_enumerator_t;

static int32_t _generic_dt_control_fileop_images_job_run(dt_job_t *job,
                                                         int32_t (*fileop_callback)(const int32_t,
                                                                                    const int32_t),
                                                         const char *desc, const char *desc_pl)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  GList *t = params->index;
  const guint total = g_list_length(t);
  char message[512] = { 0 };
  double fraction = 0;
  gchar *newdir = (gchar *)params->data;

  g_snprintf(message, sizeof(message), ngettext(desc, desc_pl, total), total);
  dt_control_job_set_progress_message(job, message);

  // create new film roll for the destination directory
  dt_film_t new_film;
  const int32_t film_id = dt_film_new(&new_film, newdir);
  g_free(newdir);

  if(film_id <= 0)
  {
    dt_control_log(_("failed to create film roll for destination directory, aborting move.."));
    return -1;
  }

  gboolean completeSuccess = TRUE;
  while(t && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED)
  {
    completeSuccess &= (fileop_callback(GPOINTER_TO_INT(t->data), film_id) != -1);
    t = g_list_next(t);
    fraction += 1.0 / total;
    dt_control_job_set_progress(job, fraction);
  }

  if(completeSuccess)
  {
    char collect[1024];
    snprintf(collect, sizeof(collect), "1:0:0:%s$", new_film.dirname);
    dt_collection_deserialize(collect);
  }
  dt_film_remove_empty();
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, g_list_copy(params->index));
  dt_control_queue_redraw_center();
  return 0;
}

static void *dt_control_image_enumerator_alloc()
{
  dt_control_image_enumerator_t *params = calloc(1, sizeof(dt_control_image_enumerator_t));
  if(!params)
    return NULL;

  return params;
}

static void dt_control_image_enumerator_cleanup(void *p)
{
  dt_control_image_enumerator_t *params = p;
  g_list_free(params->index);
  params->index = NULL;
  free(params);
}

typedef enum {PROGRESS_NONE, PROGRESS_SIMPLE, PROGRESS_CANCELLABLE} progress_type_t;

static dt_job_t *dt_control_generic_images_job_create(dt_job_execute_callback execute, const char *message,
                                                      int flag, gpointer data, progress_type_t progress_type,
                                                      gboolean only_visible)
{
  dt_job_t *job = dt_control_job_create(execute, "%s", message);
  if(!job)
    return NULL;

  dt_control_image_enumerator_t *params = dt_control_image_enumerator_alloc();
  if(!params)
  {
    dt_control_job_dispose(job);
    return NULL;
  }
  if(progress_type != PROGRESS_NONE)
    dt_control_job_add_progress(job, _(message), progress_type == PROGRESS_CANCELLABLE);

  params->index = g_list_copy((GList *)dt_view_get_images_to_act_on(only_visible, TRUE));

  dt_control_job_set_params(job, params, dt_control_image_enumerator_cleanup);
  params->flag = flag;
  params->data = data;
  return job;
}

static dt_job_t *dt_control_generic_image_job_create(dt_job_execute_callback execute, const char *message,
                                                     int flag, gpointer data, progress_type_t progress_type,
                                                     int imgid)
{
  dt_job_t *job = dt_control_job_create(execute, "%s", message);
  if(!job)
    return NULL;

  dt_control_image_enumerator_t *params = dt_control_image_enumerator_alloc();

  if(!params)
  {
    dt_control_job_dispose(job);
    return NULL;
  }
  if(progress_type != PROGRESS_NONE)
    dt_control_job_add_progress(job, _(message), progress_type == PROGRESS_CANCELLABLE);

  params->index = g_list_append(NULL, GINT_TO_POINTER(imgid));
  dt_control_job_set_params(job, params, dt_control_image_enumerator_cleanup);

  params->flag = flag;
  params->data = data;
  return job;
}

static int32_t dt_control_write_sidecar_files_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = dt_control_job_get_params(job);
  GList *t = params->index;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images SET write_timestamp = STRFTIME('%s', 'now') WHERE id = ?1", -1,
                              &stmt, NULL);
  while(t)
  {
    gboolean from_cache = FALSE;
    const int imgid = GPOINTER_TO_INT(t->data);
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, (int32_t)imgid, 'r');
    char dtfilename[PATH_MAX] = { 0 };
    dt_image_full_path(img->id, dtfilename, sizeof(dtfilename), &from_cache);
    dt_image_path_append_version(img->id, dtfilename, sizeof(dtfilename));
    g_strlcat(dtfilename, ".xmp", sizeof(dtfilename));
    if(!dt_exif_xmp_write(imgid, dtfilename))
    {
      // put the timestamp into db. this can't be done in exif.cc since that code gets called
      // for the copy exporter, too
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
      sqlite3_clear_bindings(stmt);
    }
    dt_image_cache_read_release(darktable.image_cache, img);
    t = g_list_next(t);
  }
  sqlite3_finalize(stmt);
  return 0;
}

typedef struct dt_control_merge_hdr_t
{
  uint32_t first_imgid;
  uint32_t first_filter;
  uint8_t first_xtrans[6][6];

  float *pixels, *weight;
  int wd;
  int ht;
  dt_image_orientation_t orientation;
  float whitelevel;
  float epsw;
  // 0 - ok; 1 - errors, abort
  gboolean abort;
} dt_control_merge_hdr_t;

typedef struct dt_control_merge_hdr_format_t
{
  dt_imageio_module_data_t parent;
  dt_control_merge_hdr_t *d;
} dt_control_merge_hdr_format_t;

static int dt_control_merge_hdr_bpp(dt_imageio_module_data_t *data)
{
  return 32;
}

static int dt_control_merge_hdr_levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_FLOAT;
}

static const char *dt_control_merge_hdr_mime(dt_imageio_module_data_t *data)
{
  return "memory";
}

static float envelope(const float xx)
{
  const float x = CLAMPS(xx, 0.0f, 1.0f);
  // const float alpha = 2.0f;
  const float beta = 0.5f;
  if(x < beta)
  {
    // return 1.0f-fabsf(x/beta-1.0f)^2
    const float tmp = fabsf(x / beta - 1.0f);
    return 1.0f - tmp * tmp;
  }
  else
  {
    const float tmp1 = (1.0f - x) / (1.0f - beta);
    const float tmp2 = tmp1 * tmp1;
    const float tmp3 = tmp2 * tmp1;
    return 3.0f * tmp2 - 2.0f * tmp3;
  }
}

static int dt_control_merge_hdr_process(dt_imageio_module_data_t *datai, const char *filename,
                                        const void *const ivoid,
                                        dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                                        void *exif, int exif_len, int imgid, int num, int total,
                                        dt_dev_pixelpipe_t *pipe, const gboolean export_masks)
{
  dt_control_merge_hdr_format_t *data = (dt_control_merge_hdr_format_t *)datai;
  dt_control_merge_hdr_t *d = data->d;

  // just take a copy. also do it after blocking read, so filters will make sense.
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  const dt_image_t image = *img;
  dt_image_cache_read_release(darktable.image_cache, img);

  if(!d->pixels)
  {
    d->first_imgid = imgid;
    d->first_filter = image.buf_dsc.filters;
    // sensor layout is just passed on to be written to dng.
    // we offset it to the crop of the image here, so we don't
    // need to load in the FCxtrans dependency into the dng writer.
    // for some stupid reason the dng needs this layout wrt cropped
    // offsets, not globally.
    dt_iop_roi_t roi = {0};
    roi.x = image.crop_x;
    roi.y = image.crop_y;
    for(int j=0;j<6;j++)
      for(int i = 0; i < 6; i++) d->first_xtrans[j][i] = FCxtrans(j, i, &roi, image.buf_dsc.xtrans);
    d->pixels = calloc(datai->width * datai->height, sizeof(float));
    d->weight = calloc(datai->width * datai->height, sizeof(float));
    d->wd = datai->width;
    d->ht = datai->height;
    d->orientation = image.orientation;
  }

  if(image.buf_dsc.filters == 0u || image.buf_dsc.channels != 1 || image.buf_dsc.datatype != TYPE_UINT16)
  {
    dt_control_log(_("exposure bracketing only works on raw images."));
    d->abort = TRUE;
    return 1;
  }
  else if(datai->width != d->wd || datai->height != d->ht || d->first_filter != image.buf_dsc.filters
          || d->orientation != image.orientation)
  {
    dt_control_log(_("images have to be of same size and orientation!"));
    d->abort = TRUE;
    return 1;
  }

  // if no valid exif data can be found, assume peleng fisheye at f/16, 8mm, with half of the light lost in
  // the system => f/22
  const float eap = image.exif_aperture > 0.0f ? image.exif_aperture : 22.0f;
  const float efl = image.exif_focal_length > 0.0f ? image.exif_focal_length : 8.0f;
  const float rad = .5f * efl / eap;
  const float aperture = M_PI * rad * rad;
  const float iso = image.exif_iso > 0.0f ? image.exif_iso : 100.0f;
  const float exp = image.exif_exposure > 0.0f ? image.exif_exposure : 1.0f;
  const float cal = 100.0f / (aperture * exp * iso);
  // about proportional to how many photons we can expect from this shot:
  const float photoncnt = 100.0f * aperture * exp / iso;
  float saturation = 1.0f;
  d->whitelevel = fmaxf(d->whitelevel, saturation * cal);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ivoid, cal, photoncnt) \
  shared(d, saturation) \
  schedule(static)
#endif
  for(int y = 0; y < d->ht; y++)
    for(int x = 0; x < d->wd; x++)
    {
      // read unclamped raw value with subtracted black and rescaled to 1.0 saturation.
      // this is the output of the rawprepare iop.
      const float in = ((float *)ivoid)[x + d->wd * y];
      // weights based on siggraph 12 poster
      // zijian zhu, zhengguo li, susanto rahardja, pasi fraenti
      // 2d denoising factor for high dynamic range imaging
      float w = photoncnt;

      // need some safety margin due to upsampling and 16-bit quantization + dithering?
      float offset = 3000.0f / (float)UINT16_MAX;

      // cannot do an envelope based on single pixel values here, need to get
      // maximum value of all color channels. to find that, go through the
      // pattern block (we conservatively do a 3x3 for bayer or xtrans):
      int xx = x & ~1, yy = y & ~1;
      float M = 0.0f, m = FLT_MAX;
      if(xx < d->wd - 2 && yy < d->ht - 2)
      {
        for(int i = 0; i < 3; i++)
          for(int j = 0; j < 3; j++)
          {
            M = MAX(M, ((float *)ivoid)[xx + i + d->wd * (yy + j)]);
            m = MIN(m, ((float *)ivoid)[xx + i + d->wd * (yy + j)]);
          }
        // move envelope a little to allow non-zero weight even for clipped regions.
        // this is because even if the 2x2 block is clipped somewhere, the other channels
        // might still prove useful. we'll check for individual channel saturation below.
        w *= d->epsw + envelope((M + offset) / saturation);
      }

      if(M + offset >= saturation)
      {
        if(d->weight[x + d->wd * y] <= 0.0f)
        { // only consider saturated pixels in case we have nothing better:
          if(d->weight[x + d->wd * y] == 0 || m < -d->weight[x + d->wd * y])
          {
            if(m + offset >= saturation)
              d->pixels[x + d->wd * y] = 1.0f; // let's admit we were completely clipped, too
            else
              d->pixels[x + d->wd * y] = in * cal / d->whitelevel;
            d->weight[x + d->wd * y]
                = -m; // could use -cal here, but m is per pixel and safer for varying illumination conditions
          }
        }
        // else silently ignore, others have filled in a better color here already
      }
      else
      {
        if(d->weight[x + d->wd * y] <= 0.0)
        { // cleanup potentially blown highlights from earlier images
          d->pixels[x + d->wd * y] = 0.0f;
          d->weight[x + d->wd * y] = 0.0f;
        }
        d->pixels[x + d->wd * y] += w * in * cal;
        d->weight[x + d->wd * y] += w;
      }
    }

  return 0;
}

static int32_t dt_control_merge_hdr_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = dt_control_job_get_params(job);
  GList *t = params->index;
  const guint total = g_list_length(t);
  char message[512] = { 0 };
  double fraction = 0;
  snprintf(message, sizeof(message), ngettext("merging %d image", "merging %d images", total), total);

  dt_control_job_set_progress_message(job, message);
  dt_control_merge_hdr_t d = (dt_control_merge_hdr_t){.epsw = 1e-8f, .abort = FALSE };
  dt_imageio_module_format_t buf = (dt_imageio_module_format_t){.mime = dt_control_merge_hdr_mime,
                                                                .levels = dt_control_merge_hdr_levels,
                                                                .bpp = dt_control_merge_hdr_bpp,
                                                                .write_image = dt_control_merge_hdr_process };
  dt_control_merge_hdr_format_t dat = (dt_control_merge_hdr_format_t){.parent = { 0 }, .d = &d };

  int num = 1;

  while(t)
  {
    if(d.abort)
      goto end;

    const uint32_t imgid = GPOINTER_TO_INT(t->data);

    dt_imageio_export_with_flags(imgid, "unused", &buf, (dt_imageio_module_data_t *)&dat, TRUE, FALSE, FALSE, TRUE,
                                 FALSE, "pre:rawprepare", FALSE, FALSE, DT_COLORSPACE_NONE, NULL, DT_INTENT_LAST, NULL,
                                 NULL, num, total, NULL);
    t = g_list_next(t);
    /* update the progress bar */
    fraction += 1.0 / (total + 1);
    dt_control_job_set_progress(job, fraction);
    num++;
  }

  if(d.abort)
    goto end;

// normalize by white level to make clipping at 1.0 work as expected
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(d)
#endif
  for(size_t k = 0; k < (size_t)d.wd * d.ht; k++)
    if(d.weight[k] > 0.0)
      d.pixels[k] = fmaxf(0.0f, d.pixels[k] / (d.whitelevel * d.weight[k]));

  // output hdr as digital negative with exif data.
  uint8_t *exif = NULL;
  char pathname[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(d.first_imgid, pathname, sizeof(pathname), &from_cache);

  // last param is dng mode
  const int exif_len = dt_exif_read_blob(&exif, pathname, d.first_imgid, 0, d.wd, d.ht, 1);
  char *c = pathname + strlen(pathname);
  while(*c != '.' && c > pathname)
    c--;
  g_strlcpy(c, "-hdr.dng", sizeof(pathname) - (c - pathname));
  dt_imageio_write_dng(pathname, d.pixels, d.wd, d.ht, exif, exif_len, d.first_filter, (const uint8_t (*)[6])d.first_xtrans, 1.0f);
  free(exif);

  dt_control_job_set_progress(job, 1.0);

  while(*c != '/' && c > pathname)
    c--;
  dt_control_log(_("wrote merged HDR `%s'"), c + 1);
  // import new image
  gchar *directory = g_path_get_dirname((const gchar *)pathname);
  dt_film_t film;
  const int filmid = dt_film_new(&film, directory);
  const uint32_t imageid = dt_image_import(filmid, pathname, TRUE);
  g_free(directory);

  // refresh the thumbtable view
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, g_list_append(NULL, GINT_TO_POINTER(imageid)));
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_control_queue_redraw_center();

end:
  free(d.pixels);
  free(d.weight);

  return 0;
}

static int32_t dt_control_duplicate_images_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = dt_control_job_get_params(job);
  GList *t = params->index;
  const guint total = g_list_length(t);
  double fraction = 0.0f;
  char message[512] = { 0 };
  snprintf(message, sizeof(message), ngettext("duplicating %d image", "duplicating %d images", total), total);
  dt_control_job_set_progress_message(job, message);
  while(t)
  {
    const int imgid = GPOINTER_TO_INT(t->data);
    const int newimgid = dt_image_duplicate(imgid);
    if(newimgid != -1)
    {
      dt_history_copy_and_paste_on_image(imgid, newimgid, FALSE, NULL, TRUE, TRUE);
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, NULL);
    }
    t = g_list_next(t);
    fraction += 1.0 / total;
    dt_control_job_set_progress(job, fraction);
  }
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_control_queue_redraw_center();
  return 0;
}

static int32_t dt_control_flip_images_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = dt_control_job_get_params(job);
  const int cw = params->flag;
  GList *t = params->index;
  const guint total = g_list_length(t);
  double fraction = 0.0f;
  char message[512] = { 0 };

  dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);

  snprintf(message, sizeof(message), ngettext("flipping %d image", "flipping %d images", total), total);
  dt_control_job_set_progress_message(job, message);
  while(t)
  {
    const int imgid = GPOINTER_TO_INT(t->data);
    dt_image_flip(imgid, cw);
    t = g_list_next(t);
    fraction += 1.0 / total;
    dt_image_set_aspect_ratio(imgid, FALSE);
    dt_control_job_set_progress(job, fraction);
  }

  dt_undo_end_group(darktable.undo);

  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, g_list_copy(params->index));
  dt_control_queue_redraw_center();
  return 0;
}

static char *_get_image_list(GList *l)
{
  const guint size = g_list_length(l);
  char num[8];
  char *buffer = calloc(size, sizeof(num));
  gboolean first = TRUE;

  buffer[0] = '\0';

  while(l)
  {
    const int imgid = GPOINTER_TO_INT(l->data);
    snprintf(num, sizeof(num), "%s%6d", first ? "" : ",", imgid);
    g_strlcat(buffer, num, size * sizeof(num));
    l = g_list_next(l);
    first = FALSE;
  }
  return buffer;
}

static void _set_remove_flag(char *imgs)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images SET flags = (flags|?1) WHERE id IN (?2)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, DT_IMAGE_REMOVE);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgs, -1, SQLITE_STATIC);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static GList *_get_full_pathname(char *imgs)
{
  sqlite3_stmt *stmt = NULL;
  GList *list = NULL;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT DISTINCT folder || '" G_DIR_SEPARATOR_S "' || filename FROM "
                                                             "main.images i, main.film_rolls f "
                                                             "ON i.film_id = f.id WHERE i.id IN (?1)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, imgs, -1, SQLITE_STATIC);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    list = g_list_append(list, g_strdup((const gchar *)sqlite3_column_text(stmt, 0)));
  }
  sqlite3_finalize(stmt);
  return list;
}

static int32_t dt_control_remove_images_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = dt_control_job_get_params(job);
  GList *t = params->index;
  char *imgs = _get_image_list(t);
  guint total = g_list_length(t);
  char message[512] = { 0 };
  snprintf(message, sizeof(message), ngettext("removing %d image", "removing %d images", total), total);
  dt_control_job_set_progress_message(job, message);
  sqlite3_stmt *stmt = NULL;

  // check that we can safely remove the image
  gboolean remove_ok = TRUE;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM main.images WHERE id IN (?2) AND flags&?1=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, DT_IMAGE_LOCAL_COPY);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgs, -1, SQLITE_STATIC);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int imgid = sqlite3_column_int(stmt, 0);
    if(!dt_image_safe_remove(imgid))
    {
      remove_ok = FALSE;
      break;
    }
  }
  sqlite3_finalize(stmt);

  if(!remove_ok)
  {
    dt_control_log(_("cannot remove local copy when the original file is not accessible."));
    free(imgs);
    return 0;
  }

  // update remove status
  _set_remove_flag(imgs);
  dt_collection_update(darktable.collection);
  // We need a list of files to regenerate .xmp files if there are duplicates
  GList *list = _get_full_pathname(imgs);
  free(imgs);
  double fraction = 0.0f;

  while(t)
  {
    int imgid = GPOINTER_TO_INT(t->data);
    dt_image_remove(imgid);
    t = g_list_next(t);
    fraction += 1.0 / total;
    dt_control_job_set_progress(job, fraction);
  }

  while(list)
  {
    char *imgname = (char *)list->data;
    dt_image_synch_all_xmp(imgname);
    list = g_list_delete_link(list, list);
  }

  dt_film_remove_empty();
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, g_list_copy(params->index));
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_control_queue_redraw_center();

  return 0;
}

typedef struct _dt_delete_modal_dialog_t
{
  int send_to_trash;
  const char *filename;
  const char *error_message;

  gint dialog_result;

  dt_pthread_mutex_t mutex;
  pthread_cond_t cond;
} _dt_delete_modal_dialog_t;

enum _dt_delete_status
{
  _DT_DELETE_STATUS_UNKNOWN = 0,
  _DT_DELETE_STATUS_OK_TO_REMOVE = 1,
  _DT_DELETE_STATUS_SKIP_FILE = 2,
  _DT_DELETE_STATUS_STOP_PROCESSING = 3
};

enum _dt_delete_dialog_choice
{
  _DT_DELETE_DIALOG_CHOICE_DELETE = 1,
  _DT_DELETE_DIALOG_CHOICE_DELETE_ALL = 2,
  _DT_DELETE_DIALOG_CHOICE_REMOVE = 3,
  _DT_DELETE_DIALOG_CHOICE_CONTINUE = 4,
  _DT_DELETE_DIALOG_CHOICE_STOP = 5
};

static gboolean _dt_delete_dialog_main_thread(gpointer user_data)
{
  _dt_delete_modal_dialog_t* modal_dialog = (_dt_delete_modal_dialog_t*)user_data;
  dt_pthread_mutex_lock(&modal_dialog->mutex);

  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
      GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_QUESTION,
      GTK_BUTTONS_NONE,
      modal_dialog->send_to_trash
        ? _("could not send %s to trash%s%s")
        : _("could not physically delete %s%s%s"),
      modal_dialog->filename,
      modal_dialog->error_message != NULL ? ": " : "",
      modal_dialog->error_message != NULL ? modal_dialog->error_message : "");
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif

  if (modal_dialog->send_to_trash)
  {
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("physically delete"), _DT_DELETE_DIALOG_CHOICE_DELETE);
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("physically delete all files"), _DT_DELETE_DIALOG_CHOICE_DELETE_ALL);
  }

  gtk_dialog_add_button(GTK_DIALOG(dialog), _("only remove from the collection"), _DT_DELETE_DIALOG_CHOICE_REMOVE);
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("skip to next file"), _DT_DELETE_DIALOG_CHOICE_CONTINUE);
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("stop process"), _DT_DELETE_DIALOG_CHOICE_STOP);

  gtk_window_set_title(
      GTK_WINDOW(dialog),
      modal_dialog->send_to_trash
        ? _("trashing error")
        : _("deletion error"));
  modal_dialog->dialog_result = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
  pthread_cond_signal(&modal_dialog->cond);
  dt_pthread_mutex_unlock(&modal_dialog->mutex);
  // Don't call again on next idle time
  return FALSE;
}

static gint _dt_delete_file_display_modal_dialog(int send_to_trash, const char *filename, const char *error_message)
{
  _dt_delete_modal_dialog_t modal_dialog;
  modal_dialog.send_to_trash = send_to_trash;
  modal_dialog.filename = filename;
  modal_dialog.error_message = error_message;
  modal_dialog.dialog_result = GTK_RESPONSE_NONE;

  dt_pthread_mutex_init(&modal_dialog.mutex, NULL);
  pthread_cond_init(&modal_dialog.cond, NULL);
  dt_pthread_mutex_lock(&modal_dialog.mutex);
  gdk_threads_add_idle(_dt_delete_dialog_main_thread, &modal_dialog);

  while (modal_dialog.dialog_result == GTK_RESPONSE_NONE)
    dt_pthread_cond_wait(&modal_dialog.cond, &modal_dialog.mutex);

  dt_pthread_mutex_unlock(&modal_dialog.mutex);
  dt_pthread_mutex_destroy(&modal_dialog.mutex);
  pthread_cond_destroy(&modal_dialog.cond);

  return modal_dialog.dialog_result;
}

static enum _dt_delete_status delete_file_from_disk(const char *filename, gboolean *delete_on_trash_error)
{
  enum _dt_delete_status delete_status = _DT_DELETE_STATUS_UNKNOWN;
  GFile *gfile = g_file_new_for_path(filename);
  int send_to_trash = dt_conf_get_bool("send_to_trash");

  while (delete_status == _DT_DELETE_STATUS_UNKNOWN)
  {
    gboolean delete_success = FALSE;
    GError *gerror = NULL;
    if (send_to_trash)
    {
#ifdef __APPLE__
      delete_success = dt_osx_file_trash(filename, &gerror);
#elif defined(_WIN32)
      delete_success = dt_win_file_trash(gfile, NULL /*cancellable*/, &gerror);
#else
      delete_success = g_file_trash(gfile, NULL /*cancellable*/, &gerror);
#endif
    }
    else
      delete_success = g_file_delete(gfile, NULL /*cancellable*/, &gerror);

    // Delete is a success or the file does not exists: OK to remove from collection
    if (delete_success || g_error_matches(gerror, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
      delete_status = _DT_DELETE_STATUS_OK_TO_REMOVE;
    else if (send_to_trash && *delete_on_trash_error)
    {
      // Loop again, this time delete instead of trashing
      delete_status = _DT_DELETE_STATUS_UNKNOWN;
      send_to_trash = FALSE;
    }
    else
    {
      const char *filename_display = NULL;
      GFileInfo *gfileinfo = g_file_query_info(
          gfile,
          G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
          G_FILE_QUERY_INFO_NONE,
          NULL /*cancellable*/,
          NULL /*error*/);
      if (gfileinfo != NULL)
        filename_display = g_file_info_get_attribute_string(
            gfileinfo,
            G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME);

      gint res = _dt_delete_file_display_modal_dialog(
          send_to_trash,
          filename_display == NULL ? filename : filename_display,
          gerror == NULL ? NULL : gerror->message);

      if (send_to_trash && res == _DT_DELETE_DIALOG_CHOICE_DELETE)
      {
        // Loop again, this time delete instead of trashing
        delete_status = _DT_DELETE_STATUS_UNKNOWN;
        send_to_trash = FALSE;
      }
      else if (send_to_trash && res == _DT_DELETE_DIALOG_CHOICE_DELETE_ALL)
      {
        // Loop again, this time delete instead of trashing
        delete_status = _DT_DELETE_STATUS_UNKNOWN;
        send_to_trash = FALSE;
        *delete_on_trash_error = TRUE;
      }
      else if (res == _DT_DELETE_DIALOG_CHOICE_REMOVE)
        delete_status = _DT_DELETE_STATUS_OK_TO_REMOVE;
      else if (res == _DT_DELETE_DIALOG_CHOICE_CONTINUE)
        delete_status = _DT_DELETE_STATUS_SKIP_FILE;
      else
        delete_status = _DT_DELETE_STATUS_STOP_PROCESSING;
    }
    if (gerror != NULL)
      g_error_free(gerror);
  }

  if (gfile != NULL)
    g_object_unref(gfile);

  return delete_status;
}


static int32_t dt_control_delete_images_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = dt_control_job_get_params(job);
  GList *t = params->index;
  char *imgs = _get_image_list(t);
  char imgidstr[25] = { 0 };
  guint total = g_list_length(t);
  double fraction = 0.0f;
  char message[512] = { 0 };
  gboolean delete_on_trash_error = FALSE;

  if (dt_conf_get_bool("send_to_trash"))
    snprintf(message, sizeof(message), ngettext("trashing %d image", "trashing %d images", total), total);
  else
    snprintf(message, sizeof(message), ngettext("deleting %d image", "deleting %d images", total), total);

  dt_control_job_set_progress_message(job, message);
  sqlite3_stmt *stmt;
  dt_collection_update(darktable.collection);

  // We need a list of files to regenerate .xmp files if there are duplicates
  GList *list = _get_full_pathname(imgs);

  free(imgs);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM main.images WHERE filename IN (SELECT filename FROM "
                              "main.images WHERE id = ?1) AND film_id IN (SELECT film_id FROM main.images WHERE "
                              "id = ?1)", -1, &stmt, NULL);
  while(t)
  {
    enum _dt_delete_status delete_status = _DT_DELETE_STATUS_UNKNOWN;
    const int imgid = GPOINTER_TO_INT(t->data);
    char filename[PATH_MAX] = { 0 };
    gboolean from_cache = FALSE;
    dt_image_full_path(imgid, filename, sizeof(filename), &from_cache);

#ifdef _WIN32
    char *dirname = g_path_get_dirname(filename);
#endif

    int duplicates = 0;
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW) duplicates = sqlite3_column_int(stmt, 0);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    // remove from disk:
    if(duplicates == 1)
    {
      // first check for local copies, never delete a file whose original file is not accessible
      if (dt_image_local_copy_reset(imgid))
        goto delete_next_file;

      snprintf(imgidstr, sizeof(imgidstr), "%d", imgid);
      _set_remove_flag(imgidstr);
      dt_image_remove(imgid);

      // there are no further duplicates so we can remove the source data file
      delete_status = delete_file_from_disk(filename, &delete_on_trash_error);
      if (delete_status != _DT_DELETE_STATUS_OK_TO_REMOVE)
        goto delete_next_file;

      // all sidecar files - including left-overs - can be deleted;
      // left-overs can result when previously duplicates have been REMOVED;
      // no need to keep them as the source data file is gone.

      GList *files = dt_image_find_duplicates(filename);

      GList *file_iter = g_list_first(files);
      while(file_iter != NULL)
      {
        delete_status = delete_file_from_disk(file_iter->data, &delete_on_trash_error);
        if (delete_status != _DT_DELETE_STATUS_OK_TO_REMOVE)
          break;
        file_iter = g_list_next(file_iter);
      }

      g_list_free_full(files, g_free);
    }
    else
    {
      // don't remove the actual source data if there are further duplicates using it;
      // just delete the xmp file of the duplicate selected.
      dt_image_path_append_version(imgid, filename, sizeof(filename));
      g_strlcat(filename, ".xmp", sizeof(filename));
      // remove image from db first ...
      snprintf(imgidstr, sizeof(imgidstr), "%d", imgid);
      _set_remove_flag(imgidstr);
      dt_image_remove(imgid);
      // ... and delete afterwards because removing will re-write the XMP
      delete_status = delete_file_from_disk(filename, &delete_on_trash_error);
    }

delete_next_file:
#ifdef _WIN32
    g_free(dirname);
#endif
    t = g_list_next(t);
    fraction += 1.0 / total;
    dt_control_job_set_progress(job, fraction);
    if (delete_status == _DT_DELETE_STATUS_STOP_PROCESSING)
      break;
  }

  sqlite3_finalize(stmt);

  while(list)
  {
    char *imgname = (char *)list->data;
    dt_image_synch_all_xmp(imgname);
    list = g_list_delete_link(list, list);
  }
  g_list_free(list);
  dt_film_remove_empty();
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, g_list_copy(params->index));
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_control_queue_redraw_center();
  return 0;
}

static int32_t dt_control_move_images_job_run(dt_job_t *job)
{
  return _generic_dt_control_fileop_images_job_run(job, &dt_image_move, _("moving %d image"),
                                                   _("moving %d images"));
}

static int32_t dt_control_copy_images_job_run(dt_job_t *job)
{
  return _generic_dt_control_fileop_images_job_run(job, &dt_image_copy, _("copying %d image"),
                                                   _("copying %d images"));
}

static int32_t dt_control_local_copy_images_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  GList *t = params->index;
  guint tagid = 0;
  const guint total = g_list_length(t);
  double fraction = 0;
  const gboolean is_copy = params->flag == 1;
  char message[512] = { 0 };

  if(is_copy)
    snprintf(message, sizeof(message),
             ngettext("creating local copy of %d image", "creating local copies of %d images", total), total);
  else
    snprintf(message, sizeof(message),
             ngettext("removing local copy of %d image", "removing local copies of %d images", total), total);

  dt_control_log("%s", message);
  dt_control_job_set_progress_message(job, message);
  dt_tag_new("darktable|local-copy", &tagid);

  gboolean tag_change = FALSE;
  while(t && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED)
  {
    const int imgid = GPOINTER_TO_INT(t->data);

    if(is_copy)
    {
      if(dt_image_local_copy_set(imgid) == 0)
        if(dt_tag_attach(tagid, imgid, FALSE, FALSE))
          tag_change = TRUE;
    }
    else if(dt_image_local_copy_reset(imgid) == 0)
      if(dt_tag_detach(tagid, imgid, FALSE, FALSE)) tag_change = TRUE;

    t = g_list_next(t);
    fraction += 1.0 / total;
    dt_control_job_set_progress(job, fraction);
  }

  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, g_list_copy(params->index));

  if(tag_change) 
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);

  dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_control_queue_redraw_center();
  return 0;
}

static int32_t dt_control_refresh_exif_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  GList *t = params->index;
  guint total = g_list_length(t);
  double fraction = 0.0f;
  char message[512] = { 0 };
  snprintf(message, sizeof(message), ngettext("refreshing info for %d image", "refreshing info for %d images", total), total);
  dt_control_job_set_progress_message(job, message);
  while(t)
  {
    const int imgid = GPOINTER_TO_INT(t->data);
    if(imgid >= 0)
    {
      gboolean from_cache = TRUE;
      char sourcefile[PATH_MAX];
      dt_image_full_path(imgid, sourcefile, sizeof(sourcefile), &from_cache);

      dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
      if(img)
      {
        dt_exif_read(img, sourcefile);
        dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_SAFE);
      }
      else
        fprintf(stderr,"[dt_control_refresh_exif_run] couldn't dt_image_cache_get for imgid %i\n", imgid);

      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED);
    }
    else
      fprintf(stderr,"[dt_control_refresh_exif_run] illegal imgid %i\n", imgid);

    t = g_list_next(t);
    fraction += 1.0 / total;
    dt_control_job_set_progress(job, fraction);
  }
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, g_list_copy(params->index));
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  dt_control_queue_redraw_center();
  return 0;
}

static int32_t dt_control_export_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  dt_control_export_t *settings = (dt_control_export_t *)params->data;
  GList *t = params->index;
  dt_imageio_module_format_t *mformat = dt_imageio_get_format_by_index(settings->format_index);
  g_assert(mformat);
  dt_imageio_module_storage_t *mstorage = dt_imageio_get_storage_by_index(settings->storage_index);
  g_assert(mstorage);
  dt_imageio_module_data_t *sdata = settings->sdata;

  gboolean tag_change = FALSE;
  // get a thread-safe fdata struct (one jpeg struct per thread etc):
  dt_imageio_module_data_t *fdata = mformat->get_params(mformat);

  if(mstorage->initialize_store)
  {
    if(mstorage->initialize_store(mstorage, sdata, &mformat, &fdata, &t, settings->high_quality, settings->upscale))
      goto end; // bail out, something went wrong

    mformat->set_params(mformat, fdata, mformat->params_size(mformat));
    mstorage->set_params(mstorage, sdata, mstorage->params_size(mstorage));
  }
  // Get max dimensions...
  uint32_t w, h, fw, fh, sw, sh;
  fw = fh = sw = sh = 0;
  mstorage->dimension(mstorage, sdata, &sw, &sh);
  mformat->dimension(mformat, fdata, &fw, &fh);

  if(sw == 0 || fw == 0)
    w = sw > fw ? sw : fw;
  else
    w = sw < fw ? sw : fw;

  if(sh == 0 || fh == 0)
    h = sh > fh ? sh : fh;
  else
    h = sh < fh ? sh : fh;

  const guint total = g_list_length(t);
  dt_control_log(ngettext("exporting %d image..", "exporting %d images..", total), total);
  double fraction = 0;
  // set up the fdata struct
  fdata->max_width = (settings->max_width != 0 && w != 0) ? MIN(w, settings->max_width) : MAX(w, settings->max_width);
  fdata->max_height = (settings->max_height != 0 && h != 0) ? MIN(h, settings->max_height) : MAX(h, settings->max_height);
  g_strlcpy(fdata->style, settings->style, sizeof(fdata->style));
  fdata->style_append = settings->style_append;
  // Invariant: the tagid for 'darktable|changed' will not change while this function runs. Is this a
  // sensible assumption?
  guint tagid = 0, etagid = 0;
  dt_tag_new("darktable|changed", &tagid);
  dt_tag_new("darktable|exported", &etagid);

  dt_export_metadata_t metadata;
  metadata.flags = 0;
  metadata.list = dt_util_str_to_glist("\1", settings->metadata_export);

  if (metadata.list)
  {
    metadata.flags = strtol(metadata.list->data, NULL, 16);
    metadata.list = g_list_remove(metadata.list, metadata.list->data);
  }

  while(t && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED)
  {
    const int imgid = GPOINTER_TO_INT(t->data);
    t = g_list_next(t);
    const guint num = total - g_list_length(t);
    // progress message
    char message[512] = { 0 };
    snprintf(message, sizeof(message), _("exporting %d / %d to %s"), num, total, mstorage->name(mstorage));
    // update the message. initialize_store() might have changed the number of images
    dt_control_job_set_progress_message(job, message);
    // remove 'changed' tag from image
    if(dt_tag_detach(tagid, imgid, FALSE, FALSE))
      tag_change = TRUE;
    // make sure the 'exported' tag is set on the image
    if(dt_tag_attach(etagid, imgid, FALSE, FALSE))
      tag_change = TRUE;
    /* register export timestamp in cache */
    dt_image_cache_set_export_timestamp(darktable.image_cache, imgid);
    // check if image still exists:
    const dt_image_t *image = dt_image_cache_get(darktable.image_cache, (int32_t)imgid, 'r');

    if(image)
    {
      char imgfilename[PATH_MAX] = { 0 };
      gboolean from_cache = TRUE;
      dt_image_full_path(image->id, imgfilename, sizeof(imgfilename), &from_cache);
      if(!g_file_test(imgfilename, G_FILE_TEST_IS_REGULAR))
      {
        dt_control_log(_("image `%s' is currently unavailable"), image->filename);
        fprintf(stderr, "image `%s' is currently unavailable\n", imgfilename);
        // dt_image_remove(imgid);
        dt_image_cache_read_release(darktable.image_cache, image);
      }
      else
      {
        dt_image_cache_read_release(darktable.image_cache, image);
        if(mstorage->store(mstorage, sdata, imgid, mformat, fdata, num, total, settings->high_quality, settings->upscale,
                           settings->export_masks, settings->icc_type, settings->icc_filename, settings->icc_intent,
                           &metadata) != 0)
          dt_control_job_cancel(job);
      }
    }

    fraction += 1.0 / total;
    if(fraction > 1.0) fraction = 1.0;
    dt_control_job_set_progress(job, fraction);
  }

  g_list_free_full(metadata.list, g_free);

  if(mstorage->finalize_store)
    mstorage->finalize_store(mstorage, sdata);

end:
  // all threads free their fdata
  mformat->free_params(mformat, fdata);
  // notify the user via the window manager
  dt_ui_notify_user();

  if(tag_change)
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);

  return 0;
}

void dt_control_merge_hdr()
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_generic_images_job_create(&dt_control_merge_hdr_job_run, N_("merge hdr image"),
                     0, NULL, PROGRESS_CANCELLABLE, TRUE));
}

void dt_control_duplicate_images()
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_generic_images_job_create(&dt_control_duplicate_images_job_run,
                                                          N_("duplicate images"), 0, NULL, PROGRESS_SIMPLE, TRUE));
}

void dt_control_flip_images(const int32_t cw)
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_generic_images_job_create(&dt_control_flip_images_job_run, N_("flip images"), cw,
                                                          NULL, PROGRESS_SIMPLE, TRUE));
}

gboolean dt_control_remove_images()
{
  // get all selected images now, to avoid the set changing during ui interaction
  dt_job_t *job = dt_control_generic_images_job_create(&dt_control_remove_images_job_run, N_("remove images"), 0,
                                                       NULL, PROGRESS_SIMPLE, FALSE);
  if(dt_conf_get_bool("ask_before_remove"))
  {
    GtkWidget *dialog;
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

    const dt_control_image_enumerator_t *e = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
    const int number = g_list_length(e->index);
    if(number == 0)
    {
      dt_control_job_dispose(job);
      return TRUE;
    }

    dialog = gtk_message_dialog_new(
        GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        ngettext("do you really want to remove %d image from the collection?",
                 "do you really want to remove %d images from the collection?", number),
        number);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif

    gtk_window_set_title(GTK_WINDOW(dialog), _("remove images?"));
    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if(res != GTK_RESPONSE_YES)
    {
      dt_control_job_dispose(job);
      return FALSE;
    }
  }
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
  return TRUE;
}

void dt_control_delete_images()
{
  // first get all selected images, to avoid the set changing during ui interaction
  dt_job_t *job = dt_control_generic_images_job_create(&dt_control_delete_images_job_run, N_("delete images"), 0,
                                                       NULL, PROGRESS_SIMPLE, FALSE);
  int send_to_trash = dt_conf_get_bool("send_to_trash");
  if(dt_conf_get_bool("ask_before_delete"))
  {
    GtkWidget *dialog;
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

    const dt_control_image_enumerator_t *e = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
    const int number = g_list_length(e->index);

    // Do not show the dialog if no image is selected:
    if(number == 0)
    {
      dt_control_job_dispose(job);
      return;
    }

    dialog = gtk_message_dialog_new(
        GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        send_to_trash ? ngettext("do you really want to send %d image to trash?",
                                 "do you really want to send %d images to trash?", number)
                      : ngettext("do you really want to physically delete %d image from disk?",
                                 "do you really want to physically delete %d images from disk?", number),
        number);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif

    gtk_window_set_title(GTK_WINDOW(dialog), send_to_trash ? _("trash images?") : _("delete images?"));
    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if(res != GTK_RESPONSE_YES)
    {
      dt_control_job_dispose(job);
      return;
    }
  }

  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
}

void dt_control_delete_image(int imgid)
{
  // first get all selected images, to avoid the set changing during ui interaction
  dt_job_t *job = dt_control_generic_image_job_create(&dt_control_delete_images_job_run, N_("delete images"), 0,
                                                      NULL, PROGRESS_SIMPLE, imgid);
  int send_to_trash = dt_conf_get_bool("send_to_trash");

  if(dt_conf_get_bool("ask_before_delete"))
  {
    GtkWidget *dialog;
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
    // Do not show the dialog if no valid image
    if(imgid < 1)
    {
      dt_control_job_dispose(job);
      return;
    }

    dialog = gtk_message_dialog_new(
        GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        send_to_trash ? _("do you really want to send selected image to trash?")
                      : _("do you really want to physically delete selected image from disk?"));
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif

    gtk_window_set_title(GTK_WINDOW(dialog), send_to_trash ? _("trash images?") : _("delete images?"));
    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if(res != GTK_RESPONSE_YES)
    {
      dt_control_job_dispose(job);
      return;
    }
  }
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
}

void dt_control_move_images()
{
  // Open file chooser dialog
  gchar *dir = NULL;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  dt_job_t *job = dt_control_generic_images_job_create(&dt_control_move_images_job_run, N_("move images"), 0, dir,
                                                       PROGRESS_CANCELLABLE, FALSE);
  const dt_control_image_enumerator_t *e = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  const int number = g_list_length(e->index);

  if(number == 0)
  {
    dt_control_job_dispose(job);
    return;
  }

  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("select directory"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_cancel"),
      GTK_RESPONSE_CANCEL, _("_select as destination"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
    dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));

  gtk_widget_destroy(filechooser);

  if(!dir || !g_file_test(dir, G_FILE_TEST_IS_DIR))
    goto abort;

  // ugly, but we need to set this after constructing the job:
  ((dt_control_image_enumerator_t *)dt_control_job_get_params(job))->data = dir;

  if(dt_conf_get_bool("ask_before_move"))
  {
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                                               ngettext("do you really want to physically move %d image to %s?\n"
                                                        "(all duplicates will be moved along)",
                                                        "do you really want to physically move %d images to %s?\n"
                                                        "(all duplicates will be moved along)",
                                                        number),
                                               number, dir);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif
    gtk_window_set_title(GTK_WINDOW(dialog), ngettext("move image?", "move images?", number));
    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if(res != GTK_RESPONSE_YES)
      goto abort;
  }

  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
  return;

abort:
  g_free(dir);
  dt_control_job_dispose(job);
}

void dt_control_copy_images()
{
  // Open file chooser dialog
  gchar *dir = NULL;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  dt_job_t *job = dt_control_generic_images_job_create(&dt_control_copy_images_job_run, N_("copy images"), 0, dir,
                                                       PROGRESS_CANCELLABLE, FALSE);
  const dt_control_image_enumerator_t *e = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  const int number = g_list_length(e->index);
  if(number == 0)
  {
    dt_control_job_dispose(job);
    return;
  }

  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("select directory"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_cancel"),
      GTK_RESPONSE_CANCEL, _("_select as destination"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
    dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));

  gtk_widget_destroy(filechooser);

  if(!dir || !g_file_test(dir, G_FILE_TEST_IS_DIR))
    goto abort;

  // ugly, but we need to set this after constructing the job:
  ((dt_control_image_enumerator_t *)dt_control_job_get_params(job))->data = dir;

  if(dt_conf_get_bool("ask_before_copy"))
  {
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        ngettext("do you really want to physically copy %d image to %s?",
                 "do you really want to physically copy %d images to %s?", number),
        number, dir);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif
    gtk_window_set_title(GTK_WINDOW(dialog), ngettext("copy image?", "copy images?", number));

    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if(res != GTK_RESPONSE_YES)
      goto abort;
  }

  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
  return;

abort:
  g_free(dir);
  dt_control_job_dispose(job);
}

void dt_control_set_local_copy_images()
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_generic_images_job_create(&dt_control_local_copy_images_job_run,
                                                          N_("local copy images"), 1, NULL, PROGRESS_CANCELLABLE,
                                                          FALSE));
}

void dt_control_reset_local_copy_images()
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_generic_images_job_create(&dt_control_local_copy_images_job_run,
                                                          N_("local copy images"), 0, NULL, PROGRESS_CANCELLABLE,
                                                          FALSE));
}

void dt_control_refresh_exif()
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_generic_images_job_create(&dt_control_refresh_exif_run, N_("refresh exif"), 0,
                                                          NULL, PROGRESS_CANCELLABLE, FALSE));
}

static dt_control_image_enumerator_t *dt_control_export_alloc()
{
  dt_control_image_enumerator_t *params = dt_control_image_enumerator_alloc();
  if(!params)
    return NULL;

  params->data = calloc(1, sizeof(dt_control_export_t));
  if(!params->data)
  {
    dt_control_image_enumerator_cleanup(params);
    return NULL;
  }

  return params;
}

static void dt_control_export_cleanup(void *p)
{
  dt_control_image_enumerator_t *params = p;
  dt_control_export_t *settings = (dt_control_export_t *)params->data;
  dt_imageio_module_storage_t *mstorage = dt_imageio_get_storage_by_index(settings->storage_index);
  dt_imageio_module_data_t *sdata = settings->sdata;

  mstorage->free_params(mstorage, sdata);
  g_free(settings->icc_filename);
  g_free(settings->metadata_export);
  free(params->data);

  dt_control_image_enumerator_cleanup(params);
}

void dt_control_export(GList *imgid_list, int max_width, int max_height, int format_index, int storage_index,
                       gboolean high_quality, gboolean upscale, gboolean export_masks, char *style, gboolean style_append,
                       dt_colorspaces_color_profile_type_t icc_type, const gchar *icc_filename,
                       dt_iop_color_intent_t icc_intent, const gchar *metadata_export)
{
  dt_job_t *job = dt_control_job_create(&dt_control_export_job_run, "export");
  
  if(!job) return;

  dt_control_image_enumerator_t *params = dt_control_export_alloc();

  if(!params)
  {
    dt_control_job_dispose(job);
    return;
  }

  dt_control_job_set_params(job, params, dt_control_export_cleanup);
  params->index = imgid_list;
  dt_control_export_t *data = params->data;
  data->max_width = max_width;
  data->max_height = max_height;
  data->format_index = format_index;
  data->storage_index = storage_index;
  dt_imageio_module_storage_t *mstorage = dt_imageio_get_storage_by_index(storage_index);
  g_assert(mstorage);
  // get shared storage param struct (global sequence counter, one picasa connection etc)
  dt_imageio_module_data_t *sdata = mstorage->get_params(mstorage);

  if(sdata == NULL)
  {
    dt_control_log(_("failed to get parameters from storage module `%s', aborting export.."),
                   mstorage->name(mstorage));
    dt_control_job_dispose(job);
    return;
  }

  data->sdata = sdata;
  data->high_quality = high_quality;
  data->export_masks = export_masks;
  data->upscale = upscale;
  g_strlcpy(data->style, style, sizeof(data->style));
  data->style_append = style_append;
  data->icc_type = icc_type;
  data->icc_filename = g_strdup(icc_filename);
  data->icc_intent = icc_intent;
  data->metadata_export = g_strdup(metadata_export);

  dt_control_job_add_progress(job, _("export images"), TRUE);
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_EXPORT, job);
  // tell the storage that we got its params for an export so it can reset itself to a safe state
  mstorage->export_dispatched(mstorage);
}

void dt_control_write_sidecar_files()
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_generic_images_job_create(&dt_control_write_sidecar_files_job_run,
                                                          N_("write sidecar files"), 0, NULL, PROGRESS_NONE,
                                                          FALSE));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
