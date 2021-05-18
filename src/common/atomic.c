/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.
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

#include "common/atomic.h"

#if !defined(__STDC_NO_ATOMICS__)
// using C11 atomics, everything is handled in the header file, so we don't need to define anything in this file

#elif defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNU_MINOR__ >= 8))
// using GNU intrinsics. everything is handled in the header file

#else
// we fell back to using a global mutex for synchronization
// this is that mutex's definition
pthread_mutex_t dt_atom_mutex = PTHREAD_MUTEX_INITIALIZER;

#endif // __STDC_NO_ATOMICS__