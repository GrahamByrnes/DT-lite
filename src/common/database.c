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
#include "gui/legacy_presets.h"

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

  /* data database filename */
  gchar *dbfilename_data, *lockfile_data;

  /* library database filename */
  gchar *dbfilename_library, *lockfile_library;

  /* ondisk DB */
  sqlite3 *handle;

  gchar *error_message, *error_dbfilename;
  int error_other_pid;
} dt_database_t;


/* migrates database from old place to new */
static void _database_migrate_to_xdg_structure();

/* delete old mipmaps files */
static void _database_delete_mipmaps_files();

#define _SQLITE3_EXEC(a, b, c, d, e)                                                                         \
  if(sqlite3_exec(a, b, c, d, e) != SQLITE_OK)                                                               \
  {                                                                                                          \
    all_ok = FALSE;                                                                                          \
    failing_query = b;                                                                                       \
    goto end;                                                                                                \
  }

/* migrate from the legacy db format (with the 'settings' blob) to the first version this system knows */
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
  ////////////////////////////// db_info
  _SQLITE3_EXEC(db->handle, "CREATE TABLE main.db_info (key VARCHAR PRIMARY KEY, value VARCHAR)",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT OR REPLACE INTO main.db_info (key, value) VALUES ('version', 1)",
                NULL, NULL, NULL);
  ////////////////////////////// film_rolls
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS main.film_rolls_folder_index ON film_rolls (folder)",
                NULL, NULL, NULL);
  ////////////////////////////// images
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
      "i.film_id = main.images.film_id AND i.id < main.images.id) WHERE version IS NULL",
      NULL, NULL, NULL);
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
  ////////////////////////////// selected_images
  // selected_images should have a primary key. add it if it's missing:
  _SQLITE3_EXEC(db->handle, "CREATE TEMPORARY TABLE dt_migration_table (imgid INTEGER)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT INTO dt_migration_table SELECT imgid FROM main.selected_images", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE main.selected_images", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE TABLE main.selected_images (imgid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT OR IGNORE INTO main.selected_images SELECT imgid FROM dt_migration_table",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE dt_migration_table", NULL, NULL, NULL);
  ////////////////////////////// history
  sqlite3_exec(db->handle, "ALTER TABLE main.history ADD COLUMN blendop_params BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.history ADD COLUMN blendop_version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.history ADD COLUMN multi_priority INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.history ADD COLUMN multi_name VARCHAR(256)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS main.history_imgid_index ON history (imgid)", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.history SET blendop_version = 1 WHERE blendop_version IS NULL", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.history SET multi_priority = 0 WHERE multi_priority IS NULL", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.history SET multi_name = ' ' WHERE multi_name IS NULL", NULL, NULL, NULL);
  ////////////////////////////// mask
  _SQLITE3_EXEC(db->handle, "CREATE TABLE IF NOT EXISTS main.mask (imgid INTEGER, formid INTEGER, form INTEGER, "
                            "name VARCHAR(256), version INTEGER, "
                            "points BLOB, points_count INTEGER, source BLOB)",
                NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.mask ADD COLUMN source BLOB", NULL, NULL,
               NULL); // in case the table was there already but missed that column
  ////////////////////////////// tagged_images
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS main.tagged_images_tagid_index ON tagged_images (tagid)",
                NULL, NULL, NULL);
  ////////////////////////////// styles
  _SQLITE3_EXEC(db->handle,
                "CREATE TABLE IF NOT EXISTS main.styles (id INTEGER, name VARCHAR, description VARCHAR)", NULL,
                NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.styles ADD COLUMN id INTEGER", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.styles SET id = rowid WHERE id IS NULL", NULL, NULL, NULL);
  ////////////////////////////// style_items
  _SQLITE3_EXEC(db->handle, "CREATE TABLE IF NOT EXISTS main.style_items (styleid INTEGER, num INTEGER, module "
                            "INTEGER, operation VARCHAR(256), op_params BLOB, "
                            "enabled INTEGER, blendop_params BLOB, blendop_version INTEGER, multi_priority "
                            "INTEGER, multi_name VARCHAR(256))",
                NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.style_items ADD COLUMN blendop_params BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.style_items ADD COLUMN blendop_version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.style_items ADD COLUMN multi_priority INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.style_items ADD COLUMN multi_name VARCHAR(256)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.style_items SET blendop_version = 1 WHERE blendop_version IS NULL", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.style_items SET multi_priority = 0 WHERE multi_priority IS NULL", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.style_items SET multi_name = ' ' WHERE multi_name IS NULL", NULL, NULL,
                NULL);
  ////////////////////////////// color_labels
  // color_labels could have a PRIMARY KEY that we don't want
  _SQLITE3_EXEC(db->handle, "CREATE TEMPORARY TABLE dt_migration_table (imgid INTEGER, color INTEGER)", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT INTO dt_migration_table SELECT imgid, color FROM main.color_labels", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE main.color_labels", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE TABLE main.color_labels (imgid INTEGER, color INTEGER)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE UNIQUE INDEX main.color_labels_idx ON color_labels (imgid, color)", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "INSERT OR IGNORE INTO main.color_labels SELECT imgid, color FROM dt_migration_table",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE dt_migration_table", NULL, NULL, NULL);
  ////////////////////////////// meta_data
  _SQLITE3_EXEC(db->handle, "CREATE TABLE IF NOT EXISTS main.meta_data (id INTEGER, key INTEGER, value VARCHAR)",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS main.metadata_index ON meta_data (id, key)", NULL, NULL,
                NULL);
  ////////////////////////////// presets
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
                                   "operation = ?3 AND op_version = ?4",
                       -1, &innerstmt, NULL);
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

/* do the real migration steps, returns the version the db was converted to */
static int _upgrade_library_schema_step(dt_database_t *db, int version)
{
  sqlite3_stmt *stmt;
  int new_version = version;
  if(version == CURRENT_DATABASE_VERSION_LIBRARY)
    return version;
  else if(version == 0)
  {
    // this can't happen, we started with 1, but it's a good example how this function works
    // <do some magic to the db>
    new_version = 1; // the version we transformed the db to. this way it might be possible to roll back or
                     // add fast paths
  }
  else if(version == 1)
  {
    // 1 -> 2 added write_timestamp
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN write_timestamp INTEGER",
             "[init] can't add `write_timestamp' column to database\n");
    TRY_EXEC("UPDATE main.images SET write_timestamp = STRFTIME('%s', 'now') WHERE write_timestamp IS NULL",
             "[init] can't initialize `write_timestamp' with current point in time\n");
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 2;
  }
  else if(version == 2)
  {
    // 2 -> 3 reset raw_black and raw_maximum. in theory we should change the columns from REAL to INTEGER,
    // but sqlite doesn't care about types so whatever
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    TRY_EXEC("UPDATE main.images SET raw_black = 0, raw_maximum = 16384",
             "[init] can't reset raw_black and raw_maximum\n");
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 3;
  }
  else if(version == 3)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("CREATE TRIGGER insert_tag AFTER INSERT ON main.tags"
             " BEGIN"
             "   INSERT INTO tagxtag SELECT id, new.id, 0 FROM TAGS;"
             "   UPDATE tagxtag SET count = 1000000 WHERE id1=new.id AND id2=new.id;"
             " END",
             "[init] can't create insert_tag trigger\n");
    TRY_EXEC("CREATE TRIGGER delete_tag BEFORE DELETE ON main.tags"
             " BEGIN"
             "   DELETE FROM tagxtag WHERE id1=old.id OR id2=old.id;"
             "   DELETE FROM tagged_images WHERE tagid=old.id;"
             " END",
             "[init] can't create delete_tag trigger\n");
    TRY_EXEC("CREATE TRIGGER attach_tag AFTER INSERT ON main.tagged_images"
             " BEGIN"
             "   UPDATE tagxtag"
             "     SET count = count + 1"
             "     WHERE (id1=new.tagid AND id2 IN (SELECT tagid FROM tagged_images WHERE imgid=new.imgid))"
             "        OR (id2=new.tagid AND id1 IN (SELECT tagid FROM tagged_images WHERE imgid=new.imgid));"
             " END",
             "[init] can't create attach_tag trigger\n");
    TRY_EXEC("CREATE TRIGGER detach_tag BEFORE DELETE ON main.tagged_images"
             " BEGIN"
             "   UPDATE tagxtag"
             "     SET count = count - 1"
             "     WHERE (id1=old.tagid AND id2 IN (SELECT tagid FROM tagged_images WHERE imgid=old.imgid))"
             "        OR (id2=old.tagid AND id1 IN (SELECT tagid FROM tagged_images WHERE imgid=old.imgid));"
             " END",
             "[init] can't create detach_tag trigger\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 4;
  }
  else if(version == 4)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("ALTER TABLE main.presets RENAME TO tmp_presets",  "[init] can't rename table presets\n");

    TRY_EXEC("CREATE TABLE main.presets (name VARCHAR, description VARCHAR, operation VARCHAR, op_params BLOB,"
             "enabled INTEGER, blendop_params BLOB, model VARCHAR, maker VARCHAR, lens VARCHAR,"
             "iso_min REAL, iso_max REAL, exposure_min REAL, exposure_max REAL, aperture_min REAL,"
             "aperture_max REAL, focal_length_min REAL, focal_length_max REAL, writeprotect INTEGER,"
             "autoapply INTEGER, filter INTEGER, def INTEGER, format INTEGER, op_version INTEGER,"
             "blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
             "[init] can't create new presets table\n");

    TRY_EXEC("INSERT INTO main.presets (name, description, operation, op_params, enabled, blendop_params, model, "
             "maker, lens, iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max,"
             "focal_length_min, focal_length_max, writeprotect, autoapply, filter, def, format, op_version, "
             "blendop_version, multi_priority, multi_name) SELECT name, description, operation, op_params, "
             "enabled, blendop_params, model, maker, lens, iso_min, iso_max, exposure_min, exposure_max, "
             "aperture_min, aperture_max, focal_length_min, focal_length_max, writeprotect, autoapply, filter, "
             "def, isldr, op_version, blendop_version, multi_priority, multi_name FROM tmp_presets",
             "[init] can't populate presets table from tmp_presets\n");

    TRY_EXEC("DROP TABLE tmp_presets", "[init] can't delete table tmp_presets\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 5;
  }
  else if(version == 5)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("CREATE INDEX main.images_filename_index ON images (filename)",
             "[init] can't create index on image filename\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 6;
  }
  else if(version == 6)
  {
    // some ancient tables can have the styleid column of style_items be called style_id. fix that.
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    if(sqlite3_exec(db->handle, "SELECT style_id FROM main.style_items", NULL, NULL, NULL) == SQLITE_OK)
    {
      TRY_EXEC("ALTER TABLE main.style_items RENAME TO tmp_style_items",
               "[init] can't rename table style_items\n");

      TRY_EXEC("CREATE TABLE main.style_items (styleid INTEGER, num INTEGER, module INTEGER, "
               "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
               "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
               "[init] can't create new style_items table\n");

      TRY_EXEC("INSERT INTO main.style_items (styleid, num, module, operation, op_params, enabled,"
               "                         blendop_params, blendop_version, multi_priority, multi_name)"
               "                  SELECT style_id, num, module, operation, op_params, enabled,"
               "                         blendop_params, blendop_version, multi_priority, multi_name"
               "                  FROM   tmp_style_items",
               "[init] can't populate style_items table from tmp_style_items\n");

      TRY_EXEC("DROP TABLE tmp_style_items", "[init] can't delete table tmp_style_items\n");
    }

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 7;
  }
  else if(version == 7)
  {
    // make sure that we have no film rolls with a NULL folder
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("ALTER TABLE main.film_rolls RENAME TO tmp_film_rolls", "[init] can't rename table film_rolls\n");

    TRY_EXEC("CREATE TABLE main.film_rolls "
             "(id INTEGER PRIMARY KEY, datetime_accessed CHAR(20), "
             "folder VARCHAR(1024) NOT NULL)",
             "[init] can't create new film_rolls table\n");

    TRY_EXEC("INSERT INTO main.film_rolls (id, datetime_accessed, folder) "
             "SELECT id, datetime_accessed, folder "
             "FROM   tmp_film_rolls "
             "WHERE  folder IS NOT NULL",
             "[init] can't populate film_rolls table from tmp_film_rolls\n");

    TRY_EXEC("DROP TABLE tmp_film_rolls", "[init] can't delete table tmp_film_rolls\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 8;
  }
  else if(version == 8)
  {
    // 8 -> 9 added history_end column to images
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("ALTER TABLE main.images ADD COLUMN history_end INTEGER",
             "[init] can't add `history_end' column to database\n");

    TRY_EXEC("UPDATE main.images SET history_end = (SELECT IFNULL(MAX(num) + 1, 0) FROM main.history "
             "WHERE imgid = id)", "[init] can't initialize `history_end' with last history entry\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 9;
  }
  else if(version == 9)
  {
    // 9 -> 10 cleanup of last update :(
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("UPDATE main.images SET history_end = (SELECT IFNULL(MAX(num) + 1, 0) FROM main.history "
             "WHERE imgid = id)", "[init] can't set `history_end' to 0 where it was NULL\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 10;
  }
  else if(version == 10)
  {
    // 10 -> 11 added altitude column to images
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("ALTER TABLE main.images ADD COLUMN altitude REAL",
             "[init] can't add `altitude' column to database\n");

    TRY_EXEC("UPDATE main.images SET altitude = NULL", "[init] can't initialize `altitude' with NULL\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 11;
  }
  else if(version == 11)
  {
    // 11 -> 12 tagxtag was removed in order to reduce database size
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("DROP TRIGGER main.detach_tag", "[init] can't drop trigger `detach_tag' from database\n");

    TRY_EXEC("DROP TRIGGER main.attach_tag", "[init] can't drop trigger `attach_tag' from database\n");

    TRY_EXEC("DROP TRIGGER main.delete_tag", "[init] can't drop trigger `delete_tag' from database\n");

    TRY_EXEC("DROP TRIGGER main.insert_tag", "[init] can't drop trigger `insert_tag' from database\n");

    TRY_EXEC("DROP TABLE main.tagxtag", "[init] can't drop table `tagxtag' from database\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 12;
  }
  else if(version == 12)
  {
    // 11 -> 12 move presets, styles and tags over to the data database
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    ////////////// presets
#undef FINALIZE
#define FINALIZE                                                                   \
    do                                                                             \
    {                                                                              \
      sqlite3_finalize(stmt);                                                      \
      sqlite3_finalize(select_stmt);                                               \
      sqlite3_finalize(count_clashes_stmt);                                        \
      sqlite3_finalize(update_name_stmt);                                          \
      sqlite3_finalize(insert_stmt);                                               \
      sqlite3_finalize(delete_stmt);                                               \
    } while(0)

    stmt = NULL;
    sqlite3_stmt *insert_stmt = NULL, *delete_stmt = NULL, *select_stmt = NULL, *count_clashes_stmt = NULL,
    *update_name_stmt = NULL;
    // remove presets that are already in data.
    // we can't use a NATURAL JOIN here as that fails when columns have NULL values. :-(
    TRY_EXEC("DELETE FROM main.presets WHERE rowid IN (SELECT p1.rowid FROM main.presets p1 "
             "JOIN data.presets p2 ON "
                 "p1.name IS p2.name AND "
                 "p1.description IS p2.description AND "
                 "p1.operation IS p2.operation AND "
                 "p1.op_version IS p2.op_version AND "
                 "p1.op_params IS p2.op_params AND "
                 "p1.enabled IS p2.enabled AND "
                 "p1.blendop_params IS p2.blendop_params AND "
                 "p1.blendop_version IS p2.blendop_version AND "
                 "p1.multi_priority IS p2.multi_priority AND "
                 "p1.multi_name IS p2.multi_name AND "
                 "p1.model IS p2.model AND "
                 "p1.maker IS p2.maker AND "
                 "p1.lens IS p2.lens AND "
                 "p1.iso_min IS p2.iso_min AND "
                 "p1.iso_max IS p2.iso_max AND "
                 "p1.exposure_min IS p2.exposure_min AND "
                 "p1.exposure_max IS p2.exposure_max AND "
                 "p1.aperture_min IS p2.aperture_min AND "
                 "p1.aperture_max IS p2.aperture_max AND "
                 "p1.focal_length_min IS p2.focal_length_min AND "
                 "p1.focal_length_max IS p2.focal_length_max AND "
                 "p1.writeprotect IS p2.writeprotect AND "
                 "p1.autoapply IS p2.autoapply AND "
                 "p1.filter IS p2.filter AND "
                 "p1.def IS p2.def AND "
                 "p1.format IS p2.format "
             "WHERE p1.writeprotect = 0)",
             "[init] can't delete already migrated presets from database\n");

    // find all presets that are clashing with something else in presets. that can happen as we introduced an
    // index on presets in data which wasn't in place in library.
    TRY_PREPARE(select_stmt, "SELECT p.rowid, r FROM main.presets AS p, (SELECT rowid AS r, name, operation, "
                             "op_version FROM main.presets GROUP BY name, operation, op_version HAVING "
                             "COUNT(*) > 1) USING (name, operation, op_version) WHERE p.rowid != r",
                "[init] can't prepare selecting presets with same name, operation, op_version from database\n");

    // see if an updated preset name still causes problems
    TRY_PREPARE(count_clashes_stmt, "SELECT COUNT(*) FROM main.presets AS p, (SELECT name, operation, op_version "
                                    "FROM main.presets WHERE rowid = ?1) AS i ON p.name = i.name || \" #\" || ?2 "
                                    "AND p.operation = i.operation AND p.op_version = i.op_version",
                "[init] can't prepare selection of preset count by name from database\n");

    // update the preset name for good
    TRY_PREPARE(update_name_stmt, "UPDATE main.presets SET name = name || \" #\" || ?1 WHERE rowid = ?2",
                "[init] can't prepare updating of preset name in database\n");

    // find all presets that would be clashing with something in data
    TRY_PREPARE(stmt, "SELECT p1.rowid FROM main.presets p1 INNER JOIN data.presets p2 "
                      "USING (name, operation, op_version) WHERE p1.writeprotect = 0",
                "[init] can't access table `presets' in database\n");

    // ... and move them over with a new name
    TRY_PREPARE(insert_stmt, "INSERT OR FAIL INTO data.presets (name, description, operation, op_version, "
                             "op_params, enabled, blendop_params, blendop_version, multi_priority, multi_name, "
                             "model, maker, lens, iso_min, iso_max, exposure_min, exposure_max, aperture_min, "
                             "aperture_max, focal_length_min, focal_length_max, writeprotect, autoapply, filter, "
                             "def, format) "
                             "SELECT name || \" #\" || ?1, description, operation, op_version, op_params, "
                             "enabled, blendop_params, blendop_version, multi_priority, multi_name, model, maker, "
                             "lens, iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, "
                             "focal_length_min, focal_length_max, writeprotect, autoapply, filter, def, format "
                             "FROM main.presets p1 WHERE p1.rowid = ?2",
                "[init] can't prepare insertion statement\n");

    TRY_PREPARE(delete_stmt, "DELETE FROM main.presets WHERE rowid = ?1", "[init] can't prepare deletion statement\n");

    // first rename presets with (name, operation, op_version) not being unique
    while(sqlite3_step(select_stmt) == SQLITE_ROW)
    {
      int own_rowid = sqlite3_column_int(select_stmt, 0);
      int other_rowid = sqlite3_column_int(select_stmt, 1);
      int preset_version = 0;

      do
      {
        preset_version++;
        sqlite3_reset(count_clashes_stmt);
        sqlite3_clear_bindings(count_clashes_stmt);
        sqlite3_bind_int(count_clashes_stmt, 1, other_rowid);
        sqlite3_bind_int(count_clashes_stmt, 2, preset_version);
      }
      while(sqlite3_step(count_clashes_stmt) == SQLITE_ROW && sqlite3_column_int(count_clashes_stmt, 0) > 0);

      sqlite3_bind_int(update_name_stmt, 1, preset_version);
      sqlite3_bind_int(update_name_stmt, 2, own_rowid);
      TRY_STEP(update_name_stmt, SQLITE_DONE, "[init] can't rename preset in database\n");
      sqlite3_reset(update_name_stmt);
      sqlite3_reset(update_name_stmt);
    }

    // now rename to avoid clashes with data.presets
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int preset_version = 0;
      int rowid = sqlite3_column_int(stmt, 0);

      do
      {
        preset_version++;
        sqlite3_reset(insert_stmt);
        sqlite3_clear_bindings(insert_stmt);
        sqlite3_bind_int(insert_stmt, 1, preset_version);
        sqlite3_bind_int(insert_stmt, 2, rowid);
      } while(sqlite3_step(insert_stmt) != SQLITE_DONE);

      sqlite3_reset(delete_stmt);
      sqlite3_clear_bindings(delete_stmt);
      sqlite3_bind_int(delete_stmt, 1, rowid);
      TRY_STEP(delete_stmt, SQLITE_DONE, "[init] can't delete preset from database\n");
    }

    // all that is left in presets should be those that can be moved over without any further concerns
    TRY_EXEC("INSERT OR FAIL INTO data.presets SELECT name, description, operation, "
             "op_version, op_params, enabled, blendop_params, blendop_version, "
             "multi_priority, multi_name, model, maker, lens, iso_min, iso_max, "
             "exposure_min, exposure_max, aperture_min, aperture_max, "
             "focal_length_min, focal_length_max, writeprotect, autoapply, filter, "
             "def, format FROM main.presets WHERE writeprotect = 0",
             "[init] can't copy presets to the data database\n");
    // ... delete them on the old side
    TRY_EXEC("DELETE FROM main.presets WHERE writeprotect = 0",
             "[init] can't copy presets to the data database\n");

    FINALIZE;
#undef FINALIZE

    ////////////// styles
#define FINALIZE                                                                   \
    do                                                                             \
    {                                                                              \
      sqlite3_finalize(stmt);                                                      \
      sqlite3_finalize(insert_stmt);                                               \
      sqlite3_finalize(select_stmt);                                               \
      sqlite3_finalize(delete_stmt);                                               \
      sqlite3_finalize(update_name_stmt);                                          \
      sqlite3_finalize(select_new_stmt);                                           \
      sqlite3_finalize(copy_style_items_stmt);                                     \
      sqlite3_finalize(delete_style_items_stmt);                                   \
    } while(0)

    stmt = NULL;
    select_stmt = NULL;
    update_name_stmt = NULL;
    insert_stmt = NULL;
    delete_stmt = NULL;
    sqlite3_stmt *select_new_stmt = NULL, *copy_style_items_stmt = NULL, *delete_style_items_stmt = NULL;

    TRY_PREPARE(stmt, "SELECT id, name FROM main.styles", "[init] can't prepare style selection from database\n");
    TRY_PREPARE(select_stmt, "SELECT rowid FROM data.styles WHERE name = ?1 LIMIT 1",
                "[init] can't prepare style item selection from database\n");
    TRY_PREPARE(update_name_stmt, "UPDATE main.styles SET name = ?1 WHERE id = ?2",
                "[init] can't prepare style name update\n");
    TRY_PREPARE(insert_stmt, "INSERT INTO data.styles (id, name, description) "
                             "SELECT (SELECT COALESCE(MAX(id),0)+1 FROM data.styles), name, description "
                             "FROM main.styles where id = ?1",
                "[init] can't prepare style insertion for database\n");
    TRY_PREPARE(delete_stmt, "DELETE FROM main.styles WHERE id = ?1",
                "[init] can't prepare style deletion for database\n");
    TRY_PREPARE(select_new_stmt, "SELECT id FROM data.styles WHERE rowid = ?1",
                "[init] can't prepare style selection from data database\n");
    TRY_PREPARE(copy_style_items_stmt, "INSERT INTO data.style_items "
                                       "(styleid, num, module, operation, op_params, enabled, blendop_params, "
                                       "blendop_version, multi_priority, multi_name) "
                                       "SELECT ?1, num, module, operation, op_params, enabled, blendop_params, "
                                       "blendop_version, multi_priority, multi_name FROM main.style_items "
                                       "WHERE styleid = ?2",
                "[init] can't prepare style item copy into data database\n");
    TRY_PREPARE(delete_style_items_stmt, "DELETE FROM main.style_items WHERE styleid = ?1",
                "[init] can't prepare style item deletion for database\n");

    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int id = sqlite3_column_int(stmt, 0);
      const char *name = (const char *)sqlite3_column_text(stmt, 1);

      // find a unique name of the style for data.styles
      sqlite3_bind_text(select_stmt, 1, name, -1, SQLITE_TRANSIENT);
      if(sqlite3_step(select_stmt) == SQLITE_ROW)
      {
        // we need to append a version
        int style_version = 0;
        char *new_name = NULL;
        do
        {
          style_version++;
          g_free(new_name);
          new_name = g_strdup_printf("%s #%d", name, style_version);
          sqlite3_reset(select_stmt);
          sqlite3_clear_bindings(select_stmt);
          sqlite3_bind_text(select_stmt, 1, new_name, -1, SQLITE_TRANSIENT);
        } while(sqlite3_step(select_stmt) == SQLITE_ROW);

        // update the name in the old place
        sqlite3_bind_text(update_name_stmt, 1, new_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(update_name_stmt, 2, id);
        TRY_STEP(update_name_stmt, SQLITE_DONE, "[init] can't update name of style in database\n");
        sqlite3_reset(update_name_stmt);
        sqlite3_clear_bindings(update_name_stmt);
        g_free(new_name);
      }

      // move the style to data.styles and get the rowid
      sqlite3_bind_int(insert_stmt, 1, id);
      TRY_STEP(insert_stmt, SQLITE_DONE, "[init] can't insert style into data database\n");
      sqlite3_int64 last_rowid = sqlite3_last_insert_rowid(db->handle);

      // delete style from styles
      sqlite3_bind_int(delete_stmt, 1, id);
      TRY_STEP(delete_stmt, SQLITE_DONE, "[init] can't delete style from database\n");

      sqlite3_bind_int(select_new_stmt, 1, last_rowid);
      TRY_STEP(select_new_stmt, SQLITE_ROW, "[init] can't select new style from data database\n");
      int new_id = sqlite3_column_int(select_new_stmt, 0);

      // now that we have the style over in data.styles and the new id we can just copy over all style items
      sqlite3_bind_int(copy_style_items_stmt, 1, new_id);
      sqlite3_bind_int(copy_style_items_stmt, 2, id);
      TRY_STEP(copy_style_items_stmt, SQLITE_DONE, "[init] can't copy style items into data database\n");

      // delete the style items from the old table
      sqlite3_bind_int(delete_style_items_stmt, 1, id);
      TRY_STEP(delete_style_items_stmt, SQLITE_DONE, "[init] can't delete style items from database\n");

      // cleanup for the next round
      sqlite3_reset(insert_stmt);
      sqlite3_clear_bindings(insert_stmt);
      sqlite3_reset(select_stmt);
      sqlite3_clear_bindings(select_stmt);
      sqlite3_reset(delete_stmt);
      sqlite3_clear_bindings(delete_stmt);
      sqlite3_reset(select_new_stmt);
      sqlite3_clear_bindings(select_new_stmt);
      sqlite3_reset(copy_style_items_stmt);
      sqlite3_clear_bindings(copy_style_items_stmt);
      sqlite3_reset(delete_style_items_stmt);
      sqlite3_clear_bindings(delete_style_items_stmt);
    }
    FINALIZE;
#undef FINALIZE

    ////////////// tags
#define FINALIZE

    // tags
    TRY_EXEC("INSERT OR IGNORE INTO data.tags (name, icon, description, flags) "
             "SELECT name, icon, description, flags FROM main.tags",
             "[init] can't prepare insertion of used tags into data database\n");

    // tagged images
    // we need a temp table to update tagged_images due to its primary key
    TRY_EXEC("CREATE TEMPORARY TABLE tagged_images_tmp (imgid INTEGER, tagid INTEGER)",
             "[init] can't create temporary table for updating `tagged_images'\n");

    TRY_EXEC("INSERT INTO tagged_images_tmp (imgid, tagid) "
             "SELECT imgid, (SELECT t2.id FROM main.tags t1, data.tags t2 USING (name) WHERE t1.id = tagid) "
             "FROM main.tagged_images", "[init] can't insert into `tagged_images_tmp'\n");

    TRY_EXEC("DELETE FROM main.tagged_images", "[init] can't delete tagged images in database\n");

    TRY_EXEC("INSERT OR IGNORE INTO main.tagged_images (imgid, tagid) SELECT imgid, tagid FROM tagged_images_tmp",
             "[init] can't copy updated values back to `tagged_images'\n");

    TRY_EXEC("DROP TABLE tagged_images_tmp", "[init] can't drop table `tagged_images_tmp' from database\n");

    ////////////// cleanup - drop the indexes and tags
    TRY_EXEC("DROP INDEX IF EXISTS main.presets_idx", "[init] can't drop index `presets_idx' from database\n");
    TRY_EXEC("DROP TABLE main.presets", "[init] can't drop table `presets' from database\n");
    TRY_EXEC("DROP TABLE main.style_items", "[init] can't drop table `style_items' from database\n");
    TRY_EXEC("DROP TABLE main.styles", "[init] can't drop table `styles' from database\n");
    TRY_EXEC("DROP TABLE main.tags", "[init] can't drop table `tags' from database\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 13;
  } else if(version == 13)
  {
    // 12 -> 13 bring back the used tag names to library.db so people can use it independently of data.db
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("CREATE TABLE main.used_tags (id INTEGER, name VARCHAR NOT NULL)",
             "[init] can't create `used_tags` table\n");

    TRY_EXEC("CREATE INDEX main.used_tags_idx ON used_tags (id, name)",
             "[init] can't create index on table `used_tags' in database\n");

    TRY_EXEC("INSERT INTO main.used_tags (id, name) SELECT t.id, t.name FROM data.tags AS t, main.tagged_images "
             "AS i ON t.id = i.tagid GROUP BY t.id",
             "[init] can't insert used tags into `used_tags` table in database\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 14;
  }
  else if(version == 14)
  {
    // 13 -> fix the index on used_tags to be a UNIQUE index :-/
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("DELETE FROM main.used_tags WHERE rowid NOT IN (SELECT rowid FROM used_tags GROUP BY id)",
             "[init] can't delete duplicated entries from `used_tags' in database\n");

    TRY_EXEC("DROP INDEX main.used_tags_idx", "[init] can't drop index `used_tags_idx' from database\n");

    TRY_EXEC("CREATE UNIQUE INDEX main.used_tags_idx ON used_tags (id, name)",
             "[init] can't create index `used_tags_idx' in database\n");

    TRY_EXEC("DELETE FROM main.tagged_images WHERE tagid IS NULL",
             "[init] can't delete NULL entries from `tagged_images' in database");

    TRY_EXEC("DELETE FROM main.used_tags WHERE id NOT IN (SELECT DISTINCT tagid FROM main.tagged_images)",
             "[init] can't delete unused tags from `used_tags' in database\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 15;
  }
  else if(version == 15)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    ////////////////////////////// custom image order
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN position INTEGER",
             "[init] can't add `position' column to images table in database\n");
    TRY_EXEC("CREATE INDEX main.image_position_index ON images (position)",
             "[init] can't create index for custom image order table\n");

    // Set the initial image sequence. The image id - the sequence images were imported -
    // defines the initial order of images.
    //
    // An int64 is used for the position index. The upper 31 bits define the initial order.
    // The lower 32bit provide space to reorder images.
    //
    // see: dt_collection_move_before()
    //
    TRY_EXEC("UPDATE main.images SET position = id << 32",
             "[init] can't update positions custom image order table\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 16;
  }
  else if(version == 16)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    ////////////////////////////// final image aspect ratio
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN aspect_ratio REAL",
             "[init] can't add `aspect_ratio' column to images table in database\n");
    TRY_EXEC("UPDATE main.images SET aspect_ratio = 0.0",
             "[init] can't update aspect_ratio in database\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 17;
  }
  else if(version == 17)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    ////////////////////////////// masks history
    TRY_EXEC("CREATE TABLE main.masks_history (imgid INTEGER, num INTEGER, formid INTEGER, form INTEGER, name VARCHAR(256), "
             "version INTEGER, points BLOB, points_count INTEGER, source BLOB)",
             "[init] can't create `masks_history` table\n");

    TRY_EXEC("CREATE INDEX main.masks_history_imgid_index ON masks_history (imgid)",
             "[init] can't create index `masks_history_imgid_index' in database\n");

    // to speed up the mask look-up, and makes the following UPDATE instantaneous whereas it could takes hours
    TRY_EXEC("CREATE INDEX main.mask_imgid_index ON mask (imgid);",
             "[init] can't create index `mask_imgid_index' in database\n");

    // create a mask manager entry on history for all images containing all forms
    // make room for mask manager history entry
    TRY_EXEC("UPDATE main.history SET num=num+1 WHERE imgid IN (SELECT imgid FROM main.mask WHERE main.mask.imgid=main.history.imgid)",
             "[init] can't update `num' with num+1\n");

    // update history end
    TRY_EXEC("UPDATE main.images SET history_end = history_end+1 WHERE id IN (SELECT imgid FROM main.mask WHERE main.mask.imgid=main.images.id)",
             "[init] can't update `history_end' with history_end+1\n");

    // copy all masks into history
    TRY_EXEC("INSERT INTO main.masks_history (imgid, num, formid, form, name, version, points, points_count, source) SELECT "
             "imgid, 0, formid, form, name, version, points, points_count, source FROM main.mask",
             "[init] can't insert into masks_history\n");

    // create a mask manager entry for each image that has masks
    TRY_EXEC("INSERT INTO main.history (imgid, num, operation, op_params, module, enabled, "
             "blendop_params, blendop_version, multi_priority, multi_name) "
             "SELECT DISTINCT imgid, 0, 'mask_manager', NULL, 1, 0, NULL, 0, 0, '' FROM main.mask "
             "GROUP BY imgid",
             "[init] can't insert mask manager into history\n");

    TRY_EXEC("DROP TABLE main.mask", "[init] can't drop table `mask' from database\n");

    ////////////////////////////// custom iop order
    GList *prior_v1 = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_LEGACY);

    TRY_EXEC("ALTER TABLE main.images ADD COLUMN iop_order_version INTEGER",
             "[init] can't add `iop_order_version' column to images table in database\n");

    TRY_EXEC("UPDATE main.images SET iop_order_version = 0",
             "[init] can't update iop_order_version in database\n");

    TRY_EXEC("UPDATE main.images SET iop_order_version = 1 WHERE "
             "EXISTS(SELECT * FROM main.history WHERE main.history.imgid = main.images.id)",
             "[init] can't update iop_order_version in database\n");

    TRY_EXEC("ALTER TABLE main.history ADD COLUMN iop_order REAL",
             "[init] can't add `iop_order' column to history table in database\n");

    // create a temp table with the previous priorities
    TRY_EXEC("CREATE TEMPORARY TABLE iop_order_tmp (iop_order REAL, operation VARCHAR(256))",
             "[init] can't create temporary table for updating `main.history'\n");

    // fill temp table with all operations up to this release
    // it will be used to create the pipe and update the iop_order on history
    GList *priorities = g_list_first(prior_v1);
    while(priorities)
    {
      dt_iop_order_entry_t *prior = (dt_iop_order_entry_t *)priorities->data;

      sqlite3_prepare_v2(
          db->handle,
          "INSERT INTO iop_order_tmp (iop_order, operation) VALUES (?1, ?2)",
          -1, &stmt, NULL);
      sqlite3_bind_double(stmt, 1, prior->o.iop_order_f);
      sqlite3_bind_text(stmt, 2, prior->operation, -1, SQLITE_TRANSIENT);
      TRY_STEP(stmt, SQLITE_DONE, "[init] can't insert default value in iop_order_tmp\n");
      sqlite3_finalize(stmt);

      priorities = g_list_next(priorities);
    }
    g_list_free_full(prior_v1, free);

    // create the order of the pipe
    // iop_order is by default the module priority
    // if there's multi-instances we add the multi_priority
    // multi_priority is in reverse order in this version,
    // so we assume that is always less than 1000 and reverse it
    // it is possible that multi_priority = 0 don't appear in history
    // so just in case 1 / 1000 to every instance
    TRY_EXEC("UPDATE main.history SET iop_order = ((("
        "SELECT MAX(multi_priority) FROM main.history hist1 WHERE hist1.imgid = main.history.imgid AND hist1.operation = main.history.operation "
             ") + 1. - multi_priority) / 1000.) + "
             "IFNULL((SELECT iop_order FROM iop_order_tmp WHERE iop_order_tmp.operation = "
             "main.history.operation), -999999.) ",
             "[init] can't update iop_order in history table\n");

    // check if there's any entry in history that was not updated
    sqlite3_stmt *sel_stmt;
    TRY_PREPARE(sel_stmt, "SELECT DISTINCT operation FROM main.history WHERE iop_order <= 0 OR iop_order IS NULL",
                "[init] can't prepare selecting history iop_order\n");
    while(sqlite3_step(sel_stmt) == SQLITE_ROW)
    {
      const char *op_name = (const char *)sqlite3_column_text(sel_stmt, 0);
      printf("operation %s with no iop_order while upgrading database\n", op_name);
    }
    sqlite3_finalize(sel_stmt);

    TRY_EXEC("DROP TABLE iop_order_tmp", "[init] can't drop table `iop_order_tmp' from database\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 18;
  }
  // maybe in the future, see commented out code elsewhere
  //   else if(version == XXX)
  //   {
  //     sqlite3_exec(db->handle, "ALTER TABLE film_rolls ADD COLUMN external_drive VARCHAR(1024)", NULL,
  //     NULL, NULL);
  //   }
  else if(version == 18)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("UPDATE images SET orientation=-2 WHERE orientation=1;",
             "[init] can't update images orientation 1 from database\n");

    TRY_EXEC("UPDATE images SET orientation=1 WHERE orientation=2;",
             "[init] can't update images orientation 2 from database\n");

    TRY_EXEC("UPDATE images SET orientation=-6 WHERE orientation=5;",
             "[init] can't update images orientation 5 from database\n");

    TRY_EXEC("UPDATE images SET orientation=5 WHERE orientation=6;",
             "[init] can't update images orientation 6 from database\n");

    TRY_EXEC("UPDATE images SET orientation=2 WHERE orientation=-2;",
             "[init] can't update images orientation -1 from database\n");

    TRY_EXEC("UPDATE images SET orientation=6 WHERE orientation=-6;",
             "[init] can't update images orientation -6 from database\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 19;
  }
  else if(version == 19)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    // create a temp table to invert all multi_priority
    TRY_EXEC("CREATE TEMPORARY TABLE m_prio (id INTEGER, operation VARCHAR(256), prio INTEGER)",
             "[init] can't create temporary table for updating `history and style_items'\n");

    TRY_EXEC("CREATE INDEX m_prio_id_index ON m_prio (id)",
             "[init] can't create temporary index for updating `history and style_items'\n");
    TRY_EXEC("CREATE INDEX m_prio_op_index ON m_prio (operation)",
             "[init] can't create temporary index for updating `history and style_items'\n");

    TRY_EXEC("INSERT INTO m_prio SELECT imgid, operation, MAX(multi_priority)"
             " FROM main.history GROUP BY imgid, operation",
             "[init] can't populate m_prio\n");

    TRY_EXEC("UPDATE main.history SET multi_priority = "
             "(SELECT prio FROM m_prio "
             " WHERE main.history.operation = operation AND main.history.imgid = id) - main.history.multi_priority",
             "[init] can't update multi_priority for history\n");

    TRY_EXEC("DROP TABLE m_prio", "[init] can't drop table `m_prio' from database\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 20;
  }
  else if(version == 20)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("DROP INDEX IF EXISTS main.used_tags_idx", "[init] can't drop index `used_tags_idx' from database\n");
    TRY_EXEC("DROP TABLE used_tags", "[init] can't delete table used_tags\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

    new_version = 21;
  }
  else if(version == 21)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // create a temp table to invert all multi_priority
    TRY_EXEC("CREATE TABLE module_order (imgid INTEGER PRIMARY KEY, version INTEGER, iop_list VARCHAR)",
             "[init] can't create module_order table'\n");

    // for all images:
    sqlite3_stmt *mig_stmt;
    TRY_PREPARE(mig_stmt, "SELECT imgid, operation, multi_priority, iop_order, mi.iop_order_version"
                          " FROM main.history AS hi, main.images AS mi"
                          " WHERE hi.imgid = mi.id"
                          " GROUP BY imgid, operation, multi_priority"
                          " ORDER BY imgid, iop_order",
                "[init] can't prepare selecting history for iop_order migration (v21)\n");

    GList *item_list = NULL;
    int current_imgid = -1;
    int current_order_version = -1;

    gboolean has_row = (sqlite3_step(mig_stmt) == SQLITE_ROW);

    while(has_row)
    {
      const int32_t imgid = sqlite3_column_int(mig_stmt, 0);
      char operation[20] = { 0 };
      g_strlcpy(operation, (const char *)sqlite3_column_text(mig_stmt, 1), sizeof(operation));
      const int multi_priority = sqlite3_column_int(mig_stmt, 2);
      const double iop_order = sqlite3_column_double(mig_stmt, 3);
      const int iop_order_version = sqlite3_column_int(mig_stmt, 4);

      has_row = (sqlite3_step(mig_stmt) == SQLITE_ROW);

      // a new image, let's initialize the iop_order_version
      if(imgid != current_imgid || !has_row)
      {
        // new image, let's handle it
        if(item_list != NULL)
        {
          // we keep legacy, everything else is migrated to v3.0
          const dt_iop_order_t new_order_version = current_order_version == 2 ? DT_IOP_ORDER_LEGACY : DT_IOP_ORDER_V30;

          GList *iop_order_list = dt_ioppr_get_iop_order_list_version(new_order_version);

          // merge entries into iop_order_list

          // first remove all item_list iop from the iop_order_list

          GList *e = g_list_first(item_list);
          GList *n = NULL;
          dt_iop_order_entry_t *n_entry = NULL;

          while(e)
          {
            dt_iop_order_entry_t *e_entry = (dt_iop_order_entry_t *)e->data;

            GList *s = g_list_first(iop_order_list);
            while(s && strcmp(((dt_iop_order_entry_t *)s->data)->operation, e_entry->operation))
            {
              s = g_list_next(s);
            }
            if(s)
            {
              iop_order_list = g_list_delete_link(iop_order_list, s);
            }

            // skip all multipe instances
            n = e;
            do
            {
              n = g_list_next(n);
              if(!n) break;
              n_entry = (dt_iop_order_entry_t *)n->data;
            } while(!strcmp(n_entry->operation, e_entry->operation));
            e = n;
          }

          // then add all item_list into iop_order_list

          e = g_list_first(item_list);
          while(e)
          {
            dt_iop_order_entry_t *e_entry = (dt_iop_order_entry_t *)e->data;
            iop_order_list = g_list_append(iop_order_list, e_entry);
            e = g_list_next(e);
          }

          // and finally reoder the full list based on the iop-order

          iop_order_list = g_list_sort(iop_order_list, dt_sort_iop_list_by_order_f);

          const dt_iop_order_t kind = dt_ioppr_get_iop_order_list_kind(iop_order_list);

          // check if we have some multi-instances

          gboolean has_multiple_instances = FALSE;
          GList *l = g_list_first(iop_order_list);

          while(l)
          {
            GList *next = g_list_next(l);
            if(next
               && (strcmp(((dt_iop_order_entry_t *)(l->data))->operation,
                          ((dt_iop_order_entry_t *)(next->data))->operation) == 0))
            {
              has_multiple_instances = TRUE;
              break;
            }
            l = next;
          }

          // write iop_order_list and/or version into module_order

          sqlite3_stmt *ins_stmt = NULL;
          if(kind == DT_IOP_ORDER_CUSTOM || has_multiple_instances)
          {
            char *iop_list_txt = dt_ioppr_serialize_text_iop_order_list(iop_order_list);

            sqlite3_prepare_v2(db->handle,
                               "INSERT INTO module_order VALUES (?1, ?2, ?3)", -1,
                               &ins_stmt, NULL);
            sqlite3_bind_int(ins_stmt, 1, current_imgid);
            sqlite3_bind_int(ins_stmt, 2, kind);
            sqlite3_bind_text(ins_stmt, 3, iop_list_txt, -1, SQLITE_TRANSIENT);
            TRY_STEP(ins_stmt, SQLITE_DONE, "[init] can't insert into module_order (custom order)\n");
            sqlite3_finalize(ins_stmt);

            g_free(iop_list_txt);
          }
          else
          {
            sqlite3_prepare_v2(db->handle,
                               "INSERT INTO module_order VALUES (?1, ?2, NULL)", -1,
                               &ins_stmt, NULL);
            sqlite3_bind_int(ins_stmt, 1, current_imgid);
            sqlite3_bind_int(ins_stmt, 2, kind);
            TRY_STEP(ins_stmt, SQLITE_DONE, "[init] can't insert into module_order (standard order)\n");
            sqlite3_finalize(ins_stmt);
          }

          g_list_free(item_list);
          g_list_free(iop_order_list);

          item_list = NULL;
        }

        current_imgid = imgid;
        current_order_version = iop_order_version;
      }

      dt_iop_order_entry_t *item = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
      memcpy(item->operation, operation, sizeof(item->operation));
      item->instance = multi_priority;
      item->o.iop_order_f = iop_order; // used to order the enties only
      item_list = g_list_append(item_list, item);
    }
    sqlite3_finalize(mig_stmt);

    // remove iop_order from history table

    TRY_EXEC("CREATE TABLE h (imgid INTEGER, num INTEGER, module INTEGER, "
             "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
             "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
             "[init] can't create module_order table\n");
    TRY_EXEC("CREATE INDEX h_imgid_index ON h (imgid)",
             "[init] can't create index h_imgid_index\n");
    TRY_EXEC("INSERT INTO h SELECT imgid, num, module, operation, op_params, enabled, "
             "blendop_params, blendop_version, multi_priority, multi_name FROM main.history",
             "[init] can't create module_order table\n");
    TRY_EXEC("DROP TABLE history",
             "[init] can't drop table history\n");
    TRY_EXEC("ALTER TABLE h RENAME TO history",
             "[init] can't rename h to history\n");
    TRY_EXEC("DROP INDEX h_imgid_index",
             "[init] can't drop index h_imgid_index\n");
    TRY_EXEC("CREATE INDEX main.history_imgid_index ON history (imgid)",
             "[init] can't create index images_imgid_index\n");

    // remove iop_order_version from images

    TRY_EXEC("CREATE TABLE i (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, film_id INTEGER, "
             "width INTEGER, height INTEGER, filename VARCHAR, maker VARCHAR, model VARCHAR, "
             "lens VARCHAR, exposure REAL, aperture REAL, iso REAL, focal_length REAL, "
             "focus_distance REAL, datetime_taken CHAR(20), flags INTEGER, "
             "output_width INTEGER, output_height INTEGER, crop REAL, "
             "raw_parameters INTEGER, raw_denoise_threshold REAL, "
             "raw_auto_bright_threshold REAL, raw_black INTEGER, raw_maximum INTEGER, "
             "caption VARCHAR, description VARCHAR, license VARCHAR, sha1sum CHAR(40), "
             "orientation INTEGER, histogram BLOB, lightmap BLOB, longitude REAL, "
             "latitude REAL, altitude REAL, color_matrix BLOB, colorspace INTEGER, version INTEGER, "
             "max_version INTEGER, write_timestamp INTEGER, history_end INTEGER, position INTEGER, aspect_ratio REAL)",
             "[init] can't create table i\n");
    TRY_EXEC("INSERT INTO i SELECT id, group_id, film_id, width, height, filename, maker, model,"
             " lens, exposure, aperture, iso, focal_length, focus_distance, datetime_taken, flags,"
             " output_width, output_height, crop, raw_parameters, raw_denoise_threshold,"
             " raw_auto_bright_threshold, raw_black, raw_maximum, caption, description, license, sha1sum,"
             " orientation, histogram, lightmap, longitude, latitude, altitude, color_matrix, colorspace, version,"
             " max_version, write_timestamp, history_end, position, aspect_ratio "
             "FROM images",
             "[init] can't populate table h\n");
    TRY_EXEC("DROP TABLE images",
             "[init] can't drop table images\n");
    TRY_EXEC("ALTER TABLE i RENAME TO images",
             "[init] can't rename i to images\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

    new_version = 22;
  }
  else if(version == 22)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    TRY_EXEC("CREATE INDEX IF NOT EXISTS main.images_group_id_index ON images (group_id)",
             "[init] can't create group_id index on image\n");
    TRY_EXEC("CREATE INDEX IF NOT EXISTS  main.images_film_id_index ON images (film_id)",
             "[init] can't create film_id index on image\n");
    TRY_EXEC("CREATE INDEX IF NOT EXISTS main.images_filename_index ON images (filename)",
             "[init] can't create filename index on image\n");
    TRY_EXEC("CREATE INDEX IF NOT EXISTS main.image_position_index ON images (position)",
             "[init] can't create position index on image\n");

    TRY_EXEC("CREATE INDEX IF NOT EXISTS main.film_rolls_folder_index ON film_rolls (folder)",
             "[init] can't create folder index on film_rolls\n");
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

    new_version = 23;
  }
  else if(version == 23)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    TRY_EXEC("CREATE TABLE main.history_hash (imgid INTEGER PRIMARY KEY, "
             "basic_hash BLOB, auto_hash BLOB, current_hash BLOB)",
             "[init] can't create table history_hash\n");

    // use the former dt_image_altered() to initialise the history_hash table
    // insert an history_hash entry for all images which have an history
    // note that images without history don't get hash and are considered as basic
    sqlite3_stmt *h_stmt;
    char *workflow = dt_conf_get_string("plugins/darkroom/workflow");
    const gboolean basecurve_auto_apply = strcmp(workflow, "display-referred") == 0;
    g_free(workflow);
    const gboolean sharpen_auto_apply = dt_conf_get_bool("plugins/darkroom/sharpen/auto_apply");
    char *query = NULL;
    query = dt_util_dstrcat(query,
                            "SELECT id, CASE WHEN imgid IS NULL THEN 0 ELSE 1 END as altered "
                            // first, images which are both in images and history (avoids history orphans)
                            "FROM (SELECT DISTINCT id FROM main.images JOIN main.history ON imgid = id) "
                            "LEFT JOIN (SELECT DISTINCT imgid FROM main.images JOIN main.history ON imgid = id "
                            "           WHERE num < history_end AND enabled = 1"
                            "             AND operation NOT IN ('flip', 'dither', 'highlights', 'rawprepare', "
                            "             'colorin', 'colorout', 'gamma', 'demosaic', 'temperature'%s%s)) "
                            "ON imgid = id",
                             basecurve_auto_apply ? ", 'basecurve'" : "",
                             sharpen_auto_apply ? ", 'sharpen'" : "");
    TRY_PREPARE(h_stmt, query,
                "[init] can't prepare selecting history for history_hash migration\n");
    while(sqlite3_step(h_stmt) == SQLITE_ROW)
    {
      const int32_t imgid= sqlite3_column_int(h_stmt, 0);
      const int32_t altered = sqlite3_column_int(h_stmt, 1);

      guint8 *hash = NULL;
      GChecksum *checksum = g_checksum_new(G_CHECKSUM_MD5);
      gsize hash_len = 0;

      // get history
      sqlite3_stmt *h2_stmt;
      sqlite3_prepare_v2(db->handle,
                         "SELECT operation, op_params, blendop_params"
                         " FROM main.history"
                         " WHERE imgid = ?1 AND enabled = 1"
                         " ORDER BY num",
                         -1, &h2_stmt, NULL);
      sqlite3_bind_int(h2_stmt, 1, imgid);
      while(sqlite3_step(h2_stmt) == SQLITE_ROW)
      {
        // operation
        char *buf = (char *)sqlite3_column_text(h2_stmt, 0);
        if(buf) g_checksum_update(checksum, (const guchar *)buf, -1);
        // op_params
        buf = (char *)sqlite3_column_blob(h2_stmt, 1);
        int params_len = sqlite3_column_bytes(h2_stmt, 1);
        if(buf) g_checksum_update(checksum, (const guchar *)buf, params_len);
        // blendop_params
        buf = (char *)sqlite3_column_blob(h2_stmt, 2);
        params_len = sqlite3_column_bytes(h2_stmt, 2);
        if(buf) g_checksum_update(checksum, (const guchar *)buf, params_len);
      }
      sqlite3_finalize(h2_stmt);

      // get module order
      h2_stmt = NULL;
      sqlite3_prepare_v2(db->handle,
                         "SELECT version, iop_list"
                         " FROM main.module_order"
                         " WHERE imgid = ?1",
                         -1, &h2_stmt, NULL);
      sqlite3_bind_int(h2_stmt, 1, imgid);
      if(sqlite3_step(h2_stmt) == SQLITE_ROW)
      {
        const int version_h = sqlite3_column_int(h2_stmt, 0);
        g_checksum_update(checksum, (const guchar *)&version_h, sizeof(version_h));
        if(version_h == DT_IOP_ORDER_CUSTOM)
        {
          // iop_list
          const char *buf = (char *)sqlite3_column_text(h2_stmt, 1);
          if(buf) g_checksum_update(checksum, (const guchar *)buf, -1);
        }
      }
      sqlite3_finalize(h2_stmt);

      const gsize checksum_len = g_checksum_type_get_length(G_CHECKSUM_MD5);
      hash = g_malloc(checksum_len);
      hash_len = checksum_len;
      g_checksum_get_digest(checksum, hash, &hash_len);

      g_checksum_free(checksum);
      // insert the hash for that image
      h2_stmt = NULL;
      sqlite3_prepare_v2(db->handle,
                         "INSERT INTO main.history_hash"
                         " VALUES (?1, ?2, NULL, ?3)",
                         -1, &h2_stmt, NULL);
      sqlite3_bind_int(h2_stmt, 1, imgid);
      sqlite3_bind_blob(h2_stmt, 2, altered ? NULL : hash, altered ? 0 : hash_len, SQLITE_TRANSIENT);
      sqlite3_bind_blob(h2_stmt, 3, hash, hash_len, SQLITE_TRANSIENT);
      TRY_STEP(h2_stmt, SQLITE_DONE, "[init] can't insert into history_hash\n");
      sqlite3_finalize(h2_stmt);
      g_free(hash);
    }
    sqlite3_finalize(h_stmt);
    g_free(query);

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

    new_version = 24;
  }
  else if(version == 24)
  {
    TRY_EXEC("ALTER TABLE main.history_hash ADD COLUMN mipmap_hash BLOB",
             "[init] can't add `mipmap_hash' column to history_hash table in database\n");

    new_version = 25;
  }
  else if(version == 25)
  {
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN exposure_bias REAL",
             "[init] can't add `exposure_bias' column to images table in database\n");

    new_version = 26;
  }
  else if(version == 26)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("CREATE TABLE main.new_film_rolls "
             "(id INTEGER PRIMARY KEY, "
             "access_timestamp INTEGER, "
             "folder VARCHAR(1024) NOT NULL)",
             "[init] can't create new_film_rolls table\n");

    TRY_EXEC("INSERT INTO main.new_film_rolls"
             "(id, access_timestamp, folder) "
             "SELECT id, "
             "strftime('%s', replace(substr(datetime_accessed, 1, 10), ':', '-') || substr(datetime_accessed, 11), 'utc'), "
             "folder "
             "FROM film_rolls "
             "WHERE folder IS NOT NULL",
             "[init] can't populate new_film_rolls table from film_rolls\n");

    TRY_EXEC("DROP TABLE film_rolls",
             "[init] can't delete table film_rolls\n");

    TRY_EXEC("ALTER TABLE main.new_film_rolls RENAME TO film_rolls",
             "[init] can't rename table new_film_rolls to film_rolls\n");

    TRY_EXEC("CREATE INDEX main.film_rolls_folder_index ON film_rolls (folder)",
             "[init] can't create index `film_rolls_folder_index' on table `film_rolls'\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 27;
  }
  else if(version == 27)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("ALTER TABLE main.images ADD COLUMN import_timestamp INTEGER DEFAULT -1",
             "[init] can't add `import_timestamp' column to images table in database\n");
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN change_timestamp INTEGER DEFAULT -1",
             "[init] can't add `change_timestamp' column to images table in database\n");
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN export_timestamp INTEGER DEFAULT -1",
             "[init] can't add `export_timestamp' column to images table in database\n");
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN print_timestamp INTEGER DEFAULT -1",
             "[init] can't add `print_timestamp' column to images table in database\n");

    TRY_EXEC("UPDATE main.images SET import_timestamp = (SELECT access_timestamp "
               "FROM main.film_rolls WHERE film_rolls.id = images.film_id)",
             "[init] can't populate import_timestamp column from film_rolls.access_timestamp.\n");

    TRY_EXEC("UPDATE main.images SET change_timestamp = images.write_timestamp "
               "WHERE images.write_timestamp IS NOT NULL "
                 "AND images.id = (SELECT imgid FROM tagged_images "
                   "JOIN data.tags ON tags.id = tagged_images.tagid "
                     "WHERE data.tags.name = 'darktable|changed')",
             "[init] can't populate change_timestamp column from images.write_timestamp.\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 28;
  }
  else if(version == 28)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    // clear flag DT_IMAGE_REJECTED (was not used)
    TRY_EXEC("UPDATE main.images SET flags = (flags & ~8)",
             "[init] can't clear rejected flags");

    // add DT_IMAGE_REJECTED and clear rating for all images being rejected
    TRY_EXEC("UPDATE main.images SET flags = (flags | 8) & ~7 WHERE (flags & 7) = 6",
             "[init] can't set rejected flags");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 29;
  }
  else if(version == 29)
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
  sqlite3_prepare_v2(db->handle, "INSERT OR REPLACE INTO main.db_info (key, value) VALUES ('version', ?1)", -1, &stmt,
                     NULL);
  sqlite3_bind_int(stmt, 1, new_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return new_version;
}

/* do the real migration steps, returns the version the db was converted to */
static int _upgrade_data_schema_step(dt_database_t *db, int version)
{
  sqlite3_stmt *stmt;
  int new_version = version;
  if(version == CURRENT_DATABASE_VERSION_DATA)
    return version;
  else if(version == 0)
  {
    // this can't happen, we started with 1, but it's a good example how this function works
    // <do some magic to the db>
    new_version = 1; // the version we transformed the db to. this way it might be possible to roll back or
    // add fast paths
  }
  else if(version == 1)
  {
    // style_items:
    //    NO TRY_EXEC has the column could be there before version 1 (master build)
    //    TRY_EXEC("ALTER TABLE data.style_items ADD COLUMN iop_order REAL",
    //             "[init] can't add `iop_order' column to style_items table in database\n");
    sqlite3_exec(db->handle, "ALTER TABLE data.style_items ADD COLUMN iop_order REAL", NULL, NULL, NULL);

    sqlite3_stmt *sel_stmt = NULL;
    GList *prior_v1 = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_LEGACY);

    // create a temp table with the previous priorities
    TRY_EXEC("CREATE TEMPORARY TABLE iop_order_tmp (iop_order REAL, operation VARCHAR(256))",
             "[init] can't create temporary table for updating `data.style_items'\n");

    // fill temp table with all operations up to this release
    // it will be used to create the pipe and update the iop_order on history
    GList *priorities = g_list_first(prior_v1);
    while(priorities)
    {
      dt_iop_order_entry_t *prior = (dt_iop_order_entry_t *)priorities->data;

      sqlite3_prepare_v2(
          db->handle,
          "INSERT INTO iop_order_tmp (iop_order, operation) VALUES (?1, ?2)",
          -1, &stmt, NULL);
      sqlite3_bind_double(stmt, 1, prior->o.iop_order_f);
      sqlite3_bind_text(stmt, 2, prior->operation, -1, SQLITE_TRANSIENT);
      TRY_STEP(stmt, SQLITE_DONE, "[init] can't insert default value in iop_order_tmp\n");
      sqlite3_finalize(stmt);

      priorities = g_list_next(priorities);
    }
    g_list_free_full(prior_v1, free);

    // do the same as for history
    TRY_EXEC("UPDATE data.style_items SET iop_order = ((("
        "SELECT MAX(multi_priority) FROM data.style_items style1 WHERE style1.styleid = data.style_items.styleid AND style1.operation = data.style_items.operation "
             ") + 1. - multi_priority) / 1000.) + "
             "IFNULL((SELECT iop_order FROM iop_order_tmp WHERE iop_order_tmp.operation = "
             "data.style_items.operation), -999999.) ",
             "[init] can't update iop_order in style_items table\n");

    TRY_PREPARE(sel_stmt, "SELECT DISTINCT operation FROM data.style_items WHERE iop_order <= 0 OR iop_order IS NULL",
                "[init] can't prepare selecting style_items iop_order\n");
    while(sqlite3_step(sel_stmt) == SQLITE_ROW)
    {
      const char *op_name = (const char *)sqlite3_column_text(sel_stmt, 0);
      printf("operation %s with no iop_order while upgrading style_items in database\n", op_name);
    }
    sqlite3_finalize(sel_stmt);

    TRY_EXEC("DROP TABLE iop_order_tmp", "[init] can't drop table `iop_order_tmp' from database\n");

    new_version = 2;
  }
  else if(version == 2)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    //    With sqlite above or equal to 3.25.0 RENAME COLUMN can be used instead of the following code
    //    TRY_EXEC("ALTER TABLE data.tags RENAME COLUMN description TO synonyms;",
    //             "[init] can't change tags column name from description to synonyms\n");

    TRY_EXEC("ALTER TABLE data.tags RENAME TO tmp_tags",  "[init] can't rename table tags\n");

    TRY_EXEC("CREATE TABLE data.tags (id INTEGER PRIMARY KEY, name VARCHAR, "
             "synonyms VARCHAR, flags INTEGER)",
             "[init] can't create new tags table\n");

    TRY_EXEC("INSERT INTO data.tags (id, name, synonyms, flags) SELECT id, name, description, flags "
             "FROM tmp_tags",
             "[init] can't populate tags table from tmp_tags\n");

    TRY_EXEC("DROP TABLE tmp_tags", "[init] can't delete table tmp_tags\n");

    TRY_EXEC("CREATE UNIQUE INDEX data.tags_name_idx ON tags (name)",
             "[init] can't create tags_name_idx on tags table\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

    new_version = 3;
  }
  else if(version == 3)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    // create a temp table to invert all multi_priority
    TRY_EXEC("CREATE TEMPORARY TABLE m_prio (id INTEGER, operation VARCHAR(256), prio INTEGER)",
             "[init] can't create temporary table for updating `history and style_items'\n");

    TRY_EXEC("INSERT INTO m_prio SELECT styleid, operation, MAX(multi_priority)"
             " FROM data.style_items GROUP BY styleid, operation",
             "[init] can't populate m_prio\n");

    // update multi_priority for style items and history
    TRY_EXEC("UPDATE data.style_items SET multi_priority = "
             "(SELECT prio FROM m_prio "
             " WHERE data.style_items.operation = operation AND data.style_items.styleid = id)"
             " - data.style_items.multi_priority",
             "[init] can't update multi_priority for style_items\n");

    TRY_EXEC("DROP TABLE m_prio", "[init] can't drop table `m_prio' from database\n");
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

    new_version = 4;
  }
  else if(version == 4)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    // remove iop_order from style_item table
    TRY_EXEC("ALTER TABLE data.style_items RENAME TO s",
             "[init] can't rename style_items to s\n");
    TRY_EXEC("CREATE TABLE data.style_items (styleid INTEGER, num INTEGER, module INTEGER, "
             "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
             "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
             "[init] can't create style_items table'\n");
    TRY_EXEC("INSERT INTO data.style_items SELECT styleid, num, module, operation, op_params, enabled, "
             " blendop_params, blendop_version, multi_priority, multi_name "
             "FROM s",
             "[init] can't populate style_items table'\n");
    TRY_EXEC("DROP TABLE s",
             "[init] can't drop table s'\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

    new_version = 5;
  }
  else if(version == 5)
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
  sqlite3_prepare_v2(db->handle, "INSERT OR REPLACE INTO data.db_info (key, value) VALUES ('version', ?1)", -1, &stmt,
                     NULL);
  sqlite3_bind_int(stmt, 1, new_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return new_version;
}

#undef FINALIZE

#undef TRY_EXEC
#undef TRY_STEP
#undef TRY_PREPARE

/* upgrade library db from 'version' to CURRENT_DATABASE_VERSION_LIBRARY. don't touch this function but
 * _upgrade_library_schema_step() instead. */
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

/* upgrade data db from 'version' to CURRENT_DATABASE_VERSION_DATA. don't touch this function but
 * _upgrade_data_schema_step() instead. */
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

/* create the current database schema and set the version in db_info accordingly */
static void _create_library_schema(dt_database_t *db)
{
  sqlite3_stmt *stmt;
  ////////////////////////////// db_info
  sqlite3_exec(db->handle, "CREATE TABLE main.db_info (key VARCHAR PRIMARY KEY, value VARCHAR)", NULL,
               NULL, NULL);
  sqlite3_prepare_v2(
      db->handle, "INSERT OR REPLACE INTO main.db_info (key, value) VALUES ('version', ?1)", -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, CURRENT_DATABASE_VERSION_LIBRARY);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  ////////////////////////////// film_rolls
  sqlite3_exec(db->handle,
               "CREATE TABLE main.film_rolls "
               "(id INTEGER PRIMARY KEY, access_timestamp INTEGER, "
               //                        "folder VARCHAR(1024), external_drive VARCHAR(1024))", //
               //                        FIXME: make sure to bump CURRENT_DATABASE_VERSION_LIBRARY and add a
               //                        case to _upgrade_library_schema_step when adding this!
               "folder VARCHAR(1024) NOT NULL)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.film_rolls_folder_index ON film_rolls (folder)", NULL, NULL, NULL);
  ////////////////////////////// images
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

  ////////////////////////////// selected_images
  sqlite3_exec(db->handle, "CREATE TABLE main.selected_images (imgid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  ////////////////////////////// history
  sqlite3_exec(
      db->handle,
      "CREATE TABLE main.history (imgid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
      NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.history_imgid_index ON history (imgid)", NULL, NULL, NULL);
  ////////////////////////////// masks history
  sqlite3_exec(db->handle,
               "CREATE TABLE main.masks_history (imgid INTEGER, num INTEGER, formid INTEGER, form INTEGER, name VARCHAR(256), "
               "version INTEGER, points BLOB, points_count INTEGER, source BLOB)",
               NULL, NULL, NULL);

  sqlite3_exec(db->handle,
      "CREATE INDEX main.masks_history_imgid_index ON masks_history (imgid)",
      NULL, NULL, NULL);

  ////////////////////////////// tagged_images
  sqlite3_exec(db->handle, "CREATE TABLE main.tagged_images (imgid INTEGER, tagid INTEGER, position INTEGER, "
                           "PRIMARY KEY (imgid, tagid))", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.tagged_images_tagid_index ON tagged_images (tagid)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.tagged_images_imgid_index ON selected_images (imgid)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.tagged_images_position_index ON selected_images (position)", NULL, NULL, NULL);
  ////////////////////////////// color_labels
  sqlite3_exec(db->handle, "CREATE TABLE main.color_labels (imgid INTEGER, color INTEGER)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX main.color_labels_idx ON color_labels (imgid, color)", NULL, NULL,
               NULL);
  ////////////////////////////// meta_data
  sqlite3_exec(db->handle, "CREATE TABLE main.meta_data (id INTEGER, key INTEGER, value VARCHAR)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.metadata_index ON meta_data (id, key)", NULL, NULL, NULL);

  sqlite3_exec(db->handle, "CREATE TABLE main.module_order (imgid INTEGER PRIMARY KEY, version INTEGER, iop_list VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE main.history_hash (imgid INTEGER PRIMARY KEY, "
               "basic_hash BLOB, auto_hash BLOB, current_hash BLOB, mipmap_hash BLOB)",
               NULL, NULL, NULL);
}

/* create the current database schema and set the version in db_info accordingly */
static void _create_data_schema(dt_database_t *db)
{
  sqlite3_stmt *stmt;
  ////////////////////////////// db_info
  sqlite3_exec(db->handle, "CREATE TABLE data.db_info (key VARCHAR PRIMARY KEY, value VARCHAR)", NULL,
               NULL, NULL);
  sqlite3_prepare_v2(
        db->handle, "INSERT OR REPLACE INTO data.db_info (key, value) VALUES ('version', ?1)", -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, CURRENT_DATABASE_VERSION_DATA);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  ////////////////////////////// tags
  sqlite3_exec(db->handle, "CREATE TABLE data.tags (id INTEGER PRIMARY KEY, name VARCHAR, "
                           "synonyms VARCHAR, flags INTEGER)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX data.tags_name_idx ON tags (name)", NULL, NULL, NULL);
  ////////////////////////////// styles
  sqlite3_exec(db->handle, "CREATE TABLE data.styles (id INTEGER PRIMARY KEY, name VARCHAR, description VARCHAR, iop_list VARCHAR)",
                        NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX data.styles_name_index ON styles (name)", NULL, NULL, NULL);
  ////////////////////////////// style_items
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
  ////////////////////////////// presets
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

  /* first let's get rid of non-utf8 tags. */
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
      {
        pid_is_alive = FALSE;
      }
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
  {
    lock_acquired = TRUE;
  }
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
    if(!copyStatus) fprintf(stderr, "[backup failed] %s -> %s\n", filename, backup);
  }

  g_free(version);
  g_free(backup);
}

dt_database_t *dt_database_init(const char *alternative, const gboolean load_data, const gboolean has_gui)
{
  /*  set the threading mode to Serialized */
  sqlite3_config(SQLITE_CONFIG_SERIALIZED);

  sqlite3_initialize();

start:
  if(alternative == NULL)
  {
    /* migrate default database location to new default */
    _database_migrate_to_xdg_structure();
  }

  /* delete old mipmaps files */
  _database_delete_mipmaps_files();

  /* lets construct the db filename  */
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

  /* we also need a 2nd db with permanent data like presets, styles and tags */
  char dbfilename_data[PATH_MAX] = { 0 };
  if(load_data)
    snprintf(dbfilename_data, sizeof(dbfilename_data), "%s/data.db", datadir);
  else
    snprintf(dbfilename_data, sizeof(dbfilename_data), ":memory:");

  /* create database */
  dt_database_t *db = (dt_database_t *)g_malloc0(sizeof(dt_database_t));
  db->dbfilename_data = g_strdup(dbfilename_data);
  db->dbfilename_library = g_strdup(dbfilename_library);

  /* make sure the folder exists. this might not be the case for new databases */
  /* also check if a database backup is needed */
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

  /* having more than one instance of darktable using the same database is a bad idea */
  /* try to get locks for the databases */
  db->lock_acquired = _lock_databases(db);

  if(!db->lock_acquired)
  {
    fprintf(stderr, "[init] database is locked, probably another process is already using it\n");
    g_free(dbname);
    return db;
  }


  /* opening / creating database */
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

  /* attach a memory database to db connection for use with temporary tables
     used during instance life time, which is discarded on exit.
  */
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

  /* now that we got functional databases that are locked for us we can make sure that the schema is set up */

  // first we update the data database to the latest version so that we can potentially move data from the library
  // over when updating that one
  if(!have_data_db)
  {
    _create_data_schema(db); // a brand new db it seems
  }
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

  // create a table legacy_presets with all the presets from pre-auto-apply-cleanup darktable.
  dt_legacy_presets_create(db);

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

/* delete old mipmaps files */
static void _database_delete_mipmaps_files()
{
  /* This migration is intended to be run only from 0.9.x to new cache in 1.0 */

  // Directory
  char cachedir[PATH_MAX] = { 0 }, mipmapfilename[PATH_MAX] = { 0 };
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

  snprintf(mipmapfilename, sizeof(mipmapfilename), "%s/mipmaps", cachedir);

  if(access(mipmapfilename, F_OK) != -1)
  {
    fprintf(stderr, "[mipmap_cache] dropping old version file: %s\n", mipmapfilename);
    g_unlink(mipmapfilename);

    snprintf(mipmapfilename, sizeof(mipmapfilename), "%s/mipmaps.fallback", cachedir);

    if(access(mipmapfilename, F_OK) != -1) g_unlink(mipmapfilename);
  }
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
  {
    val = sqlite3_column_int(stmt, 0);
  }
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
  {
    dt_print(DT_DEBUG_SQL, "[db maintenance] paintenance problem. if no errors logged, it should work fine next time.\n");
  }
  else
  {
    
  }
}
#undef ERRCHECK

gboolean _ask_for_maintenance(const gboolean has_gui, const gboolean closing_time, const guint64 size)
{
  if(!has_gui)
  {
    return FALSE;
  }

  char *later_info = NULL;
  char *size_info = g_format_size(size);
  char *config = dt_conf_get_string("database/maintenance_check");
  if((closing_time && (!g_strcmp0(config, "on both"))) || !g_strcmp0(config, "on startup"))
  {
    later_info = _("click later to be asked on next startup");
  }
  else if (!closing_time && (!g_strcmp0(config, "on both")))
  {
    later_info = _("click later to be asked when closing darktable");
  }
  else if (!g_strcmp0(config, "on close"))
  {
    later_info = _("click later to be asked next time when closing darktable");
  }


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
  {
    return;
  }

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
    {
      _dt_database_maintenance(db);
    }
  }
}

void dt_database_optimize(const struct dt_database_t *db)
{
  // optimize should in most cases be no-op and have no noticeable downsides
  // this should be ran on every exit
  // see: https://www.sqlite.org/pragma.html#pragma_optimize
  DT_DEBUG_SQLITE3_EXEC(db->handle, "PRAGMA optimize", NULL, NULL, NULL);
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
