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

#pragma once

#include <gtk/gtk.h>
#include <string.h>
#include <librsvg/rsvg.h>

/** dynamically allocate and concatenate string */
gchar *dt_util_dstrcat(gchar *str, const gchar *format, ...) __attribute__((format(printf, 2, 3)));

/** replace all occurrences of pattern by substitute. the returned value has to be freed after use. */
gchar *dt_util_str_replace(const gchar *string, const gchar *pattern, const gchar *substitute);
/** count the number of occurrences of needle in haystack */
guint dt_util_str_occurence(const gchar *haystack, const gchar *needle);
/** generate a string from the elements of the list, separated by separator. the result has to be freed. */
gchar *dt_util_glist_to_str(const gchar *separator, GList *items);
/** generate a GList from the elements of a string, separated by separator. the result has to be freed. */
GList *dt_util_str_to_glist(const gchar *separator, const gchar *text);
/** take a list of strings and remove all duplicates. the result will be sorted. */
GList *dt_util_glist_uniq(GList *items);
/** fixes the given path by replacing a possible tilde with the correct home directory */
gchar *dt_util_fix_path(const gchar *path);
size_t dt_utf8_strlcpy(char *dest, const char *src, size_t n);
/** get the size of a file in bytes */
off_t dt_util_get_file_size(const char *filename);
/** returns true if dirname is empty */
gboolean dt_util_is_dir_empty(const char *dirname);
/** returns a valid UTF-8 string for the given char array. has to be freed with g_free(). */
gchar *dt_util_foo_to_utf8(const char *string);
/** returns the number of occurence of character in a text. */
guint dt_util_string_count_char(const char *text, const char needle);

cairo_surface_t *dt_util_get_logo(const float size);
cairo_surface_t *dt_util_get_logo_text(const float size);

// returns the RsvgDimensionData of a supplied RsvgHandle
RsvgDimensionData dt_get_svg_dimension(RsvgHandle *svg);

// renders svg data
void dt_render_svg(RsvgHandle *svg, cairo_t *cr, double width, double height, double offset_x, double offset_y);

// make paths absolute and try to normalize on Windows. also deal with character encoding on Windows.
gchar *dt_util_normalize_path(const gchar *input);

// format exposure time string
gchar *dt_util_format_exposure(const float exposuretime);
char *dt_read_file(const char *const filename, size_t *filesize);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
