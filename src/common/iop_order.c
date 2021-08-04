/*
    This file is part of darktable,
    Copyright (C) 2018-2020 darktable developers.

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

#include "common/darktable.h"
#include "common/iop_order.h"
#include "common/styles.h"
#include "common/debug.h"
#include "develop/imageop.h"
#include "develop/pixelpipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void _ioppr_reset_iop_order(GList *iop_order_list);

/** Note :
 * we do not use finite-math-only and fast-math because divisions by zero are not manually avoided in the code
 * fp-contract=fast enables hardware-accelerated Fused Multiply-Add
 * the rest is loop reorganization and vectorization optimization
 **/
#if defined(__GNUC__)
#pragma GCC optimize ("unroll-loops", "tree-loop-if-convert", \
                      "tree-loop-distribution", "no-strict-aliasing", \
                      "loop-interchange", "loop-nest-optimize", "tree-loop-im", \
                      "unswitch-loops", "tree-loop-ivcanon", "ira-loop-pressure", \
                      "split-ivs-in-unroller", "variable-expansion-in-unroller", \
                      "split-loops", "ivopts", "predictive-commoning",\
                      "tree-loop-linear", "loop-block", "loop-strip-mine", \
                      "fp-contract=fast", \
                      "tree-vectorize")
#endif

const char *iop_order_string[] =
{
  N_("custom"),
  N_("legacy"),
  N_("v3.0")
};

const char *dt_iop_order_string(const dt_iop_order_t order)
{
  if(order >= DT_IOP_ORDER_LAST)
    return "???";
  else
    return iop_order_string[order];
}

// note legacy_order & v30_order have the original iop-order double that is
// used only for the initial database migration.
//
// in the new code only the iop-order as int is used to order the module on the GUI.

// @@_NEW_MOUDLE: For new module it is required to insert the new module name in both lists below.

const dt_iop_order_entry_t v30_order[] = {
  { { 1.0f }, "rawprepare", 0},
  { { 2.0f }, "temperature", 0},
  { { 3.0f }, "highlights", 0},
  { { 4.0f }, "hotpixels", 0},
  { { 5.0f }, "demosaic", 0},
  { { 6.0f }, "rotatepixels", 0},
  { { 7.0f }, "scalepixels", 0},
  { { 8.0f }, "lens", 0},
  { { 9.0f }, "hazeremoval", 0},
  { {10.0f }, "ashift", 0},
  { {11.0f }, "flip", 0},
  { {12.0f }, "clipping", 0},
  { {13.0f }, "spots", 0},
  { {14.0f }, "exposure", 0},
  { {15.0f }, "mask_manager", 0},
  { {16.0f }, "negadoctor", 0},
  { {17.0f }, "colorin", 0},
  { {18.0f }, "channelmixer", 0},    // user defined RGB to RGB matrix conversion
  { {19.0f }, "basecurve", 0},
  { {20.0f }, "tonecurve", 0},
  { {21.0f }, "colorcorrection", 0},
  { {22.0f }, "vibrance", 0},        // gray remains fixed, extra color is added
  { {23.0f }, "grain", 0},
  { {24.0f }, "splittoning", 0},
  { {25.0f }, "vignette", 0},
  { {26.0f }, "colorout", 0},
  { {27.0f }, "finalscale", 0},
  { {28.0f }, "overexposed", 0},
  { {29.0f }, "borders", 0},
  { {30.0f }, "gamma", 0},
  { { 0.0f }, "", 0 }
};

static void *_dup_iop_order_entry(const void *src, gpointer data);
static int _count_entries_operation(GList *e_list, const char *operation);


static GList *_insert_before(GList *iop_order_list, const char *module, const char *new_module)
{
  gboolean exists = FALSE;
  // first check that new module is missing
  GList *l = iop_order_list;

  while(l)
  {
    const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)l->data;
    if(!strcmp(entry->operation, new_module))
    {
      exists = TRUE;
      break;
    }
    l = g_list_next(l);
  }
  // the insert it if needed
  if(!exists)
  {
    l = iop_order_list;
    while(l)
    {
      const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)l->data;

      if(!strcmp(entry->operation, module))
      {
        dt_iop_order_entry_t *new_entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));

        g_strlcpy(new_entry->operation, new_module, sizeof(new_entry->operation));
        new_entry->instance = 0;
        new_entry->o.iop_order = 0;

        iop_order_list = g_list_insert_before(iop_order_list, l, new_entry);
        break;
      }
      l = g_list_next(l);
    }
  }

  return iop_order_list;
}

dt_iop_order_t dt_ioppr_get_iop_order_version(const int32_t imgid)
{
  dt_iop_order_t iop_order_version = DT_IOP_ORDER_V30;
  // check current iop order version
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT version FROM main.module_order WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  
  if(sqlite3_step(stmt) == SQLITE_ROW)
    iop_order_version = sqlite3_column_int(stmt, 0);
    
  sqlite3_finalize(stmt);
  return iop_order_version;
}

// a rule prevents operations to be switched,
// that is a prev operation will not be allowed to be moved on top of the next operation.
GList *dt_ioppr_get_iop_order_rules()
{
  GList *rules = NULL;

  const dt_iop_order_rule_t rule_entry[] = {
    { .op_prev = "rawprepare",  .op_next = "invert"      },
    { .op_prev = "temperature", .op_next = "highlights"  },
    { .op_prev = "hotpixels",   .op_next = "rawdenoise"  },
    { .op_prev = "rawdenoise",  .op_next = "demosaic"    },
    { .op_prev = "demosaic",    .op_next = "colorin"     },
    { .op_prev = "colorin",     .op_next = "colorout"    },
    { .op_prev = "colorout",    .op_next = "gamma"       },
    { .op_prev = "flip",        .op_next = "clipping"    }, // clipping GUI broken if flip is done on top
    { .op_prev = "ashift",      .op_next = "clipping"    }, // clipping GUI broken if ashift is done on top
    { "\0", "\0" } };

  int i = 0;
  while(rule_entry[i].op_prev[0])
  {
    dt_iop_order_rule_t *rule = calloc(1, sizeof(dt_iop_order_rule_t));

    memcpy(rule->op_prev, rule_entry[i].op_prev, sizeof(rule->op_prev));
    memcpy(rule->op_next, rule_entry[i].op_next, sizeof(rule->op_next));

    rules = g_list_append(rules, rule);
    i++;
  }

  return rules;
}

GList *dt_ioppr_get_iop_order_link(GList *iop_order_list, const char *op_name, const int multi_priority)
{
  GList *link = NULL;

  GList *iops_order = iop_order_list;
  while(iops_order)
  {
    dt_iop_order_entry_t *order_entry = (dt_iop_order_entry_t *)iops_order->data;

    if(strcmp(order_entry->operation, op_name) == 0
       && (order_entry->instance == multi_priority || multi_priority == -1))
    {
      link = iops_order;
      break;
    }

    iops_order = g_list_next(iops_order);
  }

  return link;
}

// returns the first iop order entry that matches operation == op_name
dt_iop_order_entry_t *dt_ioppr_get_iop_order_entry(GList *iop_order_list, const char *op_name, const int multi_priority)
{
  const GList * const restrict link = dt_ioppr_get_iop_order_link(iop_order_list, op_name, multi_priority);
  if(link)
    return (dt_iop_order_entry_t *)link->data;
  else
    return NULL;
}

// returns the iop_order associated with the iop order entry that matches operation == op_name
int dt_ioppr_get_iop_order(GList *iop_order_list, const char *op_name, const int multi_priority)
{
  int iop_order = INT_MAX;
  const dt_iop_order_entry_t *const restrict order_entry =
    dt_ioppr_get_iop_order_entry(iop_order_list, op_name, multi_priority);

  if(order_entry)
    iop_order = order_entry->o.iop_order;
  else
    fprintf(stderr, "cannot get iop-order for %s instance %d\n", op_name, multi_priority);

  return iop_order;
}

gboolean dt_ioppr_is_iop_before(GList *iop_order_list, const char *base_operation,
                                const char *operation, const int multi_priority)
{
  const int base_order = dt_ioppr_get_iop_order(iop_order_list, base_operation, -1);
  const int op_order = dt_ioppr_get_iop_order(iop_order_list, operation, multi_priority);
  return op_order < base_order;
}

gint dt_sort_iop_list_by_order(gconstpointer a, gconstpointer b)
{
  const dt_iop_order_entry_t *const restrict am = (const dt_iop_order_entry_t *)a;
  const dt_iop_order_entry_t *const restrict bm = (const dt_iop_order_entry_t *)b;
  if(am->o.iop_order > bm->o.iop_order) return 1;
  if(am->o.iop_order < bm->o.iop_order) return -1;
  return 0;
}

gint dt_sort_iop_list_by_order_f(gconstpointer a, gconstpointer b)
{
  const dt_iop_order_entry_t *const restrict am = (const dt_iop_order_entry_t *)a;
  const dt_iop_order_entry_t *const restrict bm = (const dt_iop_order_entry_t *)b;
  if(am->o.iop_order_f > bm->o.iop_order_f) return 1;
  if(am->o.iop_order_f < bm->o.iop_order_f) return -1;
  return 0;
}

dt_iop_order_t dt_ioppr_get_iop_order_list_kind(GList *iop_order_list)
{
  // first check if this is the v30 order
  int k = 0;
  GList *l = iop_order_list;
  gboolean ok = TRUE;

  while(l)
  {
    const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)l->data;
    if(strcmp(v30_order[k].operation, entry->operation))
    {
      ok = FALSE;
      break;
    }
    else
    {
      // skip all the other instance of same module if any
      while(g_list_next(l)
            && !strcmp(v30_order[k].operation, ((dt_iop_order_entry_t *)(g_list_next(l)->data))->operation))
        l = g_list_next(l);
    }

    k++;
    l = g_list_next(l);
  }

  if(ok)
    return DT_IOP_ORDER_V30;

  return DT_IOP_ORDER_CUSTOM;
}

gboolean dt_ioppr_has_multiple_instances(GList *iop_order_list)
{
  GList *l = iop_order_list;

  while(l)
  {
    GList *next = g_list_next(l);
    if(next
       && (strcmp(((dt_iop_order_entry_t *)(l->data))->operation,
                  ((dt_iop_order_entry_t *)(next->data))->operation) == 0))
    {
      return TRUE;
    }
    l = next;
  }
  return FALSE;
}

gboolean dt_ioppr_write_iop_order(const dt_iop_order_t kind, GList *iop_order_list, const int32_t imgid)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT OR REPLACE INTO main.module_order VALUES (?1, 0, NULL)", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) != SQLITE_DONE) return FALSE;
  sqlite3_finalize(stmt);

  if(kind == DT_IOP_ORDER_CUSTOM || dt_ioppr_has_multiple_instances(iop_order_list))
  {
    gchar *iop_list_txt = dt_ioppr_serialize_text_iop_order_list(iop_order_list);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE main.module_order SET version = ?2, iop_list = ?3 WHERE imgid = ?1", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, kind);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, iop_list_txt, -1, SQLITE_TRANSIENT);

    if(sqlite3_step(stmt) != SQLITE_DONE)
      return FALSE;

    sqlite3_finalize(stmt);
    g_free(iop_list_txt);
  }
  else
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE main.module_order SET version = ?2, iop_list = NULL WHERE imgid = ?1", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, kind);
    if(sqlite3_step(stmt) != SQLITE_DONE) return FALSE;
    sqlite3_finalize(stmt);
  }

  return TRUE;
}

gboolean dt_ioppr_write_iop_order_list(GList *iop_order_list, const int32_t imgid)
{
  const dt_iop_order_t kind = dt_ioppr_get_iop_order_list_kind(iop_order_list);
  return dt_ioppr_write_iop_order(kind, iop_order_list, imgid);
}

GList *_table_to_list(const dt_iop_order_entry_t entries[])
{
  GList *iop_order_list = NULL;
  int k = 0;
  while(entries[k].operation[0])
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));

    g_strlcpy(entry->operation, entries[k].operation, sizeof(entry->operation));
    entry->instance = 0;
    entry->o.iop_order_f = entries[k].o.iop_order_f;
    iop_order_list = g_list_append(iop_order_list, entry);

    k++;
  }

  return iop_order_list;
}

GList *dt_ioppr_get_iop_order_list_version(dt_iop_order_t version)
{
  GList *iop_order_list = NULL;

  if(version == DT_IOP_ORDER_V30)
    iop_order_list = _table_to_list(v30_order);

  return iop_order_list;
}

GList *dt_ioppr_get_iop_order_list(int32_t imgid, gboolean sorted)
{
  GList *iop_order_list = NULL;

  if(imgid > 0)
  {
    sqlite3_stmt *stmt;

    // we read the iop-order-list in the preset table, the actual version is
    // the first int32_t serialized into the io_params. This is then a sequential
    // search, but there will not be many such presets and we do call this routine
    // only when loading an image and when changing the iop-order.

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT version, iop_list"
                                " FROM main.module_order"
                                " WHERE imgid=?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const dt_iop_order_t version = sqlite3_column_int(stmt, 0);
      const gboolean has_iop_list = (sqlite3_column_type(stmt, 1) != SQLITE_NULL);

      if(version == DT_IOP_ORDER_CUSTOM || has_iop_list)
      {
        const char *buf = (char *)sqlite3_column_text(stmt, 1);
        if(buf)
          iop_order_list = dt_ioppr_deserialize_text_iop_order_list(buf);

        if(!iop_order_list)
          // preset not found, fall back to last built-in version, will be loaded below
          fprintf(stderr, "[dt_ioppr_get_iop_order_list] error building iop_order_list imgid %d\n", imgid);
        else
          // @@_NEW_MOUDLE: For new module it is required to insert the new module name in the iop-order list here.
          //                The insertion can be done depending on the current iop-order list kind.
          _insert_before(iop_order_list, "nlmeans", "negadoctor");
      }
      else if(version == DT_IOP_ORDER_V30)
        iop_order_list = _table_to_list(v30_order);
      else
        fprintf(stderr, "[dt_ioppr_get_iop_order_list] invalid iop order version %d for imgid %d\n", version, imgid);

      if(iop_order_list)
        _ioppr_reset_iop_order(iop_order_list);
    }

    sqlite3_finalize(stmt);
  }

  // fallback to last iop order list (also used to initialize the pipe when imgid = 0)
  // and new image not yet loaded or whose history has been reset.
  if(!iop_order_list)
    iop_order_list = _table_to_list(v30_order);
  
  if(sorted)
    iop_order_list = g_list_sort(iop_order_list, dt_sort_iop_list_by_order);

  return iop_order_list;
}

static void _ioppr_reset_iop_order(GList *iop_order_list)
{
  // iop-order must start with a number > 0 and be incremented. There is no
  // other constraints.
  int iop_order = 1;
  GList *l = iop_order_list;

  while(l)
  {
    dt_iop_order_entry_t *e = (dt_iop_order_entry_t *)l->data;
    e->o.iop_order = iop_order++;
    l = g_list_next(l);
  }
}

void dt_ioppr_resync_iop_list(dt_develop_t *dev)
{
  // make sure that the iop_order_list does not contains possibly removed modules

  GList *l = g_list_first(dev->iop_order_list);
  while(l)
  {
    GList *next = g_list_next(l);
    const dt_iop_order_entry_t *const restrict e = (dt_iop_order_entry_t *)l->data;
    const dt_iop_module_t *const restrict mod = dt_iop_get_module_by_op_priority(dev->iop, e->operation, e->instance);
    if(mod == NULL)
      dev->iop_order_list = g_list_remove_link(dev->iop_order_list, l);

    l = next;
  }
}

void dt_ioppr_resync_modules_order(dt_develop_t *dev)
{
  _ioppr_reset_iop_order(dev->iop_order_list);
  // and reset all module iop_order
  GList *modules = g_list_first(dev->iop);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);
    GList *next = g_list_next(modules);

    // modules with iop_order set to INT_MAX we keep them as they will be removed (non visible)
    // _lib_modulegroups_update_iop_visibility.
    if(mod->iop_order != INT_MAX)
      mod->iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, mod->op, mod->multi_priority);

    modules = next;
  }

  dev->iop = g_list_sort(dev->iop, dt_sort_iop_by_order);
}

// sets the iop_order on each module of *_iop_list
// iop_order is set only for base modules, multi-instances will be flagged as unused with INT_MAX
// if a module do not exists on iop_order_list it is flagged as unused with INT_MAX
void dt_ioppr_set_default_iop_order(dt_develop_t *dev, const int32_t imgid)
{
  // get the iop-order for this image
  GList *iop_order_list = dt_ioppr_get_iop_order_list(imgid, FALSE);
  // we assign a single iop-order to each module
  _ioppr_reset_iop_order(iop_order_list);

  if(dev->iop_order_list)
    g_list_free_full(dev->iop_order_list, free);

  dev->iop_order_list = iop_order_list;
  // we now set the module list given to this iop-order
  dt_ioppr_resync_modules_order(dev);
}

void dt_ioppr_migrate_iop_order(struct dt_develop_t *dev, const int32_t imgid)
{
  dt_ioppr_set_default_iop_order(dev, imgid);
  dt_dev_reload_history_items(dev);
}

void dt_ioppr_change_iop_order(struct dt_develop_t *dev, const int32_t imgid, GList *new_iop_list)
{
  GList *iop_list = dt_ioppr_iop_order_copy_deep(new_iop_list);
  GList *mi = dt_ioppr_extract_multi_instances_list(darktable.develop->iop_order_list);

  if(mi)
    iop_list = dt_ioppr_merge_multi_instance_iop_order_list(iop_list, mi);

  dt_dev_write_history(darktable.develop);
  dt_ioppr_write_iop_order(DT_IOP_ORDER_CUSTOM, iop_list, imgid);
  g_list_free_full(iop_list, free);
  dt_ioppr_migrate_iop_order(darktable.develop, imgid);
}

GList *dt_ioppr_extract_multi_instances_list(GList *iop_order_list)
{
  GList *mi = NULL;
  GList *l = g_list_first(iop_order_list);

  while(l)
  {
    const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)l->data;

    if(_count_entries_operation(iop_order_list, entry->operation) > 1)
    {
      dt_iop_order_entry_t *copy = (dt_iop_order_entry_t *)_dup_iop_order_entry((void *)entry, NULL);
      mi = g_list_append(mi, copy);
    }

    l = g_list_next(l);
  }

  return mi;
}

GList *dt_ioppr_merge_module_multi_instance_iop_order_list(GList *iop_order_list,
                                                           const char *operation, GList *multi_instance_list)
{
  const int count_to = _count_entries_operation(iop_order_list, operation);
  const int count_from = g_list_length(multi_instance_list);
  int item_nb = 0;
  GList *link = g_list_first(iop_order_list);
  GList *l = g_list_first(multi_instance_list);

  while(l)
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)l->data;
    item_nb++;

    if(item_nb <= count_to)
    {
      link = dt_ioppr_get_iop_order_link(link, operation, -1);
      dt_iop_order_entry_t *e = (dt_iop_order_entry_t *)link->data;
      e->instance = entry->instance;
      // free this entry as not merged into the list
      free(entry);
      // next replace should happen to any module after this one
      link = g_list_next(link);
    }
    else
      iop_order_list = g_list_insert_before(iop_order_list, link, entry);

    l= g_list_next(l);
  }

  // if needed removes all other instance of this operation which are superfluous
  if(count_from < count_to)
  {
    while(link)
    {
      const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)link->data;
      GList *next = g_list_next(link);

      if(strcmp(operation, entry->operation) == 0)
        iop_order_list = g_list_remove_link(iop_order_list, link);

      link = next;
    }
  }

  return iop_order_list;
}

GList *dt_ioppr_merge_multi_instance_iop_order_list(GList *iop_order_list, GList *multi_instance_list)
{
  GList *op = NULL;
  GList *copy = dt_ioppr_iop_order_copy_deep(multi_instance_list);
  GList *l = g_list_first(copy);

  while(l)
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)l->data;
    GList *l_next = g_list_next(l);
    op = g_list_append(op, entry);
    copy = g_list_remove_link(copy, l);

    GList *mi = l_next;
    while(mi)
    {
      GList *next = g_list_next(mi);
      dt_iop_order_entry_t *mi_entry = (dt_iop_order_entry_t *)mi->data;
      if(strcmp(entry->operation, mi_entry->operation) == 0)
      {
        op = g_list_append(op, mi_entry);
        copy = g_list_remove_link(copy, mi);
      }

      mi = next;
    }

    // copy operation as entry may be freed
    char operation[20];
    memcpy(operation, entry->operation, sizeof(entry->operation));
    iop_order_list = dt_ioppr_merge_module_multi_instance_iop_order_list(iop_order_list, operation, op);
    g_list_free(op);
    op = NULL;

    l = g_list_first(copy);
  }

  return iop_order_list;
}

static void _count_iop_module(GList *iop, const char *operation, int *max_multi_priority, int *count,
                              int *max_multi_priority_enabled, int *count_enabled)
{
  *max_multi_priority = 0;
  *count = 0;
  *max_multi_priority_enabled = 0;
  *count_enabled = 0;

  GList *modules = iop;
  while(modules)
  {
    const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;
    if(!strcmp(mod->op, operation))
    {
      (*count)++;

      if(*max_multi_priority < mod->multi_priority)
        *max_multi_priority = mod->multi_priority;

      if(mod->enabled)
      {
        (*count_enabled)++;

        if(*max_multi_priority_enabled < mod->multi_priority)
          *max_multi_priority_enabled = mod->multi_priority;
      }
    }
    modules = g_list_next(modules);
  }

  assert(*count >= *count_enabled);
}

static int _count_entries_operation(GList *e_list, const char *operation)
{
  int count = 0;

  GList *l = g_list_first(e_list);
  while(l)
  {
    dt_iop_order_entry_t *ep = (dt_iop_order_entry_t *)l->data;
    if(!strcmp(ep->operation, operation)) count++;
    l = g_list_next(l);
  }

  return count;
}

static gboolean _operation_already_handled(GList *e_list, const char *operation)
{
  GList *l = g_list_previous(e_list);

  while(l)
  {
    const dt_iop_order_entry_t *const restrict ep = (dt_iop_order_entry_t *)l->data;
    if(!strcmp(ep->operation, operation)) return TRUE;
    l = g_list_previous(l);
  }
  return FALSE;
}

// returns the nth module's priority being active or not
int _get_multi_priority(dt_develop_t *dev, const char *operation, const int n, const gboolean only_disabled)
{
  GList *l = dev->iop;
  int count = 0;
  while(l)
  {
    const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)l->data;
    if((!only_disabled || mod->enabled == FALSE) && !strcmp(mod->op, operation))
    {
      count++;
      if(count == n) return mod->multi_priority;
    }

    l = g_list_next(l);
  }

  return INT_MAX;
}

void dt_ioppr_update_for_entries(dt_develop_t *dev, GList *entry_list, gboolean append)
{
  GList *e_list = entry_list;

  // for each priority list to be checked
  while(e_list)
  {
    const dt_iop_order_entry_t *const restrict ep = (dt_iop_order_entry_t *)e_list->data;
    gboolean force_append = FALSE;

    // we also need to force append (even if overwrite mode is
    // selected - append = FALSE) when a module has a specific name
    // and this name is not present into the current iop list.

    if(*ep->name && !dt_iop_get_module_by_instance_name(dev->iop, ep->operation, ep->name))
      force_append = TRUE;

    int max_multi_priority = 0, count = 0;
    int max_multi_priority_enabled = 0, count_enabled = 0;
    // is it a currently active module and if so how many active instances we have
    _count_iop_module(dev->iop, ep->operation,
                      &max_multi_priority, &count, &max_multi_priority_enabled, &count_enabled);

    // look for this operation into the target iop-order list and add there as much operation as needed
    GList *l = g_list_last(dev->iop_order_list);

    while(l)
    {
      const dt_iop_order_entry_t *const restrict e = (dt_iop_order_entry_t *)l->data;
      if(!strcmp(e->operation, ep->operation) && !_operation_already_handled(e_list, ep->operation))
      {
        // how many instances of this module in the entry list, and re-number multi-priority accordingly
        const int new_active_instances = _count_entries_operation(entry_list, ep->operation);
        int add_count = 0;
        int start_multi_priority = 0;
        int nb_replace = 0;

        if(append || force_append)
        {
          nb_replace = count - count_enabled;
          add_count = MAX(0, new_active_instances - nb_replace);
          start_multi_priority = max_multi_priority + 1;
        }
        else
        {
          nb_replace = count;
          add_count = MAX(0, new_active_instances - count);
          start_multi_priority = max_multi_priority + 1;
        }

        // update multi_priority to be unique in iop list
        int multi_priority = start_multi_priority;
        int nb = 0;

        GList *s = entry_list;
        while(s)
        {
          dt_iop_order_entry_t *item = (dt_iop_order_entry_t *)s->data;
          if(!strcmp(item->operation, e->operation))
          {
            nb++;
            if(nb <= nb_replace)
              // this one replaces current module, get it's multi-priority
              item->instance = _get_multi_priority(dev, item->operation, nb, append);
            else
              // otherwise create a new multi-priority
              item->instance = multi_priority++;
          }
          s = g_list_next(s);
        }

        multi_priority = start_multi_priority;

        l = g_list_next(l);

        for(int k = 0; k<add_count; k++)
        {
          dt_iop_order_entry_t *n = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
          g_strlcpy(n->operation, ep->operation, sizeof(n->operation));
          n->instance = multi_priority++;
          n->o.iop_order = 0;
          dev->iop_order_list = g_list_insert_before(dev->iop_order_list, l, n);
        }
        break;
      }

      l = g_list_previous(l);
    }

    e_list = g_list_next(e_list);
  }

  _ioppr_reset_iop_order(dev->iop_order_list);

//  dt_ioppr_print_iop_order(dev->iop_order_list, "upd sitem");
}

void dt_ioppr_update_for_style_items(dt_develop_t *dev, GList *st_items, gboolean append)
{
  GList *si_list = g_list_first(st_items);
  GList *e_list = NULL;

  // for each priority list to be checked
  while(si_list)
  {
    const dt_style_item_t *const restrict si = (dt_style_item_t *)si_list->data;

    dt_iop_order_entry_t *n = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
    memcpy(n->operation, si->operation, sizeof(n->operation));
    n->instance = si->multi_priority;
    g_strlcpy(n->name, si->multi_name, sizeof(n->name));
    n->o.iop_order = 0;
    e_list = g_list_append(e_list, n);

    si_list = g_list_next(si_list);
  }

  dt_ioppr_update_for_entries(dev, e_list, append);

  // write back the multi-priority

  si_list = g_list_first(st_items);
  GList *el = g_list_first(e_list);
  while(si_list)
  {
    dt_style_item_t *si = (dt_style_item_t *)si_list->data;
    const dt_iop_order_entry_t *const restrict e = (dt_iop_order_entry_t *)el->data;

    si->multi_priority = e->instance;
    si->iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, si->operation, si->multi_priority);

    el = g_list_next(el);
    si_list = g_list_next(si_list);
  }

  g_list_free(e_list);
}

void dt_ioppr_update_for_modules(dt_develop_t *dev, GList *modules, gboolean append)
{
  GList *m_list = g_list_first(modules);
  GList *e_list = NULL;

  // for each priority list to be checked
  while(m_list)
  {
    const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)m_list->data;

    dt_iop_order_entry_t *n = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
    g_strlcpy(n->operation, mod->op, sizeof(n->operation));
    n->instance = mod->multi_priority;
    g_strlcpy(n->name, mod->multi_name, sizeof(n->name));
    n->o.iop_order = 0;
    e_list = g_list_append(e_list, n);

    m_list = g_list_next(m_list);
  }

  dt_ioppr_update_for_entries(dev, e_list, append);

  // write back the multi-priority

  m_list = g_list_first(modules);
  GList *el = g_list_first(e_list);
  while(m_list)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)m_list->data;
    dt_iop_order_entry_t *e = (dt_iop_order_entry_t *)el->data;

    mod->multi_priority = e->instance;
    mod->iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, mod->op, mod->multi_priority);

    el = g_list_next(el);
    m_list = g_list_next(m_list);
  }

  g_list_free_full(e_list, free);
}

// returns the first dt_dev_history_item_t on history_list where hist->module == mod
static dt_dev_history_item_t *_ioppr_search_history_by_module(GList *history_list, dt_iop_module_t *mod)
{
  dt_dev_history_item_t *hist_entry = NULL;

  GList *history = g_list_first(history_list);
  while(history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);

    if(hist->module == mod)
    {
      hist_entry = hist;
      break;
    }

    history = g_list_next(history);
  }

  return hist_entry;
}

// check if there's duplicate iop_order entries in iop_list
// if so, updates the iop_order to be unique, but only if the module is disabled and not in history
void dt_ioppr_check_duplicate_iop_order(GList **_iop_list, GList *history_list)
{
  GList *iop_list = *_iop_list;
  dt_iop_module_t *mod_prev = NULL;

  // get the first module
  GList *modules = g_list_first(iop_list);
  if(modules)
  {
    mod_prev = (dt_iop_module_t *)(modules->data);
    modules = g_list_next(modules);
  }
  // check for each module if iop_order is the same as the previous one
  // if so, change it, but only if disabled and not in history
  while(modules)
  {
    int reset_list = 0;
    dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);

    if(mod->iop_order == mod_prev->iop_order && mod->iop_order != INT_MAX)
    {
      int can_move = 0;

      if(!mod->enabled && _ioppr_search_history_by_module(history_list, mod) == NULL)
      {
        can_move = 1;
        GList *modules1 = g_list_next(modules);

        if(modules1)
        {
          dt_iop_module_t *mod_next = (dt_iop_module_t *)(modules1->data);

          if(mod->iop_order != mod_next->iop_order)
            mod->iop_order += (mod_next->iop_order - mod->iop_order) / 2.0;
          else
          {
            dt_ioppr_check_duplicate_iop_order(&modules, history_list);
            reset_list = 1;
          }
        }
        else
          mod->iop_order += 1.0;
      }
      else if(!mod_prev->enabled && _ioppr_search_history_by_module(history_list, mod_prev) == NULL)
      {
        can_move = 1;
        GList *modules1 = g_list_previous(modules);

        if(modules1)
          modules1 = g_list_previous(modules1);

        if(modules1)
        {
          dt_iop_module_t *mod_next = (dt_iop_module_t *)(modules1->data);
          if(mod_prev->iop_order != mod_next->iop_order)
            mod_prev->iop_order -= (mod_prev->iop_order - mod_next->iop_order) / 2.0;
          else
          {
            can_move = 0;
            fprintf(stderr,
                    "[dt_ioppr_check_duplicate_iop_order 1] modules %s %s(%d) and %s %s(%d) have the same iop_order\n",
                    mod_prev->op, mod_prev->multi_name, mod_prev->iop_order, mod->op, mod->multi_name, mod->iop_order);
          }
        }
        else
          mod_prev->iop_order -= 0.5;
      }

      if(!can_move)
        fprintf(stderr,
                "[dt_ioppr_check_duplicate_iop_order] modules %s %s(%d) and %s %s(%d) have the same iop_order\n",
                mod_prev->op, mod_prev->multi_name, mod_prev->iop_order, mod->op, mod->multi_name, mod->iop_order);
    }

    if(reset_list)
    {
      modules = g_list_first(iop_list);
      if(modules)
      {
        mod_prev = (dt_iop_module_t *)(modules->data);
        modules = g_list_next(modules);
      }
    }
    else
    {
      mod_prev = mod;
      modules = g_list_next(modules);
    }
  }

  *_iop_list = iop_list;
}

// check if all so modules on iop_list have a iop_order defined in iop_order_list
int dt_ioppr_check_so_iop_order(GList *iop_list, GList *iop_order_list)
{
  int iop_order_missing = 0;
  // check if all the modules have their iop_order assigned
  GList *modules = g_list_first(iop_list);
  while(modules)
  {
    const dt_iop_module_so_t *const restrict mod = (dt_iop_module_so_t *)(modules->data);
    const dt_iop_order_entry_t *const restrict entry =
      dt_ioppr_get_iop_order_entry(iop_order_list, mod->op, 0); // mod->multi_priority);
    if(entry == NULL)
    {
      iop_order_missing = 1;
      fprintf(stderr, "[dt_ioppr_check_so_iop_order] missing iop_order for module %s\n", mod->op);
    }
    modules = g_list_next(modules);
  }

  return iop_order_missing;
}

static void *_dup_iop_order_entry(const void *src, gpointer data)
{
  const dt_iop_order_entry_t *const restrict scr_entry = (dt_iop_order_entry_t *)src;
  dt_iop_order_entry_t *new_entry = malloc(sizeof(dt_iop_order_entry_t));
  memcpy(new_entry, scr_entry, sizeof(dt_iop_order_entry_t));
  return (void *)new_entry;
}

// returns a duplicate of iop_order_list
GList *dt_ioppr_iop_order_copy_deep(GList *iop_order_list)
{
  return (GList *)g_list_copy_deep(iop_order_list, _dup_iop_order_entry, NULL);
}

// helper to sort a GList of dt_iop_module_t by iop_order
gint dt_sort_iop_by_order(gconstpointer a, gconstpointer b)
{
  const dt_iop_module_t *const restrict am = (const dt_iop_module_t *)a;
  const dt_iop_module_t *const restrict bm = (const dt_iop_module_t *)b;
  if(am->iop_order > bm->iop_order) return 1;
  if(am->iop_order < bm->iop_order) return -1;
  return 0;
}

// if module can be placed before than module_next on the pipe
// it returns the new iop_order
// if it cannot be placed it returns -1.0
// this assumes that the order is always positive
gboolean dt_ioppr_check_can_move_before_iop(GList *iop_list, dt_iop_module_t *module, dt_iop_module_t *module_next)
{
  if(module->flags() & IOP_FLAGS_FENCE)
    return FALSE;

  gboolean can_move = FALSE;
  // module is before on the pipe
  // move it up
  if(module->iop_order < module_next->iop_order)
  {
    // let's first search for module
    GList *modules = g_list_first(iop_list);
    while(modules)
    {
      const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;
      if(mod == module) break;
      modules = g_list_next(modules);
    }

    // we found the module
    if(modules)
    {
      dt_iop_module_t *mod1 = NULL;
      dt_iop_module_t *mod2 = NULL;
      // now search for module_next and the one previous to that, so iop_order can be calculated
      // also check the rules
      modules = g_list_next(modules);

      while(modules)
      {
        dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
        // if we reach module_next everything is OK
        if(mod == module_next)
        {
          mod2 = mod;
          break;
        }
        // check if module can be moved around this one
        if(mod->flags() & IOP_FLAGS_FENCE)
          break;

        // is there a rule about swapping this two?
        int rule_found = 0;
        GList *rules = g_list_first(darktable.iop_order_rules);
        while(rules)
        {
          const dt_iop_order_rule_t *const restrict rule = (dt_iop_order_rule_t *)rules->data;

          if(strcmp(module->op, rule->op_prev) == 0 && strcmp(mod->op, rule->op_next) == 0)
          {
            rule_found = 1;
            break;
          }

          rules = g_list_next(rules);
        }
        if(rule_found) break;

        mod1 = mod;
        modules = g_list_next(modules);
      }

      // we reach the module_next module
      if(mod2)
      {
        // this is already the previous module!
        if(module == mod1)
          ;
        else if(mod1->iop_order == mod2->iop_order)
          fprintf(stderr, "[dt_ioppr_get_iop_order_before_iop] %s %s(%d) and %s %s(%d) have the same iop_order\n",
              mod1->op, mod1->multi_name, mod1->iop_order, mod2->op, mod2->multi_name, mod2->iop_order);
        else
          can_move = TRUE;
      }
    }
    else
      fprintf(stderr, "[dt_ioppr_get_iop_order_before_iop] can't find module %s %s\n", module->op, module->multi_name);
  }
  // module is next on the pipe
  // move it down
  else if(module->iop_order > module_next->iop_order)
  {
    // let's first search for module
    GList *modules = g_list_last(iop_list);
    while(modules)
    {
      const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;
      if(mod == module) break;
      modules = g_list_previous(modules);
    }
    // we found the module
    if(modules)
    {
      dt_iop_module_t *mod1 = NULL;
      dt_iop_module_t *mod2 = NULL;
      // now search for module_next and the one next to that, so iop_order can be calculated
      // also check the rules
      modules = g_list_previous(modules);

      while(modules)
      {
        dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
        // we reach the module next to module_next, everything is OK
        if(mod2 != NULL)
        {
          mod1 = mod;
          break;
        }

        // check for rules
        // check if module can be moved around this one
        if(mod->flags() & IOP_FLAGS_FENCE)
          break;

        // is there a rule about swapping this two?
        int rule_found = 0;
        GList *rules = g_list_first(darktable.iop_order_rules);
        while(rules)
        {
          const dt_iop_order_rule_t *const restrict rule = (dt_iop_order_rule_t *)rules->data;

          if(strcmp(mod->op, rule->op_prev) == 0 && strcmp(module->op, rule->op_next) == 0)
          {
            rule_found = 1;
            break;
          }

          rules = g_list_next(rules);
        }
        if(rule_found) break;

        if(mod == module_next)
          mod2 = mod;

        modules = g_list_previous(modules);
      }

      // we reach the module_next module
      if(mod1)
      {
        // this is already the previous module!
        if(module == mod2)
          ;
        else if(mod1->iop_order == mod2->iop_order)
          fprintf(stderr, "[dt_ioppr_get_iop_order_before_iop] %s %s(%d) and %s %s(%d) have the same iop_order\n",
              mod1->op, mod1->multi_name, mod1->iop_order, mod2->op, mod2->multi_name, mod2->iop_order);
        else
          can_move = TRUE;
      }
    }
    else
      fprintf(stderr, "[dt_ioppr_get_iop_order_before_iop] can't find module %s %s\n", module->op, module->multi_name);
  }
  else
    fprintf(stderr, "[dt_ioppr_get_iop_order_before_iop] modules %s %s(%d) and %s %s(%d) have the same iop_order\n",
        module->op, module->multi_name, module->iop_order, module_next->op, module_next->multi_name, module_next->iop_order);

  return can_move;
}

// if module can be placed after than module_prev on the pipe
// it returns the new iop_order
// if it cannot be placed it returns -1.0
// this assumes that the order is always positive
gboolean dt_ioppr_check_can_move_after_iop(GList *iop_list, dt_iop_module_t *module, dt_iop_module_t *module_prev)
{
  gboolean can_move = FALSE;
  // moving after module_prev is the same as moving before the very next one after module_prev
  GList *modules = g_list_last(iop_list);
  dt_iop_module_t *module_next = NULL;

  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module_prev) break;

    module_next = mod;
    modules = g_list_previous(modules);
  }

  if(module_next == NULL)
  {
    fprintf(
        stderr,
        "[dt_ioppr_get_iop_order_after_iop] can't find module previous to %s %s(%d) while moving %s %s(%d) after it\n",
        module_prev->op, module_prev->multi_name, module_prev->iop_order, module->op, module->multi_name,
        module->iop_order);
  }
  else
    can_move = dt_ioppr_check_can_move_before_iop(iop_list, module, module_next);

  return can_move;
}

// changes the module->iop_order so it comes before in the pipe than module_next
// sort dev->iop to reflect the changes
// return 1 if iop_order is changed, 0 otherwise
gboolean dt_ioppr_move_iop_before(struct dt_develop_t *dev, dt_iop_module_t *module, dt_iop_module_t *module_next)
{
  GList *next = dt_ioppr_get_iop_order_link(dev->iop_order_list, module_next->op, module_next->multi_priority);
  GList *current = dt_ioppr_get_iop_order_link(dev->iop_order_list, module->op, module->multi_priority);

  if(!next || !current) return FALSE;

  dev->iop_order_list = g_list_remove_link(dev->iop_order_list, current);
  dev->iop_order_list = g_list_insert_before(dev->iop_order_list, next, current->data);
  g_list_free(current);
  dt_ioppr_resync_modules_order(dev);

  return TRUE;
}

// changes the module->iop_order so it comes after in the pipe than module_prev
// sort dev->iop to reflect the changes
// return 1 if iop_order is changed, 0 otherwise
gboolean dt_ioppr_move_iop_after(struct dt_develop_t *dev, dt_iop_module_t *module, dt_iop_module_t *module_prev)
{
  GList *prev = dt_ioppr_get_iop_order_link(dev->iop_order_list, module_prev->op, module_prev->multi_priority);
  GList *current = dt_ioppr_get_iop_order_link(dev->iop_order_list, module->op, module->multi_priority);

  if(!prev || !current) return FALSE;

  dev->iop_order_list = g_list_remove_link(dev->iop_order_list, current);

  // we want insert after => so insert before the next item
  GList *next = g_list_next(prev);
  if(prev)
    dev->iop_order_list = g_list_insert_before(dev->iop_order_list, next, current->data);
  else
    dev->iop_order_list = g_list_append(dev->iop_order_list, current->data);

  g_list_free(current);
  dt_ioppr_resync_modules_order(dev);

  return TRUE;
}

//--------------------------------------------------------------------
// from here just for debug
//--------------------------------------------------------------------

void dt_ioppr_print_module_iop_order(GList *iop_list, const char *msg)
{
  GList *modules = g_list_first(iop_list);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);
    fprintf(stderr, "[%s] module %s %s multi_priority=%i, iop_order=%d\n",
            msg, mod->op, mod->multi_name, mod->multi_priority, mod->iop_order);
    modules = g_list_next(modules);
  }
}

void dt_ioppr_print_history_iop_order(GList *history_list, const char *msg)
{
  GList *history = g_list_first(history_list);
  while(history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    fprintf(stderr, "[%s] module %s %s multi_priority=%i, iop_order=%d\n",
            msg, hist->op_name, hist->multi_name, hist->multi_priority, hist->iop_order);
    history = g_list_next(history);
  }
}

void dt_ioppr_print_iop_order(GList *iop_order_list, const char *msg)
{
  GList *iops_order = g_list_first(iop_order_list);
  while(iops_order)
  {
    dt_iop_order_entry_t *order_entry = (dt_iop_order_entry_t *)(iops_order->data);
    fprintf(stderr, "[%s] op %20s (inst %d) iop_order=%d\n",
            msg, order_entry->operation, order_entry->instance, order_entry->o.iop_order);
    iops_order = g_list_next(iops_order);
  }
}

static GList *_get_fence_modules_list(GList *iop_list)
{
  GList *fences = NULL;
  GList *modules = g_list_first(iop_list);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

    if(mod->flags() & IOP_FLAGS_FENCE)
      fences = g_list_append(fences, mod);

    modules = g_list_next(modules);
  }
  return fences;
}

static void _ioppr_check_rules(GList *iop_list, const int imgid, const char *msg)
{
  GList *modules = NULL;

  // check for IOP_FLAGS_FENCE on each module
  // create a list of fences modules
  GList *fences = _get_fence_modules_list(iop_list);

  // check if each module is between the fences
  modules = g_list_first(iop_list);
  while(modules)
  {
    const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;
    if(mod->iop_order == INT_MAX)
    {
      modules = g_list_next(modules);
      continue;
    }

    dt_iop_module_t *fence_prev = NULL;
    dt_iop_module_t *fence_next = NULL;

    GList *mod_fences = g_list_first(fences);
    while(mod_fences)
    {
      dt_iop_module_t *mod_fence = (dt_iop_module_t *)mod_fences->data;

      // mod should be before this fence
      if(mod->iop_order < mod_fence->iop_order)
      {
        if(fence_next == NULL)
          fence_next = mod_fence;
        else if(mod_fence->iop_order < fence_next->iop_order)
          fence_next = mod_fence;
      }
      // mod should be after this fence
      else if(mod->iop_order > mod_fence->iop_order)
      {
        if(fence_prev == NULL)
          fence_prev = mod_fence;
        else if(mod_fence->iop_order > fence_prev->iop_order)
          fence_prev = mod_fence;
      }

      mod_fences = g_list_next(mod_fences);
    }
    // now check if mod is between the fences
    if(fence_next && mod->iop_order > fence_next->iop_order)
      fprintf(stderr, "[_ioppr_check_rules] found fence %s %s module %s %s(%d) is after %s %s(%d) image %i (%s)\n",
              fence_next->op, fence_next->multi_name, mod->op, mod->multi_name, mod->iop_order, fence_next->op,
              fence_next->multi_name, fence_next->iop_order, imgid, msg);

    if(fence_prev && mod->iop_order < fence_prev->iop_order)
      fprintf(stderr, "[_ioppr_check_rules] found fence %s %s module %s %s(%d) is before %s %s(%d) image %i (%s)\n",
              fence_prev->op, fence_prev->multi_name, mod->op, mod->multi_name, mod->iop_order, fence_prev->op,
              fence_prev->multi_name, fence_prev->iop_order, imgid, msg);

    modules = g_list_next(modules);
  }

  // for each module check if it doesn't break a rule
  modules = g_list_first(iop_list);
  while(modules)
  {
    const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;
    if(mod->iop_order == INT_MAX)
    {
      modules = g_list_next(modules);
      continue;
    }

    // we have a module, now check each rule
    GList *rules = g_list_first(darktable.iop_order_rules);
    while(rules)
    {
      const dt_iop_order_rule_t *const restrict rule = (dt_iop_order_rule_t *)rules->data;

      // mod must be before rule->op_next
      if(strcmp(mod->op, rule->op_prev) == 0)
      {
        // check if there's a rule->op_next module before mod
        GList *modules_prev = g_list_previous(modules);
        while(modules_prev)
        {
          const dt_iop_module_t *const restrict mod_prev = (dt_iop_module_t *)modules_prev->data;

          if(strcmp(mod_prev->op, rule->op_next) == 0)
            fprintf(stderr, "[_ioppr_check_rules] found rule %s %s module %s %s(%d) is after %s %s(%d) image %i (%s)\n",
                    rule->op_prev, rule->op_next, mod->op, mod->multi_name, mod->iop_order, mod_prev->op,
                    mod_prev->multi_name, mod_prev->iop_order, imgid, msg);

          modules_prev = g_list_previous(modules_prev);
        }
      }
      // mod must be after rule->op_prev
      else if(strcmp(mod->op, rule->op_next) == 0)
      {
        // check if there's a rule->op_prev module after mod
        GList *modules_next = g_list_next(modules);
        while(modules_next)
        {
          const dt_iop_module_t *const restrict mod_next = (dt_iop_module_t *)modules_next->data;

          if(strcmp(mod_next->op, rule->op_prev) == 0)
          {
            fprintf(stderr, "[_ioppr_check_rules] found rule %s %s module %s %s(%d) is before %s %s(%d) image %i (%s)\n",
                    rule->op_prev, rule->op_next, mod->op, mod->multi_name, mod->iop_order, mod_next->op,
                    mod_next->multi_name, mod_next->iop_order, imgid, msg);
          }

          modules_next = g_list_next(modules_next);
        }
      }

      rules = g_list_next(rules);
    }

    modules = g_list_next(modules);
  }

  if(fences) g_list_free(fences);
}

void dt_ioppr_insert_module_instance(struct dt_develop_t *dev, dt_iop_module_t *module)
{
  const char *operation = module->op;
  const int32_t instance = module->multi_priority;

  dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
  g_strlcpy(entry->operation, operation, sizeof(entry->operation));
  entry->instance = instance;
  entry->o.iop_order = 0;
  GList *l = dev->iop_order_list;
  GList *place = NULL;
  int max_instance = -1;

  while(l)
  {
    const dt_iop_order_entry_t *const restrict e = (dt_iop_order_entry_t *)l->data;
    if(!strcmp(e->operation, operation) && e->instance > max_instance)
    {
      place = l;
      max_instance = e->instance;
    }

    l = g_list_next(l);
  }

  dev->iop_order_list = g_list_insert_before(dev->iop_order_list, place, entry);
}

int dt_ioppr_check_iop_order(dt_develop_t *dev, const int imgid, const char *msg)
{
  int iop_order_ok = 1;

  // check if gamma is the last iop
  {
    GList *modules = g_list_last(dev->iop);
    while(modules)
    {
      const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;
      if(mod->iop_order != INT_MAX)
        break;

      modules = g_list_previous(dev->iop);
    }
    if(modules)
    {
      const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;

      if(strcmp(mod->op, "gamma") != 0)
      {
        iop_order_ok = 0;
        fprintf(stderr, "[dt_ioppr_check_iop_order] gamma is not the last iop, last is %s %s(%d) image %i (%s)\n",
                mod->op, mod->multi_name, mod->iop_order,imgid, msg);
      }
    }
    else
    {
      // fprintf(stderr, "[dt_ioppr_check_iop_order] dev->iop is empty image %i (%s)\n",imgid, msg);
    }
  }

  // some other checks
  {
    GList *modules = g_list_last(dev->iop);
    while(modules)
    {
      const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;
      if(!mod->default_enabled && mod->iop_order != INT_MAX)
      {
        if(mod->enabled)
        {
          iop_order_ok = 0;
          fprintf(stderr, "[dt_ioppr_check_iop_order] module not used but enabled!! %s %s(%d) image %i (%s)\n",
                  mod->op, mod->multi_name, mod->iop_order,imgid, msg);
        }
        if(mod->multi_priority == 0)
        {
          iop_order_ok = 0;
          fprintf(stderr, "[dt_ioppr_check_iop_order] base module set as not used %s %s(%d) image %i (%s)\n",
                  mod->op, mod->multi_name, mod->iop_order,imgid, msg);
        }
      }

      modules = g_list_previous(dev->iop);
    }
  }

  // check if there's duplicate or out-of-order iop_order
  {
    dt_iop_module_t *mod_prev = NULL;
    GList *modules = g_list_first(dev->iop);
    while(modules)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
      if(mod->iop_order != INT_MAX)
      {
        if(mod_prev)
        {
          if(mod->iop_order < mod_prev->iop_order)
          {
            iop_order_ok = 0;
            fprintf(stderr,
                    "[dt_ioppr_check_iop_order] module %s %s(%d) should be after %s %s(%d) image %i (%s)\n",
                    mod->op, mod->multi_name, mod->iop_order, mod_prev->op, mod_prev->multi_name,
                    mod_prev->iop_order, imgid, msg);
          }
          else if(mod->iop_order == mod_prev->iop_order)
          {
            iop_order_ok = 0;
            fprintf(
                stderr,
                "[dt_ioppr_check_iop_order] module %s %s(%i)(%d) and %s %s(%i)(%d) have the same order image %i (%s)\n",
                mod->op, mod->multi_name, mod->multi_priority, mod->iop_order, mod_prev->op,
                mod_prev->multi_name, mod_prev->multi_priority, mod_prev->iop_order, imgid, msg);
          }
        }
      }
      mod_prev = mod;
      modules = g_list_next(modules);
    }
  }

  _ioppr_check_rules(dev->iop, imgid, msg);

  GList *history = g_list_first(dev->history);
  while(history)
  {
    const dt_dev_history_item_t *const restrict hist = (dt_dev_history_item_t *)(history->data);

    if(hist->iop_order == INT_MAX)
    {
      if(hist->enabled)
      {
        iop_order_ok = 0;
        fprintf(stderr, "[dt_ioppr_check_iop_order] history module not used but enabled!! %s %s(%d) image %i (%s)\n",
            hist->op_name, hist->multi_name, hist->iop_order, imgid, msg);
      }
      if(hist->multi_priority == 0)
      {
        iop_order_ok = 0;
        fprintf(stderr, "[dt_ioppr_check_iop_order] history base module set as not used %s %s(%d) image %i (%s)\n",
            hist->op_name, hist->multi_name, hist->iop_order, imgid, msg);
      }
    }

    history = g_list_next(history);
  }

  return iop_order_ok;
}

void *dt_ioppr_serialize_iop_order_list(GList *iop_order_list, size_t *size)
{
  // compute size of all modules
  *size = 0;

  GList *l = iop_order_list;
  while(l)
  {
    const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)l->data;
    *size += strlen(entry->operation) + sizeof(int32_t) * 2;
    l = g_list_next(l);
  }
  // allocate the parameter buffer
  char *params = (char *)malloc(*size);
  // set set preset iop-order version
  int pos = 0;

  l = iop_order_list;
  while(l)
  {
    const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)l->data;
    // write the len of the module name
    const int32_t len = strlen(entry->operation);
    memcpy(params+pos, &len, sizeof(int32_t));
    pos += sizeof(int32_t);
    // write the module name
    memcpy(params+pos, entry->operation, len);
    pos += len;
    // write the instance number
    memcpy(params+pos, &(entry->instance), sizeof(int32_t));
    pos += sizeof(int32_t);
    l = g_list_next(l);
  }

  return params;
}

char *dt_ioppr_serialize_text_iop_order_list(GList *iop_order_list)
{
  gchar *text = g_strdup("");
  GList *l = iop_order_list;

  while(l)
  {
    const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)l->data;
    gchar buf[64];
    snprintf(buf, sizeof(buf), "%s,%d%s", entry->operation, entry->instance, l == g_list_last(iop_order_list) ? "" : ",");
    text = g_strconcat(text, buf, NULL);
    l = g_list_next(l);
  }

  return text;
}

/* this sanity check routine is used to correct wrong iop-list that
 * could have been stored while some bugs were present in
 * dartkable. There was a window around Sep 2019 where such issue
 * existed and some xmp may have been corrupt at this time making dt
 * crash while reimporting using the xmp.
 *
 * One common case seems that the list does not end with gamma.
*/

static gboolean _ioppr_sanity_check_iop_order(GList *list)
{
  gboolean ok = TRUE;
  // First check that first module is rawprepare (even for a jpeg, we
  // are speaking of the module ordering not the activated modules.
  GList *first = g_list_first(list);
  dt_iop_order_entry_t *entry_first = (dt_iop_order_entry_t *)first->data;
  ok = ok && (g_strcmp0(entry_first->operation, "rawprepare") == 0);
  // Then check that last module is gamma
  GList *last = g_list_last(list);
  dt_iop_order_entry_t *entry_last = (dt_iop_order_entry_t *)last->data;
  ok = ok && (g_strcmp0(entry_last->operation, "gamma") == 0);

  return ok;
}

GList *dt_ioppr_deserialize_text_iop_order_list(const char *buf)
{
  GList *iop_order_list = NULL;
  GList *list = dt_util_str_to_glist(",", buf);
  GList *l = g_list_first(list);

  while(l)
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
    entry->o.iop_order = 0;
    // first operation name
    g_strlcpy(entry->operation, (char *)l->data, sizeof(entry->operation));
    // then operation instance
    l = g_list_next(l);

    if(!l)
      goto error;

    const char *data = (char *)l->data;
    int inst = 0;
    sscanf(data, "%d", &inst);
    entry->instance = inst;
    // append to the list
    iop_order_list = g_list_append(iop_order_list, entry);
    l = g_list_next(l);
  }

  g_list_free(list);
  _ioppr_reset_iop_order(iop_order_list);

  if(!_ioppr_sanity_check_iop_order(iop_order_list))
    goto error;

  return iop_order_list;

 error:
  g_list_free_full(iop_order_list, free);
  return NULL;
}

GList *dt_ioppr_deserialize_iop_order_list(const char *buf, size_t size)
{
  GList *iop_order_list = NULL;
  // parse all modules
  while(size)
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
    entry->o.iop_order = 0;
    // get length of module name
    const int32_t len = *(int32_t *)buf;
    buf += sizeof(int32_t);

    if(len < 0 || len > 20)
    { 
      free(entry);
      goto error;
    }
    // set module name
    memcpy(entry->operation, buf, len);
    *(entry->operation + len) = '\0';
    buf += len;
    // get the instance number
    entry->instance = *(int32_t *)buf;
    buf += sizeof(int32_t);

    if(entry->instance < 0 || entry->instance > 1000)
    {
      free(entry);
      goto error;
    }
    // append to the list
    iop_order_list = g_list_append(iop_order_list, entry);
    size -= (2 * sizeof(int32_t) + len);
  }

  _ioppr_reset_iop_order(iop_order_list);

  return iop_order_list;

 error:
  g_list_free_full(iop_order_list, free);
  return NULL;
}
