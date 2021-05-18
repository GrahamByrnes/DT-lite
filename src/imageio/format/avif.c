/*
 * This file is part of darktable,
 * Copyright (C) 2019-2020 darktable developers.
 *
 *  Copyright (c) 2019      Andreas Schneider
 *
 *  darktable is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  darktable is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "control/conf.h"
#include "imageio/format/imageio_format_api.h"

#include <avif/avif.h>

#define AVIF_MIN_TILE_SIZE 512
#define AVIF_MAX_TILE_SIZE 3072
#define AVIF_DEFAULT_TILE_SIZE AVIF_MIN_TILE_SIZE * 4

DT_MODULE(1)

enum avif_compression_type_e
{
  AVIF_COMP_LOSSLESS = 0,
  AVIF_COMP_LOSSY = 1,
};

enum avif_tiling_e
{
  AVIF_TILING_ON = 0,
  AVIF_TILING_OFF
};

enum avif_color_mode_e
{
  AVIF_COLOR_MODE_RGB = 0,
  AVIF_COLOR_MODE_GRAYSCALE,
};

typedef struct dt_imageio_avif_t
{
  dt_imageio_module_data_t global;
  uint32_t bit_depth;
  uint32_t color_mode;
  uint32_t compression_type;
  uint32_t quality;
  uint32_t tiling;
} dt_imageio_avif_t;

typedef struct dt_imageio_avif_gui_t
{
  GtkWidget *bit_depth;
  GtkWidget *color_mode;
  GtkWidget *compression_type;
  GtkWidget *quality;
  GtkWidget *tiling;
} dt_imageio_avif_gui_t;

static const struct
{
  char     *name;
  uint32_t bit_depth;
} avif_bit_depth[] = {
  {
    .name = N_("8 bit"),
    .bit_depth  = 8
  },
  {
    .name = N_("10 bit"),
    .bit_depth  = 10
  },
  {
    .name = N_("12 bit"),
    .bit_depth  = 12
  },
  {
    .name = NULL,
  }
};

static const char *avif_get_compression_string(enum avif_compression_type_e comp)
{
  switch (comp) {
  case AVIF_COMP_LOSSLESS:
    return "lossless";
  case AVIF_COMP_LOSSY:
    return "lossy";
  }

  return "unknown";
}

/* Lookup table for tiling choices */
static int flp2(int i)
{
                                  /* 0   1,  2,  3,  4,  5,  6,  7,  8,  9 */
  static const int flp2_table[] = {  0,  0,  2,  2,  4,  4,  4,  4,  8,  8,
                                     8,  8,  8,  8,  8,  8, 16, 16, 16, 16,
                                    16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
                                    16, 16, 32, 32, 32, 32, 32, 32, 32, 32,
                                    32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
                                    32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
                                    32, 32, 32, 32 };
                                  /* 0   1,  2,  3,  4,  5,  6,  7,  8,  9 */

  if (i >= 64) {
    return 64;
  }

  return flp2_table[i];
}

void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  /* bit depth */
  dt_lua_register_module_member(darktable.lua_state.state,
                                self,
                                dt_imageio_avif_t,
                                bit_depth,
                                int);
  luaA_enum(darktable.lua_state.state,
            enum avif_color_mode_e);
  luaA_enum_value(darktable.lua_state.state,
                  enum avif_color_mode_e,
                  AVIF_COLOR_MODE_GRAYSCALE);
  luaA_enum_value(darktable.lua_state.state,
                  enum avif_color_mode_e,
                  AVIF_COLOR_MODE_GRAYSCALE);

  luaA_enum(darktable.lua_state.state,
            enum avif_tiling_e);
  luaA_enum_value(darktable.lua_state.state,
                  enum avif_tiling_e,
                  AVIF_TILING_ON);
  luaA_enum_value(darktable.lua_state.state,
                  enum avif_tiling_e,
                  AVIF_TILING_OFF);

  /* compression type */
  luaA_enum(darktable.lua_state.state,
            enum avif_compression_type_e);
  luaA_enum_value(darktable.lua_state.state,
                  enum avif_compression_type_e,
                  AVIF_COMP_LOSSLESS);
  luaA_enum_value(darktable.lua_state.state,
                  enum avif_compression_type_e,
                  AVIF_COMP_LOSSY);

  dt_lua_register_module_member(darktable.lua_state.state,
                                self,
                                dt_imageio_avif_t,
                                compression_type,
                                enum avif_compression_type_e);

  /* quality */
  dt_lua_register_module_member(darktable.lua_state.state,
                                self,
                                dt_imageio_avif_t,
                                quality,
                                int);
#endif
}

void cleanup(dt_imageio_module_format_t *self)
{
}

int write_image(struct dt_imageio_module_data_t *data,
                const char *filename,
                const void *in,
                dt_colorspaces_color_profile_type_t over_type,
                const char *over_filename,
                void *exif,
                int exif_len,
                int imgid,
                int num,
                int total,
                struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  dt_imageio_avif_t *d = (dt_imageio_avif_t *)data;

  avifPixelFormat format = AVIF_PIXEL_FORMAT_NONE;
  avifImage *image = NULL;
  avifRGBImage rgb = {
      .format = AVIF_RGB_FORMAT_RGB,
  };
  avifEncoder *encoder = NULL;
  uint8_t *icc_profile_data = NULL;
  uint32_t icc_profile_len;
  avifResult result;
  int rc;

  const size_t width = d->global.width;
  const size_t height = d->global.height;
  const size_t bit_depth = d->bit_depth > 0 ? d->bit_depth : 0;
  enum avif_color_mode_e color_mode = d->color_mode;

  switch (color_mode)
  {
    case AVIF_COLOR_MODE_RGB:
      switch (d->compression_type)
      {
        case AVIF_COMP_LOSSLESS:
          format = AVIF_PIXEL_FORMAT_YUV444;
          break;
        case AVIF_COMP_LOSSY:
          if (d->quality > 90)
          {
              format = AVIF_PIXEL_FORMAT_YUV444;
          }
          else if (d->quality > 80)
          {
              format = AVIF_PIXEL_FORMAT_YUV422;
          }
          else
          {
            format = AVIF_PIXEL_FORMAT_YUV420;
          }
          break;
      }

      break;
    case AVIF_COLOR_MODE_GRAYSCALE:
      format = AVIF_PIXEL_FORMAT_YUV400;
      break;
  }

  image = avifImageCreate(width, height, bit_depth, format);
  if (image == NULL) {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to create AVIF image for writing [%s]\n",
             filename);
    rc = 1;
    goto out;
  }

  dt_print(DT_DEBUG_IMAGEIO,
           "Exporting AVIF image [%s] "
           "[width: %zu, height: %zu, bit depth: %zu, comp: %s, quality: %u]\n",
           filename,
           width,
           height,
           bit_depth,
           avif_get_compression_string(d->compression_type),
           d->quality);

  avifRGBImageSetDefaults(&rgb, image);
  rgb.format = AVIF_RGB_FORMAT_RGB;

  avifRGBImageAllocatePixels(&rgb);

  const float max_channel_f = (float)((1 << bit_depth) - 1);

  const size_t rowbytes = rgb.rowBytes;

  const float *const restrict in_data = (const float *)in;
  uint8_t *const restrict out = (uint8_t *)rgb.pixels;

  switch(bit_depth) {
  case 12:
  case 10: {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(in_data, width, height, out, rowbytes, max_channel_f) \
  schedule(simd:static) \
  collapse(2)
#endif
    for (size_t y = 0; y < height; y++)
    {
      for (size_t x = 0; x < width; x++)
      {
          const float *in_pixel = &in_data[(size_t)4 * ((y * width) + x)];
          uint16_t *out_pixel = (uint16_t *)&out[(y * rowbytes) + (3 * sizeof(uint16_t) * x)];

          out_pixel[0] = (uint16_t)CLAMP(in_pixel[0] * max_channel_f, 0, max_channel_f);
          out_pixel[1] = (uint16_t)CLAMP(in_pixel[1] * max_channel_f, 0, max_channel_f);
          out_pixel[2] = (uint16_t)CLAMP(in_pixel[2] * max_channel_f, 0, max_channel_f);
      }
    }
    break;
  }
  case 8: {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(in_data, width, height, out, rowbytes, max_channel_f) \
  schedule(simd:static) \
  collapse(2)
#endif
    for (size_t y = 0; y < height; y++)
    {
      for (size_t x = 0; x < width; x++)
      {
          const float *in_pixel = &in_data[(size_t)4 * ((y * width) + x)];
          uint8_t *out_pixel = (uint8_t *)&out[(y * rowbytes) + (3 * sizeof(uint8_t) * x)];

          out_pixel[0] = (uint8_t)CLAMP(in_pixel[0] * max_channel_f, 0, max_channel_f);
          out_pixel[1] = (uint8_t)CLAMP(in_pixel[1] * max_channel_f, 0, max_channel_f);
          out_pixel[2] = (uint8_t)CLAMP(in_pixel[2] * max_channel_f, 0, max_channel_f);
      }
    }
    break;
  }
  default:
    dt_control_log(_("Invalid AVIF bit depth!"));
    rc = 1;
    goto out;
  }

  avifImageRGBToYUV(image, &rgb);

  if (imgid > 0) {
    gboolean use_icc = FALSE;

#if AVIF_VERSION >= 800
    image->colorPrimaries = AVIF_COLOR_PRIMARIES_UNKNOWN;
    image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_UNKNOWN;
    image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED;

    switch (over_type) {
      case DT_COLORSPACE_SRGB:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT709;
          break;
      case DT_COLORSPACE_REC709:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_BT470M;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT709;
        break;
      case DT_COLORSPACE_LIN_REC709:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_LINEAR;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT709;
        break;
      case DT_COLORSPACE_LIN_REC2020:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT2020;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_LINEAR;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;
        break;
      case DT_COLORSPACE_PQ_REC2020:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT2020;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;
        break;
      case DT_COLORSPACE_HLG_REC2020:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT2020;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_HLG;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;
        break;
      case DT_COLORSPACE_PQ_P3:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_SMPTE432;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL;
        break;
      case DT_COLORSPACE_HLG_P3:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_SMPTE432;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_HLG;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL;
        break;
      default:
        break;
    }

    if (image->colorPrimaries == AVIF_COLOR_PRIMARIES_UNKNOWN) {
      use_icc = TRUE;
    }
#else /* AVIF_VERSION 700 */
    avifNclxColorProfile nclx = {
        .colourPrimaries = AVIF_NCLX_COLOUR_PRIMARIES_UNKNOWN,
    };

    switch (over_type) {
      case DT_COLORSPACE_SRGB:
        nclx = (avifNclxColorProfile) {
          .colourPrimaries = AVIF_NCLX_COLOUR_PRIMARIES_BT709,
          .transferCharacteristics = AVIF_NCLX_TRANSFER_CHARACTERISTICS_SRGB,
          .matrixCoefficients = AVIF_NCLX_MATRIX_COEFFICIENTS_BT709,
#if AVIF_VERSION > 700
          .range = AVIF_RANGE_FULL,
#else
          .fullRangeFlag = AVIF_NCLX_FULL_RANGE,
#endif
        };
        break;
      case DT_COLORSPACE_REC709:
        nclx = (avifNclxColorProfile) {
          .colourPrimaries = AVIF_NCLX_COLOUR_PRIMARIES_BT709,
          .transferCharacteristics = AVIF_NCLX_TRANSFER_CHARACTERISTICS_BT470M,
          .matrixCoefficients = AVIF_NCLX_MATRIX_COEFFICIENTS_BT709,
#if AVIF_VERSION > 700
          .range = AVIF_RANGE_FULL,
#else
          .fullRangeFlag = AVIF_NCLX_FULL_RANGE,
#endif
        };
        break;
      case DT_COLORSPACE_LIN_REC709:
        nclx = (avifNclxColorProfile) {
          .colourPrimaries = AVIF_NCLX_COLOUR_PRIMARIES_BT709,
          .transferCharacteristics = AVIF_NCLX_TRANSFER_CHARACTERISTICS_LINEAR,
          .matrixCoefficients = AVIF_NCLX_MATRIX_COEFFICIENTS_BT709,
#if AVIF_VERSION > 700
          .range = AVIF_RANGE_FULL,
#else
          .fullRangeFlag = AVIF_NCLX_FULL_RANGE,
#endif
        };
        break;
      case DT_COLORSPACE_LIN_REC2020:
        nclx = (avifNclxColorProfile) {
          .colourPrimaries = AVIF_NCLX_COLOUR_PRIMARIES_BT2020,
          .transferCharacteristics = AVIF_NCLX_TRANSFER_CHARACTERISTICS_LINEAR,
          .matrixCoefficients = AVIF_NCLX_MATRIX_COEFFICIENTS_BT2020_NCL,
#if AVIF_VERSION > 700
          .range = AVIF_RANGE_FULL,
#else
          .fullRangeFlag = AVIF_NCLX_FULL_RANGE,
#endif
        };
        break;
      case DT_COLORSPACE_PQ_REC2020:
        nclx = (avifNclxColorProfile) {
          .colourPrimaries = AVIF_NCLX_COLOUR_PRIMARIES_BT2020,
          .transferCharacteristics = AVIF_NCLX_TRANSFER_CHARACTERISTICS_SMPTE2084,
          .matrixCoefficients = AVIF_NCLX_MATRIX_COEFFICIENTS_BT2020_NCL,
#if AVIF_VERSION > 700
          .range = AVIF_RANGE_FULL,
#else
          .fullRangeFlag = AVIF_NCLX_FULL_RANGE,
#endif
        };
        break;
      case DT_COLORSPACE_HLG_REC2020:
        nclx = (avifNclxColorProfile) {
          .colourPrimaries = AVIF_NCLX_COLOUR_PRIMARIES_BT2020,
          .transferCharacteristics = AVIF_NCLX_TRANSFER_CHARACTERISTICS_HLG,
          .matrixCoefficients = AVIF_NCLX_MATRIX_COEFFICIENTS_BT2020_NCL,
#if AVIF_VERSION > 700
          .range = AVIF_RANGE_FULL,
#else
          .fullRangeFlag = AVIF_NCLX_FULL_RANGE,
#endif
        };
        break;
      case DT_COLORSPACE_PQ_P3:
        nclx = (avifNclxColorProfile) {
          .colourPrimaries = AVIF_NCLX_COLOUR_PRIMARIES_SMPTE432,
          .transferCharacteristics = AVIF_NCLX_TRANSFER_CHARACTERISTICS_SMPTE2084,
          .matrixCoefficients = AVIF_NCLX_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL,
#if AVIF_VERSION > 700
          .range = AVIF_RANGE_FULL,
#else
          .fullRangeFlag = AVIF_NCLX_FULL_RANGE,
#endif
        };
        break;
      case DT_COLORSPACE_HLG_P3:
        nclx = (avifNclxColorProfile) {
          .colourPrimaries = AVIF_NCLX_COLOUR_PRIMARIES_SMPTE432,
          .transferCharacteristics = AVIF_NCLX_TRANSFER_CHARACTERISTICS_HLG,
          .matrixCoefficients = AVIF_NCLX_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL,
#if AVIF_VERSION > 700
          .range = AVIF_RANGE_FULL,
#else
          .fullRangeFlag = AVIF_NCLX_FULL_RANGE,
#endif
        };
        break;
      default:
        break;
    }

    if (nclx.colourPrimaries != AVIF_NCLX_COLOUR_PRIMARIES_UNKNOWN) {
        avifImageSetProfileNCLX(image, &nclx);
        use_icc = TRUE;
    }
#endif
    dt_print(DT_DEBUG_IMAGEIO, "[avif colorprofile profile: %s - %s]\n",
             dt_colorspaces_get_name(over_type, filename),
             use_icc ? "icc" : "nclx");

    if (use_icc) {
      const dt_colorspaces_color_profile_t *cp =
        dt_colorspaces_get_output_profile(imgid,
                                          over_type,
                                          over_filename);
      cmsHPROFILE out_profile = cp->profile;

      cmsSaveProfileToMem(out_profile, 0, &icc_profile_len);
      if (icc_profile_len > 0) {
        icc_profile_data = malloc(icc_profile_len * sizeof(uint8_t));
        if (icc_profile_data == NULL) {
          rc = 1;
          goto out;
        }
        cmsSaveProfileToMem(out_profile, icc_profile_data, &icc_profile_len);
        avifImageSetProfileICC(image,
                               icc_profile_data,
                               icc_profile_len);
      }
    }
  }

  avifImageSetMetadataExif(image, exif, exif_len);

  encoder = avifEncoderCreate();
  if (encoder == NULL) {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to create AVIF encoder for image [%s]\n",
             filename);
    rc = 1;
    goto out;
  }

  switch (d->compression_type) {
  case AVIF_COMP_LOSSLESS:
    /* It isn't recommend to use the extremities */
    encoder->speed = AVIF_SPEED_SLOWEST + 1;

    encoder->minQuantizer = AVIF_QUANTIZER_LOSSLESS;
    encoder->maxQuantizer = AVIF_QUANTIZER_LOSSLESS;

    break;
  case AVIF_COMP_LOSSY:
    encoder->speed = AVIF_SPEED_DEFAULT;

    encoder->maxQuantizer = 100 - d->quality;
    encoder->maxQuantizer = CLAMP(encoder->maxQuantizer, 0, 63);

    encoder->minQuantizer = 64 - d->quality;
    encoder->minQuantizer = CLAMP(encoder->minQuantizer, 0, 63);

    break;
  }

  encoder->maxThreads = dt_get_num_threads();

  /*
   * Tiling reduces the image quality but it has a negligible impact on
   * still images.
   *
   * The minmum suggested size for a tile is 512x512.
   */
  switch (d->tiling) {
  case AVIF_TILING_ON: {
    size_t width_tile_size  = AVIF_DEFAULT_TILE_SIZE;
    size_t height_tile_size = AVIF_DEFAULT_TILE_SIZE;

    if (width >= 4096) {
        width_tile_size = AVIF_MAX_TILE_SIZE;
    }
    if (height >= 4096) {
        height_tile_size = AVIF_MAX_TILE_SIZE;
    }

    encoder->tileColsLog2 = flp2(width / width_tile_size);
    encoder->tileRowsLog2 = flp2(height / height_tile_size);
  }
  case AVIF_TILING_OFF:
    break;
  }

  dt_print(DT_DEBUG_IMAGEIO,
           "[avif quality: %u => maxQuantizer: %u, minQuantizer: %u, "
           "tileColsLog2: %u, tileRowsLog2: %u, threads: %u]\n",
           d->quality,
           encoder->maxQuantizer,
           encoder->minQuantizer,
           encoder->tileColsLog2,
           encoder->tileRowsLog2,
           encoder->maxThreads);

  avifRWData output = AVIF_DATA_EMPTY;

  result = avifEncoderWrite(encoder, image, &output);
  if (result != AVIF_RESULT_OK) {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to encode AVIF image [%s]: %s\n",
             filename, avifResultToString(result));
    rc = 1;
    goto out;
  }

  if (output.size == 0 || output.data == NULL) {
    dt_print(DT_DEBUG_IMAGEIO,
             "AVIF encoder returned empty data for [%s]\n",
             filename);
    rc = 1;
    goto out;
  }

  /*
   * Write image to disk
   */
  FILE *f = NULL;
  size_t cnt = 0;

  f = g_fopen(filename, "wb");
  if (f == NULL) {
    rc = 1;
    goto out;
  }

  cnt = fwrite(output.data, 1, output.size, f);
  fclose(f);
  if (cnt != output.size) {
    g_unlink(filename);
    rc = 1;
    goto out;
  }

  rc = 0; /* success */
out:
  avifRGBImageFreePixels(&rgb);
  avifImageDestroy(image);
  avifEncoderDestroy(encoder);
  avifRWDataFree(&output);
  free(icc_profile_data);

  return rc;
}


size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_avif_t);
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_avif_t *d = (dt_imageio_avif_t *)calloc(1, sizeof(dt_imageio_avif_t));

  if (d == NULL) {
    return NULL;
  }

  d->bit_depth = dt_conf_get_int("plugins/imageio/format/avif/bit_depth");
  if (d->bit_depth == 0 || d->bit_depth > 12) {
      d->bit_depth = 8;
  }

  d->color_mode = dt_conf_get_int("plugins/imageio/format/avif/color_mode");
  d->compression_type = dt_conf_get_int("plugins/imageio/format/avif/compression_type");

  switch (d->compression_type) {
  case AVIF_COMP_LOSSLESS:
    d->quality = 100;
    break;
  case AVIF_COMP_LOSSY:
    d->quality = dt_conf_get_int("plugins/imageio/format/avif/quality");
    if (d->quality > 100) {
        d->quality = 100;
    }
    break;
  }

  d->tiling = dt_conf_get_int("plugins/imageio/format/avif/tiling");

  return d;
}

int set_params(dt_imageio_module_format_t *self,
               const void *params,
               const int size)
{
  if (size != self->params_size(self)) {
      return 1;
  }
  const dt_imageio_avif_t *d = (dt_imageio_avif_t *)params;

  dt_imageio_avif_gui_t *g = (dt_imageio_avif_gui_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->bit_depth, d->bit_depth);
  dt_bauhaus_combobox_set(g->color_mode, d->color_mode);
  dt_bauhaus_combobox_set(g->tiling, d->tiling);
  dt_bauhaus_combobox_set(g->compression_type, d->compression_type);
  dt_bauhaus_slider_set(g->quality, d->quality);

  return 0;
}

void free_params(dt_imageio_module_format_t *self,
                 dt_imageio_module_data_t *params)
{
  free(params);
}


int bpp(struct dt_imageio_module_data_t *data)
{
  return 32; /* always request float */
}

int levels(struct dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB|IMAGEIO_FLOAT;
}

const char *mime(dt_imageio_module_data_t *data)
{
  return "image/avif";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "avif";
}

const char *name()
{
  return _("AVIF (8/10/12-bit)");
}

int flags(struct dt_imageio_module_data_t *data)
{
  return FORMAT_FLAGS_SUPPORT_XMP;
}

static void bit_depth_changed(GtkWidget *widget, gpointer user_data)
{
  const uint32_t idx = dt_bauhaus_combobox_get(widget);

  dt_conf_set_int("plugins/imageio/format/avif/bit_depth", avif_bit_depth[idx].bit_depth);
}

static void color_mode_changed(GtkWidget *widget, gpointer user_data)
{
  const enum avif_color_mode_e color_mode = dt_bauhaus_combobox_get(widget);

  dt_conf_set_int("plugins/imageio/format/avif/color_mode", color_mode);
}

static void tiling_changed(GtkWidget *widget, gpointer user_data)
{
  const enum avif_tiling_e tiling = dt_bauhaus_combobox_get(widget);

  dt_conf_set_int("plugins/imageio/format/avif/tiling", tiling);
}

static void compression_type_changed(GtkWidget *widget, gpointer user_data)
{
  const enum avif_compression_type_e compression_type = dt_bauhaus_combobox_get(widget);
  dt_imageio_module_format_t *module = (dt_imageio_module_format_t *)user_data;
  dt_imageio_avif_gui_t *gui = (dt_imageio_avif_gui_t *)module->gui_data;

  dt_conf_set_int("plugins/imageio/format/avif/compression_type", compression_type);

  switch (compression_type) {
  case AVIF_COMP_LOSSLESS:
    gtk_widget_set_sensitive(gui->quality, FALSE);
    break;
  case AVIF_COMP_LOSSY:
    gtk_widget_set_sensitive(gui->quality, TRUE);
    break;
  }
}

static void quality_changed(GtkWidget *slider, gpointer user_data)
{
  const uint32_t quality = (int)dt_bauhaus_slider_get(slider);
  dt_conf_set_int("plugins/imageio/format/avif/quality", quality);
}

void gui_init(dt_imageio_module_format_t *self)
{
  dt_imageio_avif_gui_t *gui =
      (dt_imageio_avif_gui_t *)malloc(sizeof(dt_imageio_avif_gui_t));
  const uint32_t bit_depth = dt_conf_get_int("plugins/imageio/format/avif/bit_depth");
  const enum avif_color_mode_e color_mode = dt_conf_get_int("plugins/imageio/format/avif/color_mode");
  const enum avif_tiling_e tiling = dt_conf_get_int("plugins/imageio/format/avif/tiling");
  const enum avif_compression_type_e compression_type = dt_conf_get_int("plugins/imageio/format/avif/compression_type");
  const uint32_t quality = dt_conf_get_int("plugins/imageio/format/avif/quality");

  self->gui_data = (void *)gui;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /*
   * Bit depth combo box
   */
  gui->bit_depth = dt_bauhaus_combobox_new(NULL);

  dt_bauhaus_widget_set_label(gui->bit_depth, NULL, _("bit depth"));
  size_t idx = 0;
  for (size_t i = 0; avif_bit_depth[i].name != NULL; i++) {
    dt_bauhaus_combobox_add(gui->bit_depth,  _(avif_bit_depth[i].name));
    if (avif_bit_depth[i].bit_depth == bit_depth) {
      idx = i;
    }
  }
  dt_bauhaus_combobox_set(gui->bit_depth, idx);

  gtk_widget_set_tooltip_text(gui->bit_depth,
          _("color information stored in an image, higher is better"));

  gtk_box_pack_start(GTK_BOX(self->widget), gui->bit_depth, TRUE, TRUE, 0);

  /*
   * Color mode combo box
   */
  gui->color_mode = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(gui->color_mode,
                              NULL,
                              _("color mode"));
  dt_bauhaus_combobox_add(gui->color_mode,
                          _("rgb colors"));
  dt_bauhaus_combobox_add(gui->color_mode,
                          _("grayscale"));
  dt_bauhaus_combobox_set(gui->color_mode, color_mode);

  gtk_widget_set_tooltip_text(gui->color_mode,
          _("Saving as grayscale will reduce the size for black & white images"));

  gtk_box_pack_start(GTK_BOX(self->widget),
                     gui->color_mode,
                     TRUE,
                     TRUE,
                     0);
  /*
   * Tiling combo box
   */
  gui->tiling = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(gui->tiling,
                              NULL,
                              _("tiling"));
  dt_bauhaus_combobox_add(gui->tiling,
                          _("on"));
  dt_bauhaus_combobox_add(gui->tiling,
                          _("off"));
  dt_bauhaus_combobox_set(gui->tiling, tiling);

  gtk_widget_set_tooltip_text(gui->tiling,
          _("tile an image into segments.\n"
            "\n"
            "makes encoding faster. the impact on quality reduction "
            "is negligible, but increases the file size."));

  gtk_box_pack_start(GTK_BOX(self->widget),
                     gui->tiling,
                     TRUE,
                     TRUE,
                     0);

  /*
   * Compression type combo box
   */
  gui->compression_type = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(gui->compression_type,
                              NULL,
                              _("compression type"));
  dt_bauhaus_combobox_add(gui->compression_type,
                          _(avif_get_compression_string(AVIF_COMP_LOSSLESS)));
  dt_bauhaus_combobox_add(gui->compression_type,
                          _(avif_get_compression_string(AVIF_COMP_LOSSY)));
  dt_bauhaus_combobox_set(gui->compression_type, compression_type);

  gtk_widget_set_tooltip_text(gui->compression_type,
          _("the compression for the image"));

  gtk_box_pack_start(GTK_BOX(self->widget),
                     gui->compression_type,
                     TRUE,
                     TRUE,
                     0);

  /*
   * Quality combo box
   */
  gui->quality = dt_bauhaus_slider_new_with_range(NULL,
                                                  5, /* min */
                                                  100, /* max */
                                                  1, /* step */
                                                  92, /* default */
                                                  0); /* digits */
  dt_bauhaus_widget_set_label(gui->quality, NULL, _("quality"));
  dt_bauhaus_slider_set_default(gui->quality, 95);
  dt_bauhaus_slider_set_format(gui->quality, "%.2f%%");

  gtk_widget_set_tooltip_text(gui->quality,
          _("the quality of an image, less quality means fewer details.\n"
            "\n"
            "the following applies only to lossy setting\n"
            "\n"
            "pixelformat based on quality:\n"
            "\n"
            "    91% - 100% -> YUV444\n"
            "    81% -  90% => YUV422\n"
            "     5% -  80% => YUV420\n"));

  if (quality > 0 && quality <= 100) {
      dt_bauhaus_slider_set(gui->quality, quality);
  }
  gtk_box_pack_start(GTK_BOX(self->widget), gui->quality, TRUE, TRUE, 0);

  switch (compression_type) {
  case AVIF_COMP_LOSSLESS:
    gtk_widget_set_sensitive(gui->quality, FALSE);
    break;
  case AVIF_COMP_LOSSY:
    break;
  }

  g_signal_connect(G_OBJECT(gui->bit_depth),
                   "value-changed",
                   G_CALLBACK(bit_depth_changed),
                   NULL);
  g_signal_connect(G_OBJECT(gui->color_mode),
                   "value-changed",
                   G_CALLBACK(color_mode_changed),
                   (gpointer)self);
  g_signal_connect(G_OBJECT(gui->tiling),
                   "value-changed",
                   G_CALLBACK(tiling_changed),
                   (gpointer)self);
  g_signal_connect(G_OBJECT(gui->compression_type),
                   "value-changed",
                   G_CALLBACK(compression_type_changed),
                   (gpointer)self);
  g_signal_connect(G_OBJECT(gui->quality),
                   "value-changed",
                   G_CALLBACK(quality_changed),
                   NULL);
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
  dt_imageio_avif_gui_t *gui = (dt_imageio_avif_gui_t *)self->gui_data;

  compression_type_changed(GTK_WIDGET(gui->compression_type), self);
  quality_changed(GTK_WIDGET(gui->quality), self);
  bit_depth_changed(GTK_WIDGET(gui->bit_depth), self);
}
