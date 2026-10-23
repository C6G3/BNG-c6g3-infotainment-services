#include "resources/VideoTranscoder.hpp"
#include "core/Logger.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <chrono>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ifs {

namespace {

std::string toUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::wstring& ws = path.native();
    if (ws.empty()) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return "";
    std::string result(sizeNeeded, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), &result[0], sizeNeeded, nullptr, nullptr);
    return result;
#else
    auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
#endif
}

std::string avError(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

constexpr int64_t kAudioBitRate    = 128000;
constexpr int     kAudioSampleRate = 48000;
constexpr double  kMaxLeadSilence  = 5.0;  // sound that starts after the picture is padded with silence (seconds)

struct Job {
    std::string srcUtf8, audioTmpUtf8;
    const std::atomic<bool>* cancel = nullptr; // set: stop at the next packet

    AVFormatContext* in   = nullptr;
    AVFormatContext* aOut = nullptr;
    AVStream* aIn = nullptr;  // input stream
    AVStream* aSt = nullptr;  // output stream
    AVCodecContext* aDec = nullptr;
    AVCodecContext* aEnc = nullptr; // null: audio is copied (or there is none)
    SwrContext*  swr  = nullptr;
    AVAudioFifo* fifo = nullptr;
    AVPacket* pkt    = nullptr;
    AVPacket* outPkt = nullptr;
    AVFrame* frame  = nullptr;
    AVFrame* aFrame = nullptr;

    int64_t  startUs        = 0;    // input start time (AV_TIME_BASE)
    double   duration       = 0.0;  // seconds
    int64_t  audioSamples   = 0;
    bool     audioStarted   = false;
    uint64_t skippedPackets = 0;    // undecodable packets (corrupt data) skipped, like players do
    std::string error;

    ~Job() {
        if (aOut) {
            if (aOut->pb) avio_closep(&aOut->pb);
            avformat_free_context(aOut);
        }
        avformat_close_input(&in);
        avcodec_free_context(&aDec);
        avcodec_free_context(&aEnc);
        swr_free(&swr);
        if (fifo) av_audio_fifo_free(fifo);
        av_packet_free(&pkt);
        av_packet_free(&outPkt);
        av_frame_free(&frame);
        av_frame_free(&aFrame);
    }

    bool fail(const std::string& msg, int code = 0) {
        error = code ? msg + ": " + avError(code) : msg;
        return false;
    }

    int64_t startIn(const AVStream* st) const {
        return av_rescale_q(startUs, AV_TIME_BASE_Q, st->time_base);
    }

    // ─── Setup ───────────────────────────────────────────────────────────────

    bool openInput() {
        int ret = avformat_open_input(&in, srcUtf8.c_str(), nullptr, nullptr);
        if (ret < 0) return fail("Could not open the video", ret);
        if ((ret = avformat_find_stream_info(in, nullptr)) < 0) return fail("Could not read the video streams", ret);

        int vi = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        int ai = av_find_best_stream(in, AVMEDIA_TYPE_AUDIO, -1, vi, nullptr, 0);
        if (ai >= 0) aIn = in->streams[ai];

        startUs  = in->start_time != AV_NOPTS_VALUE ? in->start_time : 0;
        duration = in->duration != AV_NOPTS_VALUE ? static_cast<double>(in->duration) / AV_TIME_BASE : 0.0;
        return true;
    }

    bool setupAudio() {
        if (!aIn) return true; // silent video: no audio file
        int ret = avformat_alloc_output_context2(&aOut, nullptr, "ogg", audioTmpUtf8.c_str());
        if (ret < 0 || !aOut) return fail("Could not create the ogg output", ret);
        aSt = avformat_new_stream(aOut, nullptr);
        if (!aSt) return fail("Could not add the audio stream");

        const AVCodecParameters* par = aIn->codecpar;
        if (par->codec_id == AV_CODEC_ID_OPUS || par->codec_id == AV_CODEC_ID_VORBIS) {
            ret = avcodec_parameters_copy(aSt->codecpar, par);
            if (ret < 0) return fail("Could not copy the audio stream", ret);
            aSt->codecpar->codec_tag = 0;
            aSt->time_base = aIn->time_base;
            return true;
        }

        const AVCodec* decoder = avcodec_find_decoder(par->codec_id);
        if (!decoder) return fail(std::string("No audio decoder for ") + avcodec_get_name(par->codec_id));
        aDec = avcodec_alloc_context3(decoder);
        if (!aDec) return fail("Out of memory");
        if ((ret = avcodec_parameters_to_context(aDec, par)) < 0) return fail("Could not set up the audio decoder", ret);
        aDec->pkt_timebase = aIn->time_base;
        aDec->thread_count = 0; // auto
        if ((ret = avcodec_open2(aDec, decoder, nullptr)) < 0) return fail("Could not open the audio decoder", ret);
        if (aDec->sample_rate <= 0) return fail("Unknown audio sample rate");

        const AVCodec* encoder = avcodec_find_encoder_by_name("libopus");
        if (!encoder) return fail("This build has no Opus encoder (libopus)");
        aEnc = avcodec_alloc_context3(encoder);
        if (!aEnc) return fail("Out of memory");
        aEnc->sample_fmt  = AV_SAMPLE_FMT_FLT;
        aEnc->sample_rate = kAudioSampleRate;
        av_channel_layout_default(&aEnc->ch_layout, 2);
        aEnc->bit_rate    = kAudioBitRate;
        aEnc->time_base   = AVRational{1, kAudioSampleRate};
        if (aOut->oformat->flags & AVFMT_GLOBALHEADER) aEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ret = avcodec_open2(aEnc, encoder, nullptr);
        if (ret < 0) return fail("Could not open the Opus encoder", ret);
        if ((ret = avcodec_parameters_from_context(aSt->codecpar, aEnc)) < 0) return fail("Could not set up the audio stream", ret);
        aSt->time_base = aEnc->time_base;

        AVChannelLayout inLayout{};
        if (aDec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || !av_channel_layout_check(&aDec->ch_layout)) {
            av_channel_layout_default(&inLayout, aDec->ch_layout.nb_channels > 0 ? aDec->ch_layout.nb_channels : 2);
        } else {
            av_channel_layout_copy(&inLayout, &aDec->ch_layout);
        }
        ret = swr_alloc_set_opts2(&swr, &aEnc->ch_layout, aEnc->sample_fmt, aEnc->sample_rate,
                                  &inLayout, aDec->sample_fmt, aDec->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&inLayout);
        if (ret < 0) return fail("Could not set up audio resampling", ret);
        if ((ret = swr_init(swr)) < 0) return fail("Could not set up audio resampling", ret);

        const int frameSize = aEnc->frame_size > 0 ? aEnc->frame_size : 960;
        fifo = av_audio_fifo_alloc(aEnc->sample_fmt, aEnc->ch_layout.nb_channels, frameSize);
        aFrame = av_frame_alloc();
        if (!fifo || !aFrame) return fail("Out of memory");
        aFrame->nb_samples  = frameSize;
        aFrame->format      = aEnc->sample_fmt;
        aFrame->sample_rate = aEnc->sample_rate;
        if ((ret = av_channel_layout_copy(&aFrame->ch_layout, &aEnc->ch_layout)) < 0) return fail("Out of memory", ret);
        if ((ret = av_frame_get_buffer(aFrame, 0)) < 0) return fail("Out of memory", ret);
        return true;
    }

    // ─── Packets ─────────────────────────────────────────────────────────────

    bool writePacket(AVPacket* p, AVRational from, int64_t offset) {
        if (p->pts != AV_NOPTS_VALUE) p->pts -= offset;
        if (p->dts != AV_NOPTS_VALUE) p->dts -= offset;
        av_packet_rescale_ts(p, from, aSt->time_base);
        p->stream_index = aSt->index;
        p->pos = -1;
        int ret = av_interleaved_write_frame(aOut, p);
        if (ret < 0) return fail("Could not write the converted file", ret);
        return true;
    }

    bool drainEncoder() {
        int ret;
        while ((ret = avcodec_receive_packet(aEnc, outPkt)) >= 0) {
            if (!writePacket(outPkt, aEnc->time_base, 0)) return false;
        }
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
        return fail("Encoding failed", ret);
    }

    bool onAudioPacket() {
        if (!aEnc) return writePacket(pkt, aIn->time_base, startIn(aIn));

        int ret = avcodec_send_packet(aDec, pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            ++skippedPackets;
            return true;
        }
        return drainAudioDecoder();
    }

    bool drainAudioDecoder() {
        int ret;
        while ((ret = avcodec_receive_frame(aDec, frame)) >= 0) {
            bool ok = queueAudio(frame);
            av_frame_unref(frame);
            if (!ok) return false;
        }
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
        return fail("Audio decoding failed", ret);
    }

    bool writeSilence(int samples) {
        std::vector<float> zeros(static_cast<size_t>(samples) * aEnc->ch_layout.nb_channels, 0.0f);
        void* data[1] = { zeros.data() };
        if (av_audio_fifo_write(fifo, data, samples) < samples) return fail("Out of memory");
        return true;
    }

    // The audio file starts at the video's start: a late sound start is padded with silence
    bool queueAudio(AVFrame* f) {
        if (!audioStarted) {
            audioStarted = true;
            int64_t ts = f->best_effort_timestamp;
            if (ts != AV_NOPTS_VALUE) {
                double lead = (ts - startIn(aIn)) * av_q2d(aIn->time_base);
                if (lead > 0.0 && lead <= kMaxLeadSilence && !writeSilence(static_cast<int>(lead * kAudioSampleRate))) {
                    return false;
                }
            }
        }
        return resample(f);
    }

    // f == nullptr drains the resampler's delay
    bool resample(AVFrame* f) {
        const int inSamples = f ? f->nb_samples : 0;
        const int maxOut = swr_get_out_samples(swr, inSamples);
        if (maxOut < 0) return fail("Audio resampling failed", maxOut);
        if (maxOut == 0) return true;
        std::vector<float> buf(static_cast<size_t>(maxOut) * aEnc->ch_layout.nb_channels);
        uint8_t* out[1] = { reinterpret_cast<uint8_t*>(buf.data()) };
        int got = swr_convert(swr, out, maxOut, f ? f->extended_data : nullptr, inSamples);
        if (got < 0) return fail("Audio resampling failed", got);
        void* data[1] = { buf.data() };
        if (got > 0 && av_audio_fifo_write(fifo, data, got) < got) return fail("Out of memory");
        return encodeAudio(false);
    }

    // flush: also encodes the last partial frame (padded with silence)
    bool encodeAudio(bool flush) {
        const int fs = aFrame->nb_samples;
        while (av_audio_fifo_size(fifo) >= fs || (flush && av_audio_fifo_size(fifo) > 0)) {
            int ret = av_frame_make_writable(aFrame);
            if (ret < 0) return fail("Out of memory", ret);
            const int n = std::min(fs, av_audio_fifo_size(fifo));
            if (av_audio_fifo_read(fifo, reinterpret_cast<void**>(aFrame->data), n) < n) return fail("Audio buffering failed");
            if (n < fs) {
                av_samples_set_silence(aFrame->data, n, fs - n, aFrame->ch_layout.nb_channels,
                                       static_cast<AVSampleFormat>(aFrame->format));
            }
            aFrame->pts = audioSamples;
            audioSamples += fs;
            if ((ret = avcodec_send_frame(aEnc, aFrame)) < 0) return fail("Opus encoding failed", ret);
            if (!drainEncoder()) return false;
        }
        return true;
    }

    // ─── Run ─────────────────────────────────────────────────────────────────

    bool run() {
        if (!openInput() || !setupAudio()) return false;
        if (!aOut) return true; // no sound
        int ret = avio_open(&aOut->pb, audioTmpUtf8.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) return fail("Could not create " + audioTmpUtf8, ret);
        if ((ret = avformat_write_header(aOut, nullptr)) < 0) return fail("Could not start writing " + audioTmpUtf8, ret);
        pkt    = av_packet_alloc();
        outPkt = av_packet_alloc();
        frame  = av_frame_alloc();
        if (!pkt || !outPkt || !frame) return fail("Out of memory");

        Logger::instance().info("VideoTranscoder", "'{}': sound {} -> {}, {:.1f}s", srcUtf8,
                                avcodec_get_name(aIn->codecpar->codec_id), aEnc ? "Opus" : "copied", duration);

        while ((ret = av_read_frame(in, pkt)) >= 0) {
            if (cancel && cancel->load()) {
                av_packet_unref(pkt);
                return fail("Cancelled");
            }
            bool ok = pkt->stream_index != aIn->index || onAudioPacket();
            av_packet_unref(pkt);
            if (!ok) return false;
        }
        if (ret != AVERROR_EOF) {
            // Truncated / damaged file: keep everything read so far (players do the same)
            Logger::instance().warn("VideoTranscoder", "'{}' stopped reading early ({}); the sound ends there",
                                    srcUtf8, avError(ret));
        }

        if (aEnc) {
            avcodec_send_packet(aDec, nullptr);
            if (!drainAudioDecoder() || !resample(nullptr) || !encodeAudio(true)) return false;
            ret = avcodec_send_frame(aEnc, nullptr);
            if (ret < 0 && ret != AVERROR_EOF) return fail("Opus encoding failed", ret);
            if (!drainEncoder()) return false;
        }

        if ((ret = av_write_trailer(aOut)) < 0) return fail("Could not finish the audio file", ret);
        avio_closep(&aOut->pb);
        return true;
    }
};

} // anonymous namespace

VideoTranscodeResult VideoTranscoder::transcodeAudio(const std::filesystem::path& src,
                                                     const std::filesystem::path& audioOut,
                                                     const std::atomic<bool>* cancel) {
    VideoTranscodeResult result;
    std::filesystem::path audioTmp = audioOut;
    audioTmp += ".part";

    const auto started = std::chrono::steady_clock::now();
    bool ok = false;
    {
        Job job;
        job.cancel       = cancel;
        job.srcUtf8      = toUtf8(src);
        job.audioTmpUtf8 = toUtf8(audioTmp);

        ok = job.run();
        result.error    = job.error;
        result.duration = job.duration;
        result.hasAudio = job.aOut != nullptr;
        if (job.skippedPackets > 0) {
            Logger::instance().warn("VideoTranscoder", "'{}': skipped {} damaged packet(s)", job.srcUtf8, job.skippedPackets);
        }
    } // file closed here

    std::error_code ec;
    if (ok && result.hasAudio) {
        std::filesystem::remove(audioOut, ec);
        std::filesystem::rename(audioTmp, audioOut, ec);
        if (ec) {
            ok = false;
            result.error = "Could not move the converted audio into the cache: " + ec.message();
        }
    }
    if (!ok) {
        std::filesystem::remove(audioTmp, ec);
        result.hasAudio = false;
        if (!(cancel && cancel->load())) {
            Logger::instance().error("VideoTranscoder", "Converting the sound of '{}' failed: {}", toUtf8(src), result.error);
        }
    } else if (result.hasAudio) {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        Logger::instance().info("VideoTranscoder", "Converted the sound of '{}' -> '{}' in {:.1f}s ({:.1f}s of sound)",
                                toUtf8(src), toUtf8(audioOut), secs, result.duration);
    }
    result.ok = ok;
    return result;
}

} // namespace ifs
