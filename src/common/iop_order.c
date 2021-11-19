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

    rules = g_list_prepend(rules, rule);
    i++;
  }
  
  return g_list_reverse(rules);
}

GList *dt_ioppr_get_iop_order_link(GList *iop_order_list, const char *op_name, const int multi_priority)
{
  GList *link = NULL;

  for(GList *iops_order = iop_order_list; iops_order; iops_order = g_list_next(iops_order))
  {
    dt_iop_order_entry_t *order_entry = (dt_iop_order_entry_t *)iops_order->data;

    if(strcmp(order_entry->operation, op_name) == 0
       && (order_entry->instance == multi_priority || multi_priority == -1))
    {
      link = iops_order;
      break;
    }
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
      ok = TRUE;
      // skip all the other instance of same module if any
      while(g_list_next(l) && !strcmp(v30_order[k].operation,
            ((dt_iop_order_entry_t *)(g_list_next(l)->data))->operation))
        l = g_list_next(l);
    }

    k++;
    if(ok)
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
    if(next && (strcmp(((dt_iop_order_entry_t *)(l->data))->operation,
                ((dt_iop_order_entry_t *)(next->data))->operation) == 0))
      return TRUE;

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
    iop_order_list = g_list_prepend(iop_order_list, entry);

    k++;
  }

  return g_list_reverse(iop_order_list);
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
  for(const GList *l = iop_order_list; l; l = g_list_next(l))
  {
    dt_iop_order_entry_t *e = (dt_iop_order_entry_t *)l->data;
    e->o.iop_order = iop_order++;
  }
}

void dt_ioppr_resync_iop_list(dt_develop_t *dev)
{
  // make sure that the iop_order_list does not contain possibly removed modules

  GList *l = dev->iop_order_list;
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
  GList *modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);
    GList *next = g_list_next(modules);

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

// returns the first dt_dev_history_item_t on history_list where hist->module == mod
static dt_dev_history_item_t *_ioppr_search_history_by_module(GList *history_list, dt_iop_module_t *mod)
{
  dt_dev_history_item_t *hist_entry = NULL;

  for(const GList *history = history_list; history; history = g_list_next(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);

    if(hist->module == mod)
    {
      hist_entry = hist;
      break;
    }
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
  GList *modules = iop_list;
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
      modules = iop_list;

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
  for(const GList *modules = iop_list; modules; modules = g_list_next(modules))
  {
    const dt_iop_module_so_t *const restrict mod = (dt_iop_module_so_t *)(modules->data);
    const dt_iop_order_entry_t *const restrict entry =
      dt_ioppr_get_iop_order_entry(iop_order_list, mod->op, 0); // mod->multi_priority);
    if(entry == NULL)
    {
      iop_order_missing = 1;
      fprintf(stderr, "[dt_ioppr_check_so_iop_order] missing iop_order for module %s\n", mod->op);
    }
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

static GList *_get_fence_modules_list(GList *iop_list)
{
  GList *fences = NULL;
  for(const GList *modules = iop_list; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

    if(mod->flags() & IOP_FLAGS_FENCE)
      fences = g_list_prepend(fences, mod);
  }
  return g_list_reverse(fences);
}

static void _ioppr_check_rules(GList *iop_list, const int imgid, const char *msg)
{
  // check for IOP_FLAGS_FENCE on each module
  // create a list of fences modules
  GList *fences = _get_fence_modules_list(iop_list);
  // check if each module is between the fences
  for(const GList *modules = iop_list; modules; modules = g_list_next(modules))
  {
    const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;

    if(mod->iop_order == INT_MAX)
      continue;

    dt_iop_module_t *fence_prev = NULL;
    dt_iop_module_t *fence_next = NULL;

    for(const GList *mod_fences = fences; mod_fences; mod_fences = g_list_next(mod_fences))
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
  }

  // for each module check if it doesn't break a rule
  for(const GList *modules = iop_list; modules; modules = g_list_next(modules))
  {
    const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;

    if(mod->iop_order == INT_MAX)
      continue;
     // we have a module, now check each rule
    for(const GList *rules = darktable.iop_order_rules; rules; rules = g_list_next(rules))
    {
      const dt_iop_order_rule_t *const restrict rule = (dt_iop_order_rule_t *)rules->data;
      // mod must be before rule->op_next
      if(strcmp(mod->op, rule->op_prev) == 0)
      {
        // check if there's a rule->op_next module before mod
        for(const GList *modules_prev = g_list_previous(modules); modules_prev;
            modules_prev = g_list_previous(modules_prev))
        {
          const dt_iop_module_t *const restrict mod_prev = (dt_iop_module_t *)modules_prev->data;

          if(strcmp(mod_prev->op, rule->op_next) == 0)
            fprintf(stderr, "[_ioppr_check_rules] found rule %s %s module %s %s(%d) is after %s %s(%d) image %i (%s)\n",
                    rule->op_prev, rule->op_next, mod->op, mod->multi_name, mod->iop_order, mod_prev->op,
                    mod_prev->multi_name, mod_prev->iop_order, imgid, msg);
        }
      }
      // mod must be after rule->op_prev
      else if(strcmp(mod->op, rule->op_next) == 0)
      {
        // check if there's a rule->op_prev module after mod
        for(const GList *modules_next = g_list_next(modules); modules_next;  modules_next = g_list_next(modules_next))
        {
          const dt_iop_module_t *const restrict mod_next = (dt_iop_module_t *)modules_next->data;

          if(strcmp(mod_next->op, rule->op_prev) == 0)
          {
            fprintf(stderr, "[_ioppr_check_rules] found rule %s %s module %s %s(%d) is before %s %s(%d) image %i (%s)\n",
                    rule->op_prev, rule->op_next, mod->op, mod->multi_name, mod->iop_order, mod_next->op,
                    mod_next->multi_name, mod_next->iop_order, imgid, msg);
          }
        }
      }
    }
  }

  if(fences) g_list_free(fences);
}

int dt_ioppr_check_iop_order(dt_develop_t *dev, const int imgid, const char *msg)
{
  int iop_order_ok = 1;
  // check if gamma is the last iop
  {
    GList *modules;
    for(modules = g_list_last(dev->iop); modules; modules = g_list_previous(dev->iop))
    {
      const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;
      if(mod->iop_order != INT_MAX)
        break;
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
    for(const GList *modules = g_list_last(dev->iop); modules; modules = g_list_previous(dev->iop))
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
    }
  }

  // check if there's duplicate or out-of-order iop_order
  {
    dt_iop_module_t *mod_prev = NULL;
    for(const GList *modules = dev->iop; modules; modules = g_list_next(modules))
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
    }
  }

  _ioppr_check_rules(dev->iop, imgid, msg);

  for(const GList *history = dev->history; history; history = g_list_next(history))
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
  }

  return iop_order_ok;
}

void *dt_ioppr_serialize_iop_order_list(GList *iop_order_list, size_t *size)
{
  // compute size of all modules
  *size = 0;

  for(const GList *l = iop_order_list; l; l = g_list_next(l))
  {
    const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)l->data;
    *size += strlen(entry->operation) + sizeof(int32_t) * 2;
  }

  if(*size == 0)
    return NULL;
  // allocate the parameter buffer
  char *params = (char *)malloc(*size);
  // set set preset iop-order version
  int pos = 0;

  for(const GList *l = iop_order_list; l; l = g_list_next(l))
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
  }

  return params;
}

char *dt_ioppr_serialize_text_iop_order_list(GList *iop_order_list)
{
  gchar *text = g_strdup("");
  const GList *const last = g_list_last(iop_order_list);

  for(const GList *l = iop_order_list; l; l = g_list_next(l))
  {
    const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)l->data;
    gchar buf[64];
    snprintf(buf, sizeof(buf), "%s,%d%s", entry->operation, entry->instance, (l == last) ? "" : ",");
    text = g_strconcat(text, buf, NULL);
  }

  return text;
}

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

  for(GList *l = list; l; l = g_list_next(l))
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
    entry->o.iop_order = 0;
    // first operation name
    g_strlcpy(entry->operation, (char *)l->data, sizeof(entry->operation));
    // then operation instance
    l = g_list_next(l);

    if(!l) goto error;

    const char *data = (char *)l->data;
    int inst = 0;
    sscanf(data, "%d", &inst);
    entry->instance = inst;
    // prepend to the list
    iop_order_list = g_list_prepend(iop_order_list, entry);
  }

  iop_order_list = g_list_reverse(iop_order_list);
  g_list_free_full(list, g_free);
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
    iop_order_list = g_list_prepend(iop_order_list, entry);
    size -= (2 * sizeof(int32_t) + len);
  }

  iop_order_list = g_list_reverse(iop_order_list);  // list was built in reverse order, so un-reverse it
  _ioppr_reset_iop_order(iop_order_list);

  return iop_order_list;

 error:
  g_list_free_full(iop_order_list, free);
  return NULL;
}
