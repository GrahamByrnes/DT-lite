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

#pragma once

#include <inttypes.h>

struct dt_dev_pixelpipe_t;
struct dt_iop_buffer_dsc_t;
struct dt_iop_roi_t;

/**
 * implements a simple pixel cache suitable for caching float images
 * corresponding to history items and zoom/pan settings in the develop module.
 * it is optimized for very few entries (~5), so most operations are O(N).
 */

typedef struct dt_dev_pixelpipe_cache_t
{
  int32_t entries;
  void **data;
  size_t *size;
  struct dt_iop_buffer_dsc_t *dsc;
  uint64_t *basichash;
  uint64_t *hash;
  int32_t *used;
  // profiling:
  uint64_t queries;
  uint64_t misses;
} dt_dev_pixelpipe_cache_t;

/** constructs a new cache with given cache line count (entries) and float buffer entry size in bytes.
  \param[out] returns 0 if fail to allocate mem cache.
*/
int dt_dev_pixelpipe_cache_init(dt_dev_pixelpipe_cache_t *cache, int entries, size_t size);
void dt_dev_pixelpipe_cache_cleanup(dt_dev_pixelpipe_cache_t *cache);

/** creates a hopefully unique hash from the complete module stack up to the module-th. */
uint64_t dt_dev_pixelpipe_cache_basichash(int imgid, struct dt_dev_pixelpipe_t *pipe, int module);
/** creates a hopefully unique hash from the complete module stack up to the module-th, including current viewport. */
uint64_t dt_dev_pixelpipe_cache_hash(int imgid, const struct dt_iop_roi_t *roi,
                                     struct dt_dev_pixelpipe_t *pipe, int module);
/** return both of the above hashes */
void dt_dev_pixelpipe_cache_fullhash(int imgid, const dt_iop_roi_t *roi, struct dt_dev_pixelpipe_t *pipe, int module,
                                     uint64_t *basichash, uint64_t *fullhash);
/** get the basichash for the last enabled module prior to the specified one */
uint64_t dt_dev_pixelpipe_cache_basichash_prior(int imgid, struct dt_dev_pixelpipe_t *pipe,
                                                const struct dt_iop_module_t *const module);

/** returns the float data buffer for the given hash from the cache. if the hash does not match any
  * cache line, the least recently used cache line will be cleared and an empty buffer is returned
  * together with a non-zero return value. */
int dt_dev_pixelpipe_cache_get(dt_dev_pixelpipe_cache_t *cache, const uint64_t basichash, const uint64_t hash,
                               const size_t size, void **data, struct dt_iop_buffer_dsc_t **dsc);
int dt_dev_pixelpipe_cache_get_important(dt_dev_pixelpipe_cache_t *cache, const uint64_t basichash,
                                         const uint64_t hash, const size_t size,
                                         void **data, struct dt_iop_buffer_dsc_t **dsc);
int dt_dev_pixelpipe_cache_get_weighted(dt_dev_pixelpipe_cache_t *cache, const uint64_t basichash,
                                        const uint64_t hash, const size_t size,
                                        void **data, struct dt_iop_buffer_dsc_t **dsc, int weight);

/** test availability of a cache line without destroying another, if it is not found. */
int dt_dev_pixelpipe_cache_available(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash);

/** invalidates all cachelines. */
void dt_dev_pixelpipe_cache_flush(dt_dev_pixelpipe_cache_t *cache);

/** invalidates all cachelines except those containing items for the given module/parameter combination */
void dt_dev_pixelpipe_cache_flush_all_but(dt_dev_pixelpipe_cache_t *cache, uint64_t basichash);

/** makes this buffer very important after it has been pulled from the cache. */
void dt_dev_pixelpipe_cache_reweight(dt_dev_pixelpipe_cache_t *cache, void *data);

/** mark the given cache line pointer as invalid. */
void dt_dev_pixelpipe_cache_invalidate(dt_dev_pixelpipe_cache_t *cache, void *data);

/** print out cache lines/hashes (debug). */
void dt_dev_pixelpipe_cache_print(dt_dev_pixelpipe_cache_t *cache);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
