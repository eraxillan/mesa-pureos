/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2017 Samsung Electronics co., Ltd. All Rights Reserved
 *
 * Based on platform_android, which has
 *
 * Copyright (C) 2010-2011 Chia-I Wu <olvaffe@gmail.com>
 * Copyright (C) 2010-2011 LunarG Inc.
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Gwan-gyeong Mun <kk.moon@samsung.com>
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <xf86drm.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "egl_dri2.h"
#include "egl_dri2_fallbacks.h"
#include "loader.h"

#include <wayland-client.h>

#include <drm_fourcc.h>

int tbm_bufmgr_fd = -1;
struct rgba_masks {
   unsigned int red;
   unsigned int green;
   unsigned int blue;
   unsigned int alpha;
};

static void tizen_free_local_buffers(struct dri2_egl_surface *dri2_surf);

/* createImageFromFds requires fourcc format */
static int get_fourcc(tbm_format format)
{
   switch (format) {
   case TBM_FORMAT_RGB565:   return __DRI_IMAGE_FOURCC_RGB565;
   case TBM_FORMAT_BGRA8888: return __DRI_IMAGE_FOURCC_ARGB8888;
   case TBM_FORMAT_RGBA8888: return __DRI_IMAGE_FOURCC_ABGR8888;
   case TBM_FORMAT_ARGB8888: return __DRI_IMAGE_FOURCC_ARGB8888;
   case TBM_FORMAT_ABGR8888: return __DRI_IMAGE_FOURCC_ABGR8888;
   case TBM_FORMAT_RGBX8888: return __DRI_IMAGE_FOURCC_XBGR8888;
   case TBM_FORMAT_XRGB8888: return __DRI_IMAGE_FOURCC_XRGB8888;
   default:
      _eglLog(_EGL_WARNING, "unsupported native buffer format 0x%x", format);
   }
   return -1;
}

static int get_fourcc_yuv(tbm_format format)
{
   switch (format) {
   case TBM_FORMAT_NV12:   return __DRI_IMAGE_FOURCC_NV12;
   case TBM_FORMAT_NV21:   return __DRI_IMAGE_FOURCC_NV12;
   case TBM_FORMAT_YUV420: return __DRI_IMAGE_FOURCC_YUV420;
   case TBM_FORMAT_YVU420: return __DRI_IMAGE_FOURCC_YVU420;
   default:
      _eglLog(_EGL_WARNING, "unsupported native yuv buffer format 0x%x", format);
   }
   return -1;
}

static bool is_yuv_format(tbm_format format)
{
   if (get_fourcc_yuv(format) == -1)
     return false;
   else
     return true;
}

static int get_format(tbm_format format)
{
   switch (format) {
   case TBM_FORMAT_RGB565:   return __DRI_IMAGE_FORMAT_RGB565;
   case TBM_FORMAT_BGRA8888: return __DRI_IMAGE_FORMAT_ARGB8888;
   case TBM_FORMAT_RGBA8888: return __DRI_IMAGE_FORMAT_ABGR8888;
   case TBM_FORMAT_ARGB8888: return __DRI_IMAGE_FORMAT_ARGB8888;
   case TBM_FORMAT_ABGR8888: return __DRI_IMAGE_FORMAT_ABGR8888;
   case TBM_FORMAT_RGBX8888: return __DRI_IMAGE_FORMAT_XBGR8888;
   case TBM_FORMAT_XRGB8888: return __DRI_IMAGE_FORMAT_XRGB8888;
   default:
      _eglLog(_EGL_WARNING, "unsupported native buffer format 0x%x", format);
   }
   return -1;
}

static int get_format_bpp(tbm_format format)
{
   int bpp;

   switch (format) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
   case TBM_FORMAT_BGRA8888:
   case TBM_FORMAT_RGBA8888:
   case TBM_FORMAT_RGBX8888:
   case TBM_FORMAT_ARGB8888:
#pragma GCC diagnostic pop
   case TBM_FORMAT_XRGB8888:
      bpp = 4;
      break;
   case TBM_FORMAT_RGB565:
      bpp = 2;
      break;
   default:
      bpp = 0;
      break;
   }

   return bpp;
}

static int get_pitch(tbm_surface_h tbm_surface)
{
   tbm_surface_info_s surf_info;

   if (tbm_surface_get_info(tbm_surface, &surf_info) != TBM_SURFACE_ERROR_NONE) {
      return 0;
   }

   return surf_info.planes[0].stride;
}

static int
get_native_buffer_fd(tbm_surface_h tbm_surface)
{
   tbm_bo_handle bo_handle;
   bo_handle = tbm_bo_get_handle(tbm_surface_internal_get_bo(tbm_surface, 0),
                                 TBM_DEVICE_3D);

   return (bo_handle.ptr != NULL) ? (int)bo_handle.u32 : -1;
}

static int
get_native_buffer_name(tbm_surface_h tbm_surface)
{
   uint32_t bo_name;

   bo_name = tbm_bo_export(tbm_surface_internal_get_bo(tbm_surface, 0));

   return (bo_name != 0 ) ? (int)bo_name : -1;
}

static EGLBoolean
tizen_window_dequeue_buffer(struct dri2_egl_surface *dri2_surf)
{
   int width, height;

   dri2_surf->tbm_surface = tpl_surface_dequeue_buffer(dri2_surf->tpl_surface);

   if (!dri2_surf->tbm_surface)
     return EGL_FALSE;

   tbm_surface_internal_ref(dri2_surf->tbm_surface);

   tpl_surface_get_size(dri2_surf->tpl_surface, &width, &height);
   if (dri2_surf->base.Width != width ||
       dri2_surf->base.Height != height) {
      dri2_surf->base.Width = width;
      dri2_surf->base.Height = height;
      tizen_free_local_buffers(dri2_surf);
   }

   /* Record all the buffers created by tpl_surface (tbm_surface_queue)
    *   and update back buffer * for updating buffer's age in swap_buffers.
    */
   EGLBoolean updated = EGL_FALSE;
   for (int i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (!dri2_surf->color_buffers[i].tbm_surface) {
         dri2_surf->color_buffers[i].tbm_surface = dri2_surf->tbm_surface;
         dri2_surf->color_buffers[i].age = 0;
      }
      if (dri2_surf->color_buffers[i].tbm_surface == dri2_surf->tbm_surface) {
         dri2_surf->back = &dri2_surf->color_buffers[i];
         updated = EGL_TRUE;
         break;
      }
   }

   if (!updated) {
      /* In case of all the buffers were recreated , reset the color_buffers */
      for (int i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
         dri2_surf->color_buffers[i].tbm_surface = NULL;
         dri2_surf->color_buffers[i].age = 0;
      }
      dri2_surf->color_buffers[0].tbm_surface = dri2_surf->tbm_surface;
      dri2_surf->back = &dri2_surf->color_buffers[0];
   }

   return EGL_TRUE;
}

static EGLBoolean
tizen_window_enqueue_buffer_with_damage(_EGLDisplay *disp,
                                        struct dri2_egl_surface *dri2_surf,
                                        const EGLint *rects,
                                        EGLint n_rects)
{
   tpl_result_t ret;
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   /* To avoid blocking other EGL calls, release the display mutex before
    * we enter tizen_window_enqueue_buffer() and re-acquire the mutex upon
    * return.
    */
   mtx_unlock(&disp->Mutex);

   if (n_rects < 1 || rects == NULL) {
     /* if there is no damage, call the normal API tpl_surface_enqueue_buffer */
     ret = tpl_surface_enqueue_buffer(dri2_surf->tpl_surface,
                                      dri2_surf->tbm_surface);
   } else {
     /* if there are rectangles of damage region,
        call the API tpl_surface_enqueue_buffer_with_damage() */
     ret = tpl_surface_enqueue_buffer_with_damage(dri2_surf->tpl_surface,
                                                  dri2_surf->tbm_surface,
                                                  n_rects, rects);
   }

   if (ret != TPL_ERROR_NONE) {
      _eglLog(_EGL_DEBUG, "%s : %d :tpl_surface_enqueue fail", __func__, __LINE__);
   }

   tbm_surface_internal_unref(dri2_surf->tbm_surface);
   dri2_surf->tbm_surface = NULL;
   dri2_surf->back = NULL;

   mtx_lock(&disp->Mutex);

   if (dri2_surf->dri_image_back) {
      dri2_dpy->image->destroyImage(dri2_surf->dri_image_back);
      dri2_surf->dri_image_back = NULL;
   }

   return EGL_TRUE;
}

static EGLBoolean
tizen_window_enqueue_buffer(_EGLDisplay *disp, struct dri2_egl_surface *dri2_surf)
{
   return tizen_window_enqueue_buffer_with_damage(disp, dri2_surf, NULL, 0);
}

static void
tizen_window_cancel_buffer(_EGLDisplay *disp, struct dri2_egl_surface *dri2_surf)
{
   tizen_window_enqueue_buffer(disp, dri2_surf);
}

static __DRIbuffer *
tizen_alloc_local_buffer(struct dri2_egl_surface *dri2_surf,
                         unsigned int att, unsigned int format)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   if (att >= ARRAY_SIZE(dri2_surf->local_buffers))
      return NULL;

   if (!dri2_surf->local_buffers[att]) {
      dri2_surf->local_buffers[att] =
         dri2_dpy->dri2->allocateBuffer(dri2_dpy->dri_screen, att, format,
               dri2_surf->base.Width, dri2_surf->base.Height);
   }

   return dri2_surf->local_buffers[att];
}

static void
tizen_free_local_buffers(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int i;

   for (i = 0; i < ARRAY_SIZE(dri2_surf->local_buffers); i++) {
      if (dri2_surf->local_buffers[i]) {
         dri2_dpy->dri2->releaseBuffer(dri2_dpy->dri_screen,
               dri2_surf->local_buffers[i]);
         dri2_surf->local_buffers[i] = NULL;
      }
   }
}

static _EGLSurface *
tizen_create_surface(_EGLDriver *drv, _EGLDisplay *disp, EGLint type,
                     _EGLConfig *conf, void *native_window,
                     const EGLint *attrib_list)
{
   __DRIcreateNewDrawableFunc createNewDrawable;
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   const __DRIconfig *config;
   tpl_surface_type_t tpl_surf_type = TPL_SURFACE_ERROR;
   tpl_result_t ret = TPL_ERROR_INVALID_PARAMETER;

   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "tizen_create_surface");
      return NULL;
   }

   if (!_eglInitSurface(&dri2_surf->base, disp, type, conf, attrib_list, native_window))
      goto cleanup_surface;

   config = dri2_get_dri_config(dri2_conf, type,
                                dri2_surf->base.GLColorspace);
   if (!config)
      goto cleanup_surface;

   if (type == EGL_WINDOW_BIT) {
      unsigned int alpha, depth;

      if (!native_window) {
         _eglError(EGL_BAD_NATIVE_WINDOW, "tizen_create_surface needs vaild native window");
         goto cleanup_surface;
      }
      dri2_surf->native_win = native_window;

      dri2_dpy->core->getConfigAttrib(config, __DRI_ATTRIB_DEPTH_SIZE, &depth);
      dri2_dpy->core->getConfigAttrib(config, __DRI_ATTRIB_ALPHA_SIZE, &alpha);

      ret = tpl_display_get_native_window_info(dri2_dpy->tpl_display,
                                               (tpl_handle_t)native_window,
                                               &dri2_surf->base.Width,
                                               &dri2_surf->base.Height,
                                               &dri2_surf->tbm_format,
                                               depth, alpha);

      if (ret != TPL_ERROR_NONE || dri2_surf->tbm_format == 0) {
        _eglError(EGL_BAD_NATIVE_WINDOW, "tizen_create_surface fails on tpl_display_get_native_window_info()");
        goto cleanup_surface;
      }

      tpl_surf_type = TPL_SURFACE_TYPE_WINDOW;
   } else if (type == EGL_PIXMAP_BIT) {
      if (!native_window) {
         _eglError(EGL_BAD_NATIVE_PIXMAP, "tizen_create_surface needs valid native pixmap");
         goto cleanup_surface;
      }
      ret = tpl_display_get_native_pixmap_info(dri2_dpy->tpl_display,
                                               (tpl_handle_t)native_window,
                                               &dri2_surf->base.Width,
                                               &dri2_surf->base.Height,
                                               &dri2_surf->tbm_format);

      if (ret != TPL_ERROR_NONE || dri2_surf->tbm_format == 0) {
        _eglError(EGL_BAD_NATIVE_PIXMAP, "tizen_create_surface fails on tpl_display_get_native_pixmap_info");
        goto cleanup_surface;
      }

      tpl_surf_type = TPL_SURFACE_TYPE_PIXMAP;
   }

   if(type == EGL_WINDOW_BIT || type == EGL_PIXMAP_BIT) {
      dri2_surf->tpl_surface = tpl_surface_create(dri2_dpy->tpl_display,
                                               (tpl_handle_t)native_window,
                                               tpl_surf_type,
                                               dri2_surf->tbm_format);
      if (!dri2_surf->tpl_surface)
         goto cleanup_surface;
   }

   if (dri2_dpy->dri2) {
      createNewDrawable = dri2_dpy->dri2->createNewDrawable;
   } else {
      createNewDrawable = dri2_dpy->swrast->createNewDrawable;
   }

   dri2_surf->dri_drawable = (*createNewDrawable)(dri2_dpy->dri_screen, config,
                                                  dri2_surf);
    if (dri2_surf->dri_drawable == NULL) {
      _eglError(EGL_BAD_ALLOC, "createNewDrawable");
       goto cleanup_tpl_surface;
    }

   return &dri2_surf->base;

cleanup_tpl_surface:
   tpl_object_unreference((tpl_object_t *)dri2_surf->tpl_surface);
cleanup_surface:
   free(dri2_surf);

   return NULL;
}

static _EGLSurface *
tizen_create_window_surface(_EGLDriver *drv, _EGLDisplay *disp,
                            _EGLConfig *conf, void *native_window,
                            const EGLint *attrib_list)
{
   return tizen_create_surface(drv, disp, EGL_WINDOW_BIT, conf,
                               native_window, attrib_list);
}

static _EGLSurface *
tizen_create_pixmap_surface(_EGLDriver *drv, _EGLDisplay *disp,
                            _EGLConfig *conf, void *native_pixmap,
                            const EGLint *attrib_list)
{
   return tizen_create_surface(drv, disp, EGL_PIXMAP_BIT, conf,
                               native_pixmap, attrib_list);
}

static _EGLSurface *
tizen_create_pbuffer_surface(_EGLDriver *drv, _EGLDisplay *disp,
                             _EGLConfig *conf, const EGLint *attrib_list)
{
   return tizen_create_surface(drv, disp, EGL_PBUFFER_BIT, conf,
                               NULL, attrib_list);
}

static EGLBoolean
tizen_destroy_surface(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   tizen_free_local_buffers(dri2_surf);

   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
      if (dri2_surf->tbm_surface)
         tizen_window_cancel_buffer(disp, dri2_surf);
   }

   if (dri2_surf->dri_image_back) {
      _eglLog(_EGL_DEBUG, "%s : %d : destroy dri_image_back", __func__, __LINE__);
      dri2_dpy->image->destroyImage(dri2_surf->dri_image_back);
      dri2_surf->dri_image_back = NULL;
   }

   if (dri2_surf->dri_image_front) {
      _eglLog(_EGL_DEBUG, "%s : %d : destroy dri_image_front", __func__, __LINE__);
      dri2_dpy->image->destroyImage(dri2_surf->dri_image_front);
      dri2_surf->dri_image_front = NULL;
   }

   dri2_dpy->core->destroyDrawable(dri2_surf->dri_drawable);

   if (dri2_surf->tpl_surface)
      tpl_object_unreference((tpl_object_t *)dri2_surf->tpl_surface);

   free(dri2_surf);

   return EGL_TRUE;
}

static int
update_buffers(struct dri2_egl_surface *dri2_surf)
{
   if (dri2_surf->base.Type != EGL_WINDOW_BIT)
      return 0;

   /* try to dequeue the next back buffer */
   if (!dri2_surf->tbm_surface && !tizen_window_dequeue_buffer(dri2_surf)) {
      _eglLog(_EGL_WARNING, "Could not dequeue buffer from native window");
      return -1;
   }

   return 0;
}

static int
get_front_bo(struct dri2_egl_surface *dri2_surf, unsigned int format)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   if (dri2_surf->dri_image_front)
      return 0;

   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
      /* According current EGL spec, front buffer rendering
       * for window surface is not supported now.
       * and mesa doesn't have the implementation of this case.
       * Add warning message, but not treat it as error.
       */
      _eglLog(_EGL_DEBUG, "DRI driver requested unsupported front buffer for window surface");
   } else if (dri2_surf->base.Type == EGL_PBUFFER_BIT) {
      dri2_surf->dri_image_front =
          dri2_dpy->image->createImage(dri2_dpy->dri_screen,
                                       dri2_surf->base.Width,
                                       dri2_surf->base.Height,
                                       format,
                                       0,
                                       dri2_surf);
      if (!dri2_surf->dri_image_front) {
         _eglLog(_EGL_WARNING, "dri2_image_front allocation failed");
         return -1;
      }
   }

   return 0;
}

static int
get_back_bo(struct dri2_egl_surface *dri2_surf, unsigned int format)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int fourcc, pitch;
   int offset = 0, fd;
   tbm_surface_info_s surf_info;

   if (dri2_surf->dri_image_back)
      return 0;

   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
      if (!dri2_surf->tbm_surface) {
         _eglLog(_EGL_WARNING, "Could not get native buffer");
         return -1;
      }

      fd = get_native_buffer_fd(dri2_surf->tbm_surface);
      if (fd < 0) {
         _eglLog(_EGL_WARNING, "Could not get native buffer FD");
         return -1;
      }

      if (tbm_surface_get_info(dri2_surf->tbm_surface, &surf_info) != TBM_SURFACE_ERROR_NONE) {
         _eglLog(_EGL_WARNING, "Could not get pitch");
         return -1;
      }

      pitch = surf_info.planes[0].stride;
      fourcc = get_fourcc(dri2_surf->tbm_format);

      if (fourcc == -1 || pitch == 0) {
         _eglLog(_EGL_WARNING, "Invalid buffer fourcc(%x) or pitch(%d)",
                 fourcc, pitch);
         return -1;
      }

      dri2_surf->base.Width = surf_info.width;
      dri2_surf->base.Height = surf_info.height;

      dri2_surf->dri_image_back =
         dri2_dpy->image->createImageFromFds(dri2_dpy->dri_screen,
                                             dri2_surf->base.Width,
                                             dri2_surf->base.Height,
                                             fourcc,
                                             &fd,
                                             1,
                                             &pitch,
                                             &offset,
                                             dri2_surf);

      if (!dri2_surf->dri_image_back) {
         _eglLog(_EGL_WARNING, "failed to create DRI image from FD");
         return -1;
      }
   } else if (dri2_surf->base.Type == EGL_PBUFFER_BIT) {
      /* The EGL 1.5 spec states that pbuffers are single-buffered. Specifically,
       * the spec states that they have a back buffer but no front buffer, in
       * contrast to pixmaps, which have a front buffer but no back buffer.
       *
       * Single-buffered surfaces with no front buffer confuse Mesa; so we deviate
       * from the spec, following the precedent of Mesa's EGL X11 platform. The
       * X11 platform correctly assigns pbuffers to single-buffered configs, but
       * assigns the pbuffer a front buffer instead of a back buffer.
       *
       * Pbuffers in the X11 platform mostly work today, so let's just copy its
       * behavior instead of trying to fix (and hence potentially breaking) the
       * world.
       */
      _eglLog(_EGL_DEBUG, "DRI driver requested unsupported back buffer for pbuffer surface");
   }

   return 0;
}

/* Some drivers will pass multiple bits in buffer_mask.
 * For such case, will go through all the bits, and
 * will not return error when unsupported buffer is requested, only
 * return error when the allocation for supported buffer failed.
 */
static int
tizen_image_get_buffers(__DRIdrawable *driDrawable, unsigned int format,
                        uint32_t *stamp, void *loaderPrivate,
                        uint32_t buffer_mask, struct __DRIimageList *images)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   images->image_mask = 0;
   images->front = NULL;
   images->back = NULL;

   if (update_buffers(dri2_surf) < 0)
      return 0;

   if (buffer_mask & __DRI_IMAGE_BUFFER_FRONT) {
      if (get_front_bo(dri2_surf, format) < 0)
         return 0;

      if (dri2_surf->dri_image_front) {
         images->front = dri2_surf->dri_image_front;
         images->image_mask |= __DRI_IMAGE_BUFFER_FRONT;
      }
   }

   if (buffer_mask & __DRI_IMAGE_BUFFER_BACK) {
      if (get_back_bo(dri2_surf, format) < 0)
         return 0;

      if (dri2_surf->dri_image_back) {
         images->back = dri2_surf->dri_image_back;
         images->image_mask |= __DRI_IMAGE_BUFFER_BACK;
      }
   }

   return 1;
}

static EGLint
tizen_query_buffer_age(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *surface)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surface);

   if (update_buffers(dri2_surf) < 0) {
      _eglError(EGL_BAD_ALLOC, "tizen_query_buffer_age");
      return 0;
   }

   return dri2_surf->back->age;
}

static EGLBoolean
tizen_swap_buffers_with_damage(_EGLDriver *drv, _EGLDisplay *disp,
                               _EGLSurface *draw, const EGLint *rects,
                               EGLint n_rects)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);

   if (dri2_surf->base.Type != EGL_WINDOW_BIT)
      return EGL_TRUE;

   for (int i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (dri2_surf->color_buffers[i].age > 0)
         dri2_surf->color_buffers[i].age++;
   }

   /* Make sure we have a back buffer in case we're swapping without
    * ever rendering. */
   if (get_back_bo(dri2_surf, 0) < 0) {
      _eglError(EGL_BAD_ALLOC, "dri2_swap_buffers");
      return EGL_FALSE;
   }

   dri2_surf->back->age = 1;

   if (dri2_surf->tbm_surface)
      tizen_window_enqueue_buffer_with_damage(disp, dri2_surf, rects, n_rects);

   if (dri2_dpy->swrast) {
      dri2_dpy->core->swapBuffers(dri2_surf->dri_drawable);
   } else {
      dri2_flush_drawable_for_swapbuffers(disp, draw);
      dri2_dpy->flush->invalidate(dri2_surf->dri_drawable);
   }

   return EGL_TRUE;
}

static EGLBoolean
tizen_swap_buffers(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *draw)
{
   return tizen_swap_buffers_with_damage (drv, disp, draw, NULL, 0);
}

static _EGLImage *
tizen_create_image_from_prime_fd_yuv(_EGLDisplay *disp, _EGLContext *ctx,
                                     tbm_surface_h tbm_surface)

{
   tbm_surface_info_s surf_info;
   tbm_fd bo_fd[TBM_SURF_PLANE_MAX];
   tbm_bo bo[TBM_SURF_PLANE_MAX];
   int num_planes;
   int i;
   int fourcc;
   size_t offsets[3] = {0, 0, 0};
   size_t pitches[3] = {0, 0, 0};
   int fds[3] = {-1, -1, -1};

   if (tbm_surface_get_info(tbm_surface, &surf_info) != TBM_SURFACE_ERROR_NONE) {
     _eglLog(_EGL_WARNING, "Could not get tbm_surface_info");
     return NULL;
   }

   num_planes = surf_info.num_planes;
   for (i = 0; i < num_planes; i++) {
      tbm_bo_handle bo_handle;
      int bo_idx = tbm_surface_internal_get_plane_bo_idx(tbm_surface, i);
      bo[i] = tbm_surface_internal_get_bo (tbm_surface, bo_idx);
      if (bo[i] == NULL) {
        _eglLog(_EGL_WARNING, "Could not get tbm_surface_internal_bo");
        return NULL;
      }
      bo_handle = tbm_bo_get_handle(bo[i], TBM_DEVICE_3D);
      bo_fd[i] = bo_handle.u32;
   }

   fourcc = get_fourcc_yuv(tbm_surface_get_format(tbm_surface));
   if (fourcc == -1) {
     _eglLog(_EGL_WARNING, "Unsupported native yuv format");
     return NULL;
   }

   switch (fourcc) {
   case __DRI_IMAGE_FOURCC_NV12:
      fds[0] = bo_fd[0];
      fds[1] = bo_fd[1];
      offsets[0] = surf_info.planes[0].offset;
      offsets[1] = surf_info.planes[1].offset;
      pitches[0] = surf_info.planes[0].stride;
      pitches[1] = surf_info.planes[1].stride;
      break;
   case __DRI_IMAGE_FOURCC_YUV420:
      fds[0] = bo_fd[0];
      fds[1] = bo_fd[1];
      fds[2] = bo_fd[2];
      offsets[0] = surf_info.planes[0].offset;
      offsets[1] = surf_info.planes[1].offset;
      offsets[2] = surf_info.planes[2].offset;
      pitches[0] = surf_info.planes[0].stride;
      pitches[1] = surf_info.planes[1].stride;
      pitches[2] = surf_info.planes[2].stride;
      break;
   case __DRI_IMAGE_FOURCC_YVU420:
      fds[0] = bo_fd[0];
      fds[1] = bo_fd[2];
      fds[2] = bo_fd[1];
      offsets[0] = surf_info.planes[0].offset;
      offsets[1] = surf_info.planes[2].offset;
      offsets[2] = surf_info.planes[1].offset;
      pitches[0] = surf_info.planes[0].stride;
      pitches[1] = surf_info.planes[2].stride;
      pitches[2] = surf_info.planes[1].stride;
      break;
    default:
      _eglLog(_EGL_WARNING, "Unsupported native yuv format");
      return NULL;
   }

   if (num_planes == 2) {
      const EGLint attr_list_2plane[] = {
         EGL_WIDTH, surf_info.width,
         EGL_HEIGHT, surf_info.height,
         EGL_LINUX_DRM_FOURCC_EXT, fourcc,
         EGL_DMA_BUF_PLANE0_FD_EXT, fds[0],
         EGL_DMA_BUF_PLANE0_PITCH_EXT, pitches[0],
         EGL_DMA_BUF_PLANE0_OFFSET_EXT, offsets[0],
         EGL_DMA_BUF_PLANE1_FD_EXT, fds[1],
         EGL_DMA_BUF_PLANE1_PITCH_EXT, pitches[1],
         EGL_DMA_BUF_PLANE1_OFFSET_EXT, offsets[1],
         EGL_NONE, 0
      };
      return dri2_create_image_dma_buf(disp, ctx, NULL, attr_list_2plane);
   } else if (num_planes == 3) {
      const EGLint attr_list_3plane[] = {
         EGL_WIDTH, surf_info.width,
         EGL_HEIGHT, surf_info.height,
         EGL_LINUX_DRM_FOURCC_EXT, fourcc,
         EGL_DMA_BUF_PLANE0_FD_EXT, fds[0],
         EGL_DMA_BUF_PLANE0_PITCH_EXT, pitches[0],
         EGL_DMA_BUF_PLANE0_OFFSET_EXT, offsets[0],
         EGL_DMA_BUF_PLANE1_FD_EXT, fds[1],
         EGL_DMA_BUF_PLANE1_PITCH_EXT, pitches[1],
         EGL_DMA_BUF_PLANE1_OFFSET_EXT, offsets[1],
         EGL_DMA_BUF_PLANE2_FD_EXT, fds[2],
         EGL_DMA_BUF_PLANE2_PITCH_EXT, pitches[2],
         EGL_DMA_BUF_PLANE2_OFFSET_EXT, offsets[2],
         EGL_NONE, 0
      };
      return dri2_create_image_dma_buf(disp, ctx, NULL, attr_list_3plane);
   } else {
      _eglLog(_EGL_WARNING, "Unsupported yuv planes");
      return NULL;
   }
}

static _EGLImage *
tizen_create_image_from_prime_fd(_EGLDisplay *disp, _EGLContext *ctx,
                                 tbm_surface_h tbm_surface , int fd)
{
   unsigned int pitch;
   tbm_surface_info_s surf_info;
   unsigned int flags = tbm_bo_get_flags(tbm_surface_internal_get_bo(tbm_surface, 0));

   if (is_yuv_format(tbm_surface_get_format(tbm_surface)))
      return tizen_create_image_from_prime_fd_yuv(disp, ctx, tbm_surface);

   const int fourcc = get_fourcc(tbm_surface_get_format(tbm_surface));
   if (fourcc == -1) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
      return NULL;
   }

   if (tbm_surface_get_info(tbm_surface, &surf_info) != TBM_SURFACE_ERROR_NONE) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
      return NULL;
   }
   pitch = surf_info.planes[0].stride;

   if (pitch == 0) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
      return NULL;
   }

   /*const EGLint attr_list[] = {
      EGL_WIDTH, surf_info.width,
      EGL_HEIGHT, surf_info.height,
      EGL_LINUX_DRM_FOURCC_EXT, fourcc,
      EGL_DMA_BUF_PLANE0_FD_EXT, fd,
      EGL_DMA_BUF_PLANE0_PITCH_EXT, pitch,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
      EGL_NONE, 0
   };*/

   //TODO: get modifier from vendor driver/TBM
   EGLint attr_list[50];
   int atti = 0;

   attr_list[atti++] = EGL_WIDTH;
   attr_list[atti++] = surf_info.width;
   attr_list[atti++] = EGL_HEIGHT;
   attr_list[atti++] = surf_info.height;
   attr_list[atti++] = EGL_LINUX_DRM_FOURCC_EXT;
   attr_list[atti++] = fourcc;
   attr_list[atti++] = EGL_DMA_BUF_PLANE0_FD_EXT;
   attr_list[atti++] = fd;
   attr_list[atti++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
   attr_list[atti++] = pitch;
   attr_list[atti++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
   attr_list[atti++] = 0;

   if (flags == TBM_BO_TILED) {
		attr_list[atti++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
		attr_list[atti++] = DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED & 0xFFFFFFFF;
		attr_list[atti++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
		attr_list[atti++] = DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED >> 32;
   }
   attr_list[atti++] = EGL_NONE;
   attr_list[atti++] = 0;


   return dri2_create_image_dma_buf(disp, ctx, NULL, attr_list);
}

static _EGLImage *
tizen_create_image_from_name(_EGLDisplay *disp, _EGLContext *ctx,
                             tbm_surface_h tbm_surface)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_image *dri2_img;
   int name;
   int format;
   unsigned int pitch;
   tbm_surface_info_s surf_info;

   name = get_native_buffer_name(tbm_surface);
   if (!name) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
      return NULL;
   }

   format = get_format(tbm_surface_get_format(tbm_surface));
   if (format == -1)
      return NULL;

   if (tbm_surface_get_info(tbm_surface, &surf_info) != TBM_SURFACE_ERROR_NONE) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
      return NULL;
   }
   pitch = surf_info.planes[0].stride;

   if (pitch == 0) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
      return NULL;
   }

   dri2_img = calloc(1, sizeof(*dri2_img));
   if (!dri2_img) {
      _eglError(EGL_BAD_ALLOC, "tizen_create_image_mesa_drm");
      return NULL;
   }

   _eglInitImage(&dri2_img->base, disp);

   dri2_img->dri_image =
      dri2_dpy->image->createImageFromName(dri2_dpy->dri_screen,
                                           surf_info.width,
                                           surf_info.height,
                                           format,
                                           name,
                                           pitch,
                                           dri2_img);
   if (!dri2_img->dri_image) {
      free(dri2_img);
      _eglError(EGL_BAD_ALLOC, "tizen_create_image_mesa_drm");
      return NULL;
   }

   return &dri2_img->base;
}

static EGLBoolean
tizen_query_surface(_EGLDriver *drv, _EGLDisplay *dpy, _EGLSurface *surf,
                    EGLint attribute, EGLint *value)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   int width = 0, height = 0;

   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
	   if (tpl_display_get_native_window_info(dri2_dpy->tpl_display,
					   dri2_surf->native_win,
					   &width, &height, NULL, 0, 0) != TPL_ERROR_NONE)
		 return EGL_FALSE;

	   switch (attribute) {
		   case EGL_WIDTH:
			   if (dri2_surf->native_win) {
				   *value = width;
				   return EGL_TRUE;
			   }
			   break;
		   case EGL_HEIGHT:
			   if (dri2_surf->native_win) {
				   *value = height;
				   return EGL_TRUE;
			   }
			   break;
		   default:
			   break;
	   }
   }
   return _eglQuerySurface(drv, dpy, surf, attribute, value);
}

static _EGLImage *
dri2_create_image_tizen_native_buffer(_EGLDisplay *disp,
                                      _EGLContext *ctx,
                                      tbm_surface_h tbm_surface)
{
   int fd;

   fd = get_native_buffer_fd(tbm_surface);
   if (fd >= 0)
      return tizen_create_image_from_prime_fd(disp, ctx, tbm_surface, fd);

   return tizen_create_image_from_name(disp, ctx, tbm_surface);
}

static _EGLImage *
dri2_create_image_tizen_wl_buffer(_EGLDisplay *disp,
                                  _EGLContext *ctx,
                                  tpl_handle_t native_pixmap)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   int fd;
   tbm_surface_h tbm_surface = NULL;

   tbm_surface = tpl_display_get_buffer_from_native_pixmap(dri2_dpy->tpl_display,
                                                           (tpl_handle_t)native_pixmap);
   if (!tbm_surface)
     return NULL;

   fd = get_native_buffer_fd(tbm_surface);
   if (fd >= 0)
      return tizen_create_image_from_prime_fd(disp, ctx, tbm_surface, fd);

   return tizen_create_image_from_name(disp, ctx, tbm_surface);
}

static _EGLImage *
tizen_create_image_khr(_EGLDriver *drv, _EGLDisplay *disp,
                       _EGLContext *ctx, EGLenum target,
                       EGLClientBuffer buffer, const EGLint *attr_list)
{
   switch (target) {
   case EGL_NATIVE_SURFACE_TIZEN:
      return dri2_create_image_tizen_native_buffer(disp, ctx,
                                                   (tbm_surface_h)buffer);
   case EGL_WAYLAND_BUFFER_WL:
      return dri2_create_image_tizen_wl_buffer(disp, ctx, (tpl_handle_t)buffer);
   default:
      return dri2_create_image_khr(drv, disp, ctx, target, buffer, attr_list);
   }
}

static void
tizen_flush_front_buffer(__DRIdrawable * driDrawable, void *loaderPrivate)
{
}

static int
tizen_get_buffers_parse_attachments(struct dri2_egl_surface *dri2_surf,
                                    unsigned int *attachments, int count)
{
   int num_buffers = 0, i;

   /* fill dri2_surf->buffers */
   for (i = 0; i < count * 2; i += 2) {
      __DRIbuffer *buf, *local;

      assert(num_buffers < ARRAY_SIZE(dri2_surf->buffers));
      buf = &dri2_surf->buffers[num_buffers];

      switch (attachments[i]) {
      case __DRI_BUFFER_BACK_LEFT:
         if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
            buf->attachment = attachments[i];
            buf->name = get_native_buffer_name(dri2_surf->tbm_surface);
            buf->cpp = get_format_bpp(tbm_surface_get_format(dri2_surf->tbm_surface));
            buf->pitch = get_pitch(dri2_surf->tbm_surface);
            buf->flags = 0;

            if (buf->name)
               num_buffers++;

            break;
         }
         /* fall through for pbuffers */
      case __DRI_BUFFER_FRONT_LEFT:
         if (dri2_surf->base.Type != EGL_PBUFFER_BIT)
            break;
      case __DRI_BUFFER_DEPTH:
      case __DRI_BUFFER_STENCIL:
      case __DRI_BUFFER_ACCUM:
      case __DRI_BUFFER_DEPTH_STENCIL:
      case __DRI_BUFFER_HIZ:
         local = tizen_alloc_local_buffer(dri2_surf, attachments[i], attachments[i + 1]);

         if (local) {
            *buf = *local;
            num_buffers++;
         }
         break;
      case __DRI_BUFFER_FRONT_RIGHT:
      case __DRI_BUFFER_FAKE_FRONT_LEFT:
      case __DRI_BUFFER_FAKE_FRONT_RIGHT:
      case __DRI_BUFFER_BACK_RIGHT:
      default:
         /* no front or right buffers */
         break;
      }
   }

   return num_buffers;
}

static __DRIbuffer *
tizen_get_buffers_with_format(__DRIdrawable * driDrawable,
                              int *width, int *height,
                              unsigned int *attachments, int count,
                              int *out_count, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   if (update_buffers(dri2_surf) < 0)
      return NULL;

   dri2_surf->buffer_count =
      tizen_get_buffers_parse_attachments(dri2_surf, attachments, count);

   if (width)
      *width = dri2_surf->base.Width;
   if (height)
      *height = dri2_surf->base.Height;

   *out_count = dri2_surf->buffer_count;

   return dri2_surf->buffers;
}

static int
tizen_swrast_get_stride_for_format(tbm_format format, int w)
{
   switch (format) {
   case TBM_FORMAT_RGB565:
      return 2 * w;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
   case TBM_FORMAT_BGRA8888:
   case TBM_FORMAT_RGBA8888:
#pragma GCC diagnostic pop
   case TBM_FORMAT_RGBX8888:
   default:
      return 4 * w;
   }
}

static void
tizen_swrast_get_drawable_info(__DRIdrawable * draw,
                               int *x, int *y, int *w, int *h,
                               void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   if (!dri2_surf->tbm_surface) {
      if (update_buffers(dri2_surf) < 0)
         return;
   }

   *x = 0;
   *y = 0;
   *w = dri2_surf->base.Width;
   *h = dri2_surf->base.Height;
}

static void
tizen_swrast_get_image(__DRIdrawable * read,
                       int x, int y, int w, int h,
                       char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   tbm_surface_info_s surf_info;
   int ret = TBM_SURFACE_ERROR_NONE;
   int internal_stride, stride, i;

   if (!dri2_surf->tbm_surface) {
      if (update_buffers(dri2_surf) < 0)
         return;
   }

   ret = tbm_surface_map(dri2_surf->tbm_surface, TBM_SURF_OPTION_READ, &surf_info);

   if (ret != TBM_SURFACE_ERROR_NONE) {
      _eglLog(_EGL_WARNING, "Could not tbm_surface_map");
      return;
   }

   internal_stride = surf_info.planes[0].stride;
   stride = w * 4;

   for (i = 0; i < h; i++) {
      memcpy(data + i * stride,
             surf_info.planes[0].ptr + (x + i) * internal_stride + y, stride);
   }

   tbm_surface_unmap(dri2_surf->tbm_surface);
}

static void
tizen_swrast_put_image2(__DRIdrawable * draw, int op,
                        int x, int y, int w, int h, int stride,
                        char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   tbm_surface_info_s surf_info;
   int ret = TBM_SURFACE_ERROR_NONE;
   int internal_stride, i;

   if (op != __DRI_SWRAST_IMAGE_OP_DRAW &&
       op != __DRI_SWRAST_IMAGE_OP_SWAP)
      return;

   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
      if (!dri2_surf->tbm_surface) {
        if (update_buffers(dri2_surf) < 0) {
           _eglLog(_EGL_WARNING, "Could not get native buffer");
           return;
        }
      }

      ret = tbm_surface_map(dri2_surf->tbm_surface, TBM_SURF_OPTION_WRITE, &surf_info);
      if (ret != TBM_SURFACE_ERROR_NONE) {
         _eglLog(_EGL_WARNING, "Could not tbm_surface_map");
         return;
      }

      internal_stride = surf_info.planes[0].stride;

      for (i = 0; i < h; i++) {
         memcpy(surf_info.planes[0].ptr + (x + i) * internal_stride + y,
                data + i * stride, stride);
      }

      tbm_surface_unmap(dri2_surf->tbm_surface);
   }
}

static void
tizen_swrast_put_image(__DRIdrawable * draw, int op,
                         int x, int y, int w, int h,
                         char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   int stride;

   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
      if (!dri2_surf->tbm_surface) {
        if (update_buffers(dri2_surf) < 0) {
           _eglLog(_EGL_WARNING, "Could not get native buffer");
           return;
        }
      }

      stride = tizen_swrast_get_stride_for_format(dri2_surf->tbm_format, w);
      tizen_swrast_put_image2(draw, op, x, y, w, h, stride, data, loaderPrivate);
   }
}

static EGLBoolean
tizen_add_configs(_EGLDriver *drv, _EGLDisplay *dpy)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   int count, i;

   for (i = 0, count = 0; dri2_dpy->driver_configs[i]; i++) {
      static const struct rgba_masks pbuffer_masks[] = {
         { 0xff0000, 0xff00, 0xff, 0xff000000 }, /* ARGB8888 */
         { 0xff0000, 0xff00, 0xff, 0x0 },        /* RGB888 */
         { 0x00f800, 0x07e0, 0x1f, 0x0 },        /* RGB565 */
      };
      struct dri2_egl_config *dri2_cfg;
      unsigned int red, blue, green, alpha, depth;
      int surface_type = 0;
      tpl_bool_t is_slow;
      EGLint attr_list[] = {
         EGL_NATIVE_VISUAL_ID, 0,
         EGL_NONE,
      };
      tpl_result_t res;
      struct rgba_masks masks;
      int j;

      dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[i],
                                      __DRI_ATTRIB_RED_SIZE, &red);
      dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[i],
                                      __DRI_ATTRIB_GREEN_SIZE, &green);
      dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[i],
                                      __DRI_ATTRIB_BLUE_SIZE, &blue);
      dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[i],
                                      __DRI_ATTRIB_ALPHA_SIZE, &alpha);
      dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[i],
                                      __DRI_ATTRIB_DEPTH_SIZE, &depth);

      res = tpl_display_query_config(dri2_dpy->tpl_display, TPL_SURFACE_TYPE_WINDOW,
                                     red, green, blue, alpha, depth,
                                     &attr_list[1], &is_slow);
      if (res == TPL_ERROR_NONE)
        surface_type |= EGL_WINDOW_BIT;

      res = tpl_display_query_config(dri2_dpy->tpl_display, TPL_SURFACE_TYPE_PIXMAP,
                                     red, green, blue, alpha, depth,
                                     &attr_list[1], &is_slow);
      if (res == TPL_ERROR_NONE)
        surface_type |= EGL_PIXMAP_BIT;

      dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[i],
                                      __DRI_ATTRIB_RED_MASK, &masks.red);
      dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[i],
                                      __DRI_ATTRIB_GREEN_MASK, &masks.green);
      dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[i],
                                      __DRI_ATTRIB_BLUE_MASK, &masks.blue);
      dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[i],
                                      __DRI_ATTRIB_ALPHA_MASK, &masks.alpha);

      for (j = 0; j < ARRAY_SIZE(pbuffer_masks); j++) {
         const struct rgba_masks *pbuffer_mask = &pbuffer_masks[j];

         if (pbuffer_mask->red   == masks.red &&
             pbuffer_mask->green == masks.green &&
             pbuffer_mask->blue  == masks.blue &&
             pbuffer_mask->alpha == masks.alpha) {
            surface_type |= EGL_PBUFFER_BIT;
            break;
         }
      }

      if (!surface_type)
        continue;

      dri2_cfg = dri2_add_config(dpy, dri2_dpy->driver_configs[i], count + 1,
                                 surface_type, &attr_list[0], NULL);
      if (dri2_cfg)
        count++;
   }

   return (count != 0);
}

static EGLBoolean tizen_set_swap_interval(_EGLDriver *drv, _EGLDisplay *dpy,
                                          _EGLSurface *surf, EGLint interval)
{
   tpl_result_t res = TPL_ERROR_NONE;
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   res = tpl_surface_set_post_interval(dri2_surf->tpl_surface, interval);

   if (res != TPL_ERROR_NONE)
     return EGL_FALSE;

   return EGL_TRUE;
}

static struct dri2_egl_display_vtbl tizen_display_vtbl = {
   .authenticate = NULL,
   .create_window_surface = tizen_create_window_surface,
   .create_pixmap_surface = tizen_create_pixmap_surface,
   .create_pbuffer_surface = tizen_create_pbuffer_surface,
   .destroy_surface = tizen_destroy_surface,
   .create_image = tizen_create_image_khr,
   .swap_interval = tizen_set_swap_interval,
   .swap_buffers = tizen_swap_buffers,
   .swap_buffers_with_damage = tizen_swap_buffers_with_damage,
   .swap_buffers_region = dri2_fallback_swap_buffers_region,
   .post_sub_buffer = dri2_fallback_post_sub_buffer,
   .copy_buffers = dri2_fallback_copy_buffers,
   .query_buffer_age = tizen_query_buffer_age,
   .query_surface = tizen_query_surface,
   .create_wayland_buffer_from_image = NULL,
   .get_sync_values = dri2_fallback_get_sync_values,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

static const __DRIdri2LoaderExtension tizen_dri2_loader_extension = {
   .base = { __DRI_DRI2_LOADER, 3 },

   .getBuffers           = NULL,
   .getBuffersWithFormat = tizen_get_buffers_with_format,
   .flushFrontBuffer     = tizen_flush_front_buffer,
};

static const __DRIimageLoaderExtension tizen_image_loader_extension = {
   .base = { __DRI_IMAGE_LOADER, 1 },

   .getBuffers          = tizen_image_get_buffers,
   .flushFrontBuffer    = tizen_flush_front_buffer,
};

static const __DRIswrastLoaderExtension tizen_swrast_loader_extension = {
   .base = { __DRI_SWRAST_LOADER, 2 },

   .getDrawableInfo = tizen_swrast_get_drawable_info,
   .putImage        = tizen_swrast_put_image,
   .getImage        = tizen_swrast_get_image,
   .putImage2       = tizen_swrast_put_image2,
};

static const __DRIextension *tizen_dri2_loader_extensions[] = {
   &tizen_dri2_loader_extension.base,
   &tizen_image_loader_extension.base,
   &image_lookup_extension.base,
   &use_invalidate.base,
   NULL,
};

static const __DRIextension *tizen_image_loader_extensions[] = {
   &tizen_image_loader_extension.base,
   &image_lookup_extension.base,
   &use_invalidate.base,
   NULL,
};

static const __DRIextension *tizen_swrast_loader_extensions[] = {
   &tizen_swrast_loader_extension.base,
   NULL,
};

static EGLBoolean
tizen_bind_wayland_display_wl(_EGLDriver *drv, _EGLDisplay *disp,
                              struct wl_display *wl_dpy)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   (void) drv;
   (void) wl_dpy;

   if (!dri2_dpy->tpl_display)
     return EGL_FALSE;

   if (!tpl_display_get_native_handle(dri2_dpy->tpl_display))
     return EGL_FALSE;

   return EGL_TRUE;
}

static EGLBoolean
tizen_unbind_wayland_display_wl(_EGLDriver *drv, _EGLDisplay *disp,
                                struct wl_display *wl_dpy)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   (void) drv;
   (void) wl_dpy;

   if (!dri2_dpy->tpl_display)
     return EGL_FALSE;

   if (!tpl_display_get_native_handle(dri2_dpy->tpl_display))
     return EGL_FALSE;

   return EGL_TRUE;
}

static EGLBoolean
tizen_query_wayland_buffer_wl(_EGLDriver *drv, _EGLDisplay *disp,
                              struct wl_resource *buffer_resource,
                              EGLint attribute, EGLint *value)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   tbm_format tbm_format = 0;
   int width = 0, height = 0;
   tpl_result_t res;

   if (!dri2_dpy->tpl_display)
     return EGL_FALSE;

   if (!tpl_display_get_native_handle(dri2_dpy->tpl_display))
     return EGL_FALSE;

   res = tpl_display_get_native_pixmap_info(dri2_dpy->tpl_display,
                                            (tpl_handle_t)buffer_resource,
                                            &width, &height, &tbm_format);
   if (res != TPL_ERROR_NONE)
     return EGL_FALSE;

   switch (attribute) {
   case EGL_TEXTURE_FORMAT:
      switch (tbm_format) {
      case TBM_FORMAT_ARGB8888:
         *value = EGL_TEXTURE_RGBA;
         return EGL_TRUE;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
      case TBM_FORMAT_XRGB8888:
      case TBM_FORMAT_RGB565:
#pragma GCC diagnostic pop
         *value = EGL_TEXTURE_RGB;
         return EGL_TRUE;
      default:
         break;
      }
      break;
   case EGL_WIDTH:
      *value = width;
      return EGL_TRUE;
   case EGL_HEIGHT:
      *value = height;
      return EGL_TRUE;
   default:
      break;
   }

   return EGL_FALSE;
}

EGLBoolean
dri2_initialize_tizen(_EGLDriver *drv, _EGLDisplay *disp)
{
   _EGLDevice *dev;
   struct dri2_egl_display *dri2_dpy;
   tpl_display_t *tpl_display = NULL;
   const char *err;
   int tbm_bufmgr_fd = -1;
   char *tbm_bufmgr_device_name = NULL;
   int hw_accel = (getenv("LIBGL_ALWAYS_SOFTWARE") == NULL);

   loader_set_logger(_eglLog);

   dri2_dpy = calloc(1, sizeof *dri2_dpy);
   if (!dri2_dpy)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");

   disp->DriverData = (void *) dri2_dpy;
   if (disp->PlatformDisplay == NULL) {
      disp->PlatformDisplay = wl_display_connect(NULL);
      if (disp->PlatformDisplay == NULL) {
         err = "DRI2: failed to get native display";
         goto cleanup_display;
      }
      dri2_dpy->own_device = 1;
   }

   /* TPL_BACKEND_UNKNOWN type make checking runtime type check */
   tpl_display = tpl_display_create(TPL_BACKEND_UNKNOWN, disp->PlatformDisplay);
   if (!tpl_display) {
      err = "DRI2: failed to create tpl_display";
      goto cleanup_display;
   }
   dri2_dpy->tpl_display = tpl_display;

   /* Get tbm_bufmgr's fd */
   tbm_bufmgr_fd = tbm_drm_helper_get_fd();

   if (tbm_bufmgr_fd == -1) {
      err = "DRI2: failed to get tbm_bufmgr fd";
      goto cleanup_device;
   }

   if (hw_accel) {
      int fd = -1;

      if (drmGetNodeTypeFromFd(tbm_bufmgr_fd) == DRM_NODE_RENDER) {

         tbm_bufmgr_device_name = loader_get_device_name_for_fd(tbm_bufmgr_fd);
         if (tbm_bufmgr_device_name == NULL) {
            err = "DRI2: failed to get tbm_bufmgr device name";
            goto cleanup_device;
         }
         fd = loader_open_device(tbm_bufmgr_device_name);

      } else {
         if (!tbm_drm_helper_get_auth_info(&fd, NULL, NULL)) {

            /* FIXME: tbm_drm_helper_get_auth_info() does not support the case of
             *        display server for now. this code is fallback routine for
             *        that Enlightenment Server fails on tbm_drm_helper_get_auth_info.
             *        When tbm_drm_helper_get_auth_info() supports display server
             *        case, then remove below routine.
             */
#if 1
            tbm_bufmgr_device_name = loader_get_device_name_for_fd(tbm_bufmgr_fd);
            if (tbm_bufmgr_device_name == NULL) {
               err = "DRI2: failed to get tbm_bufmgr device name";
               goto cleanup_device;
            }
            fd = loader_open_device(tbm_bufmgr_device_name);
#else
            err = "DRI2: failed to get fd from tbm_drm_helper_get_auth_info()";
            goto cleanup_device;
#endif
         }
      }

      if (fd == -1) {
         err = "DRI2: failed to get fd";
         goto cleanup_device;
      }
      dri2_dpy->fd = fd;

      dri2_dpy->driver_name = loader_get_driver_for_fd(dri2_dpy->fd);
      if (dri2_dpy->driver_name == NULL) {
         err = "DRI2: failed to get driver name";
         goto cleanup_device;
      }
      if (!dri2_load_driver(disp)) {
         err = "DRI2: failed to load driver";
         goto cleanup_driver_name;
      }

      dri2_dpy->is_render_node = drmGetNodeTypeFromFd(dri2_dpy->fd) == DRM_NODE_RENDER;
      /* render nodes cannot use Gem names, and thus do not support
       * the __DRI_DRI2_LOADER extension */

      if (!dri2_dpy->is_render_node)
        dri2_dpy->loader_extensions = tizen_dri2_loader_extensions;
      else
        dri2_dpy->loader_extensions = tizen_image_loader_extensions;

   } else {
      dri2_dpy->fd = tbm_bufmgr_fd;
      dri2_dpy->driver_name = strdup("swrast");
      if (!dri2_load_driver_swrast(disp)) {
         err = "DRI2: failed to load swrast driver";
         goto cleanup_device;
      }
      dri2_dpy->loader_extensions = tizen_swrast_loader_extensions;
   }

   if (!dri2_create_screen(disp)) {
      err = "DRI2: failed to create screen";
      goto cleanup_driver;
   }


   dev = _eglAddDevice(dri2_dpy->fd, false);
   if (!dev) {
      err = "DRI2: failed to find EGLDevice";
      goto cleanup_screen;;
   }

   disp->Device = dev;

   if (!dri2_setup_extensions(disp)) {
      err = "DRI2: failed to find required DRI extensions";
      goto cleanup_screen;
   }

   dri2_setup_screen(disp);

   if (!tizen_add_configs(drv, disp)) {
      err = "DRI2: failed to add configs";
      goto cleanup_screen;
   }

   /* Fill vtbl last to prevent accidentally calling virtual function during
    * initialization.
    */
   dri2_dpy->vtbl = &tizen_display_vtbl;

   disp->Extensions.EXT_buffer_age = EGL_FALSE;
   disp->Extensions.EXT_swap_buffers_with_damage = EGL_FALSE;
   disp->Extensions.TIZEN_image_native_surface = EGL_TRUE;
   disp->Extensions.WL_bind_wayland_display = EGL_TRUE;
   disp->Extensions.WL_create_wayland_buffer_from_image = EGL_FALSE;

   drv->API.BindWaylandDisplayWL = tizen_bind_wayland_display_wl;
   drv->API.UnbindWaylandDisplayWL = tizen_unbind_wayland_display_wl;
   drv->API.QueryWaylandBufferWL = tizen_query_wayland_buffer_wl;

   return EGL_TRUE;

cleanup_screen:
   dri2_dpy->core->destroyScreen(dri2_dpy->dri_screen);
cleanup_driver:
   dlclose(dri2_dpy->driver);
cleanup_driver_name:
   if (hw_accel)
     free(dri2_dpy->driver_name);
cleanup_device:
   if (tbm_bufmgr_device_name)
     free(tbm_bufmgr_device_name);
   tpl_object_unreference((tpl_object_t *)tpl_display);
cleanup_display:
   if (dri2_dpy->own_device) {
      wl_display_disconnect(disp->PlatformDisplay);
      disp->PlatformDisplay = NULL;
   }
   free(dri2_dpy);
   disp->DriverData = NULL;

   return _eglError(EGL_NOT_INITIALIZED, err);
}

void
dri2_teardown_tizen(_EGLDisplay *disp)
{
    struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

    if (dri2_dpy->tpl_display)
        tpl_object_unreference((tpl_object_t *)(dri2_dpy->tpl_display));

    if (tbm_bufmgr_fd > 0)
        close(tbm_bufmgr_fd);

    if (dri2_dpy->own_device)
        wl_display_disconnect(disp->PlatformDisplay);
}
