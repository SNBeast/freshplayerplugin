/*
 * Copyright © 2013-2017  Rinat Ibragimov
 *
 * This file is part of FreshPlayerPlugin.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"
#include "glx.h"
#include "pp_interface.h"
#include "pp_resource.h"
#include "ppb_buffer.h"
#include "ppb_graphics3d.h"
#include "ppb_instance.h"
#include "ppb_message_loop.h"
#include "ppb_video_decoder.h"
#include "reverse_constant.h"
#include "static_assert.h"
#include "tables.h"
#include "trace_core.h"
#include "utils.h"
#include <GLES2/gl2.h>
#include <X11/Xlib.h>
#include <glib.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <ppapi/c/dev/ppp_video_decoder_dev.h>
#include <ppapi/c/pp_errors.h>
#include <pthread.h>
#include <stdlib.h>

enum hwdec_api_e {
    HWDEC_NONE = 0,
    HWDEC_VAAPI,
};

#define DEFAULT_PICTURE_BUFFERS 18  // H.264: 16 references and 2 working

// H.264 have maximum 16 reference frames. Plus one current. Plus one frame for delayed
// release when PPAPI client first calls .decode() and only then calls
// .reuse_picture_buffer()

struct pp_video_decoder_s {
    COMMON_STRUCTURE_FIELDS
    const struct PPP_VideoDecoder_Dev_0_11 *ppp_video_decoder_dev;
    PP_Resource             orig_graphics3d;
    PP_Resource             graphics3d;
    int                     codec_id;
    const AVCodec          *avcodec;
    AVCodecContext         *avctx;
    AVCodecParserContext   *avparser;
    AVFrame                *avframe;
    AVFrame                *swframe;
    struct SwsContext      *sws_ctx;
    uint8_t                *rgba_buf;
    size_t                  rgba_buf_size;
    int                     sws_width;
    int                     sws_height;
    enum AVPixelFormat      sws_src_fmt;
    int32_t                 last_consumed_bitstream_buffer_id;
    size_t                  buffer_count;
    struct {
        int32_t     id;
        uint32_t    width;
        uint32_t    height;
        uint32_t    texture_id;
        uint32_t    used;
    } *buffers;

    unsigned int            initialized : 1;
    unsigned int            buffers_were_requested : 1;
    unsigned int            failed_state : 1;
    enum hwdec_api_e        hwdec_api;  ///< HW decoding API used by this resource
};

STATIC_ASSERT(sizeof(struct pp_video_decoder_s) <= LARGEST_RESOURCE_SIZE);

static
void
deinitialize_decoder(struct pp_video_decoder_s *vd)
{
    if (vd->graphics3d) {
        pp_resource_unref(vd->graphics3d);
        vd->graphics3d = 0;
    }

    if (vd->avparser) {
        av_parser_close(vd->avparser);
        vd->avparser = NULL;
    }

    if (vd->avctx) {
        avcodec_free_context(&vd->avctx);
    }

    if (vd->avframe)
        av_frame_free(&vd->avframe);

    if (vd->swframe)
        av_frame_free(&vd->swframe);

    if (vd->sws_ctx) {
        sws_freeContext(vd->sws_ctx);
        vd->sws_ctx = NULL;
    }

    av_freep(&vd->rgba_buf);
    vd->rgba_buf_size = 0;
    vd->sws_width = 0;
    vd->sws_height = 0;
    vd->sws_src_fmt = AV_PIX_FMT_NONE;

    for (uintptr_t k = 0; k < vd->buffer_count; k ++) {
        vd->ppp_video_decoder_dev->DismissPictureBuffer(vd->instance->id, vd->self_id,
                                                        vd->buffers[k].id);
    }

    vd->hwdec_api = HWDEC_NONE;
    vd->buffer_count = 0;
    vd->buffers_were_requested = 0;
    vd->initialized = 0;
    free_and_nullify(vd->buffers);
}

static
void
ppb_video_decoder_destroy_priv(void *p)
{
    struct pp_video_decoder_s *vd = p;

    if (vd->orig_graphics3d) {
        pp_resource_unref(vd->orig_graphics3d);
        vd->orig_graphics3d = 0;
    }

    deinitialize_decoder(vd);
}

static
enum AVPixelFormat
get_format(struct AVCodecContext *s, const enum AVPixelFormat *fmt)
{
    struct pp_video_decoder_s *vd = s->opaque;

    if (vd->hwdec_api == HWDEC_VAAPI) {
        for (int k = 0; fmt[k] != AV_PIX_FMT_NONE; k ++) {
            if (fmt[k] == AV_PIX_FMT_VAAPI) {
                trace_info_f("      VA-API: enabled via FFmpeg hwcontext\n");
                return AV_PIX_FMT_VAAPI;
            }
        }
    }

    trace_info_f("      VA-API: unavailable for stream, falling back to software\n");
    vd->hwdec_api = HWDEC_NONE;
    return fmt[0];
}

static
int
initialize_decoder(struct pp_video_decoder_s *vd)
{
    // create auxiliary GL context
    int32_t attribs[] = {
        PP_GRAPHICS3DATTRIB_WIDTH,      32,     // dimensions can be arbitrary
        PP_GRAPHICS3DATTRIB_HEIGHT,     32,
        PP_GRAPHICS3DATTRIB_RED_SIZE,   8,
        PP_GRAPHICS3DATTRIB_GREEN_SIZE, 8,
        PP_GRAPHICS3DATTRIB_BLUE_SIZE,  8,
        PP_GRAPHICS3DATTRIB_ALPHA_SIZE, 8,
        PP_GRAPHICS3DATTRIB_DEPTH_SIZE, 16,
        PP_GRAPHICS3DATTRIB_NONE,
    };

    vd->graphics3d = ppb_graphics3d_create(vd->instance->id, vd->orig_graphics3d, attribs);
    if (!vd->graphics3d) {
        trace_error("%s, can't create graphics3d context\n", __func__);
        goto err;
    }

    vd->hwdec_api = HWDEC_NONE;
    vd->avcodec = avcodec_find_decoder(vd->codec_id);
    if (!vd->avcodec) {
        trace_error("%s, can't create codec\n", __func__);
        goto err;
    }

    vd->avparser = av_parser_init(vd->codec_id);
    if (!vd->avparser) {
        trace_error("%s, can't create parser\n", __func__);
        goto err;
    }

    vd->avctx = avcodec_alloc_context3(vd->avcodec);
    if (!vd->avctx) {
        trace_error("%s, can't create codec context\n", __func__);
        goto err;
    }

    vd->avctx->opaque = vd;
    vd->avctx->get_format = get_format;

    AVBufferRef *hw_device_ref = NULL;
    if (av_hwdevice_ctx_create(&hw_device_ref, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0) == 0) {
        vd->avctx->hw_device_ctx = av_buffer_ref(hw_device_ref);
        av_buffer_unref(&hw_device_ref);
        vd->hwdec_api = HWDEC_VAAPI;
    } else {
        trace_info_f("%s, VA-API hwdevice init failed, using software decoding\n", __func__);
    }

    if (vd->hwdec_api == HWDEC_NONE) {
        vd->avctx->thread_count = 0; // auto-detect CPU threads for software decoding
    }

    if (avcodec_open2(vd->avctx, vd->avcodec, NULL) < 0) {
        trace_error("%s, can't open codec\n", __func__);
        goto err;
    }

    vd->avframe = av_frame_alloc();
    vd->swframe = av_frame_alloc();
    if (!vd->avframe || !vd->swframe) {
        trace_error("%s, can't alloc frames\n", __func__);
        goto err;
    }

    vd->initialized = 1;
    return 0;

err:
    deinitialize_decoder(vd);
    return -1;
}

PP_Resource
ppb_video_decoder_create(PP_Instance instance, PP_Resource context, PP_VideoDecoder_Profile profile)
{
    switch (profile) {
    case PP_VIDEODECODER_H264PROFILE_BASELINE:
    case PP_VIDEODECODER_H264PROFILE_MAIN:
    case PP_VIDEODECODER_H264PROFILE_EXTENDED:
    case PP_VIDEODECODER_H264PROFILE_HIGH:
        // pass, there is an implementation below
        break;

    case PP_VIDEODECODER_H264PROFILE_NONE:
    case PP_VIDEODECODER_H264PROFILE_HIGH10PROFILE:
    case PP_VIDEODECODER_H264PROFILE_HIGH422PROFILE:
    case PP_VIDEODECODER_H264PROFILE_HIGH444PREDICTIVEPROFILE:
    case PP_VIDEODECODER_H264PROFILE_SCALABLEBASELINE:
    case PP_VIDEODECODER_H264PROFILE_SCALABLEHIGH:
    case PP_VIDEODECODER_H264PROFILE_STEREOHIGH:
    case PP_VIDEODECODER_H264PROFILE_MULTIVIEWHIGH:
    case PP_VIDEODECODER_VP8PROFILE_ANY:
    case PP_VIDEODECODER_PROFILE_UNKNOWN:
    default:
        trace_error("%s, profile %d is not supported\n", __func__, profile);
        return 0;
    }

    const struct PPP_VideoDecoder_Dev_0_11 *ppp_video_decoder_dev = NULL;
    struct pp_instance_s *pp_i = tables_get_pp_instance(instance);
    if (!pp_i) {
        trace_error("%s, bad instance\n", __func__);
        return 0;
    }

    ppp_video_decoder_dev = ppp_get_interface(PPP_VIDEODECODER_DEV_INTERFACE);
    if (!ppp_video_decoder_dev) {
        trace_error("%s, no viable %s\n", __func__, PPP_VIDEODECODER_DEV_INTERFACE);
        return 0;
    }

    if (pp_resource_get_type(context) != PP_RESOURCE_GRAPHICS3D) {
        trace_error("%s, bad resource\n", __func__);
        return 0;
    }

    PP_Resource video_decoder = pp_resource_allocate(PP_RESOURCE_VIDEO_DECODER, pp_i);
    struct pp_video_decoder_s *vd = pp_resource_acquire(video_decoder, PP_RESOURCE_VIDEO_DECODER);
    if (!vd) {
        trace_error("%s, resource allocation failed\n", __func__);
        return 0;
    }

    vd->orig_graphics3d = pp_resource_ref(context);
    vd->ppp_video_decoder_dev = ppp_video_decoder_dev;
    vd->codec_id = AV_CODEC_ID_H264;        // TODO: other codecs
    vd->hwdec_api = HWDEC_NONE;

    pp_resource_release(video_decoder);
    return video_decoder;
}

PP_Bool
ppb_video_decoder_is_video_decoder(PP_Resource resource)
{
    return pp_resource_get_type(resource) == PP_RESOURCE_VIDEO_DECODER;
}

static
void
request_buffers(struct pp_video_decoder_s *vd)
{
    const PP_Instance instance = vd->instance->id;
    const struct PP_Size dimensions = { .width = vd->avctx->width,
                                        .height = vd->avctx->height };

    pp_resource_release(vd->self_id);

    vd->ppp_video_decoder_dev->ProvidePictureBuffers(instance, vd->self_id, DEFAULT_PICTURE_BUFFERS,
                                                     &dimensions, GL_TEXTURE_2D);
    pp_resource_acquire(vd->self_id, PP_RESOURCE_VIDEO_DECODER);
}

static
uint32_t
find_free_buffer(struct pp_video_decoder_s *vd)
{
    for (uint32_t idx = 0; idx < vd->buffer_count; idx ++) {
        if (!vd->buffers[idx].used) {
            vd->buffers[idx].used = 1;
            return idx;
        }
    }

    return (uint32_t)-1;
}

static
int
ensure_sws_and_rgba_buffer(struct pp_video_decoder_s *vd, int width, int height,
                           enum AVPixelFormat src_fmt)
{
    if (vd->sws_ctx && vd->sws_width == width && vd->sws_height == height && vd->sws_src_fmt == src_fmt) {
        return 0;
    }
    if (vd->sws_ctx) {
        sws_freeContext(vd->sws_ctx);
        vd->sws_ctx = NULL;
    }

    size_t needed = (size_t)av_image_get_buffer_size(AV_PIX_FMT_RGBA, width, height, 1);
    if (needed > vd->rgba_buf_size) {
        av_freep(&vd->rgba_buf);
        vd->rgba_buf = av_malloc(needed);
        if (!vd->rgba_buf) {
            vd->rgba_buf_size = 0;
            return -1;
        }
        vd->rgba_buf_size = needed;
    }

    vd->sws_ctx = sws_getContext(width, height, src_fmt,
                                 width, height, AV_PIX_FMT_RGBA,
                                 SWS_BILINEAR, NULL, NULL, NULL);
    if (!vd->sws_ctx)
        return -1;

    vd->sws_width = width;
    vd->sws_height = height;
    vd->sws_src_fmt = src_fmt;
    return 0;
}

static
void
issue_frame(struct pp_video_decoder_s *vd)
{
    AVFrame *frame = vd->avframe;
    uint32_t idx = find_free_buffer(vd);
    int32_t  bitstream_buffer_id = (int32_t)frame->pts;

    if (idx == (uint32_t)-1) {
        trace_warning("%s, no free buffer available\n", __func__);
        return;
    }

    if (frame->format == AV_PIX_FMT_VAAPI) {
        av_frame_unref(vd->swframe);
        if (av_hwframe_transfer_data(vd->swframe, frame, 0) < 0) {
            trace_error("%s, av_hwframe_transfer_data failed\n", __func__);
            vd->buffers[idx].used = 0;
            return;
        }
        frame = vd->swframe;
    }

    if (ensure_sws_and_rgba_buffer(vd, frame->width, frame->height,
                                   (enum AVPixelFormat)frame->format) < 0) {
        trace_error("%s, failed to prepare RGBA conversion buffer\n", __func__);
        vd->buffers[idx].used = 0;
        return;
    }

    uint8_t *dst_data[4] = { vd->rgba_buf, NULL, NULL, NULL };
    int dst_linesize[4]  = { frame->width * 4, 0, 0, 0 };
    sws_scale(vd->sws_ctx, (const uint8_t * const *)frame->data, frame->linesize,
              0, frame->height, dst_data, dst_linesize);

    struct pp_graphics3d_s *g3d = pp_resource_acquire(vd->graphics3d, PP_RESOURCE_GRAPHICS3D);
    if (!g3d) {
        trace_error("%s, bad resource\n", __func__);
        vd->buffers[idx].used = 0;
        return;
    }

    pthread_mutex_lock(&display.lock);

    glXMakeCurrent(display.x, g3d->glx_pixmap, g3d->glc);
    glBindTexture(GL_TEXTURE_2D, vd->buffers[idx].texture_id);

    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame->width, frame->height,
                    GL_RGBA, GL_UNSIGNED_BYTE, vd->rgba_buf);

    glBindTexture(GL_TEXTURE_2D, 0);
    glXMakeCurrent(display.x, None, NULL);

    pthread_mutex_unlock(&display.lock);

    pp_resource_release(vd->graphics3d);

    const PP_Instance instance = vd->instance->id;
    const struct PP_Picture_Dev picture = {
        .picture_buffer_id = vd->buffers[idx].id,
        .bitstream_buffer_id = bitstream_buffer_id,
    };

    pp_resource_release(vd->self_id);
    vd->ppp_video_decoder_dev->PictureReady(instance, vd->self_id, &picture);
    pp_resource_acquire(vd->self_id, PP_RESOURCE_VIDEO_DECODER);
}

static
void
decode_frame(struct pp_video_decoder_s *vd, uint8_t *data, size_t data_len,
             int32_t bitstream_buffer_id)
{
    AVPacket *packet = av_packet_alloc();
    if (!packet)
        return;

    packet->data = data;
    packet->size = data_len;
    packet->pts = bitstream_buffer_id;

    int ret = avcodec_send_packet(vd->avctx, packet);
    av_packet_free(&packet);

    if (ret < 0) {
        trace_error("%s, error %d while sending packet to decoder\n", __func__, ret);
        return;
    }

    while (1) {
        int val = avcodec_receive_frame(vd->avctx, vd->avframe);
        if (val == AVERROR(EAGAIN) || val == AVERROR_EOF) {
            // normal, nothing more for now
            break;
        }
        if (val < 0) {
            trace_error("%s, error %d while receiving frame from decoder\n", __func__, val);
            return;
        }
        if (!vd->buffers_were_requested) {
            request_buffers(vd);
            vd->buffers_were_requested = 1;
        }

        issue_frame(vd);
        av_frame_unref(vd->avframe);
    }
}

int32_t
ppb_video_decoder_decode(PP_Resource video_decoder,
                         const struct PP_VideoBitstreamBuffer_Dev *bitstream_buffer,
                         struct PP_CompletionCallback callback)
{
    struct pp_video_decoder_s *vd = pp_resource_acquire(video_decoder, PP_RESOURCE_VIDEO_DECODER);
    if (!vd) {
        trace_error("%s, bad resource\n", __func__);
        return PP_ERROR_BADRESOURCE;
    }

    if (vd->failed_state) {
        trace_warning("%s, there were errors before, giving up\n", __func__);
        pp_resource_release(video_decoder);
        return PP_ERROR_FAILED;
    }

    if (!vd->initialized) {
        int err = initialize_decoder(vd);
        if (err != 0) {
            vd->failed_state = 1;
            vd->ppp_video_decoder_dev->NotifyError(vd->instance->id, vd->self_id,
                                                   PP_VIDEODECODERERROR_PLATFORM_FAILURE);
            pp_resource_release(video_decoder);
            return PP_ERROR_FAILED;
        }
    }

    void *rawdata = ppb_buffer_map(bitstream_buffer->data);
    if (!rawdata) {
        trace_error("%s, bad bitstream buffer\n", __func__);
        pp_resource_release(video_decoder);
        return PP_ERROR_FAILED;
    }

    uint8_t *inbuf = rawdata;
    size_t   inbuf_sz = bitstream_buffer->size;

    while (inbuf_sz > 0) {
        uint8_t *outbuf = NULL;
        int      outbuf_sz = 0;
        int len = av_parser_parse2(vd->avparser, vd->avctx, &outbuf, &outbuf_sz,
                                   inbuf, inbuf_sz, 0, 0, AV_NOPTS_VALUE);
        if (outbuf_sz > 0)
            decode_frame(vd, outbuf, outbuf_sz, vd->last_consumed_bitstream_buffer_id);
        inbuf += len;
        inbuf_sz -= len;
    }

    vd->last_consumed_bitstream_buffer_id = bitstream_buffer->id;
    ppb_buffer_unmap(bitstream_buffer->data);

    pp_resource_release(video_decoder);
    ppb_message_loop_post_work_with_result(ppb_message_loop_get_current(), callback, 0, PP_OK, 0,
                                           __func__);
    return PP_OK_COMPLETIONPENDING;
}

void
ppb_video_decoder_assign_picture_buffers(PP_Resource video_decoder, uint32_t no_of_buffers,
                                         const struct PP_PictureBuffer_Dev buffers[])
{
    struct pp_video_decoder_s *vd = pp_resource_acquire(video_decoder, PP_RESOURCE_VIDEO_DECODER);
    if (!vd) {
        trace_error("%s, bad resource\n", __func__);
        return;
    }

    vd->buffers = calloc(no_of_buffers, sizeof(*vd->buffers));
    if (!vd->buffers) {
        trace_error("%s, memory allocation failure\n", __func__);
        pp_resource_release(video_decoder);
        return;
    }

    vd->buffer_count = no_of_buffers;
    for (uintptr_t k = 0; k < no_of_buffers; k ++) {
        vd->buffers[k].id =         buffers[k].id;
        vd->buffers[k].width =      buffers[k].size.width;
        vd->buffers[k].height =     buffers[k].size.height;
        vd->buffers[k].texture_id = buffers[k].texture_id;
        vd->buffers[k].used =       0;
    }

    pp_resource_release(video_decoder);
}

void
ppb_video_decoder_reuse_picture_buffer(PP_Resource video_decoder, int32_t picture_buffer_id)
{
    struct pp_video_decoder_s *vd = pp_resource_acquire(video_decoder, PP_RESOURCE_VIDEO_DECODER);
    if (!vd) {
        trace_error("%s, bad resource\n", __func__);
        return;
    }

    for (uintptr_t k = 0; k < vd->buffer_count; k ++) {
        if (vd->buffers[k].id == picture_buffer_id && vd->buffers[k].used) {
            vd->buffers[k].used = 0;
            break;
        }
    }

    pp_resource_release(video_decoder);
}

int32_t
ppb_video_decoder_flush(PP_Resource video_decoder, struct PP_CompletionCallback callback)
{
    struct pp_video_decoder_s *vd = pp_resource_acquire(video_decoder, PP_RESOURCE_VIDEO_DECODER);
    if (!vd) {
        trace_error("%s, bad resource\n", __func__);
        return PP_ERROR_BADRESOURCE;
    }

    if (vd->avctx)
        avcodec_flush_buffers(vd->avctx);

    pp_resource_release(video_decoder);
    ppb_message_loop_post_work_with_result(ppb_message_loop_get_current(), callback, 0, PP_OK, 0,
                                           __func__);
    return PP_OK_COMPLETIONPENDING;
}

int32_t
ppb_video_decoder_reset(PP_Resource video_decoder, struct PP_CompletionCallback callback)
{
    struct pp_video_decoder_s *vd = pp_resource_acquire(video_decoder, PP_RESOURCE_VIDEO_DECODER);
    if (!vd) {
        trace_error("%s, bad resource\n", __func__);
        return PP_ERROR_BADRESOURCE;
    }

    deinitialize_decoder(vd);

    pp_resource_release(video_decoder);
    ppb_message_loop_post_work_with_result(ppb_message_loop_get_current(), callback, 0, PP_OK, 0,
                                           __func__);
    return PP_OK_COMPLETIONPENDING;
}

void
ppb_video_decoder_destroy(PP_Resource video_decoder)
{
}


// trace wrappers
TRACE_WRAPPER
PP_Resource
trace_ppb_video_decoder_create(PP_Instance instance, PP_Resource context,
                               PP_VideoDecoder_Profile profile)
{
    trace_info("[PPB] {full} %s instance=%d, context=%d, profile=%s\n", __func__+6, instance,
               context, reverse_video_decoder_profile(profile));
    return ppb_video_decoder_create(instance, context, profile);
}

TRACE_WRAPPER
PP_Bool
trace_ppb_video_decoder_is_video_decoder(PP_Resource resource)
{
    trace_info("[PPB] {full} %s resource=%d\n", __func__+6, resource);
    return ppb_video_decoder_is_video_decoder(resource);
}

TRACE_WRAPPER
int32_t
trace_ppb_video_decoder_decode(PP_Resource video_decoder,
                               const struct PP_VideoBitstreamBuffer_Dev *bitstream_buffer,
                               struct PP_CompletionCallback callback)
{
    trace_info("[PPB] {full} %s video_decoder=%d, bitstream_buffer={.id=%d, .data=%d, .size=%u}, "
               "callback={.func=%p, .user_data=%p, .flags=%u}\n", __func__+6, video_decoder,
               bitstream_buffer->id, bitstream_buffer->data, bitstream_buffer->size, callback.func,
               callback.user_data, callback.flags);
    return ppb_video_decoder_decode(video_decoder, bitstream_buffer, callback);
}

TRACE_WRAPPER
void
trace_ppb_video_decoder_assign_picture_buffers(PP_Resource video_decoder, uint32_t no_of_buffers,
                                               const struct PP_PictureBuffer_Dev buffers[])
{
    trace_info("[PPB] {full} %s video_decoder=%d, no_of_buffers=%u, buffers=%p\n", __func__+6,
               video_decoder, no_of_buffers, buffers);
    return ppb_video_decoder_assign_picture_buffers(video_decoder, no_of_buffers, buffers);
}

TRACE_WRAPPER
void
trace_ppb_video_decoder_reuse_picture_buffer(PP_Resource video_decoder, int32_t picture_buffer_id)
{
    trace_info("[PPB] {full} %s video_decoder=%d, picture_buffer_id=%d\n", __func__+6,
               video_decoder, picture_buffer_id);
    return ppb_video_decoder_reuse_picture_buffer(video_decoder, picture_buffer_id);
}

TRACE_WRAPPER
int32_t
trace_ppb_video_decoder_flush(PP_Resource video_decoder, struct PP_CompletionCallback callback)
{
    trace_info("[PPB] {full} %s video_decoder=%d, callback={.func=%p, .user_data=%p, .flags=%u}\n",
               __func__+6, video_decoder, callback.func, callback.user_data, callback.flags);
    return ppb_video_decoder_flush(video_decoder, callback);
}

TRACE_WRAPPER
int32_t
trace_ppb_video_decoder_reset(PP_Resource video_decoder, struct PP_CompletionCallback callback)
{
    trace_info("[PPB] {full} %s video_decoder=%d, callback={.func=%p, .user_data=%p, .flags=%u}\n",
               __func__+6, video_decoder, callback.func, callback.user_data, callback.flags);
    return ppb_video_decoder_reset(video_decoder, callback);
}

TRACE_WRAPPER
void
trace_ppb_video_decoder_destroy(PP_Resource video_decoder)
{
    trace_info("[PPB] {zilch} %s video_decoder=%d\n", __func__+6, video_decoder);
    return ppb_video_decoder_destroy(video_decoder);
}

const struct PPB_VideoDecoder_Dev_0_16 ppb_video_decoder_dev_interface_0_16 = {
    .Create =               TWRAPF(ppb_video_decoder_create),
    .IsVideoDecoder =       TWRAPF(ppb_video_decoder_is_video_decoder),
    .Decode =               TWRAPF(ppb_video_decoder_decode),
    .AssignPictureBuffers = TWRAPF(ppb_video_decoder_assign_picture_buffers),
    .ReusePictureBuffer =   TWRAPF(ppb_video_decoder_reuse_picture_buffer),
    .Flush =                TWRAPF(ppb_video_decoder_flush),
    .Reset =                TWRAPF(ppb_video_decoder_reset),
    .Destroy =              TWRAPZ(ppb_video_decoder_destroy),
};

static
void
__attribute__((constructor))
constructor_ppb_video_decoder(void)
{
    register_interface(PPB_VIDEODECODER_DEV_INTERFACE_0_16, &ppb_video_decoder_dev_interface_0_16);
    register_resource(PP_RESOURCE_VIDEO_DECODER, ppb_video_decoder_destroy_priv);
}
