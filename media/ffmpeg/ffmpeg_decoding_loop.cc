// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_decoding_loop.h"
#include "media/base/media_log.h"
#include "base/callback.h"
#include "base/logging.h"
#include "media/ffmpeg/ffmpeg_common.h"

namespace media {

FFmpegDecodingLoop::FFmpegDecodingLoop(AVCodecContext* context,
                                       MediaLog* media_log,
                                       bool continue_on_decoding_errors)
    : media_log_(media_log),
      continue_on_decoding_errors_(continue_on_decoding_errors),
      context_(context),
      frame_(av_frame_alloc()),
      filter_frame_(av_frame_alloc()) {}

FFmpegDecodingLoop::~FFmpegDecodingLoop() = default;

void FFmpegDecodingLoop::InitFilterGraph(AVFrame *frame) {
    if (filter_initialised) return;

    int result;

    const AVFilter *buffer_src   = avfilter_get_by_name("buffer");
    const AVFilter *buffer_sink  = avfilter_get_by_name("buffersink");
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVFilterInOut *outputs = avfilter_inout_alloc();

    AVCodecContext *ctx = context_;
    char args[512];

    int frame_fix = 0; // fix bad width on some streams
    if (frame->width < 704) frame_fix = 2;
    else if (frame->width > 704) frame_fix = -16;

    snprintf(args, sizeof(args),
         "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
         frame->width + frame_fix,
         frame->height,
         frame->format,// ctx->pix_fmt,
         ctx->time_base.num,
         ctx->time_base.den,
         ctx->sample_aspect_ratio.num,
         ctx->sample_aspect_ratio.den);

    const char *description = "yadif=1:-1:0";

    filter_graph = avfilter_graph_alloc();
    result = avfilter_graph_create_filter(&buffersrc_ctx_, buffer_src, "in", args, NULL, filter_graph);
    if (result < 0) {
        if (media_log_) MEDIA_LOG(ERROR, media_log_) << "Filter graph - Unable to create buffer source";
        return;
    }

    AVBufferSinkParams *params = av_buffersink_params_alloc();
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE };

    params->pixel_fmts = pix_fmts;
    result = avfilter_graph_create_filter(&buffersink_ctx_, buffer_sink, "out", NULL, params, filter_graph);
    av_free(params);
    if (result < 0) {
        if (media_log_) MEDIA_LOG(ERROR, media_log_) << "Filter graph - Unable to create buffer sink";
        return;
    }

    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = buffersink_ctx_;
    inputs->pad_idx     = 0;
    inputs->next        = NULL;

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx_;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    result = avfilter_graph_parse_ptr(filter_graph, description, &inputs, &outputs, NULL);
    if (result < 0 && media_log_) MEDIA_LOG(ERROR, media_log_) << "Filter graph - avfilter_graph_parse_ptr ERROR";

    result = avfilter_graph_config(filter_graph, NULL);
    if (result < 0 && media_log_) MEDIA_LOG(ERROR, media_log_) << "Filter graph - avfilter_graph_config error";

    filter_initialised = true;
}

FFmpegDecodingLoop::DecodeStatus FFmpegDecodingLoop::DecodePacket(
    const AVPacket* packet,
    FrameReadyCB frame_ready_cb) {
  bool sent_packet = false, frames_remaining = true, decoder_error = false;
  while (!sent_packet || frames_remaining) {
    if (!sent_packet) {
      const int result = avcodec_send_packet(context_, packet);
      if (result < 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        DLOG(ERROR) << "Failed to send packet for decoding: " << result;
        return DecodeStatus::kSendPacketFailed;
      }

      sent_packet = result != AVERROR(EAGAIN);
    }

    // See if any frames are available. If we receive an EOF or EAGAIN, there
    // should be nothing left to do this pass since we've already provided the
    // only input packet that we have.
    const int result = avcodec_receive_frame(context_, frame_.get());
    if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) {
      frames_remaining = false;

      // TODO(dalecurtis): This should be a DCHECK() or MEDIA_LOG, but since
      // this API is new, lets make it a CHECK first and monitor reports.
      if (result == AVERROR(EAGAIN)) {
        CHECK(sent_packet) << "avcodec_receive_frame() and "
                              "avcodec_send_packet() both returned EAGAIN, "
                              "which is an API violation.";
      }

      continue;
    } else if (result < 0) {
      DLOG(ERROR) << "Failed to decode frame: " << result;
      last_averror_code_ = result;
      if (!continue_on_decoding_errors_)
        return DecodeStatus::kDecodeFrameFailed;
      decoder_error = true;
      continue;
    }

    bool frame_processing_success = false;
    if (!frame_.get()->interlaced_frame) {     // not interlaced
      frame_processing_success = frame_ready_cb.Run(frame_.get());
    } else {
        if (media_log_) MEDIA_LOG(DEBUG, media_log_) << "Detected interlaced video frame";

      if (!filter_initialised) {
          if (media_log_) MEDIA_LOG(DEBUG, media_log_) << "Init media filter";
          this->InitFilterGraph(frame_.get());
          if (media_log_) MEDIA_LOG(DEBUG, media_log_) << "Media filter ok";
      }

      if (av_buffersrc_add_frame(buffersrc_ctx_, frame_.get()) < 0) {
		if (!continue_on_decoding_errors_)
		  return DecodeStatus::kDecodeFrameFailed;
		decoder_error = true;
		continue;
	  }

      while (true) {
          const int ret = av_buffersink_get_frame(buffersink_ctx_, filter_frame_.get());
          if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
              break;
          if (ret < 0) {
              if (!continue_on_decoding_errors_)
                return DecodeStatus::kDecodeFrameFailed;
              decoder_error = true;
              break;
          }
          frame_processing_success = frame_ready_cb.Run(frame_.get());
          av_frame_unref(filter_frame_.get());
      }
    }
    
    av_frame_unref(frame_.get());
    if (!frame_processing_success)
      return DecodeStatus::kFrameProcessingFailed;
  }

  return decoder_error ? DecodeStatus::kDecodeFrameFailed : DecodeStatus::kOkay;
}

}  // namespace media
