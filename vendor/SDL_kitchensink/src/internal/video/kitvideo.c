#include <assert.h>
#include <math.h>

#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#include "bfplayer/hdr_yuv_tonemap.h"
#include "kitchensink2/internal/kitdecoder.h"
#include "kitchensink2/internal/kitlibstate.h"
#include "kitchensink2/internal/utils/kithelpers.h"
#include "kitchensink2/internal/utils/kitlog.h"
#include "kitchensink2/internal/video/kitvideo.h"
#include "kitchensink2/internal/video/kitvideoutils.h"
#include "kitchensink2/kiterror.h"
#include "kitchensink2/kitformat.h"

#define KIT_VIDEO_EARLY_FAIL 1.0

typedef struct Kit_VideoDecoder {
    struct SwsContext *sws;       ///< Video scaler context
    AVFrame *in_frame;            ///< Raw frame from decoder
    AVFrame *out_frame;           ///< Scaled+converted frame from sws
    AVFrame *tmp_frame;           ///< Intermediary frame for HW decoding
    Kit_PacketBuffer *buffer;     ///< Packet ringbuffer for decoded video packets
    Kit_VideoOutputFormat output; ///< Output video format description
    AVFrame *current;             ///< video frame we are currently reading from
    BfplayerHdrYuvLut *hdr_lut;   ///< Compact in-place HDR-to-SDR lookup table
    BfplayerHdrWorkerPool *hdr_workers;
    int sws_colorspace;
    int sws_full_range;
    unsigned long long hdr_frames;
    unsigned long long hdr_processing_us;
    unsigned int hdr_source_peak_millinits;
    unsigned int hdr_target_peak_millinits;
    int hdr_transfer;
    int hdr_input_full_range;
    int hdr_input_bt2020;
    int hdr_active;
} Kit_VideoDecoder;

static int Kit_FrameColorTransfer(
    const Kit_Decoder *decoder,
    const AVFrame *frame
) {
    if(frame->color_trc != AVCOL_TRC_UNSPECIFIED)
        return frame->color_trc;
    return decoder->stream->codecpar->color_trc;
}

static int Kit_FrameColorSpace(
    const Kit_Decoder *decoder,
    const AVFrame *frame
) {
    if(frame->colorspace != AVCOL_SPC_UNSPECIFIED)
        return frame->colorspace;
    return decoder->stream->codecpar->color_space;
}

static int Kit_FrameColorPrimaries(
    const Kit_Decoder *decoder,
    const AVFrame *frame
) {
    if(frame->color_primaries != AVCOL_PRI_UNSPECIFIED)
        return frame->color_primaries;
    return decoder->stream->codecpar->color_primaries;
}

static int Kit_FrameFullRange(
    const Kit_Decoder *decoder,
    const AVFrame *frame
) {
    int range = frame->color_range;
    if(range == AVCOL_RANGE_UNSPECIFIED)
        range = decoder->stream->codecpar->color_range;
    return range == AVCOL_RANGE_JPEG;
}

static double Kit_FrameHdrPeakNits(const AVFrame *frame) {
    double peak = 1000.0;
    const AVFrameSideData *content_light = av_frame_get_side_data(
        frame,
        AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if(content_light != NULL &&
       content_light->size >= sizeof(AVContentLightMetadata)) {
        const AVContentLightMetadata *metadata =
            (const AVContentLightMetadata *)content_light->data;
        if(metadata->MaxCLL >= 100 && metadata->MaxCLL <= 10000)
            peak = metadata->MaxCLL;
    } else {
        const AVFrameSideData *mastering = av_frame_get_side_data(
            frame,
            AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        if(mastering != NULL &&
           mastering->size >= sizeof(AVMasteringDisplayMetadata)) {
            const AVMasteringDisplayMetadata *metadata =
                (const AVMasteringDisplayMetadata *)mastering->data;
            if(metadata->has_luminance) {
                const double mastering_peak =
                    av_q2d(metadata->max_luminance);
                if(mastering_peak >= 100.0 &&
                   mastering_peak <= 10000.0)
                    peak = mastering_peak;
            }
        }
    }
    return peak;
}

static int Kit_ConfigureHdrLut(
    const Kit_Decoder *decoder,
    Kit_VideoDecoder *video_decoder,
    const AVFrame *frame
) {
    const int transfer = Kit_FrameColorTransfer(decoder, frame);
    if(transfer != AVCOL_TRC_SMPTE2084 &&
       transfer != AVCOL_TRC_ARIB_STD_B67)
        return 0;

    const int colorspace = Kit_FrameColorSpace(decoder, frame);
    const int primaries = Kit_FrameColorPrimaries(decoder, frame);
    BfplayerHdrYuvConfig config;
    config.transfer =
        transfer == AVCOL_TRC_ARIB_STD_B67
        ? BFPLAYER_HDR_TRANSFER_HLG
        : BFPLAYER_HDR_TRANSFER_PQ;
    config.input_full_range = Kit_FrameFullRange(decoder, frame);
    config.input_bt2020 =
        colorspace == AVCOL_SPC_BT2020_NCL ||
        colorspace == AVCOL_SPC_BT2020_CL ||
        primaries == AVCOL_PRI_BT2020 ||
        (colorspace == AVCOL_SPC_UNSPECIFIED &&
         primaries == AVCOL_PRI_UNSPECIFIED);
    config.source_peak_nits = Kit_FrameHdrPeakNits(frame);
    config.target_peak_nits = 100.0;

    if(video_decoder->hdr_lut != NULL) {
        const BfplayerHdrYuvConfig *current =
            &video_decoder->hdr_lut->config;
        if(current->transfer == config.transfer &&
           current->input_full_range == config.input_full_range &&
           current->input_bt2020 == config.input_bt2020 &&
           fabs(current->source_peak_nits - config.source_peak_nits) < 0.5)
            return 1;
    } else {
        video_decoder->hdr_lut = malloc(sizeof(*video_decoder->hdr_lut));
        if(video_decoder->hdr_lut == NULL)
            return 0;
    }

    if(bfplayer_build_hdr_yuv420_lut(
           &config,
           video_decoder->hdr_lut) != 0) {
        free(video_decoder->hdr_lut);
        video_decoder->hdr_lut = NULL;
        return 0;
    }
    if(video_decoder->hdr_workers == NULL)
        video_decoder->hdr_workers =
            bfplayer_create_hdr_worker_pool(7);
    __atomic_store_n(
        &video_decoder->hdr_transfer,
        config.transfer,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &video_decoder->hdr_input_full_range,
        config.input_full_range,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &video_decoder->hdr_input_bt2020,
        config.input_bt2020,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &video_decoder->hdr_source_peak_millinits,
        (unsigned int)llround(config.source_peak_nits * 1000.0),
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &video_decoder->hdr_target_peak_millinits,
        (unsigned int)llround(config.target_peak_nits * 1000.0),
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &video_decoder->hdr_active,
        1,
        __ATOMIC_RELEASE);
    return 1;
}

static void Kit_ApplyHdrToneMap(
    const Kit_Decoder *decoder,
    Kit_VideoDecoder *video_decoder
) {
    AVFrame *frame = video_decoder->out_frame;
    if(frame->format != AV_PIX_FMT_YUV420P ||
       !Kit_ConfigureHdrLut(
           decoder,
           video_decoder,
           video_decoder->in_frame))
        return;

    const int64_t started = av_gettime_relative();
    if(bfplayer_apply_hdr_yuv420_lut_parallel(
           video_decoder->hdr_workers,
           video_decoder->hdr_lut,
           frame->data[0],
           frame->linesize[0],
           frame->data[1],
           frame->linesize[1],
           frame->data[2],
           frame->linesize[2],
           frame->width,
           frame->height) != 0)
        return;
    const int64_t elapsed = av_gettime_relative() - started;
    __atomic_add_fetch(
        &video_decoder->hdr_frames,
        1ULL,
        __ATOMIC_RELAXED);
    if(elapsed > 0) {
        __atomic_add_fetch(
            &video_decoder->hdr_processing_us,
            (unsigned long long)elapsed,
            __ATOMIC_RELAXED);
    }
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    frame->color_primaries = AVCOL_PRI_BT709;
}

static struct SwsContext *Kit_GetSwsContext(
    struct SwsContext *old_context,
    int input_w,
    int input_h,
    enum AVPixelFormat in_fmt,
    int output_w,
    int output_h,
    enum AVPixelFormat out_fmt
) {
    struct SwsContext *new_context = sws_getCachedContext(
        old_context,
        input_w,
        input_h,
        in_fmt,
        output_w,
        output_h,
        out_fmt,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL);
    if(new_context == NULL) {
        Kit_SetError("Unable to initialize video converter context");
    }
    return new_context;
}

int Kit_GetVideoDecoderOutputFormat(const Kit_Decoder *decoder, Kit_VideoOutputFormat *output) {
    if(decoder == NULL) {
        memset(output, 0, sizeof(Kit_VideoOutputFormat));
        return 1;
    }
    const Kit_VideoDecoder *video_decoder = decoder->userdata;
    memcpy(output, &video_decoder->output, sizeof(Kit_VideoOutputFormat));
    return 0;
}

static void dec_flush_video_cb(Kit_Decoder *decoder) {
    assert(decoder);
    const Kit_VideoDecoder *video_decoder = decoder->userdata;
    Kit_FlushPacketBuffer(video_decoder->buffer);
}

static void dec_signal_video_cb(Kit_Decoder *decoder) {
    assert(decoder);
    const Kit_VideoDecoder *video_decoder = decoder->userdata;
    Kit_SignalPacketBuffer(video_decoder->buffer);
}

static void dec_read_video(const Kit_Decoder *decoder) {
    Kit_VideoDecoder *video_decoder = decoder->userdata;
    const enum AVPixelFormat in_fmt = video_decoder->in_frame->format;
    const enum AVPixelFormat out_fmt = Kit_FindAVPixelFormat(video_decoder->output.format);
    const int input_w = video_decoder->in_frame->width;
    const int input_h = video_decoder->in_frame->height;

    // Convert frame format, if needed. Note that converter context MAY need to be changed here,
    // as video frame size can, in theory, change whenever.
    struct SwsContext *old_sws = video_decoder->sws;
    video_decoder->sws = Kit_GetSwsContext(
        video_decoder->sws,
        input_w,
        input_h,
        in_fmt,
        video_decoder->output.width,
        video_decoder->output.height,
        out_fmt);
    if(video_decoder->sws == NULL) {
        return;
    }
    const int colorspace = Kit_FrameColorSpace(
        decoder,
        video_decoder->in_frame);
    const int full_range = Kit_FrameFullRange(
        decoder,
        video_decoder->in_frame);
    if(old_sws != video_decoder->sws ||
       colorspace != video_decoder->sws_colorspace ||
       full_range != video_decoder->sws_full_range) {
        const int *coefficients = sws_getCoefficients(
            colorspace == AVCOL_SPC_UNSPECIFIED
            ? AVCOL_SPC_BT709
            : colorspace);
        if(coefficients != NULL) {
            (void)sws_setColorspaceDetails(
                video_decoder->sws,
                coefficients,
                full_range,
                coefficients,
                full_range,
                0,
                1 << 16,
                1 << 16);
        }
        video_decoder->sws_colorspace = colorspace;
        video_decoder->sws_full_range = full_range;
    }
    if(av_frame_copy_props(
           video_decoder->out_frame,
           video_decoder->in_frame) < 0 ||
       sws_scale_frame(
           video_decoder->sws,
           video_decoder->out_frame,
           video_decoder->in_frame) < 0) {
        av_frame_unref(video_decoder->out_frame);
        return;
    }
    Kit_ApplyHdrToneMap(decoder, video_decoder);

    // Write video packet to packet buffer. This may block!
    // - if write succeeds, no need to av_packet_unref, since Kit_WritePacketBuffer will move the refs.
    // - If write fails, unref the packet. Fails should only happen if we are closing or seeking, so it is fine.
    if(!Kit_WritePacketBuffer(video_decoder->buffer, video_decoder->out_frame)) {
        av_frame_unref(video_decoder->out_frame);
    }
}

static Kit_DecoderInputResult dec_input_video_cb(const Kit_Decoder *decoder, const AVPacket *in_packet) {
    assert(decoder);
    switch(avcodec_send_packet(decoder->codec_ctx, in_packet)) {
        case AVERROR_EOF:
            return KIT_DEC_INPUT_EOF;
        case AVERROR(ENOMEM):
        case AVERROR(EAGAIN):
            return KIT_DEC_INPUT_RETRY;
        default: // Skip errors and hope for the best.
            return KIT_DEC_INPUT_OK;
    }
}

static bool dec_decode_video_cb(const Kit_Decoder *decoder, double *pts) {
    assert(decoder);
    Kit_VideoDecoder *video_decoder = decoder->userdata;
    if(avcodec_receive_frame(decoder->codec_ctx, video_decoder->tmp_frame) == 0) {
        // Process the temporary frame, and then make sure result is in in_frame.
        // If the frame is hardware frame, we need to pull it from the hardware device first!
        if(video_decoder->tmp_frame->format == decoder->hw_fmt) {
            if(av_hwframe_transfer_data(video_decoder->in_frame, video_decoder->tmp_frame, 0) < 0) {
                return false;
            }
            av_frame_copy_props(video_decoder->in_frame, video_decoder->tmp_frame);
            av_frame_unref(video_decoder->tmp_frame);
        } else {
            av_frame_move_ref(video_decoder->in_frame, video_decoder->tmp_frame);
        }

        // Process input frame (if HW decoding is used, it has been pulled from the GPU).
        *pts = video_decoder->in_frame->best_effort_timestamp * av_q2d(decoder->stream->time_base);
        dec_read_video(decoder);
        av_frame_unref(video_decoder->in_frame);
        return true;
    }
    return false;
}

static void dec_get_video_buffers_cb(const Kit_Decoder *ref, unsigned int *length, unsigned int *capacity) {
    assert(ref);
    assert(ref->userdata);
    Kit_VideoDecoder *video_decoder = ref->userdata;
    if(length != NULL)
        *length = Kit_GetPacketBufferLength(video_decoder->buffer);
    if(capacity != NULL)
        *capacity = Kit_GetPacketBufferCapacity(video_decoder->buffer);
}

static void dec_close_video_cb(Kit_Decoder *ref) {
    if(ref == NULL)
        return;
    assert(ref->userdata);
    Kit_VideoDecoder *video_decoder = ref->userdata;
    Kit_FreePacketBuffer(&video_decoder->buffer);
    av_frame_free(&video_decoder->in_frame);
    av_frame_free(&video_decoder->tmp_frame);
    av_frame_free(&video_decoder->current);
    av_frame_free(&video_decoder->out_frame);
    sws_freeContext(video_decoder->sws);
    bfplayer_destroy_hdr_worker_pool(video_decoder->hdr_workers);
    free(video_decoder->hdr_lut);
    free(video_decoder);
}

Kit_Decoder *Kit_CreateVideoDecoder(
    const Kit_Source *src, const Kit_VideoFormatRequest *format_request, Kit_Timer *sync_timer, const int stream_index
) {
    assert(src != NULL);

    const Kit_LibraryState *state = Kit_GetLibraryState();
    const AVFormatContext *format_ctx = src->format_ctx;
    AVStream *stream = NULL;
    Kit_VideoDecoder *video_decoder = NULL;
    Kit_Decoder *decoder = NULL;
    Kit_PacketBuffer *buffer = NULL;
    AVFrame *in_frame = NULL;
    AVFrame *out_frame = NULL;
    AVFrame *tmp_frame = NULL;
    AVFrame *current = NULL;
    struct SwsContext *sws = NULL;
    Kit_VideoOutputFormat output;
    enum AVPixelFormat output_format;

    // Find and set up stream.
    if(stream_index < 0 || stream_index >= format_ctx->nb_streams) {
        Kit_SetError("Invalid video stream index %d", stream_index);
        return NULL;
    }
    stream = format_ctx->streams[stream_index];

    if((video_decoder = calloc(1, sizeof(Kit_VideoDecoder))) == NULL) {
        Kit_SetError("Unable to allocate video decoder for stream %d", stream_index);
        goto exit_0;
    }
    if((decoder = Kit_CreateDecoder(
            stream,
            sync_timer,
            state->thread_count,
            format_request->hw_device_types,
            dec_input_video_cb,
            dec_decode_video_cb,
            dec_flush_video_cb,
            dec_signal_video_cb,
            dec_close_video_cb,
            dec_get_video_buffers_cb,
            video_decoder
        )) == NULL) {
        // No need to Kit_SetError, it will be set in Kit_CreateDecoder.
        goto exit_1;
    }
    if((in_frame = av_frame_alloc()) == NULL) {
        Kit_SetError("Unable to allocate temporary input video frame for stream %d", stream_index);
        goto exit_2;
    }
    if((out_frame = av_frame_alloc()) == NULL) {
        Kit_SetError("Unable to allocate temporary output video frame for stream %d", stream_index);
        goto exit_3;
    }
    if((tmp_frame = av_frame_alloc()) == NULL) {
        Kit_SetError("Unable to allocate temporary hardware video frame for stream %d", stream_index);
        goto exit_4;
    }
    if((current = av_frame_alloc()) == NULL) {
        Kit_SetError("Unable to allocate temporary flip video frame for stream %d", stream_index);
        goto exit_5;
    }
    if((buffer = Kit_CreatePacketBuffer(
            state->video_frame_buffer_size,
            (buf_obj_alloc)av_frame_alloc,
            (buf_obj_unref)av_frame_unref,
            (buf_obj_free)av_frame_free,
            (buf_obj_move)av_frame_move_ref,
            (buf_obj_ref)av_frame_ref
        )) == NULL) {
        Kit_SetError("Unable to create an output buffer for stream %d", stream_index);
        goto exit_6;
    }

    // Set format configs
    memset(&output, 0, sizeof(Kit_VideoOutputFormat));
    if(format_request->format != SDL_PIXELFORMAT_UNKNOWN) {
        output_format = Kit_FindAVPixelFormat(format_request->format);
        output.format = format_request->format;
    } else {
        output_format = Kit_FindBestAVPixelFormat(decoder->codec_ctx->pix_fmt);
        output.format = Kit_FindSDLPixelFormat(output_format);
    }
    if(output_format == AV_PIX_FMT_NONE) {
        Kit_SetError("Unsupported output pixel format");
        goto exit_7;
    }
    output.width = (format_request->width > -1) ? format_request->width : decoder->codec_ctx->width;
    output.height = (format_request->height > -1) ? format_request->height : decoder->codec_ctx->height;
    output.hw_device_type = Kit_FindHWDeviceType(decoder->hw_type);

    // Create scaler for handling format changes
    sws = Kit_GetSwsContext(
        sws,
        decoder->codec_ctx->width,
        decoder->codec_ctx->height,
        decoder->codec_ctx->pix_fmt,
        output.width,
        output.height,
        output_format);
    if(sws == NULL) {
        goto exit_7;
    }

    video_decoder->in_frame = in_frame;
    video_decoder->out_frame = out_frame;
    video_decoder->tmp_frame = tmp_frame;
    video_decoder->current = current;
    video_decoder->sws = sws;
    video_decoder->buffer = buffer;
    video_decoder->output = output;
    video_decoder->sws_colorspace = AVCOL_SPC_UNSPECIFIED;
    video_decoder->sws_full_range = -1;
    return decoder;

exit_7:
    Kit_FreePacketBuffer(&buffer);
exit_6:
    av_frame_free(&current);
exit_5:
    av_frame_free(&tmp_frame);
exit_4:
    av_frame_free(&out_frame);
exit_3:
    av_frame_free(&in_frame);
exit_2:
    Kit_CloseDecoder(&decoder);
    return NULL; // Above frees the video_decoder also.
exit_1:
    free(video_decoder);
exit_0:
    return NULL;
}

void Kit_GetVideoDecoderToneMapInfo(
    const Kit_Decoder *decoder,
    Kit_VideoToneMapInfo *info
) {
    memset(info, 0, sizeof(*info));
    if(decoder == NULL || decoder->userdata == NULL)
        return;
    const Kit_VideoDecoder *video_decoder = decoder->userdata;
    info->active = __atomic_load_n(
        &video_decoder->hdr_active,
        __ATOMIC_ACQUIRE);
    if(info->active) {
        info->transfer = __atomic_load_n(
            &video_decoder->hdr_transfer,
            __ATOMIC_RELAXED);
        info->input_full_range = __atomic_load_n(
            &video_decoder->hdr_input_full_range,
            __ATOMIC_RELAXED);
        info->input_bt2020 = __atomic_load_n(
            &video_decoder->hdr_input_bt2020,
            __ATOMIC_RELAXED);
        info->source_peak_nits =
            __atomic_load_n(
                &video_decoder->hdr_source_peak_millinits,
                __ATOMIC_RELAXED) /
            1000.0;
        info->target_peak_nits =
            __atomic_load_n(
                &video_decoder->hdr_target_peak_millinits,
                __ATOMIC_RELAXED) /
            1000.0;
    }
    info->frames = __atomic_load_n(
        &video_decoder->hdr_frames,
        __ATOMIC_RELAXED);
    info->processing_us = __atomic_load_n(
        &video_decoder->hdr_processing_us,
        __ATOMIC_RELAXED);
    info->workers = bfplayer_hdr_worker_count(
        video_decoder->hdr_workers);
}

static double Kit_GetCurrentPTS(const Kit_Decoder *decoder) {
    Kit_VideoDecoder *video_decoder = decoder->userdata;
    return video_decoder->current->best_effort_timestamp * av_q2d(decoder->stream->time_base);
}

bool Kit_BeginReadFrame(const Kit_Decoder *decoder) {
    assert(decoder != NULL);
    const Kit_VideoDecoder *video_decoder = decoder->userdata;

    if(!Kit_BeginPacketBufferRead(video_decoder->buffer, video_decoder->current, 0))
        return false;

    // Initialize timer if it's the primary sync source, and it's not yet initialized.
    Kit_InitTimerBase(decoder->sync_timer);
    if(!Kit_IsTimerInitialized(decoder->sync_timer)) {
        // If this was not the sync source and timer is not set, wait for another stream to set it.
        av_frame_unref(video_decoder->current);
        Kit_CancelPacketBufferRead(video_decoder->buffer);
        return false;
    }

    double pts = Kit_GetCurrentPTS(decoder);
    double sync_ts = Kit_GetTimerElapsed(decoder->sync_timer);
    const double early_threshold = Kit_GetLibraryState()->video_early_threshold / 1000.0;
    const double late_threshold = Kit_GetLibraryState()->video_late_threshold / 1000.0;

    // If packet is far too early, the stream jumped or was seeked.
    if(Kit_IsTimerPrimary(decoder->sync_timer)) {
        // If this stream is the sync source, then reset this as the new sync timestamp.
        if(pts > sync_ts + KIT_VIDEO_EARLY_FAIL) {
            // LOG("[VIDEO] NO SYNC pts = %lf > %lf + %lf\n", pts, sync_ts, KIT_VIDEO_EARLY_FAIL);
            Kit_AddTimerBase(decoder->sync_timer, -(pts - sync_ts));
            sync_ts = Kit_GetTimerElapsed(decoder->sync_timer);
        }
    } else {
        while(pts > sync_ts + KIT_VIDEO_EARLY_FAIL) {
            // LOG("[VIDEO] FAIL-EARLY pts = %lf > %lf + %lf\n", pts, sync_ts, KIT_VIDEO_EARLY_FAIL);
            av_frame_unref(video_decoder->current);
            Kit_FinishPacketBufferRead(video_decoder->buffer);
            if(!Kit_BeginPacketBufferRead(video_decoder->buffer, video_decoder->current, 0))
                return false;
            pts = Kit_GetCurrentPTS(decoder);
        }
    }

    // Packet is too early, wait.
    if(pts > sync_ts + early_threshold) {
        // LOG("[VIDEO] EARLY pts = %lf > %lf + %lf\n", pts, sync_ts, early_threshold);
        av_frame_unref(video_decoder->current);
        Kit_CancelPacketBufferRead(video_decoder->buffer);
        return false;
    }

    // Packet is too late, skip packets until we see something reasonable.
    while(pts < sync_ts - late_threshold) {
        // LOG("[VIDEO] LATE: pts = %lf < %lf + %lf\n", pts, sync_ts, late_threshold);
        av_frame_unref(video_decoder->current);
        Kit_FinishPacketBufferRead(video_decoder->buffer);
        if(!Kit_BeginPacketBufferRead(video_decoder->buffer, video_decoder->current, 0))
            return false;
        pts = Kit_GetCurrentPTS(decoder);
    }

    // LOG("[VIDEO] >>> SYNC!: pts = %lf, sync = %lf\n", pts, sync_ts);
    return true;
}

void Kit_EndReadFrame(Kit_Decoder *decoder) {
    const Kit_VideoDecoder *video_decoder = decoder->userdata;
    av_frame_unref(video_decoder->current);
    Kit_FinishPacketBufferRead(video_decoder->buffer);
}

int Kit_GetVideoDecoderSDLTexture(Kit_Decoder *decoder, SDL_Texture *texture, SDL_Rect *area) {
    assert(decoder != NULL);
    assert(texture != NULL);
    const Kit_VideoDecoder *video_decoder = decoder->userdata;

    // Try to read and sync frame. If this fails, then there is nothing else to do other than wait.
    if(!Kit_BeginReadFrame(decoder)) {
        return 1;
    }

    // Update output texture with current video data.
    // Note that frame size may change on the fly. Take that into account.
    SDL_Rect frame_area;
    frame_area.w = video_decoder->current->width;
    frame_area.h = video_decoder->current->height;
    frame_area.x = 0;
    frame_area.y = 0;
    switch(video_decoder->output.format) {
        case SDL_PIXELFORMAT_YV12:
        case SDL_PIXELFORMAT_IYUV:
            SDL_UpdateYUVTexture(
                texture,
                &frame_area,
                video_decoder->current->data[0],
                video_decoder->current->linesize[0],
                video_decoder->current->data[1],
                video_decoder->current->linesize[1],
                video_decoder->current->data[2],
                video_decoder->current->linesize[2]
            );
            break;
        default:
            SDL_UpdateTexture(
                texture, &frame_area, video_decoder->current->data[0], video_decoder->current->linesize[0]
            );
            break;
    }
    if(area != NULL)
        *area = frame_area;

    decoder->aspect_ratio = video_decoder->current->sample_aspect_ratio;

    Kit_EndReadFrame(decoder);
    return 0;
}

int Kit_LockVideoDecoderRaw(Kit_Decoder *decoder, unsigned char ***data, int **line_size, SDL_Rect *area) {
    assert(decoder != NULL);
    const Kit_VideoDecoder *video_decoder = decoder->userdata;

    // Try to read and sync frame. If this fails, then there is nothing else to do other than wait.
    if(!Kit_BeginReadFrame(decoder)) {
        return 1;
    }

    // Copy pointers.
    if(line_size != NULL) {
        *line_size = video_decoder->current->linesize;
    }
    if(data != NULL) {
        *data = video_decoder->current->data;
    }
    if(area != NULL) {
        area->w = video_decoder->current->width;
        area->h = video_decoder->current->height;
        area->x = 0;
        area->y = 0;
    }
    decoder->aspect_ratio = video_decoder->current->sample_aspect_ratio;
    return 0;
}

void Kit_UnlockVideoDecoderRaw(Kit_Decoder *decoder) {
    assert(decoder != NULL);
    Kit_EndReadFrame(decoder);
}
