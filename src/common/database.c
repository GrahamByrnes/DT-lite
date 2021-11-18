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

#include "common/database.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/iop_order.h"
#include "common/styles.h"
#include "common/history.h"
#include "control/conf.h"
#include "control/control.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
// whenever _create_*_schema() gets changed you HAVE to bump this version and add an update path to
// _upgrade_*_schema_step()!
#define CURRENT_DATABASE_VERSION_LIBRARY 30
#define CURRENT_DATABASE_VERSION_DATA     6

typedef struct dt_database_t
{
  gboolean lock_acquired;
  // data database filename
  gchar *dbfilename_data, *lockfile_data;
  // library database filename
  gchar *dbfilename_library, *lockfile_library;
  // ondisk DB
  sqlite3 *handle;
  gchar *error_message, *error_dbfilename;
  int error_other_pid;
} dt_database_t;

/* migrates database from old place to new */
static void _database_migrate_to_xdg_structure();

#define _SQLITE3_EXEC(a, b, c, d, e)                                                                         \
  if(sqlite3_exec(a, b, c, d, e) != SQLITE_OK)                                                               \
  {                                                                                                          \
    all_ok = FALSE;                                                                                          \
    failing_query = b;                                                                                       \
    goto end;                                                                                                \
  }

// migrate from the legacy db format (with the 'settings' blob) to the first version this system knows
static gboolean _migrate_schema(dt_database_t *db, int version)
{
  gboolean all_ok = TRUE;
  const char *failing_query = NULL;
  sqlite3_stmt *stmt;
  sqlite3_stmt *innerstmt;

  if(version != 36) // if anyone shows up with an older db we can probably add extra code
    return FALSE;

  sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
  // remove stuff that is either no longer needed or that got renamed
  _SQLITE3_EXEC(db->handle, "DROP TABLE IF EXISTS main.lock", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE IF EXISTS main.settings", NULL, NULL, NULL); // yes, we do this in many
                                                                                     // places. because it's really
                                                                                     // important to not miss it in
                                                                                     // any code path.
  _SQLITE3_EXEC(db->handle, "DROP INDEX IF EXISTS main.group_id_index", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP INDEX IF EXISTS main.imgid_index", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE IF EXISTS main.mipmaps", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE IF EXISTS main.mipmap_timestamps", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE IF EXISTS main.dt_migration_table", NULL, NULL, NULL);
  // using _create_library_schema() and filling that with the old data doesn't work since we always want to generate
  // version 1 tables
  // db_info
  _SQLITE3_EXEC(db->handle, "CREATE TABLE main.db_info (key VARCHAR PRIMARY KEY, value VARCHAR)",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT OR REPLACE INTO main.db_info (key, value) VALUES ('version', 1)",
                NULL, NULL, NULL);
  // film_rolls
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS main.film_rolls_folder_index ON film_rolls (folder)",
                NULL, NULL, NULL);
  // images
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN orientation INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN focus_distance REAL", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN group_id INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN histogram BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN lightmap BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN longitude REAL", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN latitude REAL", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN color_matrix BLOB", NULL, NULL, NULL);
  // the colorspace as specified in some image types
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN colorspace INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN max_version INTEGER", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.images SET orientation = -1 WHERE orientation IS NULL", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.images SET focus_distance = -1 WHERE focus_distance IS NULL",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.images SET group_id = id WHERE group_id IS NULL", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.images SET max_version = (SELECT COUNT(*)-1 FROM main.images i WHERE "
                            "i.filename = main.images.filename AND "
                            "i.film_id = main.images.film_id) WHERE max_version IS NULL",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(
      db->handle,
      "UPDATE main.images SET version = (SELECT COUNT(*) FROM main.images i "
      "WHERE i.filename = main.images.filename AND "
      "i.film_id = main.images.film_id AND i.id < main.images.id) WHERE version IS NULL", NULL, NULL, NULL);
  // make sure we have AUTOINCREMENT on imgid --> move the whole thing away and recreate the table :(
  _SQLITE3_EXEC(db->handle, "ALTER TABLE main.images RENAME TO dt_migration_table", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP INDEX IF EXISTS main.images_group_id_index", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP INDEX IF EXISTS main.images_film_id_index", NULL, NULL, NULL);
  _SQLITE3_EXEC(
      db->handle,
      "CREATE TABLE main.images (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, film_id INTEGER, "
      "width INTEGER, height INTEGER, filename VARCHAR, maker VARCHAR, model VARCHAR, "
      "lens VARCHAR, exposure REAL, aperture REAL, iso REAL, focal_length REAL, "
      "focus_distance REAL, datetime_taken CHAR(20), flags INTEGER, "
      "output_width INTEGER, output_height INTEGER, crop REAL, "
      "raw_parameters INTEGER, raw_denoise_threshold REAL, "
      "raw_auto_bright_threshold REAL, raw_black INTEGER, raw_maximum INTEGER, "
      "caption VARCHAR, description VARCHAR, license VARCHAR, sha1sum CHAR(40), "
      "orientation INTEGER, histogram BLOB, lightmap BLOB, longitude REAL, "
      "latitude REAL, color_matrix BLOB, colorspace INTEGER, version INTEGER, max_version INTEGER)",
      NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE INDEX main.images_group_id_index ON images (group_id)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE INDEX main.images_film_id_index ON images (film_id)", NULL, NULL, NULL);
  _SQLITE3_EXEC(
      db->handle,
      "INSERT INTO main.images (id, group_id, film_id, width, height, filename, maker, model, "
      "lens, exposure, aperture, iso, focal_length, focus_distance, datetime_taken, flags, "
      "output_width, output_height, crop, raw_parameters, raw_denoise_threshold, "
      "raw_auto_bright_threshold, raw_black, raw_maximum, caption, description, license, sha1sum, "
      "orientation, histogram, lightmap, longitude, latitude, color_matrix, colorspace, version, "
      "max_version) "
      "SELECT id, group_id, film_id, width, height, filename, maker, model, lens, exposure, aperture, iso, "
      "focal_length, focus_distance, datetime_taken, flags, output_width, output_height, crop, "
      "raw_parameters, raw_denoise_threshold, raw_auto_bright_threshold, raw_black, raw_maximum, "
      "caption, description, license, sha1sum, orientation, histogram, lightmap, longitude, "
      "latitude, color_matrix, colorspace, version, max_version FROM dt_migration_table",
      NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE dt_migration_table", NULL, NULL, NULL);
  // selected_images
  // selected_images should have a primary key. add it if it's missing:
  _SQLITE3_EXEC(db->handle, "CREATE TEMPORARY TABLE dt_migration_table (imgid INTEGER)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT INTO dt_migration_table SELECT imgid FROM main.selected_images",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE main.selected_images", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE TABLE main.selected_images (imgid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT OR IGNORE INTO main.selected_images SELECT imgid FROM dt_migration_table",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE dt_migration_table", NULL, NULL, NULL);
  // history
  sqlite3_exec(db->handle, "ALTER TABLE main.history ADD COLUMN blendop_params BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.history ADD COLUMN blendop_version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.history ADD COLUMN multi_priority INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.history ADD COLUMN multi_name VARCHAR(256)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS main.history_imgid_index ON history (imgid)",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.history SET blendop_version = 1 WHERE blendop_version IS NULL",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.history SET multi_priority = 0 WHERE multi_priority IS NULL",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.history SET multi_name = ' ' WHERE multi_name IS NULL", NULL, NULL, NULL);
  // mask
  _SQLITE3_EXEC(db->handle, "CREATE TABLE IF NOT EXISTS main.mask (imgid INTEGER, formid INTEGER, form INTEGER, "
                            "name VARCHAR(256), version INTEGER, "
                            "points BLOB, points_count INTEGER, source BLOB)",
                NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.mask ADD COLUMN source BLOB", NULL, NULL, NULL);
               // in case the table was there already but missed that column
  // tagged_images
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS main.tagged_images_tagid_index ON tagged_images (tagid)",
                NULL, NULL, NULL);
  // styles
  _SQLITE3_EXEC(db->handle,
                "CREATE TABLE IF NOT EXISTS main.styles (id INTEGER, name VARCHAR, description VARCHAR)",
                NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.styles ADD COLUMN id INTEGER", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.styles SET id = rowid WHERE id IS NULL", NULL, NULL, NULL);
  // style_items
  _SQLITE3_EXEC(db->handle, "CREATE TABLE IF NOT EXISTS main.style_items (styleid INTEGER, num INTEGER, module "
                            "INTEGER, operation VARCHAR(256), op_params BLOB, "
                            "enabled INTEGER, blendop_params BLOB, blendop_version INTEGER, multi_priority "
                            "INTEGER, multi_name VARCHAR(256))",
                NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.style_items ADD COLUMN blendop_params BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.style_items ADD COLUMN blendop_version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.style_items ADD COLUMN multi_priority INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.style_items ADD COLUMN multi_name VARCHAR(256)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.style_items SET blendop_version = 1 WHERE blendop_version IS NULL",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.style_items SET multi_priority = 0 WHERE multi_priority IS NULL",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.style_items SET multi_name = ' ' WHERE multi_name IS NULL",
                NULL, NULL, NULL);
  // color_labels
  // color_labels could have a PRIMARY KEY that we don't want
  _SQLITE3_EXEC(db->handle, "CREATE TEMPORARY TABLE dt_migration_table (imgid INTEGER, color INTEGER)",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT INTO dt_migration_table SELECT imgid, color FROM main.color_labels",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE main.color_labels", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE TABLE main.color_labels (imgid INTEGER, color INTEGER)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE UNIQUE INDEX main.color_labels_idx ON color_labels (imgid, color)",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT OR IGNORE INTO main.color_labels SELECT imgid, color FROM dt_migration_table",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE dt_migration_table", NULL, NULL, NULL);
  // meta_data
  _SQLITE3_EXEC(db->handle, "CREATE TABLE IF NOT EXISTS main.meta_data (id INTEGER, key INTEGER, value VARCHAR)",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS main.metadata_index ON meta_data (id, key)",
                NULL, NULL, NULL);
  // presets
  _SQLITE3_EXEC(db->handle, "CREATE TABLE IF NOT EXISTS main.presets (name VARCHAR, description VARCHAR, "
                            "operation VARCHAR, op_version INTEGER, op_params BLOB, "
                            "enabled INTEGER, blendop_params BLOB, blendop_version INTEGER, multi_priority "
                            "INTEGER, multi_name VARCHAR(256), "
                            "model VARCHAR, maker VARCHAR, lens VARCHAR, iso_min REAL, iso_max REAL, "
                            "exposure_min REAL, exposure_max REAL, "
                            "aperture_min REAL, aperture_max REAL, focal_length_min REAL, focal_length_max "
                            "REAL, writeprotect INTEGER, "
                            "autoapply INTEGER, filter INTEGER, def INTEGER, isldr INTEGER)",
                NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.presets ADD COLUMN op_version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.presets ADD COLUMN blendop_params BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.presets ADD COLUMN blendop_version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.presets ADD COLUMN multi_priority INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.presets ADD COLUMN multi_name VARCHAR(256)", NULL, NULL, NULL);
  // the unique index only works if the db doesn't have any (name, operation, op_version) more than once.
  // apparently there are dbs out there which do have that. :(
  sqlite3_prepare_v2(db->handle,
                     "SELECT p.rowid, p.name, p.operation, p.op_version FROM main.presets p INNER JOIN "
                     "(SELECT * FROM (SELECT rowid, name, operation, op_version, COUNT(*) AS count "
                     "FROM main.presets GROUP BY name, operation, op_version) WHERE count > 1) s "
                     "ON p.name = s.name AND p.operation = s.operation AND p.op_version = s.op_version",
                     -1, &stmt, NULL);
  char *last_name = NULL, *last_operation = NULL;
  int last_op_version = 0;
  int i = 0;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int rowid = sqlite3_column_int(stmt, 0);
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    const char *operation = (const char *)sqlite3_column_text(stmt, 2);
    int op_version = sqlite3_column_int(stmt, 3);
    // is it still the same (name, operation, op_version) triple?
    if(!last_name || strcmp(last_name, name) || !last_operation || strcmp(last_operation, operation)
       || last_op_version != op_version)
    {
      g_free(last_name);
      g_free(last_operation);
      last_name = g_strdup(name);
      last_operation = g_strdup(operation);
      last_op_version = op_version;
      i = 0;
    }
    // find the next free amended version of name
    sqlite3_prepare_v2(db->handle, "SELECT name FROM main.presets  WHERE name = ?1 || ' (' || ?2 || ')' AND "
                                   "operation = ?3 AND op_version = ?4", -1, &innerstmt, NULL);
    while(1)
    {
      sqlite3_bind_text(innerstmt, 1, name, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(innerstmt, 2, i);
      sqlite3_bind_text(innerstmt, 3, operation, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(innerstmt, 4, op_version);
      if(sqlite3_step(innerstmt) != SQLITE_ROW) break;
      sqlite3_reset(innerstmt);
      sqlite3_clear_bindings(innerstmt);
      i++;
    }

    sqlite3_finalize(innerstmt);
    // rename preset
    const char *query = "UPDATE main.presets SET name = name || ' (' || ?1 || ')' WHERE rowid = ?2";
    sqlite3_prepare_v2(db->handle, query, -1, &innerstmt, NULL);
    sqlite3_bind_int(innerstmt, 1, i);
    sqlite3_bind_int(innerstmt, 2, rowid);

    if(sqlite3_step(innerstmt) != SQLITE_DONE)
    {
      all_ok = FALSE;
      failing_query = query;
      goto end;
    }

    sqlite3_finalize(innerstmt);
  }
  sqlite3_finalize(stmt);
  g_free(last_name);
  g_free(last_operation);
  // now we should be able to create the index
  _SQLITE3_EXEC(db->handle,
                "CREATE UNIQUE INDEX IF NOT EXISTS main.presets_idx ON presets (name, operation, op_version)",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.presets SET blendop_version = 1 WHERE blendop_version IS NULL", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.presets SET multi_priority = 0 WHERE multi_priority IS NULL", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.presets SET multi_name = ' ' WHERE multi_name IS NULL", NULL, NULL, NULL);
  // There are systems where absolute paths don't start with '/' (like Windows).
  // Since the bug which introduced absolute paths to the db was fixed before a
  // Windows build was available this shouldn't matter though.
  sqlite3_prepare_v2(db->handle, "SELECT id, filename FROM main.images WHERE filename LIKE '/%'", -1, &stmt, NULL);
  sqlite3_prepare_v2(db->handle, "UPDATE main.images SET filename = ?1 WHERE id = ?2", -1, &innerstmt, NULL);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int id = sqlite3_column_int(stmt, 0);
    const char *path = (const char *)sqlite3_column_text(stmt, 1);
    gchar *filename = g_path_get_basename(path);
    sqlite3_bind_text(innerstmt, 1, filename, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(innerstmt, 2, id);
    sqlite3_step(innerstmt);
    sqlite3_reset(innerstmt);
    sqlite3_clear_bindings(innerstmt);
    g_free(filename);
  }

  sqlite3_finalize(stmt);
  sqlite3_finalize(innerstmt);
  // We used to insert datetime_taken entries with '-' as date separators. Since that doesn't work well with
  // the regular ':' when parsing
  // or sorting we changed it to ':'. This takes care to change what we have as leftovers
  _SQLITE3_EXEC(
      db->handle,
      "UPDATE main.images SET datetime_taken = REPLACE(datetime_taken, '-', ':') WHERE datetime_taken LIKE '%-%'",
      NULL, NULL, NULL);

end:
  if(all_ok)
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
  else
  {
    fprintf(stderr, "[init] failing query: `%s'\n", failing_query);
    fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
    sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
  }

  return all_ok;
}

#undef _SQLITE3_EXEC

#define TRY_EXEC(_query, _message)                                               \
  do                                                                             \
  {                                                                              \
    if(sqlite3_exec(db->handle, _query, NULL, NULL, NULL) != SQLITE_OK)          \
    {                                                                            \
      fprintf(stderr, _message);                                                 \
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));              \
      FINALIZE;                                                                  \
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);        \
      return version;                                                            \
    }                                                                            \
  } while(0)

#define TRY_STEP(_stmt, _expected, _message)                                     \
  do                                                                             \
  {                                                                              \
    if(sqlite3_step(_stmt) != _expected)                                         \
    {                                                                            \
      fprintf(stderr, _message);                                                 \
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));              \
      FINALIZE;                                                                  \
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);        \
      return version;                                                            \
    }                                                                            \
  } while(0)

#define TRY_PREPARE(_stmt, _query, _message)                                     \
  do                                                                             \
  {                                                                              \
    if(sqlite3_prepare_v2(db->handle, _query, -1, &_stmt, NULL) != SQLITE_OK)    \
    {                                                                            \
      fprintf(stderr, _message);                                                 \
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));              \
      FINALIZE;                                                                  \
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);        \
      return version;                                                            \
    }                                                                            \
  } while(0)

// redefine this where needed
#define FINALIZE

// do the real migration steps, returns the version the db was converted to
static int _upgrade_library_schema_step(dt_database_t *db, int version)
{
  sqlite3_stmt *stmt;
  int new_version = version;

  if(version == CURRENT_DATABASE_VERSION_LIBRARY)
    return version;
  else if(version <= 29)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // add position in tagged_images table
    TRY_EXEC("ALTER TABLE main.tagged_images ADD COLUMN position INTEGER",
             "[init] can't add `position' column to tagged_images table in database\n");
    TRY_EXEC("CREATE INDEX IF NOT EXISTS main.tagged_images_imgid_index ON tagged_images (imgid)",
             "[init] can't create image index on tagged_images\n");
    TRY_EXEC("CREATE INDEX IF NOT EXISTS main.tagged_images_position_index ON tagged_images (position)",
             "[init] can't create position index on tagged_images\n");
    TRY_EXEC("UPDATE main.tagged_images SET position = (tagid + imgid) << 32",
             "[init] can't populate position on tagged_images\n");

    // remove caption and description fields from images table
    TRY_EXEC("CREATE TABLE main.i (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, film_id INTEGER, "
             "width INTEGER, height INTEGER, filename VARCHAR, maker VARCHAR, model VARCHAR, "
             "lens VARCHAR, exposure REAL, aperture REAL, iso REAL, focal_length REAL, "
             "focus_distance REAL, datetime_taken CHAR(20), flags INTEGER, "
             "output_width INTEGER, output_height INTEGER, crop REAL, "
             "raw_parameters INTEGER, raw_denoise_threshold REAL, "
             "raw_auto_bright_threshold REAL, raw_black INTEGER, raw_maximum INTEGER, "
             "license VARCHAR, sha1sum CHAR(40), "
             "orientation INTEGER, histogram BLOB, lightmap BLOB, longitude REAL, "
             "latitude REAL, altitude REAL, color_matrix BLOB, colorspace INTEGER, version INTEGER, "
             "max_version INTEGER, write_timestamp INTEGER, history_end INTEGER, position INTEGER, "
             "aspect_ratio REAL, exposure_bias REAL, "
             "import_timestamp INTEGER DEFAULT -1, change_timestamp INTEGER DEFAULT -1, "
             "export_timestamp INTEGER DEFAULT -1, print_timestamp INTEGER DEFAULT -1)",
             "[init] can't create table i\n");

    TRY_EXEC("INSERT INTO main.i SELECT id, group_id, film_id, width, height, filename, maker, model,"
             " lens, exposure, aperture, iso, focal_length, focus_distance, datetime_taken, flags,"
             " output_width, output_height, crop, raw_parameters, raw_denoise_threshold,"
             " raw_auto_bright_threshold, raw_black, raw_maximum, license, sha1sum,"
             " orientation, histogram, lightmap, longitude, latitude, altitude, color_matrix, colorspace, version,"
             " max_version, write_timestamp, history_end, position, aspect_ratio, exposure_bias,"
             " import_timestamp, change_timestamp, export_timestamp, print_timestamp "
             "FROM main.images",
             "[init] can't populate table i\n");
    TRY_EXEC("DROP TABLE main.images",
             "[init] can't drop table images\n");
    TRY_EXEC("ALTER TABLE main.i RENAME TO images",
             "[init] can't rename i to images\n");

    TRY_EXEC("CREATE INDEX main.images_group_id_index ON images (group_id)",
          "[init] can't create group_id index on images table\n");
    TRY_EXEC("CREATE INDEX main.images_film_id_index ON images (film_id)",
          "[init] can't create film_id index on images table\n");
    TRY_EXEC("CREATE INDEX main.images_filename_index ON images (filename)",
          "[init] can't create filename index on images table\n");
    TRY_EXEC("CREATE INDEX main.image_position_index ON images (position)",
          "[init] can't create position index on images table\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 30;
  }
  else
    new_version = version; // should be the fallback so that calling code sees that we are in an infinite loop
  // write the new version to db
  sqlite3_prepare_v2(db->handle, "INSERT OR REPLACE INTO main.db_info (key, value) VALUES ('version', ?1)",
                     -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, new_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return new_version;
}

// do the real migration steps, returns the version the db was converted to
static int _upgrade_data_schema_step(dt_database_t *db, int version)
{
  sqlite3_stmt *stmt;
  int new_version = version;

  if(version == CURRENT_DATABASE_VERSION_DATA)
    return version;
  else if(version <= 5)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // make style.id a PRIMARY KEY and add iop_list
    TRY_EXEC("ALTER TABLE data.styles RENAME TO s",
             "[init] can't rename styles to s\n");
    TRY_EXEC("CREATE TABLE data.styles (id INTEGER PRIMARY KEY, name VARCHAR, description VARCHAR, iop_list VARCHAR)",
             "[init] can't create styles table\n");
    TRY_EXEC("INSERT INTO data.styles SELECT id, name, description, NULL FROM s",
             "[init] can't populate styles table\n");
    TRY_EXEC("DROP TABLE s",
             "[init] can't drop table s\n");
    TRY_EXEC("CREATE INDEX IF NOT EXISTS data.styles_name_index ON styles (name)",
             "[init] can't create styles_nmae_index\n");
    // make style_items.styleid index
    TRY_EXEC("CREATE INDEX IF NOT EXISTS data.style_items_styleid_index ON style_items (styleid)",
             "[init] can't create style_items_styleid_index\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 6;
  }
  else
    new_version = version; // should be the fallback so that calling code sees that we are in an infinite loop

  // write the new version to db
  sqlite3_prepare_v2(db->handle, "INSERT OR REPLACE INTO data.db_info (key, value) VALUES ('version', ?1)",
                     -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, new_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return new_version;
}

#undef FINALIZE

#undef TRY_EXEC
#undef TRY_STEP
#undef TRY_PREPARE

// upgrade library db from 'version' to CURRENT_DATABASE_VERSION_LIBRARY. don't touch this function but
// _upgrade_library_schema_step() instead.
static gboolean _upgrade_library_schema(dt_database_t *db, int version)
{
  while(version < CURRENT_DATABASE_VERSION_LIBRARY)
  {
    const int new_version = _upgrade_library_schema_step(db, version);

    if(new_version == version)
      return FALSE; // we don't know how to upgrade this db. probably a bug in _upgrade_library_schema_step
    else
      version = new_version;
  }

  return TRUE;
}

// upgrade data db from 'version' to CURRENT_DATABASE_VERSION_DATA. don't touch this function but
// _upgrade_data_schema_step() instead.
static gboolean _upgrade_data_schema(dt_database_t *db, int version)
{
  while(version < CURRENT_DATABASE_VERSION_DATA)
  {
    const int new_version = _upgrade_data_schema_step(db, version);

    if(new_version == version)
      return FALSE; // we don't know how to upgrade this db. probably a bug in _upgrade_data_schema_step
    else
      version = new_version;
  }

  return TRUE;
}

// create the current database schema and set the version in db_info accordingly
static void _create_library_schema(dt_database_t *db)
{
  sqlite3_stmt *stmt;
  // db_info
  sqlite3_exec(db->handle, "CREATE TABLE main.db_info (key VARCHAR PRIMARY KEY, value VARCHAR)", NULL,
               NULL, NULL);
  sqlite3_prepare_v2(
      db->handle, "INSERT OR REPLACE INTO main.db_info (key, value) VALUES ('version', ?1)", -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, CURRENT_DATABASE_VERSION_LIBRARY);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  // film_rolls
  sqlite3_exec(db->handle,
               "CREATE TABLE main.film_rolls "
               "(id INTEGER PRIMARY KEY, access_timestamp INTEGER, "
               //                        "folder VARCHAR(1024), external_drive VARCHAR(1024))", //
               //                        FIXME: make sure to bump CURRENT_DATABASE_VERSION_LIBRARY and add a
               //                        case to _upgrade_library_schema_step when adding this!
               "folder VARCHAR(1024) NOT NULL)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.film_rolls_folder_index ON film_rolls (folder)", NULL, NULL, NULL);
  // images
  sqlite3_exec(
      db->handle,
      "CREATE TABLE main.images (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, film_id INTEGER, "
      "width INTEGER, height INTEGER, filename VARCHAR, maker VARCHAR, model VARCHAR, "
      "lens VARCHAR, exposure REAL, aperture REAL, iso REAL, focal_length REAL, "
      "focus_distance REAL, datetime_taken CHAR(20), flags INTEGER, "
      "output_width INTEGER, output_height INTEGER, crop REAL, "
      "raw_parameters INTEGER, raw_denoise_threshold REAL, "
      "raw_auto_bright_threshold REAL, raw_black INTEGER, raw_maximum INTEGER, "
      "license VARCHAR, sha1sum CHAR(40), "
      "orientation INTEGER, histogram BLOB, lightmap BLOB, longitude REAL, "
      "latitude REAL, altitude REAL, color_matrix BLOB, colorspace INTEGER, version INTEGER, "
      "max_version INTEGER, write_timestamp INTEGER, history_end INTEGER, position INTEGER, "
      "aspect_ratio REAL, exposure_bias REAL, "
      "import_timestamp INTEGER DEFAULT -1, change_timestamp INTEGER DEFAULT -1, "
      "export_timestamp INTEGER DEFAULT -1, print_timestamp INTEGER DEFAULT -1)",
      NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.images_group_id_index ON images (group_id)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.images_film_id_index ON images (film_id)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.images_filename_index ON images (filename)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.image_position_index ON images (position)", NULL, NULL, NULL);
  // selected_images
  sqlite3_exec(db->handle, "CREATE TABLE main.selected_images (imgid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  // history
  sqlite3_exec(
      db->handle,
      "CREATE TABLE main.history (imgid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
      NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.history_imgid_index ON history (imgid)", NULL, NULL, NULL);
  // masks history
  sqlite3_exec(db->handle,
               "CREATE TABLE main.masks_history (imgid INTEGER, num INTEGER, formid INTEGER, form INTEGER, name VARCHAR(256), "
               "version INTEGER, points BLOB, points_count INTEGER, source BLOB)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle,
      "CREATE INDEX main.masks_history_imgid_index ON masks_history (imgid)",
      NULL, NULL, NULL);
  // tagged_images
  sqlite3_exec(db->handle, "CREATE TABLE main.tagged_images (imgid INTEGER, tagid INTEGER, position INTEGER, "
                           "PRIMARY KEY (imgid, tagid))", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.tagged_images_tagid_index ON tagged_images (tagid)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.tagged_images_imgid_index ON selected_images (imgid)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.tagged_images_position_index ON selected_images (position)", NULL, NULL, NULL);
  // color_labels
  sqlite3_exec(db->handle, "CREATE TABLE main.color_labels (imgid INTEGER, color INTEGER)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX main.color_labels_idx ON color_labels (imgid, color)", NULL, NULL,
               NULL);
  // meta_data
  sqlite3_exec(db->handle, "CREATE TABLE main.meta_data (id INTEGER, key INTEGER, value VARCHAR)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.metadata_index ON meta_data (id, key)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE main.module_order (imgid INTEGER PRIMARY KEY, version INTEGER, iop_list VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE main.history_hash (imgid INTEGER PRIMARY KEY, "
               "basic_hash BLOB, auto_hash BLOB, current_hash BLOB, mipmap_hash BLOB)",
               NULL, NULL, NULL);
}

// create the current database schema and set the version in db_info accordingly
static void _create_data_schema(dt_database_t *db)
{
  sqlite3_stmt *stmt;
  // db_info
  sqlite3_exec(db->handle, "CREATE TABLE data.db_info (key VARCHAR PRIMARY KEY, value VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_prepare_v2(
        db->handle, "INSERT OR REPLACE INTO data.db_info (key, value) VALUES ('version', ?1)", -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, CURRENT_DATABASE_VERSION_DATA);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  // tags
  sqlite3_exec(db->handle, "CREATE TABLE data.tags (id INTEGER PRIMARY KEY, name VARCHAR, "
                           "synonyms VARCHAR, flags INTEGER)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX data.tags_name_idx ON tags (name)", NULL, NULL, NULL);
  // styles
  sqlite3_exec(db->handle, "CREATE TABLE data.styles (id INTEGER PRIMARY KEY, name VARCHAR, description VARCHAR, iop_list VARCHAR)",
                        NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX data.styles_name_index ON styles (name)", NULL, NULL, NULL);
  // style_items
  sqlite3_exec(
      db->handle,
      "CREATE TABLE data.style_items (styleid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
      NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE INDEX IF NOT EXISTS data.style_items_styleid_index ON style_items (styleid)",
      NULL, NULL, NULL);
  // presets
  sqlite3_exec(db->handle, "CREATE TABLE data.presets (name VARCHAR, description VARCHAR, operation "
                           "VARCHAR, op_version INTEGER, op_params BLOB, "
                           "enabled INTEGER, blendop_params BLOB, blendop_version INTEGER, "
                           "multi_priority INTEGER, multi_name VARCHAR(256), "
                           "model VARCHAR, maker VARCHAR, lens VARCHAR, iso_min REAL, iso_max REAL, "
                           "exposure_min REAL, exposure_max REAL, "
                           "aperture_min REAL, aperture_max REAL, focal_length_min REAL, "
                           "focal_length_max REAL, writeprotect INTEGER, "
                           "autoapply INTEGER, filter INTEGER, def INTEGER, format INTEGER)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX data.presets_idx ON presets (name, operation, op_version)",
               NULL, NULL, NULL);
}

// create the in-memory tables
// temporary stuff for some ops, need this for some reason with newer sqlite3:
static void _create_memory_schema(dt_database_t *db)
{
  sqlite3_exec(db->handle, "CREATE TABLE memory.color_labels_temp (imgid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.collected_images (rowid INTEGER PRIMARY KEY AUTOINCREMENT, imgid INTEGER)", NULL,
      NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.tmp_selection (imgid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.taglist "
                           "(tmpid INTEGER PRIMARY KEY, id INTEGER UNIQUE ON CONFLICT IGNORE, count INTEGER)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.similar_tags (tagid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.darktable_tags (tagid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.history (imgid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256) UNIQUE ON CONFLICT REPLACE, op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
      NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.undo_history (id INTEGER, imgid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
      NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.undo_masks_history (id INTEGER, imgid INTEGER, num INTEGER, formid INTEGER,"
      " form INTEGER, name VARCHAR(256), version INTEGER, points BLOB, points_count INTEGER, source BLOB)",
      NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.undo_module_order (id INTEGER, imgid INTEGER, version INTEGER, iop_list VARCHAR)",
      NULL, NULL, NULL);
  sqlite3_exec(db->handle,
      "CREATE TABLE memory.darktable_iop_names (operation VARCHAR(256) PRIMARY KEY, name VARCHAR(256))",
      NULL, NULL, NULL);
}

static void _sanitize_db(dt_database_t *db)
{
  sqlite3_stmt *stmt, *innerstmt;
  // first let's get rid of non-utf8 tags.
  sqlite3_prepare_v2(db->handle, "SELECT id, name FROM data.tags", -1, &stmt, NULL);
  sqlite3_prepare_v2(db->handle, "UPDATE data.tags SET name = ?1 WHERE id = ?2", -1, &innerstmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int id = sqlite3_column_int(stmt, 0);
    const char *tag = (const char *)sqlite3_column_text(stmt, 1);

    if(!g_utf8_validate(tag, -1, NULL))
    {
      gchar *new_tag = dt_util_foo_to_utf8(tag);
      fprintf(stderr, "[init]: tag `%s' is not valid utf8, replacing it with `%s'\n", tag, new_tag);
      if(tag)
      {
        sqlite3_bind_text(innerstmt, 1, new_tag, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(innerstmt, 2, id);
        sqlite3_step(innerstmt);
        sqlite3_reset(innerstmt);
        sqlite3_clear_bindings(innerstmt);
        g_free(new_tag);
      }
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_finalize(innerstmt);

  // make sure film_roll folders don't end in "/", that will result in empty entries in the collect module
  sqlite3_exec(db->handle,
               "UPDATE main.film_rolls SET folder = substr(folder, 1, length(folder) - 1) WHERE folder LIKE '%/'",
               NULL, NULL, NULL);

}

// in library we keep the names of the tags used in tagged_images. however, using that table at runtime results
// in some overhead not necessary so instead we just use the used_tags table to update tagged_images on startup
#define TRY_EXEC(_query, _message)                                                 \
  do                                                                               \
  {                                                                                \
    if(sqlite3_exec(db->handle, _query, NULL, NULL, NULL) != SQLITE_OK)            \
    {                                                                              \
      fprintf(stderr, _message);                                                   \
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));                \
      FINALIZE;                                                                    \
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);          \
      return FALSE;                                                                \
    }                                                                              \
  } while(0)

#define TRY_STEP(_stmt, _expected, _message)                                       \
  do                                                                               \
  {                                                                                \
    if(sqlite3_step(_stmt) != _expected)                                           \
    {                                                                              \
      fprintf(stderr, _message);                                                   \
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));                \
      FINALIZE;                                                                    \
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);          \
      return FALSE;                                                                \
    }                                                                              \
  } while(0)

#define TRY_PREPARE(_stmt, _query, _message)                                       \
  do                                                                               \
  {                                                                                \
    if(sqlite3_prepare_v2(db->handle, _query, -1, &_stmt, NULL) != SQLITE_OK)      \
    {                                                                              \
      fprintf(stderr, _message);                                                   \
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));                \
      FINALIZE;                                                                    \
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);          \
      return FALSE;                                                                \
    }                                                                              \
  } while(0)

#define FINALIZE                                                                   \
  do                                                                               \
  {                                                                                \
    sqlite3_finalize(stmt); stmt = NULL; /* NULL so that finalize becomes a NOP */ \
  } while(0)

#undef TRY_EXEC
#undef TRY_STEP
#undef TRY_PREPARE
#undef FINALIZE

void dt_database_show_error(const dt_database_t *db)
{
  if(!db->lock_acquired)
  {
    char lck_pathname[1024];
    snprintf(lck_pathname, sizeof(lck_pathname), "%s.lock", db->error_dbfilename);
    char *lck_dirname = g_strdup(lck_pathname);
    char *lck_filename = g_strrstr(lck_dirname, "/") + 1 ;
    *g_strrstr(lck_dirname, "/") = '\0';

    char *label_text = g_markup_printf_escaped(
        _("\n"
          " At startup, the database failed to open because at least one of the two files in the database is locked.\n"
          "\n"
          " The persistence of the lock is mainly caused by one of the two following causes:\n"
          "\n"
          " - Another occurrence of darktable has already opened this database file and locked it for its benefit.\n"
          "\n"
          " - A previous occurrence of darktable ended abnormally and therefore \n"
          "   could not close one or both files in the database properly.\n"
          "\n"
          " How to solve this problem?\n"
          "\n"
          " 1 - Search in your environment if another darktable occurrence is active. If so, use it or close it. \n"
          "     The lock indicates that the process number of this occurrence is : <i><b>%d</b></i>\n"
          "\n"
          " 2 - If you can't find this other occurrence, try closing your session and reopening it or shutting down your computer. \n"
          "     This will delete all running programs and thus close the database correctly.\n"
          "\n"
          " 3 - If these two actions are not enough, it is because at least one of the two files that materialize the locks remains \n"
          "     and that these are no longer attached to any occurrence of darktable. It is then necessary to delete it (or them). \n"
          "     The two files are named <i>data.db.lock</i> and <i>library.db.lock</i> respectively. The opening mechanism signals \n"
          "     the presence of the <i><b>%s</b></i> file in the <i><b>%s</b></i> folder. \n"
          "     (full pathname: <i><b>%s</b></i>).\n"
          "\n"
          "     <u>Caution!</u> Do not delete these files without first checking that there are no more occurrences of darktable, \n"
          "     otherwise you risk generating serious inconsistencies in your database.\n"
          "\n"
          " As soon as you have identified and removed the cause of the lock, darktable will start without any problem.\n"),
      db->error_other_pid, lck_filename, lck_dirname, lck_pathname);

    dt_gui_show_standalone_yes_no_dialog(_("darktable cannot be started because the database is locked"),
                                         label_text, _("close darktable"), NULL);

    g_free(lck_dirname);
    g_free(label_text);
  }

  g_free(db->error_message);
  g_free(db->error_dbfilename);
  ((dt_database_t *)db)->error_other_pid = 0;
  ((dt_database_t *)db)->error_message = NULL;
  ((dt_database_t *)db)->error_dbfilename = NULL;
}

static gboolean pid_is_alive(int pid)
{
  gboolean pid_is_alive;

#ifdef _WIN32
  pid_is_alive = FALSE;
  HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
  if(h)
  {
    wchar_t wfilename[MAX_PATH];
    long unsigned int n_filename = sizeof(wfilename);
    int ret = QueryFullProcessImageNameW(h, 0, wfilename, &n_filename);
    char *filename = g_utf16_to_utf8(wfilename, -1, NULL, NULL, NULL);
    if(ret && n_filename > 0 && filename && g_str_has_suffix(filename, "darktable.exe"))
      pid_is_alive = TRUE;
    g_free(filename);
    CloseHandle(h);
  }
#else
  pid_is_alive = !((kill(pid, 0) == -1) && errno == ESRCH);

#ifdef __linux__
  // If this is Linux, we can query /proc to see if the pid is
  // actually a darktable instance.
  if(pid_is_alive)
  {
    gchar *contents;
    gsize length;
    gchar filename[64];
    snprintf(filename, sizeof(filename), "/proc/%d/cmdline", pid);

    if(g_file_get_contents("", &contents, &length, NULL))
    {
      if(strstr(contents, "darktable") == NULL)
        pid_is_alive = FALSE;

      g_free(contents);
    }
  }
#endif

#endif

  return pid_is_alive;
}

static gboolean _lock_single_database(dt_database_t *db, const char *dbfilename, char **lockfile)
{
  gboolean lock_acquired = FALSE;
  mode_t old_mode;
  int lock_tries = 0;
  gchar *pid = g_strdup_printf("%d", getpid());

  if(!strcmp(dbfilename, ":memory:"))
    lock_acquired = TRUE;
  else
  {
    *lockfile = g_strconcat(dbfilename, ".lock", NULL);
lock_again:
    lock_tries++;
    old_mode = umask(0);
    int fd = g_open(*lockfile, O_RDWR | O_CREAT | O_EXCL, 0666);
    umask(old_mode);

    if(fd != -1) // the lockfile was successfully created - write our PID into it
    {
      if(write(fd, pid, strlen(pid) + 1) > -1) lock_acquired = TRUE;
      close(fd);
    }
    else // the lockfile already exists - see if it's a stale one left over from a crashed instance
    {
      char buf[64];
      memset(buf, 0, sizeof(buf));
      fd = g_open(*lockfile, O_RDWR | O_CREAT, 0666);
      if(fd != -1)
      {
        int foo;
        if((foo = read(fd, buf, sizeof(buf) - 1)) > 0)
        {
          db->error_other_pid = atoi(buf);
          if(!pid_is_alive(db->error_other_pid))
          {
            // the other process seems to no longer exist. unlink the .lock file and try again
            g_unlink(*lockfile);
            if(lock_tries < 5)
            {
              close(fd);
              goto lock_again;
            }
          }
          else
          {
            fprintf(
              stderr,
              "[init] the database lock file contains a pid that seems to be alive in your system: %d\n",
              db->error_other_pid);
            db->error_message = g_strdup_printf(_("the database lock file contains a pid that seems to be alive in your system: %d"), db->error_other_pid);
          }
        }
        else
        {
          fprintf(stderr, "[init] the database lock file seems to be empty\n");
          db->error_message = g_strdup_printf(_("the database lock file seems to be empty"));
        }
        close(fd);
      }
      else
      {
        int err = errno;
        fprintf(stderr, "[init] error opening the database lock file for reading: %s\n", strerror(err));
        db->error_message = g_strdup_printf(_("error opening the database lock file for reading: %s"), strerror(err));
      }
    }
  }

  g_free(pid);

  if(db->error_message)
    db->error_dbfilename = g_strdup(dbfilename);

  return lock_acquired;
}

static gboolean _lock_databases(dt_database_t *db)
{
  if(!_lock_single_database(db, db->dbfilename_data, &db->lockfile_data))
    return FALSE;

  if(!_lock_single_database(db, db->dbfilename_library, &db->lockfile_library))
  {
    // unlock data.db to not leave a stale lock file around
    g_unlink(db->lockfile_data);
    return FALSE;
  }
  return TRUE;
}

void ask_for_upgrade(const gchar *dbname, const gboolean has_gui)
{
  // if there's no gui just leave
  if(!has_gui)
  {
    fprintf(stderr, "[init] database `%s' is out-of-date. aborting.\n", dbname);
    exit(1);
  }
  // the database has to be upgraded, let's ask user
  char *label_text = g_markup_printf_escaped(_("the database schema has to be upgraded for\n"
                                               "\n"
                                               "<span style=\"italic\">%s</span>\n"
                                               "\n"
                                               "do you want to proceed or quit now to do a backup\n"),
                                               dbname);

  gboolean shall_we_update_the_db =
    dt_gui_show_standalone_yes_no_dialog(_("darktable - schema migration"), label_text,
                                         _("close darktable"), _("upgrade database"));
  g_free(label_text);
  // if no upgrade, we exit now, nothing we can do more
  if(!shall_we_update_the_db)
  {
    fprintf(stderr, "[init] we shall not update the database, aborting.\n");
    exit(1);
  }
}

void dt_database_backup(const char *filename)
{
  char *version = g_strdup_printf("%s", darktable_package_version);
  int k = 0;
  // get plain version (no commit id)
  while(version[k])
  {
    if((version[k] < '0' || version[k] > '9') && (version[k] != '.'))
    {
      version[k] = '\0';
      break;
    }
    k++;
  }

  gchar *backup = g_strdup_printf("%s-pre-%s", filename, version);
  GError *gerror = NULL;

  if(!g_file_test(backup, G_FILE_TEST_EXISTS))
  {
    GFile *src = g_file_new_for_path(filename);
    GFile *dest = g_file_new_for_path(backup);
    gboolean copyStatus = TRUE;

    if(g_file_test(filename, G_FILE_TEST_EXISTS))
    {
      copyStatus = g_file_copy(src, dest, G_FILE_COPY_NONE, NULL, NULL, NULL, &gerror);
      if(copyStatus) copyStatus = g_chmod(backup, S_IRUSR) == 0;
    }
    else
    {
      // there is nothing to backup, create an empty file to prevent further backup attempts
      int fd = g_open(backup, O_CREAT, S_IRUSR);
      if(fd < 0 || !g_close(fd, &gerror)) copyStatus = FALSE;
    }

    if(!copyStatus)
      fprintf(stderr, "[backup failed] %s -> %s\n", filename, backup);
  }

  g_free(version);
  g_free(backup);
}

dt_database_t *dt_database_init(const char *alternative, const gboolean load_data, const gboolean has_gui)
{
  //  set the threading mode to Serialized
  sqlite3_config(SQLITE_CONFIG_SERIALIZED);
  sqlite3_initialize();

start:
  if(alternative == NULL)
    // migrate default database location to new default */
    _database_migrate_to_xdg_structure();
  // lets construct the db filename  */
  gchar *dbname = NULL;
  gchar dbfilename_library[PATH_MAX] = { 0 };
  gchar datadir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(datadir, sizeof(datadir));

  if(alternative == NULL)
  {
    dbname = dt_conf_get_string("database");
    if(!dbname)
      snprintf(dbfilename_library, sizeof(dbfilename_library), "%s/library.db", datadir);
    else if(!strcmp(dbname, ":memory:"))
      g_strlcpy(dbfilename_library, dbname, sizeof(dbfilename_library));
    else if(dbname[0] != '/')
      snprintf(dbfilename_library, sizeof(dbfilename_library), "%s/%s", datadir, dbname);
    else
      g_strlcpy(dbfilename_library, dbname, sizeof(dbfilename_library));
  }
  else
  {
    g_strlcpy(dbfilename_library, alternative, sizeof(dbfilename_library));
    GFile *galternative = g_file_new_for_path(alternative);
    dbname = g_file_get_basename(galternative);
    g_object_unref(galternative);
  }
  // we also need a 2nd db with permanent data like presets, styles and tags
  char dbfilename_data[PATH_MAX] = { 0 };

  if(load_data)
    snprintf(dbfilename_data, sizeof(dbfilename_data), "%s/data.db", datadir);
  else
    snprintf(dbfilename_data, sizeof(dbfilename_data), ":memory:");
  // create database
  dt_database_t *db = (dt_database_t *)g_malloc0(sizeof(dt_database_t));
  db->dbfilename_data = g_strdup(dbfilename_data);
  db->dbfilename_library = g_strdup(dbfilename_library);
  // make sure the folder exists. this might not be the case for new databases
  // also check if a database backup is needed
  if(g_strcmp0(dbfilename_data, ":memory:"))
  {
    char *data_path = g_path_get_dirname(dbfilename_data);
    g_mkdir_with_parents(data_path, 0750);
    g_free(data_path);
    dt_database_backup(dbfilename_data);
  }

  if(g_strcmp0(dbfilename_library, ":memory:"))
  {
    char *library_path = g_path_get_dirname(dbfilename_library);
    g_mkdir_with_parents(library_path, 0750);
    g_free(library_path);
    dt_database_backup(dbfilename_library);
  }

  dt_print(DT_DEBUG_SQL, "[init sql] library: %s, data: %s\n", dbfilename_library, dbfilename_data);
  // having more than one instance of darktable using the same database is a bad idea
  // try to get locks for the databases
  db->lock_acquired = _lock_databases(db);

  if(!db->lock_acquired)
  {
    fprintf(stderr, "[init] database is locked, probably another process is already using it\n");
    g_free(dbname);
    return db;
  }
  // opening / creating database
  if(sqlite3_open(db->dbfilename_library, &db->handle))
  {
    fprintf(stderr, "[init] could not find database ");

    if(dbname)
      fprintf(stderr, "`%s'!\n", dbname);
    else
      fprintf(stderr, "\n");

    fprintf(stderr, "[init] maybe your %s/darktablerc is corrupt?\n", datadir);
    dt_loc_get_datadir(dbfilename_library, sizeof(dbfilename_library));
    fprintf(stderr, "[init] try `cp %s/darktablerc %s/darktablerc'\n", dbfilename_library, datadir);
    sqlite3_close(db->handle);
    g_free(dbname);
    g_free(db->lockfile_data);
    g_free(db->dbfilename_data);
    g_free(db->lockfile_library);
    g_free(db->dbfilename_library);
    g_free(db);
    return NULL;
  }

  // attach a memory database to db connection for use with temporary tables
  // used during instance life time, which is discarded on exit.
  sqlite3_exec(db->handle, "attach database ':memory:' as memory", NULL, NULL, NULL);
  // attach the data database which contains presets, styles, tags and similar things not tied to single images
  sqlite3_stmt *stmt;
  gboolean have_data_db = load_data && g_file_test(dbfilename_data, G_FILE_TEST_EXISTS);
  int rc = sqlite3_prepare_v2(db->handle, "ATTACH DATABASE ?1 AS data", -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, dbfilename_data, -1, SQLITE_TRANSIENT);

  if(rc != SQLITE_OK || sqlite3_step(stmt) != SQLITE_DONE)
  {
    sqlite3_finalize(stmt);
    fprintf(stderr, "[init] database `%s' couldn't be opened. aborting\n", dbfilename_data);
    dt_database_destroy(db);
    db = NULL;
    goto error;
  }
  sqlite3_finalize(stmt);
  // some sqlite3 config
  sqlite3_exec(db->handle, "PRAGMA synchronous = OFF", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "PRAGMA journal_mode = MEMORY", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "PRAGMA page_size = 32768", NULL, NULL, NULL);
  // now that we got functional databases that are locked for us we can make sure that the schema is set up
  // first we update the data database to the latest version so that we can potentially move data from the library
  // over when updating that one
  if(!have_data_db)
    _create_data_schema(db); // a brand new db it seems
  else
  {
    rc = sqlite3_prepare_v2(db->handle, "select value from data.db_info where key = 'version'", -1, &stmt, NULL);

    if(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
    {
      // compare the version of the db with what is current for this executable
      const int db_version = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);

      if(db_version < CURRENT_DATABASE_VERSION_DATA)
      {
        ask_for_upgrade(dbfilename_data, has_gui);
        // older: upgrade
        if(!_upgrade_data_schema(db, db_version))
        {
          // we couldn't upgrade the db for some reason. bail out.
          fprintf(stderr, "[init] database `%s' couldn't be upgraded from version %d to %d. aborting\n",
                  dbfilename_data, db_version, CURRENT_DATABASE_VERSION_DATA);
          dt_database_destroy(db);
          db = NULL;
          goto error;
        }
        // upgrade was successfull, time for some housekeeping
        sqlite3_exec(db->handle, "VACUUM data", NULL, NULL, NULL);
        sqlite3_exec(db->handle, "ANALYZE data", NULL, NULL, NULL);

      }
      else if(db_version > CURRENT_DATABASE_VERSION_DATA)
      {
        // newer: bail out
        fprintf(stderr, "[init] database version of `%s' is too new for this build of darktable. aborting\n",
                dbfilename_data);
        dt_database_destroy(db);
        db = NULL;
        goto error;
      }
      // else: the current version, do nothing
    }
    else
    {
      // oh, bad situation. the database is corrupt and can't be read!
      // we inform the user here and let him decide what to do: exit or delete and try again.
      char *label_text = g_markup_printf_escaped(_("an error has occurred while trying to open the database from\n"
                                                   "\n"
                                                   "<span style=\"italic\">%s</span>\n"
                                                   "\n"
                                                   "it seems that the database is corrupt.\n"
                                                   "do you want to close darktable now to manually restore\n"
                                                   "the database from a backup or start with a new one?"),
                                                 dbfilename_data);

      gboolean shall_we_delete_the_db =
          dt_gui_show_standalone_yes_no_dialog(_("darktable - error opening database"), label_text,
                                               _("close darktable"), _("delete database"));

      g_free(label_text);
      dt_database_destroy(db);
      db = NULL;

      if(shall_we_delete_the_db)
      {
        fprintf(stderr, "[init] deleting `%s' on user request", dbfilename_data);

        if(g_unlink(dbfilename_data) == 0)
          fprintf(stderr, " ... ok\n");
        else
          fprintf(stderr, " ... failed\n");

        goto start;
      }
      else
      {
        fprintf(stderr, "[init] database `%s' is corrupt and can't be opened! either replace it from a backup or "
        "delete the file so that darktable can create a new one the next time. aborting\n", dbfilename_data);
        goto error;
      }
    }
  }
  // next we are looking at the library database
  // does the db contain the new 'db_info' table?
  rc = sqlite3_prepare_v2(db->handle, "select value from main.db_info where key = 'version'", -1, &stmt, NULL);

  if(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
  {
    // compare the version of the db with what is current for this executable
    const int db_version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if(db_version < CURRENT_DATABASE_VERSION_LIBRARY)
    {
      ask_for_upgrade(dbfilename_library, has_gui);
      // older: upgrade
      if(!_upgrade_library_schema(db, db_version))
      {
        // we couldn't upgrade the db for some reason. bail out.
        fprintf(stderr, "[init] database `%s' couldn't be upgraded from version %d to %d. aborting\n", dbname,
                db_version, CURRENT_DATABASE_VERSION_LIBRARY);
        dt_database_destroy(db);
        db = NULL;
        goto error;
      }
      // upgrade was successfull, time for some housekeeping
      sqlite3_exec(db->handle, "VACUUM main", NULL, NULL, NULL);
      sqlite3_exec(db->handle, "ANALYZE main", NULL, NULL, NULL);
    }
    else if(db_version > CURRENT_DATABASE_VERSION_LIBRARY)
    {
      // newer: bail out. it's better than what we did before: delete everything
      fprintf(stderr, "[init] database version of `%s' is too new for this build of darktable. aborting\n",
              dbname);
      dt_database_destroy(db);
      db = NULL;
      goto error;
    }
    // else: the current version, do nothing
  }
  else if(rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB)
  {
    // oh, bad situation. the database is corrupt and can't be read!
    // we inform the user here and let him decide what to do: exit or delete and try again.
    char *label_text = g_markup_printf_escaped(_("an error has occurred while trying to open the database from\n"
                                                  "\n"
                                                  "<span style=\"italic\">%s</span>\n"
                                                  "\n"
                                                  "it seems that the database is corrupt.\n"
                                                  "do you want to close darktable now to manually restore\n"
                                                  "the database from a backup or start with a new one?"),
                                               dbfilename_library);
    gboolean shall_we_delete_the_db =
        dt_gui_show_standalone_yes_no_dialog(_("darktable - error opening database"), label_text,
                                              _("close darktable"), _("delete database"));
    g_free(label_text);
    dt_database_destroy(db);
    db = NULL;

    if(shall_we_delete_the_db)
    {
      fprintf(stderr, "[init] deleting `%s' on user request", dbfilename_library);

      if(g_unlink(dbfilename_library) == 0)
        fprintf(stderr, " ... ok\n");
      else
        fprintf(stderr, " ... failed\n");

      goto start;
    }
    else
    {
      fprintf(stderr, "[init] database `%s' is corrupt and can't be opened! either replace it from a backup or "
                      "delete the file so that darktable can create a new one the next time. aborting\n", dbname);
      goto error;
    }
  }
  else
  {
    // does it contain the legacy 'settings' table?
    sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(db->handle, "select settings from main.settings", -1, &stmt, NULL);

    if(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
    {
      // the old blob had the version as an int in the first place
      const void *set = sqlite3_column_blob(stmt, 0);
      const int db_version = *(int *)set;
      sqlite3_finalize(stmt);

      if(!_migrate_schema(db, db_version)) // bring the legacy layout to the first one known to our upgrade
                                           // path ...
      {
        // we couldn't migrate the db for some reason. bail out.
        fprintf(stderr, "[init] database `%s' couldn't be migrated from the legacy version %d. aborting\n",
                dbname, db_version);
        dt_database_destroy(db);
        db = NULL;
        goto error;
      }

      if(!_upgrade_library_schema(db, 1)) // ... and upgrade it
      {
        // we couldn't upgrade the db for some reason. bail out.
        fprintf(stderr, "[init] database `%s' couldn't be upgraded from version 1 to %d. aborting\n", dbname,
                CURRENT_DATABASE_VERSION_LIBRARY);
        dt_database_destroy(db);
        db = NULL;
        goto error;
      }
    }
    else
    {
      sqlite3_finalize(stmt);
      _create_library_schema(db); // a brand new db it seems
    }
  }
  // create the in-memory tables
  _create_memory_schema(db);
  // drop table settings -- we don't want old versions of dt to drop our tables
  sqlite3_exec(db->handle, "drop table main.settings", NULL, NULL, NULL);
  // take care of potential bad data in the db.
  _sanitize_db(db);

error:
  g_free(dbname);

  return db;
}

void dt_database_destroy(const dt_database_t *db)
{
  sqlite3_close(db->handle);

  if (db->lockfile_data)
  {
    g_unlink(db->lockfile_data);
    g_free(db->lockfile_data);
  }

  if (db->lockfile_library)
  {
    g_unlink(db->lockfile_library);
    g_free(db->lockfile_library);
  }

  g_free(db->dbfilename_data);
  g_free(db->dbfilename_library);
  g_free((dt_database_t *)db);

  sqlite3_shutdown();
}

sqlite3 *dt_database_get(const dt_database_t *db)
{
  return db ? db->handle : NULL;
}

const gchar *dt_database_get_path(const struct dt_database_t *db)
{
  return db->dbfilename_library;
}

static void _database_migrate_to_xdg_structure()
{
  gchar dbfilename[PATH_MAX] = { 0 };
  gchar *conf_db = dt_conf_get_string("database");

  gchar datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));

  if(conf_db && conf_db[0] != '/')
  {
    const char *homedir = getenv("HOME");
    snprintf(dbfilename, sizeof(dbfilename), "%s/%s", homedir, conf_db);
    if(g_file_test(dbfilename, G_FILE_TEST_EXISTS))
    {
      char destdbname[PATH_MAX] = { 0 };
      snprintf(destdbname, sizeof(dbfilename), "%s/%s", datadir, "library.db");
      if(!g_file_test(destdbname, G_FILE_TEST_EXISTS))
      {
        fprintf(stderr, "[init] moving database into new XDG directory structure\n");
        rename(dbfilename, destdbname);
        dt_conf_set_string("database", "library.db");
      }
    }
  }

  g_free(conf_db);
}

gboolean dt_database_get_lock_acquired(const dt_database_t *db)
{
  return db->lock_acquired;
}

int _get_pragma_val(const struct dt_database_t *db, const char* pragma)
{
  gchar* query= g_strdup_printf("PRAGMA %s", pragma);
  int val = -1;
  sqlite3_stmt *stmt;
  const int rc = sqlite3_prepare_v2(db->handle, query,-1, &stmt, NULL);
  __DT_DEBUG_ASSERT_WITH_QUERY__(rc, query);

  if(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
    val = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);
  g_free(query);
  return val;
}

#define ERRCHECK {if (err!=NULL) {dt_print(DT_DEBUG_SQL, "[db maintenance] maintenance error: '%s'\n",err); sqlite3_free(err); err=NULL;}}
void _dt_database_maintenance(const struct dt_database_t *db)
{
  char* err = NULL;
  const int main_pre_free_count = _get_pragma_val(db, "main.freelist_count");
  const int main_page_size = _get_pragma_val(db, "main.page_size");
  const int data_pre_free_count = _get_pragma_val(db, "data.freelist_count");
  const int data_page_size = _get_pragma_val(db, "data.page_size");
  const guint64 calc_pre_size = (main_pre_free_count*main_page_size) + (data_pre_free_count*data_page_size);

  if(calc_pre_size == 0)
  {
    dt_print(DT_DEBUG_SQL, "[db maintenance] maintenance deemed unnecesary, performing only analyze.\n");
    DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE data", NULL, NULL, &err);
    ERRCHECK
    DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE main", NULL, NULL, &err);
    ERRCHECK
    DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE", NULL, NULL, &err);
    ERRCHECK
    return;
  }

  DT_DEBUG_SQLITE3_EXEC(db->handle, "VACUUM data", NULL, NULL, &err);
  ERRCHECK
  DT_DEBUG_SQLITE3_EXEC(db->handle, "VACUUM main", NULL, NULL, &err);
  ERRCHECK
  DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE data", NULL, NULL, &err);
  ERRCHECK
  DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE main", NULL, NULL, &err);
  ERRCHECK

  // for some reason this is needed in some cases
  // in case above performed vacuum+analyze properly, this is noop.
  DT_DEBUG_SQLITE3_EXEC(db->handle, "VACUUM", NULL, NULL, &err);
  ERRCHECK
  DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE", NULL, NULL, &err);
  ERRCHECK

  const int main_post_free_count = _get_pragma_val(db, "main.freelist_count");
  const int data_post_free_count = _get_pragma_val(db, "data.freelist_count");
  const guint64 calc_post_size = (main_post_free_count*main_page_size) + (data_post_free_count*data_page_size);
  const gint64 bytes_freed = calc_pre_size - calc_post_size;
  dt_print(DT_DEBUG_SQL, "[db maintenance] maintenance done, %" G_GINT64_FORMAT " bytes freed.\n", bytes_freed);

  if(calc_post_size >= calc_pre_size)
    dt_print(DT_DEBUG_SQL, "[db maintenance] paintenance problem. if no errors logged, it should work fine next time.\n");
}
#undef ERRCHECK

gboolean _ask_for_maintenance(const gboolean has_gui, const gboolean closing_time, const guint64 size)
{
  if(!has_gui)
    return FALSE;

  char *later_info = NULL;
  char *size_info = g_format_size(size);
  char *config = dt_conf_get_string("database/maintenance_check");

  if((closing_time && (!g_strcmp0(config, "on both"))) || !g_strcmp0(config, "on startup"))
    later_info = _("click later to be asked on next startup");
  else if (!closing_time && (!g_strcmp0(config, "on both")))
    later_info = _("click later to be asked when closing darktable");
  else if (!g_strcmp0(config, "on close"))
    later_info = _("click later to be asked next time when closing darktable");

  char *label_text = g_markup_printf_escaped(_("the database could use some maintenance\n"
                                                 "\n"
                                                 "there's <span style=\"italic\">%s</span> to be freed"
                                                 "\n\n"
                                                 "do you want to proceed now?\n\n"
                                                 "%s\n"
                                                 "you can always change maintenance preferences in core options"),
                                                 size_info, later_info);

    const gboolean shall_perform_maintenance =
      dt_gui_show_standalone_yes_no_dialog(_("darktable - schema maintenance"), label_text,
                                           _("later"), _("yes"));
    g_free(label_text);
    g_free(size_info);
    return shall_perform_maintenance;
}

void dt_database_maybe_maintenance(const struct dt_database_t *db, const gboolean has_gui, const gboolean closing_time)
{
  char *config = dt_conf_get_string("database/maintenance_check");

  if(!g_strcmp0(config, "never"))
  {
    // early bail out on "never"
    dt_print(DT_DEBUG_SQL, "[db maintenance] please consider enabling database maintenance.\n");
    return;
  }

  gboolean check_for_maintenance = FALSE;
  const gboolean force_maintenance = g_str_has_suffix (config, "(don't ask)");

  if(config)
  {
    if((strstr(config, "on both")) // should cover "(don't ask) suffix
        || (closing_time && (strstr(config, "on close")))
        || (!closing_time && (strstr(config, "on startup"))))
    {
      // we have "on both/on close/on startup" setting, so - checking!
      dt_print(DT_DEBUG_SQL, "[db maintenance] checking for maintenance, due to rule: '%s'.\n", config);
      check_for_maintenance = TRUE;
    }
    // if the config was "never", check_for_vacuum is false.
    g_free(config);
  }

  if(!check_for_maintenance)
    return;

  // checking free pages
  const int main_free_count = _get_pragma_val(db, "main.freelist_count");
  const int main_page_count = _get_pragma_val(db, "main.page_count");
  const int main_page_size = _get_pragma_val(db, "main.page_size");

  const int data_free_count = _get_pragma_val(db, "data.freelist_count");
  const int data_page_count = _get_pragma_val(db, "data.page_count");
  const int data_page_size = _get_pragma_val(db, "data.page_size");

  dt_print(DT_DEBUG_SQL,
      "[db maintenance] main: [%d/%d pages], data: [%d/%d pages].\n",
      main_free_count, main_page_count, data_free_count, data_page_count);

  if(main_page_count <= 0 || data_page_count <= 0)
  {
    //something's wrong with PRAGMA page_size returns. early bail.
    dt_print(DT_DEBUG_SQL,
        "[db maintenance] page_count <= 0 : main.page_count: %d, data.page_count: %d \n",
        main_page_count, data_page_count);
    return;
  }

  // we don't need fine-grained percentages, so let's do ints
  const int main_free_percentage = (main_free_count * 100 ) / main_page_count;
  const int data_free_percentage = (data_free_count * 100 ) / data_page_count;
  const int freepage_ratio = dt_conf_get_int("database/maintenance_freepage_ratio");

  if((main_free_percentage >= freepage_ratio)
      || (data_free_percentage >= freepage_ratio))
  {
    const guint64 calc_size = (main_free_count*main_page_size) + (data_free_count*data_page_size);
    dt_print(DT_DEBUG_SQL, "[db maintenance] maintenance suggested, %" G_GUINT64_FORMAT " bytes to free.\n", calc_size);

    if(force_maintenance || _ask_for_maintenance(has_gui, closing_time, calc_size))
      _dt_database_maintenance(db);
  }
}

void dt_database_optimize(const struct dt_database_t *db)
{
  // optimize should in most cases be no-op and have no noticeable downsides
  // this should run on every exit
  // see: https://www.sqlite.org/pragma.html#pragma_optimize
  DT_DEBUG_SQLITE3_EXEC(db->handle, "PRAGMA optimize", NULL, NULL, NULL);
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
