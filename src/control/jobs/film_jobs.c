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
#include "control/jobs/film_jobs.h"
#include "common/darktable.h"
#include "common/film.h"
#include <stdlib.h>

typedef struct dt_film_import1_t
{
  dt_film_t *film;
} dt_film_import1_t;

static void dt_film_import1(dt_job_t *job, dt_film_t *film);

static int32_t dt_film_import1_run(dt_job_t *job)
{
  dt_film_import1_t *params = dt_control_job_get_params(job);
  dt_film_import1(job, params->film);
  dt_pthread_mutex_lock(&params->film->images_mutex);
  params->film->ref--;
  dt_pthread_mutex_unlock(&params->film->images_mutex);
  if(params->film->ref <= 0)
  {
    if(dt_film_is_empty(params->film->id))
    {
      dt_film_remove(params->film->id);
    }
  }

  // notify the user via the window manager
  dt_ui_notify_user();

  return 0;
}

static void dt_film_import1_cleanup(void *p)
{
  dt_film_import1_t *params = p;

  dt_film_cleanup(params->film);
  free(params->film);

  free(params);
}

dt_job_t *dt_film_import1_create(dt_film_t *film)
{
  dt_job_t *job = dt_control_job_create(&dt_film_import1_run, "cache load raw images for preview");
  if(!job) return NULL;
  dt_film_import1_t *params = (dt_film_import1_t *)calloc(1, sizeof(dt_film_import1_t));
  if(!params)
  {
    dt_control_job_dispose(job);
    return NULL;
  }
  dt_control_job_add_progress(job, _("import images"), FALSE);
  dt_control_job_set_params(job, params, dt_film_import1_cleanup);
  params->film = film;
  dt_pthread_mutex_lock(&film->images_mutex);
  film->ref++;
  dt_pthread_mutex_unlock(&film->images_mutex);
  return job;
}

static GList *_film_recursive_get_files(const gchar *path, gboolean recursive, GList **result)
{
  gchar *fullname;

  /* let's try open current dir */
  GDir *cdir = g_dir_open(path, 0, NULL);
  if(!cdir) return *result;

  /* lets read all files in current dir, recurse
     into directories if we should import recursive.
   */
  do
  {
    /* get the current filename */
    const gchar *filename = g_dir_read_name(cdir);

    /* return if no more files are in current dir */
    if(!filename) break;
    if(filename[0] == '.') continue;

    /* build full path for filename */
    fullname = g_build_filename(path, filename, NULL);

    /* recurse into directory if we hit one and we doing a recursive import */
    if(recursive && g_file_test(fullname, G_FILE_TEST_IS_DIR))
    {
      *result = _film_recursive_get_files(fullname, recursive, result);
      g_free(fullname);
    }
    /* or test if we found a supported image format to import */
    else if(!g_file_test(fullname, G_FILE_TEST_IS_DIR) && dt_supported_image(filename))
      *result = g_list_append(*result, fullname);
    else
      g_free(fullname);

  } while(TRUE);

  /* cleanup and return results */
  g_dir_close(cdir);

  return *result;
}

/* compare used for sorting the list of files to import
   only sort on basename of full path eg. the actually filename.
*/
int _film_filename_cmp(gchar *a, gchar *b)
{
  gchar *a_basename = g_path_get_basename(a);
  gchar *b_basename = g_path_get_basename(b);
  int ret = g_strcmp0(a_basename, b_basename);
  g_free(a_basename);
  g_free(b_basename);
  return ret;
}

static void dt_film_import1(dt_job_t *job, dt_film_t *film)
{
  gboolean recursive = dt_conf_get_bool("ui_last/import_recursive");

  /* first of all gather all images to import */
  GList *images = NULL;
  images = _film_recursive_get_files(film->dirname, recursive, &images);
  if(g_list_length(images) == 0)
  {
    dt_control_log(_("no supported images were found to be imported"));
    return;
  }

#ifdef USE_LUA
  /* pre-sort image list for easier handling in Lua code */
  images = g_list_sort(images, (GCompareFunc)_film_filename_cmp);

  dt_lua_lock();
  lua_State *L = darktable.lua_state.state;
  {
    GList *elt = images;
    lua_newtable(L);
    while(elt)
    {
      lua_pushstring(L, elt->data);
      luaL_ref(L, -2);
      elt = g_list_next(elt);
    }
  }
  lua_pushvalue(L, -1);
  dt_lua_event_trigger(L, "pre-import", 1);
  {
    g_list_free_full(images, g_free);
    // recreate list of images
    images = NULL;
    lua_pushnil(L); /* first key */
    while(lua_next(L, -2) != 0)
    {
      /* uses 'key' (at index -2) and 'value' (at index -1) */
      void *filename = strdup(luaL_checkstring(L, -1));
      lua_pop(L, 1);
      images = g_list_prepend(images, filename);
    }
  }

  lua_pop(L, 1); // remove the table again from the stack

  dt_lua_unlock();
#endif

  if(g_list_length(images) == 0)
  {
    // no error message, lua probably emptied the list on purpose
    return;
  }

  /* we got ourself a list of images, lets sort and start import */
  images = g_list_sort(images, (GCompareFunc)_film_filename_cmp);

  /* let's start import of images */
  gchar message[512] = { 0 };
  double fraction = 0;
  guint total = g_list_length(images);
  g_snprintf(message, sizeof(message) - 1, ngettext("importing %d image", "importing %d images", total), total);
  dt_control_job_set_progress_message(job, message);


  /* loop thru the images and import to current film roll */
  dt_film_t *cfr = film;
  GList *image = g_list_first(images);
  do
  {
    gchar *cdn = g_path_get_dirname((const gchar *)image->data);

    /* check if we need to initialize a new filmroll */
    if(!cfr || g_strcmp0(cfr->dirname, cdn) != 0)
    {
      /* cleanup previously imported filmroll*/
      if(cfr && cfr != film)
      {
        if(dt_film_is_empty(cfr->id))
          dt_film_remove(cfr->id);

        dt_film_cleanup(cfr);
        free(cfr);
        cfr = NULL;
      }

      /* initialize and create a new film to import to */
      cfr = malloc(sizeof(dt_film_t));
      dt_film_init(cfr);
      dt_film_new(cfr, cdn);
    }

    g_free(cdn);

    /* import image */
    dt_image_import(cfr->id, (const gchar *)image->data, FALSE);

    fraction += 1.0 / total;
    dt_control_job_set_progress(job, fraction);


  } while((image = g_list_next(image)) != NULL);

  g_list_free_full(images, g_free);

  // only redraw at the end, to not spam the cpu with exposure events
  dt_control_queue_redraw_center();
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);

  dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_IMPORTED, film->id);

  /* cleanup previously imported filmroll*/
  if(cfr && cfr != film)
  {
    dt_film_cleanup(cfr);
    free(cfr);
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
