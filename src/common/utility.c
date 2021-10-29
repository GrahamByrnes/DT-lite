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

#include "common/darktable.h"
#include "common/file_location.h"
#include "common/grealpath.h"
#include "common/utility.h"
#include "gui/gtk.h"

/* getpwnam_r availability check */
#if defined __APPLE__ || defined _POSIX_C_SOURCE >= 1 || defined _XOPEN_SOURCE || defined _BSD_SOURCE        \
    || defined _SVID_SOURCE || defined _POSIX_SOURCE || defined __DragonFly__ || defined __FreeBSD__         \
    || defined __NetBSD__ || defined __OpenBSD__
  #include <pwd.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

#ifdef _WIN32
  #include <Windows.h>
  #include <WinBase.h>
  #include <FileAPI.h>
#endif

#include <math.h>
#include <glib/gi18n.h>

#include <sys/stat.h>
#include <ctype.h>

#ifdef HAVE_CONFIG_H
  #include <config.h>
#endif

#include <librsvg/rsvg.h>
// ugh, ugly hack. why do people break stuff all the time?
#ifndef RSVG_CAIRO_H
#include <librsvg/rsvg-cairo.h>
#endif

gchar *dt_util_dstrcat(gchar *str, const gchar *format, ...)
{
  va_list args;
  gchar *ns;
  va_start(args, format);
  const size_t clen = str ? strlen(str) : 0;
  const int alen = g_vsnprintf(NULL, 0, format, args);
  const int nsize = alen + clen + 1;

  /* realloc for new string */
  ns = g_realloc(str, nsize);
  if(str == NULL) ns[0] = '\0';
  va_end(args);

  /* append string */
  va_start(args, format);
  g_vsnprintf(ns + clen, alen + 1, format, args);
  va_end(args);

  ns[nsize - 1] = '\0';

  return ns;
}

guint dt_util_str_occurence(const gchar *haystack, const gchar *needle)
{
  guint o = 0;
  if(haystack && needle)
  {
    const gchar *p = haystack;
    if((p = g_strstr_len(p, strlen(p), needle)) != NULL)
    {
      do
      {
        o++;
      } while((p = g_strstr_len((p + 1), strlen(p + 1), needle)) != NULL);
    }
  }
  return o;
}

gchar *dt_util_str_replace(const gchar *string, const gchar *pattern, const gchar *substitute)
{
  gint occurences = dt_util_str_occurence(string, pattern);
  gchar *nstring;
  if(occurences)
  {
    nstring = g_malloc_n(strlen(string) + (occurences * strlen(substitute)) + 1, sizeof(gchar));
    const gchar *pend = string + strlen(string);
    const gchar *s = string, *p = string;
    gchar *np = nstring;
    if((s = g_strstr_len(s, strlen(s), pattern)) != NULL)
    {
      do
      {
        memcpy(np, p, s - p);
        np += (s - p);
        memcpy(np, substitute, strlen(substitute));
        np += strlen(substitute);
        p = s + strlen(pattern);
      } while((s = g_strstr_len((s + 1), strlen(s + 1), pattern)) != NULL);
    }
    memcpy(np, p, pend - p);
    np[pend - p] = '\0';
  }
  else
    nstring = g_strdup(string); // otherwise it's a hell to decide whether to free this string later.
  return nstring;
}

gchar *dt_util_glist_to_str(const gchar *separator, GList *items)
{
  if(items == NULL) return NULL;

  const unsigned int count = g_list_length(items);
  gchar *result = NULL;

  // add the entries to an char* array
  items = g_list_first(items);
  gchar **strings = g_malloc0_n(count + 1, sizeof(gchar *));
  if(items != NULL)
  {
    int i = 0;
    do
    {
      strings[i++] = items->data;
    } while((items = g_list_next(items)) != NULL);
  }

  // join them into a single string
  result = g_strjoinv(separator, strings);

  // free the array
  g_free(strings);

  return result;
}

GList *dt_util_glist_uniq(GList *items)
{
  if(!items) return NULL;

  gchar *last = NULL;
  GList *last_item = NULL;

  items = g_list_sort(items, (GCompareFunc)g_strcmp0);
  GList *iter = items;
  while(iter)
  {
    gchar *value = (gchar *)iter->data;
    if(!g_strcmp0(last, value))
    {
      g_free(value);
      items = g_list_delete_link(items, iter);
      iter = last_item;
    }
    else
    {
      last = value;
      last_item = iter;
    }
    iter = g_list_next(iter);
  }
  return items;
}


gchar *dt_util_fix_path(const gchar *path)
{
  if(path == NULL || *path == '\0')
    return NULL;

  gchar *rpath = NULL;

  /* check if path has a prepended tilde */
  if(path[0] == '~')
  {
    size_t len = strlen(path);
    char *user = NULL;
    int off = 1;

    /* if the character after the tilde is not a slash we parse
     * the path until the next slash to extend this part with the
     * home directory of the specified user
     *
     * e.g.: ~foo will be evaluated as the home directory of the
     * user foo */

    if(len > 1 && path[1] != '/')
    {
      while(path[off] != '\0' && path[off] != '/')
        ++off;

      user = g_strndup(path + 1, off - 1);
    }

    gchar *home_path = dt_loc_get_home_dir(user);
    g_free(user);

    if(home_path == NULL)
      return g_strdup(path);

    rpath = g_build_filename(home_path, path + off, NULL);
    g_free(home_path);
  }
  else
    rpath = g_strdup(path);

  return rpath;
}

/**
 * dt_utf8_strlcpy:
 * @dest: buffer to fill with characters from @src
 * @src: UTF-8 encoded string
 * @n: size of @dest
 *
 * Like the BSD-standard strlcpy() function, but
 * is careful not to truncate in the middle of a character.
 * The @src string must be valid UTF-8 encoded text.
 * (Use g_utf8_validate() on all text before trying to use UTF-8
 * utility functions with it.)
 *
 * Return value: strlen(src)
 * Implementation by Philip Page, see https://bugzilla.gnome.org/show_bug.cgi?id=520116
 **/
size_t dt_utf8_strlcpy(char *dest, const char *src, size_t n)
{
  register const gchar *s = src;
  while(s - src < n && *s)
  {
    s = g_utf8_next_char(s);
  }

  if(s - src >= n)
  {
    /* We need to truncate; back up one. */
    s = g_utf8_prev_char(s);
    strncpy(dest, src, s - src);
    dest[s - src] = '\0';
    /* Find the full length for return value. */
    while(*s)
    {
      s = g_utf8_next_char(s);
    }
  }
  else
  {
    /* Plenty of room, just copy */
    strncpy(dest, src, s - src);
    dest[s - src] = '\0';
  }
  return s - src;
}

off_t dt_util_get_file_size(const char *filename)
{
#ifdef _WIN32
  struct _stati64 st;
  if(_stati64(filename, &st) == 0) return st.st_size;
#else
  struct stat st;
  if(stat(filename, &st) == 0) return st.st_size;
#endif

  return -1;
}

gboolean dt_util_is_dir_empty(const char *dirname)
{
  int n = 0;
  GDir *dir = g_dir_open(dirname, 0, NULL);
  if(dir == NULL) // Not a directory or doesn't exist
    return TRUE;
  while(g_dir_read_name(dir) != NULL)
  {
    if(++n > 1) break;
  }
  g_dir_close(dir);
  if(n == 0) // Directory Empty
    return TRUE;
  else
    return FALSE;
}

gchar *dt_util_foo_to_utf8(const char *string)
{
  gchar *tag = NULL;

  if(g_utf8_validate(string, -1, NULL)) // first check if it's utf8 already
    tag = g_strdup(string);
  else
    tag = g_convert(string, -1, "UTF-8", "LATIN1", NULL, NULL, NULL); // let's try latin1

  if(!tag) // hmm, neither utf8 nor latin1, let's fall back to ascii and just remove everything that isn't
  {
    tag = g_strdup(string);
    char *c = tag;
    while(*c)
    {
      if((*c < 0x20) || (*c >= 0x7f)) *c = '?';
      c++;
    }
  }
  return tag;
}

static cairo_surface_t *_util_get_svg_img(gchar *logo, const float size)
{
  GError *error = NULL;
  cairo_surface_t *surface = NULL;
  char datadir[PATH_MAX] = { 0 };

  dt_loc_get_datadir(datadir, sizeof(datadir));
  char *dtlogo = g_build_filename(datadir, "pixmaps", logo, NULL);
  RsvgHandle *svg = rsvg_handle_new_from_file(dtlogo, &error);
  if(svg)
  {
    RsvgDimensionData dimension;
    dimension = dt_get_svg_dimension(svg);

    const float ppd = darktable.gui ? darktable.gui->ppd : 1.0;

    const float svg_size = MAX(dimension.width, dimension.height);
    const float factor = size > 0.0 ? size / svg_size : -1.0 * size;
    const float final_width = dimension.width * factor * ppd,
                final_height = dimension.height * factor * ppd;
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, final_width);

    guint8 *image_buffer = (guint8 *)calloc(stride * final_height, sizeof(guint8));
    if(darktable.gui)
      surface = dt_cairo_image_surface_create_for_data(image_buffer, CAIRO_FORMAT_ARGB32, final_width,
                                                      final_height, stride);
    else // during startup we don't know ppd yet and darktable.gui isn't initialized yet.
      surface = cairo_image_surface_create_for_data(image_buffer, CAIRO_FORMAT_ARGB32, final_width,
                                                       final_height, stride);
    if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
    {
      fprintf(stderr, "warning: can't load darktable logo from SVG file `%s'\n", dtlogo);
      cairo_surface_destroy(surface);
      free(image_buffer);
      image_buffer = NULL;
      surface = NULL;
    }
    else
    {
      cairo_t *cr = cairo_create(surface);
      cairo_scale(cr, factor, factor);
      dt_render_svg(svg, cr, final_width, final_height, 0, 0);
      cairo_destroy(cr);
      cairo_surface_flush(surface);
    }
    g_object_unref(svg);
  }
  else
  {
    fprintf(stderr, "warning: can't load darktable logo from SVG file `%s'\n%s\n", dtlogo, error->message);
    g_error_free(error);
  }

  g_free(logo);
  g_free(dtlogo);
  return surface;
}

cairo_surface_t *dt_util_get_logo(const float size)
{
  char *logo;
  logo = g_strdup("idbutton.svg");
  return _util_get_svg_img(logo, size);
}

cairo_surface_t *dt_util_get_logo_text(const float size)
{
  return _util_get_svg_img(g_strdup("dt_text.svg"), size);
}

RsvgDimensionData dt_get_svg_dimension(RsvgHandle *svg)
{
  RsvgDimensionData dimension;
  // rsvg_handle_get_dimensions has been deprecated in librsvg 2.52
  #if LIBRSVG_CHECK_VERSION(2,52,0)
    double width;
    double height;
    rsvg_handle_get_intrinsic_size_in_pixels(svg, &width, &height);
    dimension.width = width;
    dimension.height = height;
  #else
    rsvg_handle_get_dimensions(svg, &dimension);
  #endif
  return dimension;
}

void dt_render_svg(RsvgHandle *svg, cairo_t *cr, double width, double height, double offset_x, double offset_y)
{
  // rsvg_handle_render_cairo has been deprecated in librsvg 2.52
  #if LIBRSVG_CHECK_VERSION(2,52,0)
    RsvgRectangle viewport = {
      .x = offset_x,
      .y = offset_y,
      .width = width,
      .height = height,
    };
    rsvg_handle_render_document(svg, cr, &viewport, NULL);
  #else
    rsvg_handle_render_cairo(svg, cr);
  #endif
}

// make paths absolute and try to normalize on Windows. also deal with character encoding on Windows.
gchar *dt_util_normalize_path(const gchar *_input)
{
#ifdef _WIN32
  gchar *input;
  if(g_utf8_validate(_input, -1, NULL))
    input = g_strdup(_input);
  else
  {
    input = g_locale_to_utf8(_input, -1, NULL, NULL, NULL);
    if(!input) return NULL;
  }
#else
  const gchar *input = _input;
#endif

  gchar *filename = g_filename_from_uri(input, NULL, NULL);

  if(!filename)
  {
    if(g_str_has_prefix(input, "file://")) // in this case we should take care of %XX encodings in the string
                                           // (for example %20 = ' ')
    {
      input += strlen("file://");
      filename = g_uri_unescape_string(input, NULL);
    }
    else
      filename = g_strdup(input);
  }

#ifdef _WIN32
  g_free(input);
#endif

  if(g_path_is_absolute(filename) == FALSE)
  {
    char *current_dir = g_get_current_dir();
    char *tmp_filename = g_build_filename(current_dir, filename, NULL);
    g_free(filename);
    filename = g_realpath(tmp_filename);
    if(filename == NULL)
    {
      g_free(current_dir);
      g_free(tmp_filename);
      g_free(filename);
      return NULL;
    }
    g_free(current_dir);
    g_free(tmp_filename);
  }

#ifdef _WIN32
  // on Windows filenames are case insensitive, so we can end up with an arbitrary number of different spellings for the same file.
  // another problem is that path separators can either be / or \ leading to even more problems.

  // TODO:
  // this only handles filenames in the old <drive letter>:\path\to\file form, not the \\?\UNC\ form and not some others like \Device\...

  // the Windows api expects wide chars and not utf8 :(
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  g_free(filename);
  if(!wfilename)
    return NULL;

  wchar_t LongPath[MAX_PATH] = {0};
  DWORD size = GetLongPathNameW(wfilename, LongPath, MAX_PATH);
  g_free(wfilename);
  if(size == 0 || size > MAX_PATH)
    return NULL;

  // back to utf8!
  filename = g_utf16_to_utf8(LongPath, -1, NULL, NULL, NULL);
  if(!filename)
    return NULL;

  GFile *gfile = g_file_new_for_path(filename);
  g_free(filename);
  if(!gfile)
    return NULL;
  filename = g_file_get_path(gfile);
  g_object_unref(gfile);
  if(!filename)
    return NULL;

  char drive_letter = g_ascii_toupper(filename[0]);
  if(drive_letter < 'A' || drive_letter > 'Z' || filename[1] != ':')
  {
    g_free(filename);
    return NULL;
  }
  filename[0] = drive_letter;
#endif

  return filename;
}

guint dt_util_string_count_char(const char *text, const char needle)
{
  guint count = 0;
  while(text[0])
  {
    if (text[0] == needle) count ++;
    text ++;
  }
  return count;
}

GList *dt_util_str_to_glist(const gchar *separator, const gchar *text)
{
  if(text == NULL) return NULL;
  GList *list = NULL;
  gchar *item = NULL;
  gchar *entry = g_strdup(text);
  gchar *prev = entry;
  int len = strlen(prev);
  while (len)
  {
    gchar *next = g_strstr_len(prev, -1, separator);
    if (next)
    {
      const gchar c = next[0];
      next[0] = '\0';
      item = g_strdup(prev);
      next[0] = c;
      prev = next + strlen(separator);
      len = strlen(prev);
      list = g_list_prepend(list, item);
      if (!len) list = g_list_prepend(list, g_strdup(""));
    }
    else
    {
      item = g_strdup(prev);
      len = 0;
      list = g_list_prepend(list, item);
    }
  }
  list = g_list_reverse(list);
  g_free(entry);
  return list;
}

// format exposure time given in seconds to a string in a unified way
char *dt_util_format_exposure(const float exposuretime)
{
  char *result;
  if(exposuretime >= 1.0f)
  {
    if(nearbyintf(exposuretime) == exposuretime)
      result = g_strdup_printf("%.0f″", exposuretime);
    else
      result = g_strdup_printf("%.1f″", exposuretime);
  }
  /* want to catch everything below 0.3 seconds */
  else if(exposuretime < 0.29f)
    result = g_strdup_printf("1/%.0f", 1.0 / exposuretime);

  /* catch 1/2, 1/3 */
  else if(nearbyintf(1.0f / exposuretime) == 1.0f / exposuretime)
    result = g_strdup_printf("1/%.0f", 1.0 / exposuretime);

  /* catch 1/1.3, 1/1.6, etc. */
  else if(10 * nearbyintf(10.0f / exposuretime) == nearbyintf(100.0f / exposuretime))
    result = g_strdup_printf("1/%.1f", 1.0 / exposuretime);

  else
    result = g_strdup_printf("%.1f″", exposuretime);

  return result;
}

char *dt_read_file(const char *const filename, size_t *filesize)
{
  if (filesize) *filesize = 0;
  FILE *fd = g_fopen(filename, "rb");
  if(!fd) return NULL;

  fseek(fd, 0, SEEK_END);
  const size_t end = ftell(fd);
  rewind(fd);

  char *content = (char *)malloc(sizeof(char) * end);
  if(!content) return NULL;

  const size_t count = fread(content, sizeof(char), end, fd);
  fclose(fd);
  if (count == end)
  {
    if (filesize) *filesize = end;
    return content;
  }
  free(content);
  return NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
