/*
    This file is part of darktable,
    Copyright (C) 2013-2020 darktable developers.

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

#include <common/imageio_module.h>
#include <lua/lua.h>

// forward declaration
struct dt_imageio_module_storage_t;


#define dt_lua_register_storage(L, storage, type_name)                                                       \
  dt_lua_register_storage_type(L, storage, luaA_type_find(#type_name))
void dt_lua_register_storage_type(lua_State *L, struct dt_imageio_module_storage_t *module, luaA_Type type_id);



int dt_lua_init_early_storage(lua_State *L);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
