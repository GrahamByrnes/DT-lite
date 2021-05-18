void expose(dt_view_t *self, cairo_t *cri, int32_t width, int32_t height,int32_t pointerx,int32_t pointery)
{
  cairo_set_source_rgb(cri, .2, .2, .2);
  cairo_save(cri);
  dt_develop_t *dev = (dt_develop_t *)self->data;
  const int32_t tb = dev->border_size;
  // account for border, make it transparent for other modules called below:
  pointerx -= tb;
  pointery -= tb;

  if(dev->gui_synch && !dev->image_loading)
  {
    // synch module guis from gtk thread:
    ++darktable.gui->reset;
    GList *modules = dev->iop;
    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      dt_iop_gui_update(module);
      modules = g_list_next(modules);
    }
    --darktable.gui->reset;
    dev->gui_synch = 0;
  }

  if(dev->image_status == DT_DEV_PIXELPIPE_DIRTY || dev->image_status == DT_DEV_PIXELPIPE_INVALID
     || dev->pipe->input_timestamp < dev->preview_pipe->input_timestamp)
    dt_dev_process_image(dev);

  if(dev->preview_status == DT_DEV_PIXELPIPE_DIRTY || dev->preview_status == DT_DEV_PIXELPIPE_INVALID
     || dev->pipe->input_timestamp > dev->preview_pipe->input_timestamp)
    dt_dev_process_preview(dev);

  if(dev->preview2_status == DT_DEV_PIXELPIPE_DIRTY || dev->preview2_status == DT_DEV_PIXELPIPE_INVALID
     || dev->pipe->input_timestamp > dev->preview2_pipe->input_timestamp)
    dt_dev_process_preview2(dev);

  dt_pthread_mutex_t *mutex = NULL;
  int stride;
  const float zoom_y = dt_control_get_dev_zoom_y();
  const float zoom_x = dt_control_get_dev_zoom_x();
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float backbuf_scale = dt_dev_get_zoom_scale(dev, zoom, 1.0f, 0) * darktable.gui->ppd;

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
  // adjust scroll bars
  float zx = zoom_x, zy = zoom_y, boxw = 1.0f, boxh = 1.0f;
  dt_dev_check_zoom_bounds(dev, &zx, &zy, zoom, closeup, &boxw, &boxh);

  if(boxw > 0.95f)
  {
    zx = .0f;
    boxw = 1.01f;
  }
  if(boxh > 0.95f)
  {
    zy = .0f;
    boxh = 1.01f;
  }

  dt_view_set_scrollbar(self, zx, -0.5 + boxw/2, 0.5, boxw/2, zy, -0.5+ boxh/2, 0.5, boxh/2);

  if(dev->pipe->output_backbuf && // do we have an image?
    dev->pipe->output_imgid == dev->image_storage.id && // is the right image?
    dev->pipe->backbuf_scale == backbuf_scale && // is this the zoom scale we want to display?
    dev->pipe->backbuf_zoom_x == zoom_x && dev->pipe->backbuf_zoom_y == zoom_y)
  {
    // draw image
    mutex = &dev->pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);
    float wd = dev->pipe->output_backbuf_width;
    float ht = dev->pipe->output_backbuf_height;
    stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, wd);
    surface = dt_cairo_image_surface_create_for_data(dev->pipe->output_backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride);
    wd /= darktable.gui->ppd;
    ht /= darktable.gui->ppd;

    if(dev->iso_12646.enabled)
      // force middle grey in background
      cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    else
    {
      if(dev->full_preview)
        dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_PREVIEW_BG);
      else
        dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);
    }

    cairo_paint(cr);
    cairo_translate(cr, ceilf(.5f * (width - wd)), ceilf(.5f * (height - ht)));

    if(closeup)
    {
      const double scale = 1<<closeup;
      cairo_scale(cr, scale, scale);
      cairo_translate(cr, -(.5 - 0.5/scale) * wd, -(.5 - 0.5/scale) * ht);
    }

    if(dev->iso_12646.enabled)
    {
      // draw the white frame around picture
      cairo_rectangle(cr, -tb / 3., -tb / 3.0, wd + 2. * tb / 3., ht + 2. * tb / 3.);
      cairo_set_source_rgb(cr, 1., 1., 1.);
      cairo_fill(cr);
    }

    cairo_rectangle(cr, 0, 0, wd, ht);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), darktable.gui->filter_image);
    cairo_paint(cr);

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
  // if we are in full preview mode, we don"t want anything else than the image 
  if(dev->full_preview)
    return;
  


  {
    if(dev->form_visible && display_masks)
      dt_masks_events_post_expose(dev->gui_module, cri, width, height, pointerx, pointery);
    // module
    if(dev->gui_module && dev->gui_module->gui_post_expose)
      dev->gui_module->gui_post_expose(dev->gui_module, cri, width, height, pointerx, pointery);
  }
  
}