#include "video/sofdec_player.hpp"

#if FZGX_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

#include <algorithm>
#include <cstring>

namespace fzgx {

#if FZGX_HAS_FFMPEG

namespace {

struct MemIoBuffer {
    const uint8_t* data{nullptr};
    size_t size{0};
    size_t pos{0};
};

int avio_mem_read(void* opaque, uint8_t* buf, int buf_size) {
    auto* mem = static_cast<MemIoBuffer*>(opaque);
    if (!mem || mem->pos >= mem->size) {
        return AVERROR_EOF;
    }
    const size_t avail = mem->size - mem->pos;
    const size_t take = std::min(avail, static_cast<size_t>(buf_size));
    std::memcpy(buf, mem->data + mem->pos, take);
    mem->pos += take;
    return static_cast<int>(take);
}

int64_t avio_mem_seek(void* opaque, int64_t offset, int whence) {
    auto* mem = static_cast<MemIoBuffer*>(opaque);
    if (!mem) {
        return AVERROR(EINVAL);
    }
    if (whence == AVSEEK_SIZE) {
        return static_cast<int64_t>(mem->size);
    }
    int64_t new_pos = 0;
    if (whence == SEEK_SET) {
        new_pos = offset;
    } else if (whence == SEEK_CUR) {
        new_pos = static_cast<int64_t>(mem->pos) + offset;
    } else if (whence == SEEK_END) {
        new_pos = static_cast<int64_t>(mem->size) + offset;
    } else {
        return AVERROR(EINVAL);
    }
    if (new_pos < 0 || static_cast<size_t>(new_pos) > mem->size) {
        return AVERROR(EINVAL);
    }
    mem->pos = static_cast<size_t>(new_pos);
    return new_pos;
}

} // namespace

struct SofdecPlayer::Impl {
    std::vector<uint8_t> raw_bytes;
    MemIoBuffer io_mem{};
    uint8_t* avio_buf{nullptr};
    AVIOContext* avio_ctx{nullptr};
    AVFormatContext* fmt_ctx{nullptr};

    int video_idx{-1};
    AVCodecContext* video_dec{nullptr};
    SwsContext* sws{nullptr};

    int audio_idx{-1};
    AVCodecContext* audio_dec{nullptr};
    SwrContext* swr{nullptr};

    AVPacket* pkt{nullptr};
    AVFrame* frame{nullptr};
    AVFrame* next_video_frame{nullptr};
    bool has_pending_video{false};
    double pending_video_pts{0.0};
    double last_video_pts{0.0};
    bool eof_reached{false};
};

bool SofdecPlayer::is_ffmpeg_supported() {
    return true;
}

SofdecPlayer::~SofdecPlayer() {
    close();
}

bool SofdecPlayer::open_from_memory(std::vector<uint8_t> sfd_bytes,
                                    const std::string& label,
                                    std::string* error_out) {
    close();
    if (sfd_bytes.empty()) {
        if (error_out) *error_out = "Empty .sfd buffer.";
        return false;
    }

    impl_ = new Impl();
    impl_->raw_bytes = std::move(sfd_bytes);
    impl_->io_mem.data = impl_->raw_bytes.data();
    impl_->io_mem.size = impl_->raw_bytes.size();
    impl_->io_mem.pos = 0;

    constexpr int kAvioBufSize = 64 * 1024;
    impl_->avio_buf = static_cast<uint8_t*>(av_malloc(kAvioBufSize));
    if (!impl_->avio_buf) {
        close();
        if (error_out) *error_out = "av_malloc failed.";
        return false;
    }
    impl_->avio_ctx = avio_alloc_context(
        impl_->avio_buf, kAvioBufSize, 0, &impl_->io_mem, avio_mem_read, nullptr, avio_mem_seek);
    if (!impl_->avio_ctx) {
        av_free(impl_->avio_buf);
        impl_->avio_buf = nullptr;
        close();
        if (error_out) *error_out = "avio_alloc_context failed.";
        return false;
    }

    impl_->fmt_ctx = avformat_alloc_context();
    impl_->fmt_ctx->pb = impl_->avio_ctx;
    impl_->fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&impl_->fmt_ctx, label.c_str(), nullptr, nullptr) < 0) {
        close();
        if (error_out) *error_out = "avformat_open_input failed for Sofdec stream.";
        return false;
    }
    if (avformat_find_stream_info(impl_->fmt_ctx, nullptr) < 0) {
        close();
        if (error_out) *error_out = "avformat_find_stream_info failed.";
        return false;
    }

    const AVCodec* vcodec = nullptr;
    impl_->video_idx = av_find_best_stream(impl_->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &vcodec, 0);
    if (impl_->video_idx < 0 || !vcodec) {
        close();
        if (error_out) *error_out = "No playable video stream found in .sfd.";
        return false;
    }

    impl_->video_dec = avcodec_alloc_context3(vcodec);
    avcodec_parameters_to_context(impl_->video_dec, impl_->fmt_ctx->streams[impl_->video_idx]->codecpar);
    if (avcodec_open2(impl_->video_dec, vcodec, nullptr) < 0) {
        close();
        if (error_out) *error_out = "Failed opening video decoder for .sfd.";
        return false;
    }

    width_ = static_cast<uint32_t>(impl_->video_dec->width);
    height_ = static_cast<uint32_t>(impl_->video_dec->height);
    if (width_ == 0 || height_ == 0) {
        close();
        if (error_out) *error_out = "Invalid video dimensions in .sfd.";
        return false;
    }

    impl_->sws = sws_getContext(
        static_cast<int>(width_), static_cast<int>(height_), impl_->video_dec->pix_fmt,
        static_cast<int>(width_), static_cast<int>(height_), AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl_->sws) {
        close();
        if (error_out) *error_out = "sws_getContext failed.";
        return false;
    }

    // Optional audio stream (e.g. ADX/SFA inside .sfd)
    const AVCodec* acodec = nullptr;
    impl_->audio_idx = av_find_best_stream(impl_->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &acodec, 0);
    if (impl_->audio_idx >= 0 && acodec) {
        impl_->audio_dec = avcodec_alloc_context3(acodec);
        avcodec_parameters_to_context(impl_->audio_dec, impl_->fmt_ctx->streams[impl_->audio_idx]->codecpar);
        if (avcodec_open2(impl_->audio_dec, acodec, nullptr) >= 0) {
            AVChannelLayout out_layout{};
            av_channel_layout_default(&out_layout, 2);
            if (swr_alloc_set_opts2(
                    &impl_->swr,
                    &out_layout,
                    AV_SAMPLE_FMT_FLT,
                    static_cast<int>(AudioEngine::kOutputSampleRate),
                    &impl_->audio_dec->ch_layout,
                    impl_->audio_dec->sample_fmt,
                    impl_->audio_dec->sample_rate,
                    0,
                    nullptr) == 0) {
                swr_init(impl_->swr);
            }
            av_channel_layout_uninit(&out_layout);
        } else {
            avcodec_free_context(&impl_->audio_dec);
            impl_->audio_idx = -1;
        }
    }

    impl_->pkt = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    impl_->next_video_frame = av_frame_alloc();
    rgba_frame_.resize(static_cast<size_t>(width_) * height_ * 4, 0);

    if (impl_->fmt_ctx->duration != AV_NOPTS_VALUE) {
        duration_sec_ = static_cast<double>(impl_->fmt_ctx->duration) / static_cast<double>(AV_TIME_BASE);
    } else {
        duration_sec_ = 0.0;
    }

    clock_sec_ = 0.0;
    frame_serial_ = 0;
    label_ = label;
    paused_ = false;
    playing_ = true;
    status_message_ = "Playing " + label + " (" + std::to_string(width_) + "x" + std::to_string(height_) + ")";
    return true;
}

void SofdecPlayer::close() {
    playing_ = false;
    paused_ = false;
    width_ = 0;
    height_ = 0;
    clock_sec_ = 0.0;
    duration_sec_ = 0.0;
    rgba_frame_.clear();
    label_.clear();
    if (!impl_) {
        return;
    }
    if (impl_->next_video_frame) av_frame_free(&impl_->next_video_frame);
    if (impl_->frame) av_frame_free(&impl_->frame);
    if (impl_->pkt) av_packet_free(&impl_->pkt);
    if (impl_->swr) swr_free(&impl_->swr);
    if (impl_->sws) sws_freeContext(impl_->sws);
    if (impl_->audio_dec) avcodec_free_context(&impl_->audio_dec);
    if (impl_->video_dec) avcodec_free_context(&impl_->video_dec);
    if (impl_->fmt_ctx) avformat_close_input(&impl_->fmt_ctx);
    if (impl_->avio_ctx) {
        av_freep(&impl_->avio_ctx->buffer);
        avio_context_free(&impl_->avio_ctx);
    }
    delete impl_;
    impl_ = nullptr;
    status_message_ = "Idle";
}

bool SofdecPlayer::update(double dt_seconds, AudioEngine& audio) {
    if (!impl_ || !playing_ || paused_) {
        return false;
    }

    clock_sec_ += std::clamp(dt_seconds, 0.0, 0.1);
    bool decoded_new_video = false;

    auto decode_audio_frames = [&]() {
        if (!impl_->audio_dec || !impl_->swr) {
            return;
        }
        while (avcodec_receive_frame(impl_->audio_dec, impl_->frame) == 0) {
            const int out_samples = swr_get_out_samples(impl_->swr, impl_->frame->nb_samples);
            if (out_samples > 0) {
                std::vector<float> pcm(static_cast<size_t>(out_samples) * 2);
                uint8_t* out_ptr = reinterpret_cast<uint8_t*>(pcm.data());
                const int converted = swr_convert(
                    impl_->swr, &out_ptr, out_samples,
                    const_cast<const uint8_t**>(impl_->frame->extended_data), impl_->frame->nb_samples);
                if (converted > 0) {
                    audio.push_fmv_pcm(pcm.data(), static_cast<size_t>(converted));
                }
            }
            av_frame_unref(impl_->frame);
        }
    };

    auto commit_video_frame = [&](AVFrame* f) {
        uint8_t* dst_planes[4] = {rgba_frame_.data(), nullptr, nullptr, nullptr};
        int dst_lines[4] = {static_cast<int>(width_ * 4), 0, 0, 0};
        sws_scale(impl_->sws, f->data, f->linesize, 0, static_cast<int>(height_), dst_planes, dst_lines);
        ++frame_serial_;
        decoded_new_video = true;
    };

    if (impl_->has_pending_video) {
        if (impl_->pending_video_pts <= clock_sec_) {
            commit_video_frame(impl_->next_video_frame);
            av_frame_unref(impl_->next_video_frame);
            impl_->has_pending_video = false;
        } else {
            return false;
        }
    }

    const AVRational tb = impl_->fmt_ctx->streams[impl_->video_idx]->time_base;
    while (!impl_->has_pending_video && !impl_->eof_reached) {
        if (avcodec_receive_frame(impl_->video_dec, impl_->next_video_frame) == 0) {
            double pts = impl_->last_video_pts + (1.0 / 29.97);
            if (impl_->next_video_frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                pts = static_cast<double>(impl_->next_video_frame->best_effort_timestamp) * av_q2d(tb);
            }
            impl_->last_video_pts = pts;
            if (pts <= clock_sec_ + 0.005) {
                commit_video_frame(impl_->next_video_frame);
                av_frame_unref(impl_->next_video_frame);
            } else {
                impl_->pending_video_pts = pts;
                impl_->has_pending_video = true;
            }
            continue;
        }

        const int rd = av_read_frame(impl_->fmt_ctx, impl_->pkt);
        if (rd < 0) {
            impl_->eof_reached = true;
            avcodec_send_packet(impl_->video_dec, nullptr);
            if (impl_->audio_dec) {
                avcodec_send_packet(impl_->audio_dec, nullptr);
                decode_audio_frames();
            }
            break;
        }

        if (impl_->pkt->stream_index == impl_->video_idx) {
            avcodec_send_packet(impl_->video_dec, impl_->pkt);
        } else if (impl_->pkt->stream_index == impl_->audio_idx && impl_->audio_dec) {
            avcodec_send_packet(impl_->audio_dec, impl_->pkt);
            decode_audio_frames();
        }
        av_packet_unref(impl_->pkt);
    }

    if (impl_->eof_reached && !impl_->has_pending_video) {
        playing_ = false;
        status_message_ = "Finished " + label_;
    }

    return decoded_new_video;
}

#else

struct SofdecPlayer::Impl {};

bool SofdecPlayer::is_ffmpeg_supported() {
    return false;
}

SofdecPlayer::~SofdecPlayer() = default;

bool SofdecPlayer::open_from_memory(std::vector<uint8_t>, const std::string&, std::string* error_out) {
    status_message_ = "FFmpeg libraries were not found at build time; Sofdec .sfd playback is disabled.";
    if (error_out) *error_out = status_message_;
    return false;
}

void SofdecPlayer::close() {
    playing_ = false;
}

bool SofdecPlayer::update(double, AudioEngine&) {
    return false;
}

#endif

} // namespace fzgx
