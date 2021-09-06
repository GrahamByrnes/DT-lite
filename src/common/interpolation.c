/* --------------------------------------------------------------------------
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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
* ------------------------------------------------------------------------*/

#include "common/interpolation.h"
#include "common/darktable.h"
#include "control/conf.h"

#include <assert.h>
#include <glib.h>
#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

/** Border extrapolation modes */
enum border_mode
{
  BORDER_REPLICATE, // aaaa|abcdefg|gggg
  BORDER_WRAP,      // defg|abcdefg|abcd
  BORDER_MIRROR,    // edcb|abcdefg|fedc
  BORDER_CLAMP      // ....|abcdefg|....
};

/* Supporting them all might be overkill, let the compiler trim all
 * unnecessary modes in clip for resampling codepath*/
#define RESAMPLING_BORDER_MODE BORDER_REPLICATE

/* Supporting them all might be overkill, let the compiler trim all
 * unnecessary modes in interpolation codepath */
#define INTERPOLATION_BORDER_MODE BORDER_MIRROR

// Defines the maximum kernel half length
// !! Make sure to sync this with the filter array !!
#define MAX_HALF_FILTER_WIDTH 3

/* --------------------------------------------------------------------------
 * Generic helpers
 * ------------------------------------------------------------------------*/

/** Compute ceil value of a float
 * @remark Avoid libc ceil for now. Maybe we'll revert to libc later.
 * @param x Value to ceil
 * @return ceil value
 */
static inline float ceil_fast(float x)
{
  if(x <= 0.f)
    return (float)(int)x;
  else
    return -((float)(int)-x) + 1.f;
}

/** Clip into specified range
 * @param idx index to filter
 * @param length length of line
 */
static inline int clip(int i, int min, int max, enum border_mode mode)
{
  switch(mode)
  {
    case BORDER_REPLICATE:
      if(i < min)
        i = min;
      else if(i > max)
        i = max;
      break;
    case BORDER_MIRROR:
      if(i < min)
        i = min - i;
      else if(i > max)
        i = 2 * max - i;
      break;

    case BORDER_WRAP:
      if(i < min)
        i = max - (min - i);
      else if(i > max)
        i = min + (i - max);
      break;

    case BORDER_CLAMP:
      if(i < min || i > max)
        /* Should not be used as is, we prevent -1 usage, filtering the taps
         * we clip the sample indexes for. So understand this function is
         * specific to its caller. */
        i = -1;
      break;
  }

  return i;
}

static inline void prepare_tap_boundaries(int *tap_first, int *tap_last, const enum border_mode mode,
                                          const int filterwidth, const int t, const int max)
{
  /* Check lower bound pixel index and skip as many pixels as necessary to
   * fall into range */
  *tap_first = 0;

  if(mode == BORDER_CLAMP && t < 0)
    *tap_first = -t;
  // Same for upper bound pixel
  *tap_last = filterwidth;

  if(mode == BORDER_CLAMP && t + filterwidth >= max)
    *tap_last = max - t;
}

/** Make sure an aligned chunk will not misalign its following chunk
 * proposing an adapted length
 *
 * @param l Length required for current chunk
 * @param align Required alignment for next chunk
 *
 * @return Required length for keeping alignment ok if chaining data chunks
 */
static inline size_t increase_for_alignment(size_t l, size_t align)
{
  align -= 1;
  return (l + align) & (~align);
}

/** Compute an approximate sine.
 * This function behaves correctly for the range [-pi pi] only.
 * It has the following properties:
 * <ul>
 *   <li>It has exact values for 0, pi/2, pi, -pi/2, -pi</li>
 *   <li>It has matching derivatives to sine for these same points</li>
 *   <li>Its relative error margin is <= 1% iirc</li>
 *   <li>It computational cost is 5 mults + 3 adds + 2 abs</li>
 * </ul>
 * @param t Radian parameter
 * @return guess what
 */
static inline float sinf_fast(float t)
{
  static const float a = 4 / (M_PI * M_PI);
  static const float p = 0.225f;

  t = a * t * (M_PI - fabsf(t));

  return t * (p * (fabsf(t) - 1) + 1);
}

/* --------------------------------------------------------------------------
 * Bilinear interpolation
 * ------------------------------------------------------------------------*/

static inline float bilinear(float width, float t)
{
  float r;
  t = fabsf(t);

  if(t > 1.f)
    r = 0.f;
  else
    r = 1.f - t;

  return r;
}

/* --------------------------------------------------------------------------
 * Bicubic interpolation
 * ------------------------------------------------------------------------*/

static inline float bicubic(float width, float t)
{
  float r;
  t = fabsf(t);
  if(t >= 2.f)
    r = 0.f;
  else if(t > 1.f && t < 2.f)
  {
    float t2 = t * t;
    r = 0.5f * (t * (-t2 + 5.f * t - 8.f) + 4.f);
  }
  else
  {
    float t2 = t * t;
    r = 0.5f * (t * (3.f * t2 - 5.f * t) + 2.f);
  }

  return r;
}

/* --------------------------------------------------------------------------
 * Lanczos interpolation
 * ------------------------------------------------------------------------*/

#define DT_LANCZOS_EPSILON (1e-9f)

/* Based on a forum entry at
 * http://devmaster.net/forums/topic/4648-fast-and-accurate-sinecosine/
 *  */

static inline float lanczos(float width, float t)
{
  // Compute a value for sinf(pi.t) in [-pi pi] for which the value will be correct
  int a = (int)t;
  float r = t - (float)a;
  // Compute the correct sign for sinf(pi.r)
  union
  {
    float f;
    uint32_t i;
  } sign;
  sign.i = ((a & 1) << 31) | 0x3f800000;

  return (DT_LANCZOS_EPSILON + width * sign.f * sinf_fast(M_PI * r) * sinf_fast(M_PI * t / width))
         / (DT_LANCZOS_EPSILON + M_PI * M_PI * t * t);
}
#undef DT_LANCZOS_EPSILON

/* --------------------------------------------------------------------------
 * All our known interpolators
 * ------------------------------------------------------------------------*/

 // Make sure MAX_HALF_FILTER_WIDTH is at least equal to the maximum width
static const struct dt_interpolation
  dt_interpolator[] = {
  {.id = DT_INTERPOLATION_BILINEAR,
   .name = "bilinear",
   .width = 1,
   .func = &bilinear,
  },
  {.id = DT_INTERPOLATION_BICUBIC,
   .name = "bicubic",
   .width = 2,
   .func = &bicubic,
  },
  {.id = DT_INTERPOLATION_LANCZOS2,
   .name = "lanczos2",
   .width = 2,
   .func = &lanczos,
  },
  {.id = DT_INTERPOLATION_LANCZOS3,
   .name = "lanczos3",
   .width = 3,
   .func = &lanczos,
  },
};

/* --------------------------------------------------------------------------
 * Kernel utility methods
 * ------------------------------------------------------------------------*/

/** Computes an upsampling filtering kernel
 *
 * @param itor [in] Interpolator used
 * @param kernel [out] resulting itor->width*2 filter taps
 * @param norm [out] Kernel norm
 * @param first [out] first input sample index used
 * @param t [in] Interpolated coordinate */
static inline void compute_upsampling_kernel(const struct dt_interpolation *itor, float *kernel,
                                                   float *norm, int *first, float t)
{
  int f = (int)t - itor->width + 1;
  if(first)
    *first = f;
  /* Find closest integer position and then offset that to match first
   * filtered sample position */
  t = t - (float)f;
  // Will hold kernel norm
  float n = 0.f;
  // Compute the raw kernel
  for(int i = 0; i < 2 * itor->width; i++)
  {
    float tap = itor->func((float)itor->width, t);
    n += tap;
    kernel[i] = tap;
    t -= 1.f;
  }
  if(norm)
    *norm = n;
}

/** Computes a downsampling filtering kernel
 *
 * @param itor [in] Interpolator used
 * @param kernelsize [out] Number of taps
 * @param kernel [out] resulting taps (at least itor->width/inoout elements for no overflow)
 * @param norm [out] Kernel norm
 * @param first [out] index of the first sample for which the kernel is to be applied
 * @param outoinratio [in] "out samples" over "in samples" ratio
 * @param xout [in] Output coordinate */
static inline void compute_downsampling_kernel(const struct dt_interpolation *itor, int *taps,
                                                     int *first, float *kernel, float *norm,
                                                     float outoinratio, int xout)
{
  // Keep this at hand
  float w = (float)itor->width;
  /* Compute the phase difference between output pixel and its
   * input corresponding input pixel */
  float xin = ceil_fast(((float)xout - w) / outoinratio);
  if(first)
    *first = (int)xin;
  // Compute first interpolator parameter
  float t = xin * outoinratio - (float)xout;
  // Will hold kernel norm
  float n = 0.f;
  // Compute all filter taps
  *taps = (int)((w - t) / outoinratio);

  for(int i = 0; i < *taps; i++)
  {
    *kernel = itor->func(w, t);
    n += *kernel;
    t += outoinratio;
    kernel++;
  }

  if(norm)
    *norm = n;
}

/* --------------------------------------------------------------------------
 * Sample interpolation function (see usage in iop/lens.c and iop/clipping.c)
 * ------------------------------------------------------------------------*/

#define MAX_KERNEL_REQ ((2 * (MAX_HALF_FILTER_WIDTH) + 3) & (~3))

float dt_interpolation_compute_sample(const struct dt_interpolation *itor, const float *in, const float x,
                                      const float y, const int width, const int height,
                                      const int samplestride, const int linestride)
{
  assert(itor->width < (MAX_HALF_FILTER_WIDTH + 1));

  float kernelh[MAX_KERNEL_REQ];
  float kernelv[MAX_KERNEL_REQ];
  // Compute both horizontal and vertical kernels
  float normh;
  float normv;
  compute_upsampling_kernel(itor, kernelh, &normh, NULL, x);
  compute_upsampling_kernel(itor, kernelv, &normv, NULL, y);
  int ix = (int)x;
  int iy = (int)y;
  // clip if pixel + filter outside image
  float r;
  const int itwidth = itor->width;

  if(ix >= (itwidth - 1) && iy >= (itwidth - 1) && ix < (width - itwidth)
     && iy < (height - itwidth))
  {
    // Inside image boundary case
    in = (float *)in + linestride * iy + ix * samplestride;
    in = in - (itwidth - 1) * (samplestride + linestride);
    // Apply the kernel
    float s = 0.f;

    for(int i = 0; i < 2 * itwidth; i++)
    {
      float h = 0.0f;

      for(int j = 0; j < 2 * itwidth; j++)
        h += kernelh[j] * in[j * samplestride];

      s += kernelv[i] * h;
      in += linestride;
    }

    r = s / (normh * normv);
  }
  else if(ix >= 0 && iy >= 0 && ix < width && iy < height)
  {
    // Point to the upper left pixel index wise
    iy -= itwidth - 1;
    ix -= itwidth - 1;
    static const enum border_mode bordermode = INTERPOLATION_BORDER_MODE;
    assert(bordermode != BORDER_CLAMP); // XXX in clamp mode, norms would be wrong

    int xtap_first;
    int xtap_last;
    prepare_tap_boundaries(&xtap_first, &xtap_last, bordermode, 2 * itwidth, ix, width);

    int ytap_first;
    int ytap_last;
    prepare_tap_boundaries(&ytap_first, &ytap_last, bordermode, 2 * itwidth, iy, height);
    // Apply the kernel
    float s = 0.f;

    for(int i = ytap_first; i < ytap_last; i++)
    {
      int clip_y = clip(iy + i, 0, height - 1, bordermode);
      float h = 0.0f;

      for(int j = xtap_first; j < xtap_last; j++)
      {
        int clip_x = clip(ix + j, 0, width - 1, bordermode);
        const float *ipixel = in + clip_y * linestride + clip_x * samplestride;
        h += kernelh[j] * ipixel[0];
      }

      s += kernelv[i] * h;
    }

    r = s / (normh * normv);
  }
  else // invalid coordinate
    r = 0.0f;

  return r;
}

/* --------------------------------------------------------------------------
 * Pixel interpolation function (see usage in iop/lens.c and iop/clipping.c)
 * ------------------------------------------------------------------------*/

static void dt_interpolation_compute_pixel_plain(const struct dt_interpolation *itor, const float *in,
                                                   float *out, const float x, const float y, const int width,
                                                   const int height, const int linestride, const int ch)
{
  assert(itor->width < (MAX_HALF_FILTER_WIDTH + 1));
  float kernelh[MAX_KERNEL_REQ];
  float kernelv[MAX_KERNEL_REQ];
  // Compute both horizontal and vertical kernels
  float normh;
  float normv;
  compute_upsampling_kernel(itor, kernelh, &normh, NULL, x);
  compute_upsampling_kernel(itor, kernelv, &normv, NULL, y);
  // Precompute the inverse of the filter norm for later use
  const float oonorm = (1.f / (normh * normv));
  // Must clip if falls outside image
  int ix = (int)x;
  int iy = (int)y;

  if(ix >= (itor->width - 1) && iy >= (itor->width - 1) && ix < (width - itor->width)
     && iy < (height - itor->width))
  {
    // Inside image boundary case, Go to top left pixel
    in = (float *)in + linestride * iy + ix * ch;
    in = in - (itor->width - 1) * (ch + linestride);
    float *pixel = calloc(ch, sizeof(float));

    for(int i = 0; i < 2 * itor->width; i++)
    {
      float *h = calloc(ch, sizeof(float));

      for(int j = 0; j < 2 * itor->width; j++)
        for(int c = 0; c < ch; c++)
          *(h + c) += kernelh[j] * in[j * ch + c];

      for(int c = 0; c < ch; c++)
        *(pixel + c) += kernelv[i] * *(h + c);

      free(h);

      in += linestride;
    }

    for(int c = 0; c < ch; c++)
      out[c] = oonorm * *(pixel + c);

    free(pixel);
  }
  else if(ix >= 0 && iy >= 0 && ix < width && iy < height)
  {
    // Point to the upper left pixel index wise
    iy -= itor->width - 1;
    ix -= itor->width - 1;
    static const enum border_mode bordermode = INTERPOLATION_BORDER_MODE;
    assert(bordermode != BORDER_CLAMP); // XXX in clamp mode, norms would be wrong

    int xtap_first;
    int xtap_last;
    prepare_tap_boundaries(&xtap_first, &xtap_last, bordermode, 2 * itor->width, ix, width);

    int ytap_first;
    int ytap_last;
    prepare_tap_boundaries(&ytap_first, &ytap_last, bordermode, 2 * itor->width, iy, height);
    // Apply the kernel
    float *pixel = calloc(ch, sizeof(float));

    for(int i = ytap_first; i < ytap_last; i++)
    {
      int clip_y = clip(iy + i, 0, height - 1, bordermode);
      float *h = calloc(ch, sizeof(float));

      for(int j = xtap_first; j < xtap_last; j++)
      {
        int clip_x = clip(ix + j, 0, width - 1, bordermode);
        const float *ipixel = in + clip_y * linestride + clip_x * ch;

        for(int c = 0; c < ch; c++)
          *(h + c) += kernelh[j] * ipixel[c];
      }

      for(int c = 0; c < ch; c++)
        *(pixel + c) += kernelv[i] * *(h + c);

      free(h);
    }

    for(int c = 0; c < 3; c++)
      out[c] = oonorm * *(pixel + c);

    free(pixel);
  }
  else
    for(int c = 0; c < ch; c++)
      out[c] = 0.0f;
}

void dt_interpolation_compute_pixel4c(const struct dt_interpolation *itor, const float *in, float *out,
                                      const float x, const float y, const int width, const int height,
                                      const int linestride)
{
    return dt_interpolation_compute_pixel_plain(itor, in, out, x, y, width, height, linestride, 4);
}

void dt_interpolation_compute_pixel1c(const struct dt_interpolation *itor, const float *in, float *out,
                                      const float x, const float y, const int width, const int height,
                                      const int linestride)
{
  return dt_interpolation_compute_pixel_plain(itor, in, out, x, y, width, height, linestride, 1);
}

/* --------------------------------------------------------------------------
 * Interpolation factory
 * ------------------------------------------------------------------------*/

const struct dt_interpolation *dt_interpolation_new(enum dt_interpolation_type type)
{
  const struct dt_interpolation *itor = NULL;

  if(type == DT_INTERPOLATION_USERPREF)
  {
    // Find user preferred interpolation method
    gchar *uipref = dt_conf_get_string("plugins/lighttable/export/pixel_interpolator");

    for(int i = DT_INTERPOLATION_FIRST; uipref && i < DT_INTERPOLATION_LAST; i++)
      if(!strcmp(uipref, dt_interpolator[i].name))
      {
        // Found the one
        itor = &dt_interpolator[i];
        break;
      }

    g_free(uipref);
    type = DT_INTERPOLATION_DEFAULT;
  }
  if(!itor)
  {
    // Did not find the userpref one or we've been asked for a specific one
    for(int i = DT_INTERPOLATION_FIRST; i < DT_INTERPOLATION_LAST; i++)
    {
      if(dt_interpolator[i].id == type)
      {
        itor = &dt_interpolator[i];
        break;
      }

      if(dt_interpolator[i].id == DT_INTERPOLATION_DEFAULT)
        itor = &dt_interpolator[i];
    }
  }

  return itor;
}

/* --------------------------------------------------------------------------
 * Image resampling
 * ------------------------------------------------------------------------*/

/** Prepares a 1D resampling plan
 *
 * This consists of the following information
 * <ul>
 * <li>A list of lengths that tell how many pixels are relevant for the
 *    next output</li>
 * <li>A list of required filter kernels</li>
 * <li>A list of sample indexes</li>
 * </ul>
 *
 * How to apply the resampling plan:
 * <ol>
 * <li>Pick a length from the length array</li>
 * <li>until length is reached
 *     <ol>
 *     <li>pick a kernel tap></li>
 *     <li>pick the relevant sample according to the picked index</li>
 *     <li>multiply them and accumulate</li>
 *     </ol>
 * </li>
 * <li>here goes a single output sample</li>
 * </ol>
 *
 * This until you reach the number of output pixels
 *
 * @param itor interpolator used to resample
 * @param in [in] Number of input samples
 * @param out [in] Number of output samples
 * @param plength [out] Array of lengths for each pixel filtering (number
 * of taps/indexes to use). This array mus be freed with dt_free_align() when you're
 * done with the plan.
 * @param pkernel [out] Array of filter kernel taps
 * @param pindex [out] Array of sample indexes to be used for applying each kernel tap
 * arrays of information
 * @param pmeta [out] Array of int triplets (length, kernel, index) telling where to start for an arbitrary
 * out position meta[3*out]
 * @return 0 for success, !0 for failure
 */
static int prepare_resampling_plan(const struct dt_interpolation *itor, int in, const int in_x0, int out,
                                   const int out_x0, float scale, int **plength, float **pkernel,
                                   int **pindex, int **pmeta)
{
  // Safe return values
  *plength = NULL;
  *pkernel = NULL;
  *pindex = NULL;

  if(pmeta)
    *pmeta = NULL;

  if(scale == 1.f)
    return 0;
  // Compute common upsampling/downsampling memory requirements
  int maxtapsapixel;

  if(scale > 1.f)
    maxtapsapixel = 2 * itor->width;
  else
    // Downscale... going for worst case values memory wise
    maxtapsapixel = ceil_fast((float)2 * (float)itor->width / scale);

  int nlengths = out;
  int nindex = maxtapsapixel * out;
  int nkernel = maxtapsapixel * out;
  size_t lengthreq = nlengths * sizeof(int);
  size_t indexreq = nindex * sizeof(int);
  size_t kernelreq = nkernel * sizeof(float);
  size_t scratchreq = maxtapsapixel * sizeof(float) + 4 * sizeof(float);
  // NB: because sse versions compute four taps a time
  size_t metareq = pmeta ? 3 * sizeof(int) * out : 0;

  void *blob = NULL;
  size_t totalreq = kernelreq + lengthreq + indexreq + scratchreq + metareq;
  blob = dt_alloc_align(64, totalreq);

  if(!blob)
    return 1;

  int *lengths = (int *)blob;
  blob = (char *)blob + lengthreq;
  int *index = (int *)blob;
  blob = (char *)blob + indexreq;
  float *kernel = (float *)blob;
  blob = (char *)blob + kernelreq;
  float *scratchpad = scratchreq ? (float *)blob : NULL;
  blob = (char *)blob + scratchreq;
  int *meta = metareq ? (int *)blob : NULL;

  // setting this as a const should help the compilers trim all unnecessary codepaths
  const enum border_mode bordermode = RESAMPLING_BORDER_MODE;
  
  if(scale > 1.f)
  {
    int kidx = 0;
    int iidx = 0;
    int lidx = 0;
    int midx = 0;

    for(int x = 0; x < out; x++)
    {
      if(meta)
      {
        meta[midx++] = lidx;
        meta[midx++] = kidx;
        meta[midx++] = iidx;
      }
      // Projected position in input samples
      float fx = (float)(out_x0 + x) / scale;
      // Compute the filter kernel at that position
      int first;
      compute_upsampling_kernel(itor, scratchpad, NULL, &first, fx);
      /* Check lower and higher bound pixel index and skip as many pixels as
       * necessary to fall into range */
      int tap_first;
      int tap_last;
      prepare_tap_boundaries(&tap_first, &tap_last, bordermode, 2 * itor->width, first, in);
      // Track number of taps that will be used
      lengths[lidx++] = tap_last - tap_first;
      // Precompute the inverse of the norm
      float norm = 0.f;

      for(int tap = tap_first; tap < tap_last; tap++)
        norm += scratchpad[tap];

      norm = 1.f / norm;
      first += tap_first;

      for(int tap = tap_first; tap < tap_last; tap++)
      {
        kernel[kidx++] = scratchpad[tap] * norm;
        index[iidx++] = clip(first++, 0, in - 1, bordermode);
      }
    }
  }
  else
  {
    int kidx = 0;
    int iidx = 0;
    int lidx = 0;
    int midx = 0;

    for(int x = 0; x < out; x++)
    {
      if(meta)
      {
        meta[midx++] = lidx;
        meta[midx++] = kidx;
        meta[midx++] = iidx;
      }
      // Compute downsampling kernel centered on output position
      int taps;
      int first;
      compute_downsampling_kernel(itor, &taps, &first, scratchpad, NULL, scale, out_x0 + x);
      /* Check lower and higher bound pixel index and skip as many pixels as
       * necessary to fall into range */
      int tap_first;
      int tap_last;
      prepare_tap_boundaries(&tap_first, &tap_last, bordermode, taps, first, in);
      // Track number of taps that will be used
      lengths[lidx++] = tap_last - tap_first;
      // Precompute the inverse of the norm
      float norm = 0.f;

      for(int tap = tap_first; tap < tap_last; tap++)
        norm += scratchpad[tap];

      norm = 1.f / norm;
      first += tap_first;

      for(int tap = tap_first; tap < tap_last; tap++)
      {
        kernel[kidx++] = scratchpad[tap] * norm;
        index[iidx++] = clip(first++, 0, in - 1, bordermode);
      }
    }
  }

  *plength = lengths;
  *pindex = index;
  *pkernel = kernel;

  if(pmeta)
    *pmeta = meta;

  return 0;
}

static void dt_interpolation_resample_plain(const struct dt_interpolation *itor, float *out,
                                            const dt_iop_roi_t *const roi_out, const int32_t out_stride,
                                            const float *const in, const dt_iop_roi_t *const roi_in,
                                            const int32_t in_stride, const int ch)
{
  int *hindex = NULL;
  int *hlength = NULL;
  float *hkernel = NULL;
  int *vindex = NULL;
  int *vlength = NULL;
  float *vkernel = NULL;
  int *vmeta = NULL;
  int r;
  // Fast code path for 1:1 copy, only cropping area can change
  if(roi_out->scale == 1.f)
  {
    const int x0 = roi_out->x * ch * sizeof(float);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(in, in_stride, out_stride, roi_out, x0) \
    shared(out)
#endif
    for(int y = 0; y < roi_out->height; y++)
      memcpy((char *)out + (size_t)out_stride * y,
             (char *)in + (size_t)in_stride * (y + roi_out->y) + x0,
             out_stride);

    return;
  }
  // Prepare resampling plans once and for all
  r = prepare_resampling_plan(itor, roi_in->width, roi_in->x, roi_out->width, roi_out->x, roi_out->scale,
                              &hlength, &hkernel, &hindex, NULL);
  if(r)
    goto exit;

  r = prepare_resampling_plan(itor, roi_in->height, roi_in->y, roi_out->height, roi_out->y, roi_out->scale,
                              &vlength, &vkernel, &vindex, &vmeta);
  if(r)
    goto exit;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, in_stride, out_stride, roi_out, ch) \
  shared(out, hindex, hlength, hkernel, vindex, vlength, vkernel, vmeta)
#endif
  for(int oy = 0; oy < roi_out->height; oy++)
  {
    // Initialize column resampling indexes
    int vlidx = vmeta[3 * oy + 0]; // V(ertical) L(ength) I(n)d(e)x
    int vkidx = vmeta[3 * oy + 1]; // V(ertical) K(ernel) I(n)d(e)x
    int viidx = vmeta[3 * oy + 2]; // V(ertical) I(ndex) I(n)d(e)x
    // Initialize row resampling indexes
    int hlidx = 0; // H(orizontal) L(ength) I(n)d(e)x
    int hkidx = 0; // H(orizontal) K(ernel) I(n)d(e)x
    int hiidx = 0; // H(orizontal) I(ndex) I(n)d(e)x
    // Number of lines contributing to the output line
    int vl = vlength[vlidx++]; // V(ertical) L(ength)
    // Process each output column
    for(int ox = 0; ox < roi_out->width; ox++)
    {
      float *vs = calloc(ch, sizeof(float));
      // Number of horizontal samples contributing to the output
      int hl = hlength[hlidx++]; // H(orizontal) L(ength)

      for(int iy = 0; iy < vl; iy++)
      {
        // This is our input line
        const float *i = (float *)((char *)in + (size_t)in_stride * vindex[viidx++]);
        float *vhs = calloc(ch, sizeof(float));

        for(int ix = 0; ix < hl; ix++)
        {
          // Apply the precomputed filter kernel
          size_t baseidx = (size_t)hindex[hiidx++] * ch;
          const float htap = hkernel[hkidx++];

          for(int c = 0; c < ch; c++)
            *(vhs + c) += i[baseidx + c] * htap;
        }
        // Accumulate contribution from this line
        const float vtap = vkernel[vkidx++];
        for(int c = 0; c < ch; c++) *(vs + c) += *(vhs + c) * vtap;
        // Reset horizontal resampling context
        hkidx -= hl;
        hiidx -= hl;
        free(vhs);
      }
      // Output pixel is ready
      float *o = (float *)((char *)out + (size_t)oy * out_stride + (size_t)ox * 4 * sizeof(float));
      for(int c = 0; c < ch; c++)
        o[c] = *(vs + c);
      // Reset vertical resampling context
      viidx -= vl;
      vkidx -= vl;
      // Progress in horizontal context
      hiidx += hl;
      hkidx += hl;
      free(vs);
    }
  }

exit:
  dt_free_align(hlength);
  dt_free_align(vlength);
}

/** Applies resampling (re-scaling) on *full* input and output buffers.
 *  roi_in and roi_out define the part of the buffers that is affected.
 */
void dt_interpolation_resample(const struct dt_interpolation *itor, float *out,
                               const dt_iop_roi_t *const roi_out, const int32_t out_stride,
                               const float *const in, const dt_iop_roi_t *const roi_in,
                               const int32_t in_stride)
{
  return dt_interpolation_resample_plain(itor, out, roi_out, out_stride, in, roi_in, in_stride, 4);
}

/** Applies resampling (re-scaling) on a specific region-of-interest of an image. The input
 *  and output buffers hold exactly those roi's. roi_in and roi_out define the relative
 *  positions of the roi's within the full input and output image, respectively.
 */
void dt_interpolation_resample_roi(const struct dt_interpolation *itor, float *out,
                                   const dt_iop_roi_t *const roi_out, const int32_t out_stride,
                                   const float *const in, const dt_iop_roi_t *const roi_in,
                                   const int32_t in_stride)
{
  dt_iop_roi_t oroi = *roi_out;
  oroi.x = oroi.y = 0;
  dt_iop_roi_t iroi = *roi_in;
  iroi.x = iroi.y = 0;

  dt_interpolation_resample_plain(itor, out, &oroi, out_stride, in, &iroi, in_stride, 4);
}

/** Applies resampling (re-scaling) on *full* input and output buffers.
 *  roi_in and roi_out define the part of the buffers that is affected.
 */
void dt_interpolation_resample_1c(const struct dt_interpolation *itor, float *out,
                                  const dt_iop_roi_t *const roi_out, const int32_t out_stride,
                                  const float *const in, const dt_iop_roi_t *const roi_in,
                                  const int32_t in_stride)
{
  return dt_interpolation_resample_plain(itor, out, roi_out, out_stride, in, roi_in, in_stride, 1);
}

/** Applies resampling (re-scaling) on a specific region-of-interest of an image. The input
 *  and output buffers hold exactly those roi's. roi_in and roi_out define the relative
 *  positions of the roi's within the full input and output image, respectively.
 */
void dt_interpolation_resample_roi_1c(const struct dt_interpolation *itor, float *out,
                                      const dt_iop_roi_t *const roi_out, const int32_t out_stride,
                                      const float *const in, const dt_iop_roi_t *const roi_in,
                                      const int32_t in_stride)
{
  dt_iop_roi_t oroi = *roi_out;
  oroi.x = oroi.y = 0;
  dt_iop_roi_t iroi = *roi_in;
  iroi.x = iroi.y = 0;

  dt_interpolation_resample_plain(itor, out, &oroi, out_stride, in, &iroi, in_stride, 1);
}
