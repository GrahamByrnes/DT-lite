static void second_window_expose(GtkWidget *widget, dt_develop_t *dev, cairo_t *cri, int32_t width, int32_t height,
                                 int32_t pointerx, int32_t pointery)
{
  cairo_set_source_rgb(cri, .2, .2, .2);
  cairo_save(cri);

  const int32_t tb = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size_win2"));
  // account for border, make it transparent for other modules called below:
  pointerx -= tb;
  pointery -= tb;

  if(dev->preview2_status == DT_DEV_PIXELPIPE_DIRTY || dev->preview2_status == DT_DEV_PIXELPIPE_INVALID
     || dev->pipe->input_timestamp > dev->preview2_pipe->input_timestamp)
    dt_dev_process_preview2(dev);

  dt_pthread_mutex_t *mutex = NULL;
  const float zoom_y = dt_second_window_get_dev_zoom_y(dev);
  const float zoom_x = dt_second_window_get_dev_zoom_x(dev);
  const dt_dev_zoom_t zoom = dt_second_window_get_dev_zoom(dev);
  const int closeup = dt_second_window_get_dev_closeup(dev);
  const float backbuf_scale = dt_second_window_get_zoom_scale(dev, zoom, 1.0f, 0) * dev->second_window.ppd;

  static cairo_surface_t *image_surface = NULL;
  static int image_surface_width = 0, image_surface_height = 0, image_surface_imgid = -1;

  if(image_surface_width != width || image_surface_height != height || image_surface == NULL)
  {
    // create double-buffered image to draw on, to make modules draw more fluently.
    image_surface_width = width;
    image_surface_height = height;
    if(image_surface) cairo_surface_destroy(image_surface);
    image_surface = dt_cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    image_surface_imgid = -1; // invalidate old stuff
  }
  cairo_surface_t *surface;
  cairo_t *cr = cairo_create(image_surface);

  if(dev->preview2_pipe->output_backbuf && // do we have an image?
    dev->preview2_pipe->backbuf_scale == backbuf_scale && // is this the zoom scale we want to display?
    dev->preview2_pipe->backbuf_zoom_x == zoom_x && dev->preview2_pipe->backbuf_zoom_y == zoom_y)
  {
    // draw image
    mutex = &dev->preview2_pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);
    float wd = dev->preview2_pipe->output_backbuf_width;
    float ht = dev->preview2_pipe->output_backbuf_height;
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, wd);
    surface = dt_cairo_image_surface_create_for_data(dev->preview2_pipe->output_backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride);
    wd /= dev->second_window.ppd;
    ht /= dev->second_window.ppd;
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);
    cairo_paint(cr);
    cairo_translate(cr, .5f * (width - wd), .5f * (height - ht));

    if(closeup)
    {
      const double scale = 1<<closeup;
      cairo_scale(cr, scale, scale);
      cairo_translate(cr, -(.5 - 0.5/scale) * wd, -(.5 - 0.5/scale) * ht);
    }

    cairo_rectangle(cr, 0, 0, wd, ht);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), darktable.gui->filter_image);
    cairo_fill(cr);

    cairo_surface_destroy(surface);
    dt_pthread_mutex_unlock(mutex);
    image_surface_imgid = dev->image_storage.id;
  }
  else if(dev->preview_pipe->output_backbuf)
  {
    // draw preview
    mutex = &dev->preview_pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);

    const float wd = dev->preview_pipe->output_backbuf_width;
    const float ht = dev->preview_pipe->output_backbuf_height;
    const float zoom_scale = dt_second_window_get_zoom_scale(dev, zoom, 1 << closeup, 1);
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);
    cairo_paint(cr);
    cairo_rectangle(cr, tb, tb, width - 2 * tb, height - 2 * tb);
    cairo_clip(cr);
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, wd);
    surface = cairo_image_surface_create_for_data(dev->preview_pipe->output_backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride);
    cairo_translate(cr, width / 2.0, height / 2.0f);
    cairo_scale(cr, zoom_scale, zoom_scale);
    cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);
    // avoid to draw the 1px garbage that sometimes shows up in the preview :(
    cairo_rectangle(cr, 0, 0, wd - 1, ht - 1);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), darktable.gui->filter_image);
    cairo_fill(cr);
    cairo_surface_destroy(surface);
    dt_pthread_mutex_unlock(mutex);
    image_surface_imgid = dev->image_storage.id;
  }

  cairo_restore(cri);

  if(image_surface_imgid == dev->image_storage.id)
  {
    cairo_destroy(cr);
    cairo_set_source_surface(cri, image_surface, 0, 0);
    cairo_paint(cri);
  }
}
