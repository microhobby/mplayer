/*
 * DRM/KMS video output driver for MPlayer
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm_fourcc.h>

#include "config.h"
#include "video_out.h"
#include "video_out_internal.h"
#include "fastmemcpy.h"
#include "sub/sub.h"
#include "mp_msg.h"
#include "aspect.h"
#include "libswscale/swscale.h"
#include "libavutil/pixfmt.h"

static const vo_info_t info = {
	"Direct Rendering Manager (DRM/KMS)",
	"drm",
	"MPlayer DRM driver",
	""
};

// Function prototypes
static int config(uint32_t width, uint32_t height, uint32_t d_width,
                 uint32_t d_height, uint32_t flags, char *title,
                 uint32_t format);
static int preinit(const char *subdevice);
static void uninit(void);
static int control(uint32_t request, void *data);
static int draw_frame(uint8_t *src[]);
static int draw_slice(uint8_t *image[], int stride[], int w, int h, int x, int y);
static void draw_osd(void);
static void flip_page(void);
static void check_events(void);
static int query_format(uint32_t format);

const LIBVO_EXTERN(drm)

#define BYTES_PER_PIXEL 4
#define BITS_PER_PIXEL 32

struct drm_framebuffer {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t size;
    uint32_t handle;
    uint8_t *map;
    uint32_t fb_id;
};

// Global variables for DRM state (MPlayer style)
static int drm_fd = -1;
static drmModeCrtc *saved_crtc = NULL;
static drmModeConnector *connector = NULL;
static drmModeEncoder *encoder = NULL;
static drmModeModeInfo mode;
static uint32_t crtc_id;
static uint32_t connector_id;

static struct drm_framebuffer primary_fb;
static struct drm_framebuffer *current_fb = NULL;

static uint32_t drm_format = DRM_FORMAT_XRGB8888;
static int screen_width;
static int screen_height;
static int image_width;
static int image_height;
static uint32_t image_format;

// Centering offsets (like fbdev2)
static int x_offset;
static int y_offset;
static uint8_t *center = NULL; // where to begin writing our image (centered)

// Format conversion context (only for format conversion, not scaling)
static struct SwsContext *sws_ctx = NULL;
static uint8_t *convert_buffer = NULL;
static int convert_buffer_size = 0;

static void (*draw_alpha_p)(int w, int h, unsigned char *src,
		unsigned char *srca, int stride, unsigned char *dst,
		int dstride);

// Helper function to convert MPlayer image format to ffmpeg pixel format
static enum AVPixelFormat imgfmt2pixfmt(int fmt)
{
    switch (fmt) {
        case IMGFMT_RGB24: return AV_PIX_FMT_RGB24;
        case IMGFMT_BGR24: return AV_PIX_FMT_BGR24;
        case IMGFMT_RGB32: return AV_PIX_FMT_RGB32;
        case IMGFMT_BGR32: return AV_PIX_FMT_BGR32;
        case IMGFMT_YV12:  return AV_PIX_FMT_YUV420P;
        case IMGFMT_I420:  return AV_PIX_FMT_YUV420P;
        case IMGFMT_YUY2:  return AV_PIX_FMT_YUYV422;
        case IMGFMT_UYVY:  return AV_PIX_FMT_UYVY422;
        default:           return AV_PIX_FMT_NONE;
    }
}

static void drm_destroy_framebuffer(struct drm_framebuffer *fb)
{
    if (fb->map) {
        munmap(fb->map, fb->size);
        fb->map = NULL;
    }
    if (fb->fb_id) {
        drmModeRmFB(drm_fd, fb->fb_id);
        fb->fb_id = 0;
    }
    if (fb->handle) {
        struct drm_mode_destroy_dumb dreq = {
            .handle = fb->handle,
        };
        drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        fb->handle = 0;
    }
}

static int drm_create_framebuffer(struct drm_framebuffer *fb, uint32_t width, uint32_t height)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;
    int ret;

    memset(fb, 0, sizeof(*fb));
    fb->width = width;
    fb->height = height;

    // create dumb buffer
    memset(&creq, 0, sizeof(creq));
    creq.width = width;
    creq.height = height;
    creq.bpp = BITS_PER_PIXEL;

    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        mp_msg(MSGT_VO, MSGL_ERR, "[drm] Cannot create dumb buffer: %s\n", strerror(errno));
        return -1;
    }

    fb->stride = creq.pitch;
    fb->size = creq.size;
    fb->handle = creq.handle;

    // create framebuffer object for the dumb-buffer
    ret = drmModeAddFB(drm_fd, width, height, 24, BITS_PER_PIXEL,
                      fb->stride, fb->handle, &fb->fb_id);
    if (ret) {
        mp_msg(MSGT_VO, MSGL_ERR, "[drm] Cannot create framebuffer: %s\n", strerror(errno));
        goto err;
    }

    // prepare buffer for memory mapping
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = fb->handle;
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
        mp_msg(MSGT_VO, MSGL_ERR, "[drm] Cannot map dumb buffer: %s\n", strerror(errno));
        goto err;
    }

    // perform actual memory mapping
    fb->map = mmap(0, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   drm_fd, mreq.offset);
    if (fb->map == MAP_FAILED) {
        mp_msg(MSGT_VO, MSGL_ERR, "[drm] Cannot map dumb buffer: %s\n", strerror(errno));
        goto err;
    }

    memset(fb->map, 0, fb->size);
    return 0;

err:
    drm_destroy_framebuffer(fb);
    return -1;
}

static int drm_find_display(void)
{
    drmModeRes *resources;
    drmModeConnector *conn;
    drmModeEncoder *enc;
    int i, j;

    resources = drmModeGetResources(drm_fd);
    if (!resources) {
        mp_msg(MSGT_VO, MSGL_ERR, "[drm] Cannot retrieve DRM resources: %s\n", strerror(errno));
        return -1;
    }

    // Find a connected connector
    for (i = 0; i < resources->count_connectors; i++) {
        conn = drmModeGetConnector(drm_fd, resources->connectors[i]);
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            connector = conn;
            connector_id = conn->connector_id;
            mode = conn->modes[0]; // Use first mode
            break;
        }
        drmModeFreeConnector(conn);
    }

    if (!connector) {
        mp_msg(MSGT_VO, MSGL_ERR, "[drm] No connected connector found\n");
        drmModeFreeResources(resources);
        return -1;
    }

    // Find encoder
    for (i = 0; i < resources->count_encoders; i++) {
        enc = drmModeGetEncoder(drm_fd, resources->encoders[i]);
        if (enc->encoder_id == connector->encoder_id) {
            encoder = enc;
            break;
        }
        drmModeFreeEncoder(enc);
    }

    if (encoder) {
        crtc_id = encoder->crtc_id;
    } else {
        // Find a CRTC
        for (i = 0; i < resources->count_crtcs; i++) {
            for (j = 0; j < connector->count_encoders; j++) {
                enc = drmModeGetEncoder(drm_fd, connector->encoders[j]);
                if (enc->possible_crtcs & (1 << i)) {
                    crtc_id = resources->crtcs[i];
                    encoder = enc;
                    goto found_crtc;
                }
                drmModeFreeEncoder(enc);
            }
        }
found_crtc:;
    }

    if (!crtc_id) {
        mp_msg(MSGT_VO, MSGL_ERR, "[drm] No suitable CRTC found\n");
        drmModeFreeResources(resources);
        return -1;
    }

    screen_width = mode.hdisplay;
    screen_height = mode.vdisplay;

    mp_msg(MSGT_VO, MSGL_INFO, "[drm] Selected mode: %dx%d@%dHz\n",
           screen_width, screen_height, mode.vrefresh);

    drmModeFreeResources(resources);
    return 0;

}

static int drm_setup_display(void)
{
    int ret;

    // Save current CRTC configuration for restoration
    saved_crtc = drmModeGetCrtc(drm_fd, crtc_id);
    if (!saved_crtc) {
        mp_msg(MSGT_VO, MSGL_ERR, "[drm] Cannot get current CRTC: %s\n", strerror(errno));
        return -1;
    }

    // Create primary framebuffer
    if (drm_create_framebuffer(&primary_fb, screen_width, screen_height) < 0) {
        mp_msg(MSGT_VO, MSGL_ERR, "[drm] Cannot create primary framebuffer\n");
        return -1;
    }

    current_fb = &primary_fb;

    // Set the display mode
    ret = drmModeSetCrtc(drm_fd, crtc_id, primary_fb.fb_id, 0, 0,
                        &connector_id, 1, &mode);
    if (ret) {
        mp_msg(MSGT_VO, MSGL_ERR, "[drm] Cannot set CRTC: %s\n", strerror(errno));
        return -1;
    }

    mp_msg(MSGT_VO, MSGL_INFO, "[drm] DRM display initialized successfully\n");

    return 0;
}

static int preinit(const char *subdevice)
{
    const char *device = subdevice ? subdevice : "/dev/dri/card0";

    // Open DRM device
    drm_fd = open(device, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        mp_msg(MSGT_VO, MSGL_ERR, "[drm] Cannot open DRM device %s: %s\n",
               device, strerror(errno));
        return -1;
    }

    // Set DRM master
    if (drmSetMaster(drm_fd)) {
        mp_msg(MSGT_VO, MSGL_ERR, "[drm] Cannot set DRM master: %s\n", strerror(errno));
        close(drm_fd);
        drm_fd = -1;
        return -1;
    }

    // Find connected display
    if (drm_find_display() < 0) {
        close(drm_fd);
        drm_fd = -1;
        return -1;
    }

    return 0;
}

static int config(uint32_t width, uint32_t height, uint32_t d_width,
                 uint32_t d_height, uint32_t flags, char *title,
                 uint32_t format)
{
    enum AVPixelFormat src_format;

    image_width = width;
    image_height = height;
    image_format = format;

    if (drm_setup_display() < 0) {
        return 1;
    }

    // Calculate centering offsets (like fbdev2)
    x_offset = (screen_width - width) / 2;
    y_offset = (screen_height - height) / 2;

    // Clip offsets to valid ranges
    if (x_offset < 0) x_offset = 0;
    if (y_offset < 0) y_offset = 0;
    if (x_offset + width > screen_width) x_offset = screen_width - width;
    if (y_offset + height > screen_height) y_offset = screen_height - height;

    // Calculate center pointer (like fbdev2)
    center = current_fb->map +
             (y_offset * current_fb->stride) +
             (x_offset * BYTES_PER_PIXEL);

    // Convert MPlayer format to FFmpeg format
    src_format = imgfmt2pixfmt(format);

    // Initialize conversion context (no scaling, just format conversion)
    sws_ctx = sws_getContext(width, height, src_format,
                            width, height, AV_PIX_FMT_BGRA,
                            SWS_POINT, NULL, NULL, NULL);
    if (!sws_ctx) {
        mp_msg(MSGT_VO, MSGL_ERR, "[drm] Cannot initialize conversion context\n");
        return 1;
    }

    // Allocate conversion buffer (same size as input, different format)
    convert_buffer_size = width * height * BYTES_PER_PIXEL;
    convert_buffer = malloc(convert_buffer_size);
    if (!convert_buffer) {
        mp_msg(MSGT_VO, MSGL_ERR, "[drm] Cannot allocate conversion buffer\n");
        return 1;
    }

    draw_alpha_p = vo_get_draw_alpha(AV_PIX_FMT_BGRA);

    mp_msg(MSGT_VO, MSGL_INFO, "[drm] Video configured: %dx%d centered at +%d+%d\n",
           width, height, x_offset, y_offset);

    return 0;
}

static int query_format(uint32_t format)
{
    // Check if the format is supported by libswscale
    return sws_isSupportedInput(imgfmt2pixfmt(format)) ?
           VFCAP_CSP_SUPPORTED | VFCAP_CSP_SUPPORTED_BY_HW : 0;
}

static int draw_slice(uint8_t *src[], int stride[], int w, int h, int x, int y)
{
    uint8_t *dst_buf[4];
    int dst_stride[4];

    if (!current_fb || !current_fb->map || !sws_ctx || !center || !convert_buffer)
        return VO_ERROR;

    // Convert format to BGRA in conversion buffer first
    dst_buf[0] = convert_buffer + (y * image_width * BYTES_PER_PIXEL) + (x * BYTES_PER_PIXEL);
    dst_buf[1] = dst_buf[2] = dst_buf[3] = NULL;
    dst_stride[0] = image_width * BYTES_PER_PIXEL;
    dst_stride[1] = dst_stride[2] = dst_stride[3] = 0;

    sws_scale(sws_ctx, (const uint8_t * const*)src, stride,
              0, h, dst_buf, dst_stride);

    // Copy the converted slice to the centered position in framebuffer
    memcpy_pic(center + (y * current_fb->stride) + (x * BYTES_PER_PIXEL),
               convert_buffer + (y * image_width * BYTES_PER_PIXEL) + (x * BYTES_PER_PIXEL),
               w * BYTES_PER_PIXEL, h,
               current_fb->stride, image_width * BYTES_PER_PIXEL);

    return VO_TRUE;
}

static void draw_alpha(int x0, int y0, int w, int h, unsigned char *src,
                      unsigned char *srca, int stride)
{
    unsigned char *dst;

    if (!draw_alpha_p || !current_fb || !current_fb->map || !center)
        return;

    dst = center + (y0 * current_fb->stride) + (x0 * BYTES_PER_PIXEL);

    (*draw_alpha_p)(w, h, src, srca, stride, dst, current_fb->stride);
}

static void draw_osd(void)
{
    vo_draw_text(screen_width, screen_height, draw_alpha);
}

static int draw_frame(uint8_t *src[])
{
    uint8_t *dst_buf[4];
    int dst_stride[4];
    int src_stride[4];
    int bytes_per_pixel;

    if (!current_fb || !current_fb->map || !sws_ctx)
        return VO_ERROR;

    // Scale directly to framebuffer
    dst_buf[0] = current_fb->map;
    dst_buf[1] = dst_buf[2] = dst_buf[3] = NULL;
    dst_stride[0] = current_fb->stride;
    dst_stride[1] = dst_stride[2] = dst_stride[3] = 0;

    // Calculate source stride based on the actual input format
    switch (image_format) {
        case IMGFMT_RGB24:
        case IMGFMT_BGR24:
            bytes_per_pixel = 3;
            break;
        case IMGFMT_RGB32:
        case IMGFMT_BGR32:
            bytes_per_pixel = 4;
            break;
        case IMGFMT_YV12:
        case IMGFMT_I420:
            // For YUV420, stride is just width for Y plane
            bytes_per_pixel = 1;
            break;
        default:
            bytes_per_pixel = 4; // Default fallback
            break;
    }

    src_stride[0] = image_width * bytes_per_pixel;
    src_stride[1] = src_stride[2] = src_stride[3] = 0;

    sws_scale(sws_ctx, (const uint8_t * const*)src, src_stride,
              0, image_height, dst_buf, dst_stride);

    return VO_TRUE;
}

static void check_events(void)
{
    // No window events to handle in DRM mode
}

static void flip_page(void)
{
    int ret;

    if (!current_fb || drm_fd < 0)
        return;

    // Force display update by setting CRTC again
    // This ensures the display shows our updated framebuffer
    ret = drmModeSetCrtc(drm_fd, crtc_id, current_fb->fb_id, 0, 0,
                        &connector_id, 1, &mode);
    if (ret) {
        mp_msg(MSGT_VO, MSGL_ERR, "[drm] flip_page: Cannot update CRTC: %s\n", strerror(errno));
    }

    // Optional: wait for VSync if supported
    // This would normally be done with drmWaitVBlank but keeping it simple for now
}

static void uninit(void)
{
    // Cleanup scaling context
    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = NULL;
    }

    // Free conversion buffer
    if (convert_buffer) {
        free(convert_buffer);
        convert_buffer = NULL;
    }

    // Restore original CRTC
    if (saved_crtc && drm_fd >= 0) {
        drmModeSetCrtc(drm_fd, saved_crtc->crtc_id,
                      saved_crtc->buffer_id,
                      saved_crtc->x, saved_crtc->y,
                      &connector_id, 1, &saved_crtc->mode);
        drmModeFreeCrtc(saved_crtc);
        saved_crtc = NULL;
    }

    // Cleanup framebuffer
    drm_destroy_framebuffer(&primary_fb);

    // Cleanup DRM resources
    if (connector) {
        drmModeFreeConnector(connector);
        connector = NULL;
    }
    if (encoder) {
        drmModeFreeEncoder(encoder);
        encoder = NULL;
    }

    // Close DRM device
    if (drm_fd >= 0) {
        drmDropMaster(drm_fd);
        close(drm_fd);
        drm_fd = -1;
    }

    mp_msg(MSGT_VO, MSGL_INFO, "[drm] DRM cleanup completed\n");
}

static int control(uint32_t request, void *data)
{
    switch (request) {
    case VOCTRL_QUERY_FORMAT:
        return query_format(*((uint32_t*)data));
    case VOCTRL_UPDATE_SCREENINFO:
        vo_screenwidth = screen_width;
        vo_screenheight = screen_height;
        aspect_save_screenres(vo_screenwidth, vo_screenheight);
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}
