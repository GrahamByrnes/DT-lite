/*
    This file is part of darktable,
    copyright (c) 2012--2016 Ulrich Pegelow.

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

#ifdef HAVE_IMAGEMAGICK
#include "common/darktable.h"
#include "imageio.h"
#include "imageio_gm.h"
#include "develop/develop.h"
#include "common/exif.h"
#include "common/colorspaces.h"
#include "control/conf.h"

#include <memory.h>
#include <stdio.h>
#include <inttypes.h>
#include <strings.h>
#include <assert.h>

#include <MagickWand/MagickWand.h>


/* we only support images with certain filename extensions via ImageMagick,
 * derived from what it declared as "supported" with GraphicsMagick; RAWs
 * are excluded as ImageMagick would render them with third party libraries
 * in reduced quality - slow and only 8-bit */
static gboolean _supported_image(const gchar *filename)
{
  const char *extensions_whitelist[] = { "tif",  "tiff", "gif", "jpc", "jp2", "bmp", "dcm", "jng",
                                         "miff", "mng",  "pbm", "pnm", "ppm", "pgm", NULL };
  gboolean supported = FALSE;
  char *ext = g_strrstr(filename, ".");
  if(!ext) return FALSE;
  ext++;
  for(const char **i = extensions_whitelist; *i != NULL; i++)
    if(!g_ascii_strncasecmp(ext, *i, strlen(*i)))
    {
      supported = TRUE;
      break;
    }
  return supported;
}


dt_imageio_retval_t dt_imageio_open_im(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  int err = DT_IMAGEIO_FILE_CORRUPTED;
  MagickWand *image = NULL;
  MagickBooleanType ret;

  if(!_supported_image(filename)) return DT_IMAGEIO_FILE_CORRUPTED;

  if(!img->exif_inited) (void)dt_exif_read(img, filename);

  image = NewMagickWand();
  if (image == NULL) goto error;

  ret = MagickReadImage(image, filename);
  if (ret != MagickTrue) {
    fprintf(stderr, "[ImageMagick_open] cannot open `%s'\n", img->filename);
    err = DT_IMAGEIO_FILE_NOT_FOUND;
    goto error;
  }
  fprintf(stderr, "[ImageMagick_open] image `%s' loading\n", img->filename);

  img->width = MagickGetImageWidth(image);
  img->height = MagickGetImageHeight(image);

  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;

  float *mipbuf = dt_mipmap_cache_alloc(mbuf, img);
  if (mipbuf == NULL) {
    fprintf(stderr,
        "[ImageMagick_open] could not alloc full buffer for image `%s'\n",
        img->filename);
    err = DT_IMAGEIO_CACHE_FULL;
    goto error;
  }

  ret = MagickExportImagePixels(image, 0, 0, img->width, img->height, "RGBP", FloatPixel, mipbuf);
  if (ret != MagickTrue) {
    fprintf(stderr,
        "[ImageMagick_open] error reading image `%s'\n", img->filename);
    goto error;
  }

  DestroyMagickWand(image);

  img->buf_dsc.filters = 0u;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_HDR;
  img->flags |= DT_IMAGE_LDR;

  return DT_IMAGEIO_OK;

error:
  DestroyMagickWand(image);
  return err;
}
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
