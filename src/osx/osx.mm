/*
    This file is part of darktable,

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

#include <Carbon/Carbon.h>
#include <ApplicationServices/ApplicationServices.h>
#include <CoreServices/CoreServices.h>
#include <AppKit/AppKit.h>
#include <gtk/gtk.h>
#include <gdk/gdkquartz.h>
#include <glib.h>
#ifdef MAC_INTEGRATION
#include <gtkosxapplication.h>
#endif
#include "osx.h"

void dt_osx_autoset_dpi(GtkWidget *widget)
{
#if 0
  GdkScreen *screen = gtk_widget_get_screen(widget);
  if(!screen)
    screen = gdk_screen_get_default();
  if(!screen)
    return;

  CGDirectDisplayID id = CGMainDisplayID();
  CGSize size_in_mm = CGDisplayScreenSize(id);
  int width = CGDisplayPixelsWide(id);
  int height = CGDisplayPixelsHigh(id);
  gdk_screen_set_resolution(screen,
      25.4 * sqrt(width * width + height * height)
           / sqrt(size_in_mm.width * size_in_mm.width + size_in_mm.height * size_in_mm.height));
#endif
}

float dt_osx_get_ppd()
{
  NSScreen *nsscreen = [NSScreen mainScreen];
#if MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_7
  if([nsscreen respondsToSelector: NSSelectorFromString(@"backingScaleFactor")]) {
    return [[nsscreen valueForKey: @"backingScaleFactor"] floatValue];
  } else {
    return [[nsscreen valueForKey: @"userSpaceScaleFactor"] floatValue];
  }
#else
  return [[nsscreen valueForKey: @"userSpaceScaleFactor"] floatValue];
#endif
}

#if !GTK_CHECK_VERSION(3, 24, 14)
static void dt_osx_disable_fullscreen(GtkWidget *widget)
{
#ifdef GDK_WINDOWING_QUARTZ
  GdkWindow *window = gtk_widget_get_window(widget);
  if(window) {
    NSWindow *native = gdk_quartz_window_get_nswindow(window);
    [native setCollectionBehavior: ([native collectionBehavior] & ~NSWindowCollectionBehaviorFullScreenPrimary) | NSWindowCollectionBehaviorFullScreenAuxiliary];
  }
#endif
}
#endif

void dt_osx_disallow_fullscreen(GtkWidget *widget)
{
#if !GTK_CHECK_VERSION(3, 24, 14)
#ifdef GDK_WINDOWING_QUARTZ
  if(gtk_widget_get_realized(widget))
    dt_osx_disable_fullscreen(widget);
  else
    g_signal_connect(G_OBJECT(widget), "realize", G_CALLBACK(dt_osx_disable_fullscreen), NULL);
#endif
#endif
}

gboolean dt_osx_file_trash(const char *filename, GError **error)
{
  @autoreleasepool {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSError *err;

    NSURL *url = [NSURL fileURLWithPath:@(filename)];

    if ([fm respondsToSelector:@selector(trashItemAtURL:resultingItemURL:error:)]) {
      if (![fm trashItemAtURL:url resultingItemURL:nil error:&err]) {
        if (error != NULL)
          *error = g_error_new_literal(G_IO_ERROR, err.code == NSFileNoSuchFileError ? G_IO_ERROR_NOT_FOUND : G_IO_ERROR_FAILED, err.localizedDescription.UTF8String);
        return FALSE;
      }
    } else {
      if (error != NULL)
        *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "trash not supported on OS X versions < 10.8");
      return FALSE;
    }
  }
  return TRUE;
}

char* dt_osx_get_bundle_res_path()
{
  char *result = NULL;
#ifdef MAC_INTEGRATION
  gchar *bundle_id;

#ifdef GTK_TYPE_OSX_APPLICATION
  bundle_id = quartz_application_get_bundle_id();
  if(bundle_id)
    result = quartz_application_get_resource_path();
#else
  bundle_id = gtkosx_application_get_bundle_id();
  if(bundle_id)
    result = gtkosx_application_get_resource_path();
#endif
  g_free(bundle_id);

#endif

  return result;
}

void dt_osx_prepare_environment()
{
  char* res_path = dt_osx_get_bundle_res_path();
  if(res_path)
  {
    g_setenv("GTK_DATA_PREFIX", res_path, TRUE);
    g_setenv("GTK_EXE_PREFIX", res_path, TRUE);
    g_setenv("GTK_PATH", res_path, TRUE);
    {
      gchar* etc_path = g_build_filename(res_path, "etc", NULL);
      g_setenv("XDG_CONFIG_DIRS", etc_path, TRUE);
      {
        gchar* gtk_im_path = g_build_filename(etc_path, "gtk-3.0", "gtk.immodules", NULL);
        g_setenv("GTK_IM_MODULE_FILE", gtk_im_path, TRUE);
        g_free(gtk_im_path);
      }
      {
        gchar* pixbuf_path = g_build_filename(etc_path, "gtk-3.0", "loaders.cache", NULL);
        g_setenv("GDK_PIXBUF_MODULE_FILE", pixbuf_path, TRUE);
        g_free(pixbuf_path);
      }
      g_free(etc_path);
    }
    {
      gchar* share_path = g_build_filename(res_path, "share", NULL);
      g_setenv("XDG_DATA_DIRS", share_path, TRUE);
      {
        gchar* schema_path = g_build_filename(share_path, "glib-2.0", "schemas", NULL);
        g_setenv("GSETTINGS_SCHEMA_DIR", schema_path, TRUE);
        g_free(schema_path);
      }
      g_free(share_path);
    }
    {
      gchar* lib_path = g_build_filename(res_path, "lib", NULL);
      {
        gchar* io_path = g_build_filename(lib_path, "libgphoto2_port", NULL);
        g_setenv("IOLIBS", io_path, TRUE);
        g_free(io_path);
      }
      {
        gchar* cam_path = g_build_filename(lib_path, "libgphoto2", NULL);
        g_setenv("CAMLIBS", cam_path, TRUE);
        g_free(cam_path);
      }
      g_free(lib_path);
    }
    g_free(res_path);
  }
}

void dt_osx_focus_window()
{
  [NSApp activateIgnoringOtherApps:YES];
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
