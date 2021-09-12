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

#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/metadata.h"
#include "common/tags.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

#include <gdk/gdkkeysyms.h>
#include <sys/param.h>
#ifdef USE_LUA
#include "lua/call.h"
#include "lua/image.h"
#endif

#define SHOW_FLAGS 1

DT_MODULE(1)

enum
{
  /* internal */
  md_internal_filmroll = 0,
  md_internal_imgid,
  md_internal_groupid,
  md_internal_filename,
  md_internal_version,
  md_internal_fullpath,
  md_internal_local_copy,
  md_internal_import_timestamp,
  md_internal_change_timestamp,
  md_internal_export_timestamp,
  md_internal_print_timestamp,
#if SHOW_FLAGS
  md_internal_flags,
#endif

  /* exif */
  md_exif_model,
  md_exif_maker,
  md_exif_lens,
  md_exif_aperture,
  md_exif_exposure,
  md_exif_exposure_bias,
  md_exif_focal_length,
  md_exif_focus_distance,
  md_exif_iso,
  md_exif_datetime,
  md_exif_width,
  md_exif_height,

  /* size of final image */
  md_width,
  md_height,

  /* xmp */
  md_xmp_metadata,

  /* geotagging */
  md_geotagging_lat = md_xmp_metadata + DT_METADATA_NUMBER,
  md_geotagging_lon,
  md_geotagging_ele,

  /* tags */
  md_tag_names,
  md_categories,

  /* entries, do not touch! */
  md_size
};

static gchar *_md_labels[md_size];

/* initialize the labels text */
static void _lib_metatdata_view_init_labels()
{
  /* internal */
  _md_labels[md_internal_filmroll] = _("filmroll");
  _md_labels[md_internal_imgid] = _("image id");
  _md_labels[md_internal_groupid] = _("group id");
  _md_labels[md_internal_filename] = _("filename");
  _md_labels[md_internal_version] = _("version");
  _md_labels[md_internal_fullpath] = _("full path");
  _md_labels[md_internal_local_copy] = _("local copy");
  _md_labels[md_internal_import_timestamp] = _("import timestamp");
  _md_labels[md_internal_change_timestamp] = _("change timestamp");
  _md_labels[md_internal_export_timestamp] = _("export timestamp");
  _md_labels[md_internal_print_timestamp] = _("print timestamp");
#if SHOW_FLAGS
  _md_labels[md_internal_flags] = _("flags");
#endif

  /* exif */
  _md_labels[md_exif_model] = _("model");
  _md_labels[md_exif_maker] = _("maker");
  _md_labels[md_exif_lens] = _("lens");
  _md_labels[md_exif_aperture] = _("aperture");
  _md_labels[md_exif_exposure] = _("exposure");
  _md_labels[md_exif_exposure_bias] = _("exposure bias");
  _md_labels[md_exif_focal_length] = _("focal length");
  _md_labels[md_exif_focus_distance] = _("focus distance");
  _md_labels[md_exif_iso] = _("ISO");
  _md_labels[md_exif_datetime] = _("datetime");
  _md_labels[md_exif_width] = _("width");
  _md_labels[md_exif_height] = _("height");

  _md_labels[md_width] = _("export width");
  _md_labels[md_height] = _("export height");

  /* xmp */
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
    const gchar *name = dt_metadata_get_name(keyid);
    _md_labels[md_xmp_metadata+i] = _(name);
  }

  /* geotagging */
  _md_labels[md_geotagging_lat] = _("latitude");
  _md_labels[md_geotagging_lon] = _("longitude");
  _md_labels[md_geotagging_ele] = _("elevation");

  /* tags */
  _md_labels[md_tag_names] = _("tags");
  _md_labels[md_categories] = _("categories");
}

typedef struct dt_lib_metadata_view_t
{
  GtkLabel *name[md_size];
  GtkLabel *metadata[md_size];
  GtkWidget *scrolled_window;
} dt_lib_metadata_view_t;

static gboolean view_onMouseScroll(GtkWidget *view, GdkEventScroll *event, dt_lib_metadata_view_t *d);

const char *name(dt_lib_module_t *self)
{
  return _("image information");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 299;
}

/* helper which eliminates non-printable characters from a string

Strings which are already in valid UTF-8 are retained.
*/
static void _filter_non_printable(char *string, size_t length)
{
  /* explicitly tell the validator to ignore the trailing nulls, otherwise this fails */
  if(g_utf8_validate(string, -1, 0)) return;

  unsigned char *str = (unsigned char *)string;
  int n = 0;

  while(*str != '\000' && n < length)
  {
    if((*str < 0x20) || (*str >= 0x7f)) *str = '.';

    str++;
    n++;
  }
}

#define NODATA_STRING "-"

/* helper function for updating a metadata value */
static void _metadata_update_value(GtkLabel *label, const char *value)
{
  gboolean validated = g_utf8_validate(value, -1, NULL);
  const gchar *str = validated ? value : NODATA_STRING;
  gtk_label_set_text(GTK_LABEL(label), str);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_tooltip_text(GTK_WIDGET(label), str);
}

static void _metadata_update_value_end(GtkLabel *label, const char *value)
{
  gboolean validated = g_utf8_validate(value, -1, NULL);
  const gchar *str = validated ? value : NODATA_STRING;
  gtk_label_set_text(GTK_LABEL(label), str);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(GTK_WIDGET(label), GTK_ALIGN_START);
  gtk_widget_set_tooltip_text(GTK_WIDGET(label), str);
}

/*
#ifdef USE_LUA
static int lua_update_metadata(lua_State*L);
#endif*/

// update all values to reflect mouse over image id or no data at all
static void _metadata_view_update_values(dt_lib_module_t *self)
{
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;
  int32_t mouse_over_id = dt_control_get_mouse_over_id();

  if(mouse_over_id == -1)
  {
    const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
    if(cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM)
    {
      mouse_over_id = darktable.develop->image_storage.id;
    }
    else
    {
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images LIMIT 1",
                                  -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW) mouse_over_id = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
    }
  }

  if(mouse_over_id >= 0)
  {
    char value[512];
    char pathname[PATH_MAX] = { 0 };

    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, mouse_over_id, 'r');
    if(!img) goto fill_minuses;
    if(img->film_id == -1)
    {
      dt_image_cache_read_release(darktable.image_cache, img);
      goto fill_minuses;
    }

    /* update all metadata */

    dt_image_film_roll(img, value, sizeof(value));
    _metadata_update_value(d->metadata[md_internal_filmroll], value);

    char tooltip[512];
    snprintf(tooltip, sizeof(tooltip), _("double click to jump to film roll\n%s"), value);
    gtk_widget_set_tooltip_text(GTK_WIDGET(d->metadata[md_internal_filmroll]), tooltip);

    snprintf(value, sizeof(value), "%d", img->id);
    _metadata_update_value(d->metadata[md_internal_imgid], value);

    snprintf(value, sizeof(value), "%d", img->group_id);
    _metadata_update_value(d->metadata[md_internal_groupid], value);

    _metadata_update_value(d->metadata[md_internal_filename], img->filename);

    snprintf(value, sizeof(value), "%d", img->version);
    _metadata_update_value(d->metadata[md_internal_version], value);

    gboolean from_cache = FALSE;
    dt_image_full_path(img->id, pathname, sizeof(pathname), &from_cache);
    _metadata_update_value(d->metadata[md_internal_fullpath], pathname);

    g_strlcpy(value, (img->flags & DT_IMAGE_LOCAL_COPY) ? _("yes") : _("no"), sizeof(value));
    _metadata_update_value(d->metadata[md_internal_local_copy], value);

    if (img->import_timestamp >=0)
    {
      char datetime[200];
      // just %c is too long and includes a time zone that we don't know from exif
      strftime(datetime, sizeof(datetime), "%a %x %X", localtime(&img->import_timestamp));
      _metadata_update_value(d->metadata[md_internal_import_timestamp], g_locale_to_utf8(datetime,-1,NULL,NULL,NULL));
    }
    else
      _metadata_update_value(d->metadata[md_internal_import_timestamp], "-");

    if (img->change_timestamp >=0)
    {
      char datetime[200];
      strftime(datetime, sizeof(datetime), "%a %x %X", localtime(&img->change_timestamp));
      _metadata_update_value(d->metadata[md_internal_change_timestamp], g_locale_to_utf8(datetime,-1,NULL,NULL,NULL));
    }
    else
      _metadata_update_value(d->metadata[md_internal_change_timestamp], "-");

    if (img->export_timestamp >=0)
    {
      char datetime[200];
      strftime(datetime, sizeof(datetime), "%a %x %X", localtime(&img->export_timestamp));
      _metadata_update_value(d->metadata[md_internal_export_timestamp], g_locale_to_utf8(datetime,-1,NULL,NULL,NULL));
    }
    else
      _metadata_update_value(d->metadata[md_internal_export_timestamp], "-");

    if (img->print_timestamp >=0)
    {
      char datetime[200];
      strftime(datetime, sizeof(datetime), "%a %x %X", localtime(&img->print_timestamp));
      _metadata_update_value(d->metadata[md_internal_print_timestamp], g_locale_to_utf8(datetime,-1,NULL,NULL,NULL));
    }
    else
      _metadata_update_value(d->metadata[md_internal_print_timestamp], "-");

    // TODO: decide if this should be removed for a release. maybe #ifdef'ing to only add it to git compiles?

    // the bits of the flags
#if SHOW_FLAGS
    {
      #define EMPTY_FIELD '.'
      #define FALSE_FIELD '.'
      #define TRUE_FIELD '!'

      char *flags_tooltip = NULL;
      char *flag_descriptions[] = { N_("unused"),
                                    N_("unused/deprecated"),
                                    N_("ldr"),
                                    N_("raw"),
                                    N_("hdr"),
                                    N_("marked for deletion"),
                                    N_("auto-applying presets applied"),
                                    N_("legacy flag. set for all new images"),
                                    N_("local copy"),
                                    N_("has .txt"),
                                    N_("has .wav"),
                                    N_("monochrome")
      };
      char *tooltip_parts[15] = { 0 };
      int next_tooltip_part = 0;

      memset(value, EMPTY_FIELD, sizeof(value));

      int stars = img->flags & 0x7;
      char *star_string = NULL;
      if(stars == 6)
      {
        value[0] = 'x';
        tooltip_parts[next_tooltip_part++] = _("image rejected");
      }
      else
      {
        value[0] = '0' + stars;
        tooltip_parts[next_tooltip_part++] = star_string = g_strdup_printf(ngettext("image has %d star", "image has %d stars", stars), stars);
      }


      if(img->flags & 8)
      {
        value[1] = TRUE_FIELD;
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[0]);
      }
      else
        value[1] = FALSE_FIELD;

      if(img->flags & DT_IMAGE_THUMBNAIL_DEPRECATED)
      {
        value[2] = TRUE_FIELD;
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[1]);
      }
      else
        value[2] = FALSE_FIELD;

      if(img->flags & DT_IMAGE_LDR)
      {
        value[3] = 'l';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[2]);
      }

      if(img->flags & DT_IMAGE_RAW)
      {
        value[4] = 'r';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[3]);
      }

      if(img->flags & DT_IMAGE_HDR)
      {
        value[5] = 'h';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[4]);
      }

      if(img->flags & DT_IMAGE_REMOVE)
      {
        value[6] = 'd';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[5]);
      }

      if(img->flags & DT_IMAGE_AUTO_PRESETS_APPLIED)
      {
        value[7] = 'a';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[6]);
      }

      if(img->flags & DT_IMAGE_NO_LEGACY_PRESETS)
      {
        value[8] = 'p';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[7]);
      }

      if(img->flags & DT_IMAGE_LOCAL_COPY)
      {
        value[9] = 'c';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[8]);
      }

      if(img->flags & DT_IMAGE_HAS_TXT)
      {
        value[10] = 't';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[9]);
      }

      if(img->flags & DT_IMAGE_HAS_WAV)
      {
        value[11] = 'w';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[10]);
      }

      if(dt_image_monochrome_flags(img))
      {
        value[12] = 'm';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[11]);
      }

      static const struct
      {
        char *tooltip;
        char flag;
      } loaders[] =
      {
        { N_("unknown"), EMPTY_FIELD},
        { N_("tiff"), 't'},
        { N_("png"), 'p'},
        { N_("j2k"), 'J'},
        { N_("jpeg"), 'j'},
        { N_("exr"), 'e'},
        { N_("rgbe"), 'R'},
        { N_("pfm"), 'P'},
        { N_("GraphicsMagick"), 'g'},
        { N_("rawspeed"), 'r'},
        { N_("netpnm"), 'n'},
        { N_("avif"), 'a'},
      };

      const int loader = (unsigned int)img->loader < sizeof(loaders) / sizeof(*loaders) ? img->loader : 0;
      value[13] = loaders[loader].flag;
      char *loader_tooltip = g_strdup_printf(_("loader: %s"), _(loaders[loader].tooltip));
      tooltip_parts[next_tooltip_part++] = loader_tooltip;

      value[14] = '\0';

      flags_tooltip = g_strjoinv("\n", tooltip_parts);
      g_free(loader_tooltip);

      _metadata_update_value(d->metadata[md_internal_flags], value);
      gtk_widget_set_tooltip_text(GTK_WIDGET(d->metadata[md_internal_flags]), flags_tooltip);

      g_free(star_string);
      g_free(flags_tooltip);

      #undef EMPTY_FIELD
      #undef FALSE_FIELD
      #undef TRUE_FIELD
    }
#endif // SHOW_FLAGS

    /* EXIF */
    _metadata_update_value_end(d->metadata[md_exif_model], img->camera_alias);
    _metadata_update_value_end(d->metadata[md_exif_lens], img->exif_lens);
    _metadata_update_value_end(d->metadata[md_exif_maker], img->camera_maker);

    snprintf(value, sizeof(value), "f/%.1f", img->exif_aperture);
    _metadata_update_value(d->metadata[md_exif_aperture], value);

    char *exposure_str = dt_util_format_exposure(img->exif_exposure);
    _metadata_update_value(d->metadata[md_exif_exposure], exposure_str);
    g_free(exposure_str);

    if(isnan(img->exif_exposure_bias))
    {
      _metadata_update_value(d->metadata[md_exif_exposure_bias], NODATA_STRING);
    }
    else
    {
      snprintf(value, sizeof(value), _("%+.2f EV"), img->exif_exposure_bias);
      _metadata_update_value(d->metadata[md_exif_exposure_bias], value);
    }

    snprintf(value, sizeof(value), "%.0f mm", img->exif_focal_length);
    _metadata_update_value(d->metadata[md_exif_focal_length], value);

    if(isnan(img->exif_focus_distance) || fpclassify(img->exif_focus_distance) == FP_ZERO)
    {
      _metadata_update_value(d->metadata[md_exif_focus_distance], NODATA_STRING);
    }
    else
    {
      snprintf(value, sizeof(value), "%.2f m", img->exif_focus_distance);
      _metadata_update_value(d->metadata[md_exif_focus_distance], value);
    }

    snprintf(value, sizeof(value), "%.0f", img->exif_iso);
    _metadata_update_value(d->metadata[md_exif_iso], value);

    struct tm tt_exif = { 0 };
    if(sscanf(img->exif_datetime_taken, "%d:%d:%d %d:%d:%d", &tt_exif.tm_year, &tt_exif.tm_mon,
      &tt_exif.tm_mday, &tt_exif.tm_hour, &tt_exif.tm_min, &tt_exif.tm_sec) == 6)
    {
      char datetime[200];
      tt_exif.tm_year -= 1900;
      tt_exif.tm_mon--;
      tt_exif.tm_isdst = -1;
      mktime(&tt_exif);
      // just %c is too long and includes a time zone that we don't know from exif
      strftime(datetime, sizeof(datetime), "%a %x %X", &tt_exif);
      _metadata_update_value(d->metadata[md_exif_datetime], g_locale_to_utf8(datetime,-1,NULL,NULL,NULL));
    }
    else
      _metadata_update_value(d->metadata[md_exif_datetime], img->exif_datetime_taken);

    if(((img->p_width != img->width) || (img->p_height != img->height))  &&
       (img->p_width || img->p_height))
    {
      snprintf(value, sizeof(value), "%d (%d)", img->p_height, img->height);
      _metadata_update_value(d->metadata[md_exif_height], value);
      snprintf(value, sizeof(value), "%d (%d) ",img->p_width, img->width);
      _metadata_update_value(d->metadata[md_exif_width], value);
    }
    else {
    snprintf(value, sizeof(value), "%d", img->height);
    _metadata_update_value(d->metadata[md_exif_height], value);
    snprintf(value, sizeof(value), "%d", img->width);
    _metadata_update_value(d->metadata[md_exif_width], value);
    }

    if(img->verified_size)
    {
      snprintf(value, sizeof(value), "%d", img->final_height);
    _metadata_update_value(d->metadata[md_height], value);
      snprintf(value, sizeof(value), "%d", img->final_width);
    _metadata_update_value(d->metadata[md_width], value);
    }
    else
    {
      _metadata_update_value(d->metadata[md_height], "-");
      _metadata_update_value(d->metadata[md_width], "-");
    }
    /* XMP */
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
      const gchar *key = dt_metadata_get_key(keyid);
      const gchar *name = dt_metadata_get_name(keyid);
      gchar *setting = dt_util_dstrcat(NULL, "plugins/lighttable/metadata/%s_flag", name);
      const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
      g_free(setting);
      const int meta_type = dt_metadata_get_type(keyid);
      if(meta_type == DT_METADATA_TYPE_INTERNAL || hidden)
      {
        gtk_widget_hide(GTK_WIDGET(d->name[md_xmp_metadata+i]));
        gtk_widget_hide(GTK_WIDGET(d->metadata[md_xmp_metadata+i]));
        g_strlcpy(value, NODATA_STRING, sizeof(value));
      }
      else
      {
        gtk_widget_show(GTK_WIDGET(d->name[md_xmp_metadata+i]));
        gtk_widget_show(GTK_WIDGET(d->metadata[md_xmp_metadata+i]));
        GList *res = dt_metadata_get(img->id, key, NULL);
        if(res)
        {
          g_strlcpy(value, (char *)res->data, sizeof(value));
          _filter_non_printable(value, sizeof(value));
          g_list_free_full(res, &g_free);
        }
        else
          g_strlcpy(value, NODATA_STRING, sizeof(value));
      }
      _metadata_update_value(d->metadata[md_xmp_metadata+i], value);
    }

    /* tags */
    GList *tags = NULL;
    char *tagstring = NULL;
    char *categoriesstring = NULL;
    if(dt_tag_get_attached(mouse_over_id, &tags, TRUE))
    {
      gint length = 0;
      for(GList *taglist = tags; taglist; taglist = g_list_next(taglist))
      {
        const char *tagname = ((dt_tag_t *)taglist->data)->leave;
        if (!(((dt_tag_t *)taglist->data)->flags & DT_TF_CATEGORY))
        {
          // tags - just keywords
          length = length + strlen(tagname) + 2;
          if(length < 45)
            tagstring = dt_util_dstrcat(tagstring, "%s, ", tagname);
          else
          {
            tagstring = dt_util_dstrcat(tagstring, "\n%s, ", tagname);
            length = strlen(tagname) + 2;
          }
        }
        else
        {
          // categories - needs parent category to make sense
          char *category = g_strdup(((dt_tag_t *)taglist->data)->tag);
          char *catend = g_strrstr(category, "|");
          if (catend)
          {
            catend[0] = '\0';
            char *catstart = g_strrstr(category, "|");
            catstart = catstart ? catstart + 1 : category;
            categoriesstring = dt_util_dstrcat(categoriesstring, categoriesstring ? "\n%s: %s " : "%s: %s ",
                  catstart, ((dt_tag_t *)taglist->data)->leave);
          }
          else
            categoriesstring = dt_util_dstrcat(categoriesstring, categoriesstring ? "\n%s" : "%s",
                  ((dt_tag_t *)taglist->data)->leave);
          g_free(category);
        }
      }
      if(tagstring) tagstring[strlen(tagstring)-2] = '\0';
    }
    _metadata_update_value(d->metadata[md_tag_names], tagstring ? tagstring : NODATA_STRING);
    _metadata_update_value(d->metadata[md_categories], categoriesstring ? categoriesstring : NODATA_STRING);

    g_free(tagstring);
    g_free(categoriesstring);
    dt_tag_free_result(&tags);

    /* release img */
    dt_image_cache_read_release(darktable.image_cache, img);
/*
#ifdef USE_LUA
    dt_lua_async_call_alien(lua_update_metadata,
        0,NULL,NULL,
        LUA_ASYNC_TYPENAME,"void*",self,
        LUA_ASYNC_TYPENAME,"int32_t",mouse_over_id,LUA_ASYNC_DONE);
#endif*/
  }

  return;

/* reset */
fill_minuses:
  for(int k = 0; k < md_size; k++) _metadata_update_value(d->metadata[k], NODATA_STRING);
/*#ifdef USE_LUA
  dt_lua_async_call_alien(lua_update_metadata,
      0,NULL,NULL,
        LUA_ASYNC_TYPENAME,"void*",self,
        LUA_ASYNC_TYPENAME,"int32_t",-1,LUA_ASYNC_DONE);
#endif*/
}

static void _jump_to()
{
  int32_t imgid = dt_control_get_mouse_over_id();
  if(imgid == -1)
  {
    sqlite3_stmt *stmt;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1, &stmt,
                                NULL);

    if(sqlite3_step(stmt) == SQLITE_ROW) imgid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  if(imgid != -1)
  {
    char path[512];
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    dt_image_film_roll_directory(img, path, sizeof(path));
    dt_image_cache_read_release(darktable.image_cache, img);
    char collect[1024];
    snprintf(collect, sizeof(collect), "1:0:0:%s$", path);
    dt_collection_deserialize(collect);
  }
}

static gboolean _filmroll_clicked(GtkWidget *widget, GdkEventButton *event, gpointer null)
{
  if(event->type != GDK_2BUTTON_PRESS) return FALSE;
  _jump_to();
  return TRUE;
}

/* callback for the mouse over image change signal */
static void _mouse_over_image_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(dt_control_running()) _metadata_view_update_values(self);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)g_malloc0(sizeof(dt_lib_metadata_view_t));
  self->data = (void *)d;
  _lib_metatdata_view_init_labels();

  GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
  GtkWidget *child_grid_window = gtk_grid_new();
  gtk_container_add(GTK_CONTAINER(scrolled_window), child_grid_window);

  d->scrolled_window = GTK_WIDGET(scrolled_window);
  self->widget = d->scrolled_window;

  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));
  gtk_grid_set_column_spacing(GTK_GRID(child_grid_window), DT_PIXEL_APPLY_DPI(5));

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(d->scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(d->scrolled_window), DT_PIXEL_APPLY_DPI(300));
  const gint height = dt_conf_get_int("plugins/lighttable/metadata_view/windowheight");
  gtk_widget_set_size_request(d->scrolled_window, -1, DT_PIXEL_APPLY_DPI(height));

  /* initialize the metadata name/value labels */
  for(int k = 0; k < md_size; k++)
  {
    GtkLabel *name = GTK_LABEL(gtk_label_new(_md_labels[k]));
    d->name[k] = name;
    d->metadata[k] = GTK_LABEL(gtk_label_new("-"));
    gtk_widget_set_name(GTK_WIDGET(d->metadata[k]), "brightbg");
    gtk_label_set_selectable(d->metadata[k], TRUE);
    gtk_label_set_xalign (d->metadata[k], 0.0f);
    if(k == md_internal_filmroll)
    {
      // film roll jump to:
      g_signal_connect(G_OBJECT(GTK_WIDGET(d->metadata[k])), "button-press-event", G_CALLBACK(_filmroll_clicked), NULL);
    }
    gtk_widget_set_halign(GTK_WIDGET(name), GTK_ALIGN_START);
    gtk_widget_set_halign(GTK_WIDGET(d->metadata[k]), GTK_ALIGN_FILL);
    gtk_grid_attach(GTK_GRID(child_grid_window), GTK_WIDGET(name), 0, k, 1, 1);
    gtk_grid_attach(GTK_GRID(child_grid_window), GTK_WIDGET(GTK_WIDGET(d->metadata[k])), 1, k, 1, 1);
  }

  /* lets signup for mouse over image change signals */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);

  /* lets signup for develop image changed signals */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                            G_CALLBACK(_mouse_over_image_callback), self);

  /* signup for develop initialize to update info of current
     image in darkroom when enter */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE,
                            G_CALLBACK(_mouse_over_image_callback), self);

  /* signup for tags changes */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_TAG_CHANGED,
                            G_CALLBACK(_mouse_over_image_callback), self);

  /* signup for metadata changes */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_METADATA_UPDATE,
                            G_CALLBACK(_mouse_over_image_callback), self);

  /* adaptable window size */
  g_signal_connect(G_OBJECT(self->widget), "scroll-event", G_CALLBACK(view_onMouseScroll), d);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
  g_free(self->data);
  self->data = NULL;
}

static gboolean view_onMouseScroll(GtkWidget *view, GdkEventScroll *event, dt_lib_metadata_view_t *d)
{
  if(event->state & GDK_CONTROL_MASK)
  {
    const gint increment = DT_PIXEL_APPLY_DPI(10.0);
    const gint min_height = gtk_scrolled_window_get_min_content_height(GTK_SCROLLED_WINDOW(d->scrolled_window));
    const gint max_height = DT_PIXEL_APPLY_DPI(1000.0);
    gint width, height;

    gtk_widget_get_size_request(GTK_WIDGET(d->scrolled_window), &width, &height);
    height = height + increment*event->delta_y;
    height = (height < min_height) ? min_height : (height > max_height) ? max_height : height;

    gtk_widget_set_size_request(GTK_WIDGET(d->scrolled_window), -1, height);
    dt_conf_set_int("plugins/lighttable/metadata_view/windowheight", height);

    return TRUE;
  }
  return FALSE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
