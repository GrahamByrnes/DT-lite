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

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "develop/pixelpipe_hb.h"
#include "external/libxcf/xcf.h"
#include "imageio/format/imageio_format_api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE(1)

// TODO:
//   - exif / xmp:
//        GIMP uses a custom way of serializing the data. see libgimpbase/gimpmetadata.c:gimp_metadata_serialize()

typedef struct dt_imageio_xcf_gui_t
{
  GtkWidget *bpp;
} dt_imageio_xcf_gui_t;

typedef struct dt_imageio_xcf_t
{
  dt_imageio_module_data_t global;
  int bpp;
} dt_imageio_xcf_t;

int write_image(dt_imageio_module_data_t *data, const char *filename, const void *ivoid,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                void *exif, int exif_len, int imgid, int num, int total, struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  const dt_imageio_xcf_t *const d = (dt_imageio_xcf_t *)data;

  int res = 1;

  uint8_t *profile = NULL;
  uint32_t profile_len = 0;
  gboolean profile_is_linear = TRUE;

  if(imgid > 0)
  {
    cmsHPROFILE out_profile = dt_colorspaces_get_output_profile(imgid, over_type, over_filename)->profile;
    cmsSaveProfileToMem(out_profile, 0, &profile_len);
    if(profile_len > 0)
    {
      profile = malloc(profile_len);
      if(!profile)
      {
        fprintf(stderr, "[xcf] error: can't allocate %u bytes of memory\n", profile_len);
        return 1;
      }
      cmsSaveProfileToMem(out_profile, profile, &profile_len);

      // try to figure out if the profile is linear
      if(cmsIsMatrixShaper(out_profile))
      {
        const cmsToneCurve *red_curve = (cmsToneCurve *)cmsReadTag(out_profile, cmsSigRedTRCTag);
        const cmsToneCurve *green_curve = (cmsToneCurve *)cmsReadTag(out_profile, cmsSigGreenTRCTag);
        const cmsToneCurve *blue_curve = (cmsToneCurve *)cmsReadTag(out_profile, cmsSigBlueTRCTag);
        if(red_curve && green_curve && blue_curve)
        {
          profile_is_linear = cmsIsToneCurveLinear(red_curve)
                              && cmsIsToneCurveLinear(green_curve)
                              && cmsIsToneCurveLinear(blue_curve);
        }
      }
    }
  }


  XCF *xcf = xcf_open(filename);

  if(!xcf)
  {
    fprintf(stderr, "[xcf] error: can't open `%s'\n", filename);
    goto exit;
  }

  xcf_set(xcf, XCF_BASE_TYPE, XCF_BASE_TYPE_RGB);
  xcf_set(xcf, XCF_WIDTH, d->global.width);
  xcf_set(xcf, XCF_HEIGHT, d->global.height);

  if(d->bpp == 8)
    xcf_set(xcf, XCF_PRECISION, profile_is_linear ? XCF_PRECISION_I_8_L : XCF_PRECISION_I_8_G);
  else if(d->bpp == 16)
    xcf_set(xcf, XCF_PRECISION, profile_is_linear ? XCF_PRECISION_I_16_L : XCF_PRECISION_I_16_G);
  else if(d->bpp == 32)
    xcf_set(xcf, XCF_PRECISION, profile_is_linear ? XCF_PRECISION_F_32_L : XCF_PRECISION_F_32_G);
  else
  {
    fprintf(stderr, "[xcf] error: bpp of %d is not supported\n", d->bpp);
    goto exit;
  }

  if(profile)
  {
    xcf_set(xcf, XCF_PROP, XCF_PROP_PARASITES, "icc-profile", XCF_PARASITE_PERSISTENT | XCF_PARASITE_UNDOABLE,
            profile_len, profile);
  }

  xcf_set(xcf, XCF_N_LAYERS, 1);
  int n_channels = 0;
  if(export_masks && pipe)
  {
    for(GList *iter = pipe->nodes; iter; iter = g_list_next(iter))
      n_channels += g_hash_table_size(((dt_dev_pixelpipe_iop_t *)iter->data)->raster_masks);
  }
  xcf_set(xcf, XCF_N_CHANNELS, n_channels);
  xcf_set(xcf, XCF_OMIT_BASE_ALPHA, 1);

  char *comment = g_strdup_printf("Created with %s", darktable_package_string);
  xcf_set(xcf, XCF_PROP, XCF_PROP_PARASITES, "gimp-comment", XCF_PARASITE_PERSISTENT, strlen(comment) + 1, comment);
  g_free(comment);

  // TODO: this needs to be serialized, together with the exif data
//   char *xmp_string = dt_exif_xmp_read_string(imgid);
//   if(xmp_string)
//   {
//     xcf_set(xcf, XCF_PROP, XCF_PROP_PARASITES, "gimp-metadata", XCF_PARASITE_PERSISTENT,
//             strlen(xmp_string) + 1, xmp_string);
//     g_free(xmp_string);
//   }

  xcf_add_layer(xcf);
  xcf_set(xcf, XCF_WIDTH, d->global.width);
  xcf_set(xcf, XCF_HEIGHT, d->global.height);
  xcf_set(xcf, XCF_NAME, _("image"));
  // we only add one layer and omit its alpha channel. thus we can just pass ivoid and ignore its 4th channel!
  xcf_add_data(xcf, ivoid, 4);

  if(n_channels > 0)
    for(GList *iter = pipe->nodes; iter; iter = g_list_next(iter))
    {
      dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)iter->data;

      GHashTableIter rm_iter;
      gpointer key, value;

      g_hash_table_iter_init(&rm_iter, piece->raster_masks);
      while(g_hash_table_iter_next(&rm_iter, &key, &value))
      {
        gboolean free_mask = TRUE;
        float *raster_mask = dt_dev_get_raster_mask(pipe, piece->module, GPOINTER_TO_INT(key), NULL, &free_mask);

        if(!raster_mask)
        {
          // this should never happen
          fprintf(stderr, "error: can't get raster mask from `%s'\n", piece->module->name());
          goto exit;
        }

        xcf_add_channel(xcf);
        xcf_set(xcf, XCF_PROP, XCF_PROP_VISIBLE, 0);

        const char *pagename = g_hash_table_lookup(piece->module->raster_mask.source.masks, key);
        if(pagename)
          xcf_set(xcf, XCF_NAME, pagename);
        else
          xcf_set(xcf, XCF_NAME, piece->module->name());

        void *channel_data = NULL;
        gboolean free_channel_data = TRUE;
        if(d->bpp == 8)
        {
          channel_data = malloc((size_t)d->global.width * d->global.height * sizeof(uint8_t));
          uint8_t *ch = (uint8_t *)channel_data;
          for(size_t i = 0; i < (size_t)d->global.width * d->global.height; i++)
            ch[i] = CLAMP((int)(raster_mask[i] * 255.0), 0, 255);
        }
        else if(d->bpp == 16)
        {
          channel_data = malloc((size_t)d->global.width * d->global.height * sizeof(uint16_t));
          uint16_t *ch = (uint16_t *)channel_data;
          for(size_t i = 0; i < (size_t)d->global.width * d->global.height; i++)
            ch[i] = CLAMP((int)(raster_mask[i] * 65535.0), 0, 65535);
        }
        else if(d->bpp == 32)
        {
          channel_data = raster_mask;
          free_channel_data = FALSE;
        }

        xcf_add_data(xcf, channel_data, 1);

        if(free_channel_data)
          free(channel_data);
        if(free_mask)
          dt_free_align(raster_mask);
      }
    }

  res = 0;

exit:
  xcf_close(xcf);
  free(profile);

  return res;

}

size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_xcf_t);
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_xcf_t *d = (dt_imageio_xcf_t *)calloc(1, sizeof(dt_imageio_xcf_t));

  d->bpp = dt_conf_get_int("plugins/imageio/format/xcf/bpp");
  if(d->bpp != 16 && d->bpp != 32)
    d->bpp = 8;

  return d;
}

void free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int set_params(dt_imageio_module_format_t *self, const void *params, int size)
{
  if(size != params_size(self)) return 1;
  const dt_imageio_xcf_t *d = (dt_imageio_xcf_t *)params;
  const dt_imageio_xcf_gui_t *g = (dt_imageio_xcf_gui_t *)self->gui_data;

  if(d->bpp == 16)
    dt_bauhaus_combobox_set(g->bpp, 1);
  else if(d->bpp == 32)
    dt_bauhaus_combobox_set(g->bpp, 2);
  else // (d->bpp == 8)
    dt_bauhaus_combobox_set(g->bpp, 0);

  return 0;
}

int flags(dt_imageio_module_data_t *data)
{
  return FORMAT_FLAGS_SUPPORT_LAYERS;
}

int bpp(dt_imageio_module_data_t *p)
{
  return ((dt_imageio_xcf_t *)p)->bpp;
}

int levels(dt_imageio_module_data_t *p)
{
  const int bpp = ((dt_imageio_xcf_t *)p)->bpp;
  int ret = IMAGEIO_RGB;

  if(bpp == 8)
    ret |= IMAGEIO_INT8;
  else if(bpp == 16)
    ret |= IMAGEIO_INT16;
  else if(bpp == 32)
    ret |= IMAGEIO_FLOAT;

  return ret;
}

const char *mime(dt_imageio_module_data_t *data)
{
  return "image/x-xcf";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "xcf";
}

const char *name()
{
  return _("xcf");
}

void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_xcf_t, bpp, int);
#endif
}
void cleanup(dt_imageio_module_format_t *self)
{
}

static void bpp_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  const int bpp = dt_bauhaus_combobox_get(widget);

  if(bpp == 1)
    dt_conf_set_int("plugins/imageio/format/xcf/bpp", 16);
  else if(bpp == 2)
    dt_conf_set_int("plugins/imageio/format/xcf/bpp", 32);
  else // (bpp == 0)
    dt_conf_set_int("plugins/imageio/format/xcf/bpp", 8);
}

void gui_init(dt_imageio_module_format_t *self)
{
  dt_imageio_xcf_gui_t *gui = (dt_imageio_xcf_gui_t *)malloc(sizeof(dt_imageio_xcf_gui_t));
  self->gui_data = (void *)gui;

  int bpp = 32;
  if(dt_conf_key_exists("plugins/imageio/format/xcf/bpp"))
    bpp = dt_conf_get_int("plugins/imageio/format/xcf/bpp");

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // Bit depth combo box
  gui->bpp = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(gui->bpp, NULL, _("bit depth"));
  dt_bauhaus_combobox_add(gui->bpp, _("8 bit"));
  dt_bauhaus_combobox_add(gui->bpp, _("16 bit"));
  dt_bauhaus_combobox_add(gui->bpp, _("32 bit (float)"));
  if(bpp == 16)
    dt_bauhaus_combobox_set(gui->bpp, 1);
  else if(bpp == 32)
    dt_bauhaus_combobox_set(gui->bpp, 2);
  else // (bpp == 8)
    dt_bauhaus_combobox_set(gui->bpp, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), gui->bpp, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(gui->bpp), "value-changed", G_CALLBACK(bpp_combobox_changed), NULL);
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
