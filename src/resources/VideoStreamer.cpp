#include "resources/VideoStreamer.hpp"
#include "resources/VideoTranscoder.hpp"
#include "platform/windows/ShortcutResolver.hpp"
#include "core/Logger.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
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

// Pieces. 1024x576 matches the screen's 1024 px texture (~25% less decode than 720p, measured); VP9
// realtime / cpu-used 8 / row-mt converts ~3.5x faster than real time on 8 threads.
constexpr double  kSegmentSeconds   = 4.0;
constexpr int     kMaxWidth         = 1024;
constexpr int     kMaxHeight        = 576;
constexpr double  kMinFrameInterval = 1.0 / 30.0; // closer frames are dropped (60 fps -> 30 fps)
constexpr int64_t kVideoBitRate     = 1500000;
constexpr int     kAheadSegments    = 8;          // conversion stays at most ~32 s ahead of the viewer
constexpr int     kFormatVersion    = 1;          // info.json "version": other versions' pieces are redone
constexpr auto    kIdleTimeout      = std::chrono::seconds(60); // no message from the page: stream stops

std::string segmentName(int index) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "v_%05d.webm", index);
    return buf;
}

} // anonymous namespace

// ─── Stream ──────────────────────────────────────────────────────────────────

struct VideoStreamer::Stream {
    VideoStreamOpenRequest req;
    Sink sink;
    std::filesystem::path src;
    std::filesystem::path dir;
    std::atomic<bool> stop{false};

    std::mutex m;                 // guards the fields below
    std::condition_variable cv;
    int  playSeg     = 0;         // piece the viewer is in
    bool seekPending = false;     // the viewer jumped to a missing piece: the running conversion stops
    std::chrono::steady_clock::time_point lastHeard = std::chrono::steady_clock::now();

    std::thread videoThread;
    std::thread audioThread;

    void send(nlohmann::json j) {
        j["requestId"] = req.requestId;
        if (sink.send) sink.send(j.dump());
    }

    bool alive() const {
        return !stop.load() && (!sink.isConnected || sink.isConnected());
    }

    bool segmentExists(int index) const {
        std::error_code ec;
        return std::filesystem::exists(dir / segmentName(index), ec);
    }

    void runAudio();
    void runVideo();
};

// The sound: converted once to "audio.ogg" (the sound system plays whole files), reused after that
void VideoStreamer::Stream::runAudio() {
    const std::filesystem::path audioOut = dir / "audio.ogg";
    std::error_code ec;
    if (std::filesystem::exists(audioOut, ec) && std::filesystem::file_size(audioOut, ec) > 0) {
        send({{"event", "VIDEO_AUDIO_READY"}, {"status", "success"}, {"hasAudio", true}, {"file", "audio.ogg"}});
        return;
    }
    VideoTranscodeResult r = VideoTranscoder::transcodeAudio(src, audioOut, &stop);
    if (stop.load()) return;
    nlohmann::json ev = {{"event", "VIDEO_AUDIO_READY"}, {"status", r.ok ? "success" : "error"},
                         {"hasAudio", r.hasAudio}, {"file", r.hasAudio ? "audio.ogg" : ""}};
    if (!r.ok) ev["error"] = r.error;
    send(ev);
}

// ─── Picture pieces ──────────────────────────────────────────────────────────

namespace {

struct Producer {
    VideoStreamer::Stream& s;

    AVFormatContext* in    = nullptr;
    AVStream*        vIn   = nullptr;
    AVCodecContext*  dec   = nullptr;
    SwsContext*      sws   = nullptr;
    AVPacket*        pkt   = nullptr;
    AVFrame*         frame = nullptr;
    AVFrame*         scaled = nullptr;
    int64_t   startUs  = 0;
    double    duration = 0.0;
    bool      hasAudio = false;
    int       outW = 0, outH = 0;
    AVRational frameRate{30, 1};
    int       segmentCount = 0; // from the container's length; knownLast replaces it once the end is reached
    int       knownLast    = -1;

    // Piece being written
    AVFormatContext* out    = nullptr;
    AVCodecContext*  enc    = nullptr;
    AVStream*        oSt    = nullptr;
    AVPacket*        outPkt = nullptr;
    std::filesystem::path partPath;
    int     curSeg      = -1;
    int     nextSeg     = 0;   // first piece the current run still has to write
    int     lastWritten = -1;
    double  lastKeptSec = -1.0e9;
    double  lastAbsSec  = -1.0;  // last decoded frame's time in the video (frames without a timestamp)
    int64_t lastPts     = -1;
    bool    hasScaled   = false; // `scaled` holds a picture (used to fill a piece with no frames)
    bool    stopRun     = false;
    uint64_t skippedPackets = 0;
    std::string error;

    explicit Producer(VideoStreamer::Stream& stream) : s(stream) {}

    ~Producer() {
        abortSegment();
        avformat_close_input(&in);
        avcodec_free_context(&dec);
        sws_freeContext(sws);
        av_packet_free(&pkt);
        av_packet_free(&outPkt);
        av_frame_free(&frame);
        av_frame_free(&scaled);
    }

    bool fail(const std::string& msg, int code = 0) {
        error = code ? msg + ": " + avError(code) : msg;
        return false;
    }

    bool open() {
        const std::string srcUtf8 = toUtf8(s.src);
        int ret = avformat_open_input(&in, srcUtf8.c_str(), nullptr, nullptr);
        if (ret < 0) return fail("Could not open the video", ret);
        if ((ret = avformat_find_stream_info(in, nullptr)) < 0) return fail("Could not read the video streams", ret);
        int vi = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vi < 0) return fail("The file has no video stream");
        vIn = in->streams[vi];
        hasAudio = av_find_best_stream(in, AVMEDIA_TYPE_AUDIO, -1, vi, nullptr, 0) >= 0;
        startUs  = in->start_time != AV_NOPTS_VALUE ? in->start_time : 0;
        duration = in->duration != AV_NOPTS_VALUE ? static_cast<double>(in->duration) / AV_TIME_BASE : 0.0;
        segmentCount = std::max(1, static_cast<int>(std::ceil(duration / kSegmentSeconds)));

        const AVCodec* decoder = avcodec_find_decoder(vIn->codecpar->codec_id);
        if (!decoder) return fail(std::string("No video decoder for ") + avcodec_get_name(vIn->codecpar->codec_id));
        dec = avcodec_alloc_context3(decoder);
        if (!dec) return fail("Out of memory");
        if ((ret = avcodec_parameters_to_context(dec, vIn->codecpar)) < 0) return fail("Could not set up the video decoder", ret);
        dec->pkt_timebase = vIn->time_base;
        dec->thread_count = 0; // auto
        if ((ret = avcodec_open2(dec, decoder, nullptr)) < 0) return fail("Could not open the video decoder", ret);
        if (dec->width <= 0 || dec->height <= 0) return fail("Unknown video size");

        double scale = std::min({1.0, static_cast<double>(kMaxWidth) / dec->width,
                                      static_cast<double>(kMaxHeight) / dec->height});
        outW = std::max(2, static_cast<int>(std::lround(dec->width * scale)) & ~1);
        outH = std::max(2, static_cast<int>(std::lround(dec->height * scale)) & ~1);
        frameRate = av_guess_frame_rate(in, vIn, nullptr);
        if (frameRate.num <= 0 || frameRate.den <= 0 || av_q2d(frameRate) > 1.0 / kMinFrameInterval) frameRate = AVRational{30, 1};

        pkt    = av_packet_alloc();
        outPkt = av_packet_alloc();
        frame  = av_frame_alloc();
        scaled = av_frame_alloc();
        if (!pkt || !outPkt || !frame || !scaled) return fail("Out of memory");
        scaled->format = AV_PIX_FMT_YUV420P;
        scaled->width  = outW;
        scaled->height = outH;
        if ((ret = av_frame_get_buffer(scaled, 0)) < 0) return fail("Out of memory", ret);
        return true;
    }

    // ─── One piece = one standalone WebM (fresh encoder: starts on a keyframe) ───

    bool startSegment(int index) {
        const AVCodec* encoder = avcodec_find_encoder_by_name("libvpx-vp9");
        if (!encoder) return fail("This build has no VP9 encoder (libvpx)");
        enc = avcodec_alloc_context3(encoder);
        if (!enc) return fail("Out of memory");
        enc->width               = outW;
        enc->height              = outH;
        enc->pix_fmt             = AV_PIX_FMT_YUV420P;
        enc->time_base           = AVRational{1, 1000};
        enc->framerate           = frameRate;
        enc->sample_aspect_ratio = dec->sample_aspect_ratio;
        enc->bit_rate            = kVideoBitRate;
        enc->gop_size            = 1000; // one keyframe per piece (its first frame)
        enc->thread_count        = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);

        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "deadline", "realtime", 0);
        av_dict_set(&opts, "cpu-used", "8", 0);
        av_dict_set(&opts, "row-mt", "1", 0);
        av_dict_set(&opts, "tile-columns", "2", 0);
        av_dict_set(&opts, "lag-in-frames", "0", 0);
        int ret = avcodec_open2(enc, encoder, &opts);
        av_dict_free(&opts);
        if (ret < 0) return fail("Could not open the VP9 encoder", ret);

        partPath = s.dir / (segmentName(index) + ".part");
        const std::string partUtf8 = toUtf8(partPath);
        ret = avformat_alloc_output_context2(&out, nullptr, "webm", partUtf8.c_str());
        if (ret < 0 || !out) return fail("Could not create the webm output", ret);
        oSt = avformat_new_stream(out, nullptr);
        if (!oSt) return fail("Could not add the video stream");
        if ((ret = avcodec_parameters_from_context(oSt->codecpar, enc)) < 0) return fail("Could not set up the video stream", ret);
        oSt->time_base      = enc->time_base;
        oSt->avg_frame_rate = frameRate;
        if ((ret = avio_open(&out->pb, partUtf8.c_str(), AVIO_FLAG_WRITE)) < 0) return fail("Could not create " + partUtf8, ret);
        if ((ret = avformat_write_header(out, nullptr)) < 0) return fail("Could not start writing " + partUtf8, ret);

        curSeg      = index;
        lastKeptSec = -1.0e9;
        lastPts     = -1;
        return true;
    }

    bool drainEncoder() {
        int ret;
        while ((ret = avcodec_receive_packet(enc, outPkt)) >= 0) {
            av_packet_rescale_ts(outPkt, enc->time_base, oSt->time_base);
            outPkt->stream_index = oSt->index;
            outPkt->pos = -1;
            if ((ret = av_interleaved_write_frame(out, outPkt)) < 0) return fail("Could not write the video piece", ret);
        }
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
        return fail("Encoding failed", ret);
    }

    bool encodeScaled(int64_t pts) {
        scaled->pts = pts;
        int ret = avcodec_send_frame(enc, scaled);
        if (ret < 0) return fail("VP9 encoding failed", ret);
        return drainEncoder();
    }

    void freeSegment() {
        if (out) {
            if (out->pb) avio_closep(&out->pb);
            avformat_free_context(out);
            out = nullptr;
        }
        avcodec_free_context(&enc);
        oSt = nullptr;
        curSeg = -1;
    }

    // A stopped run leaves no half piece behind
    void abortSegment() {
        if (curSeg < 0 && !out) return;
        freeSegment();
        std::error_code ec;
        std::filesystem::remove(partPath, ec);
    }

    bool finishSegment() {
        const int index = curSeg;
        int ret = avcodec_send_frame(enc, nullptr);
        if (ret < 0 && ret != AVERROR_EOF) return fail("VP9 encoding failed", ret);
        if (!drainEncoder()) return false;
        if ((ret = av_write_trailer(out)) < 0) return fail("Could not finish the video piece", ret);
        freeSegment();

        std::error_code ec;
        const std::filesystem::path finalPath = s.dir / segmentName(index);
        std::filesystem::remove(finalPath, ec);
        std::filesystem::rename(partPath, finalPath, ec);
        if (ec) return fail("Could not move the video piece into the cache: " + ec.message());
        lastWritten = index;
        s.send({{"event", "VIDEO_SEGMENT_READY"}, {"index", index}});
        return true;
    }

    // Whether the run goes on to piece `index` (checked between pieces)
    bool shouldWrite(int index) {
        if (!s.alive()) return false;
        if (knownLast >= 0 && index > knownLast) return false;
        if (s.segmentExists(index)) return false; // done earlier: the outer loop seeks to the next missing one
        std::lock_guard lock(s.m);
        return !s.seekPending && index - s.playSeg <= kAheadSegments;
    }

    // ─── Frames ──────────────────────────────────────────────────────────────

    bool scaleInto(const AVFrame* f) {
        sws = sws_getCachedContext(sws, f->width, f->height, static_cast<AVPixelFormat>(f->format),
                                   outW, outH, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return fail("Could not set up video scaling");
        int ret = av_frame_make_writable(scaled);
        if (ret < 0) return fail("Out of memory", ret);
        sws_scale(sws, f->data, f->linesize, 0, f->height, scaled->data, scaled->linesize);
        hasScaled = true;
        return true;
    }

    bool feed(const AVFrame* f) {
        const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        const double sec = ts != AV_NOPTS_VALUE
            ? (ts - av_rescale_q(startUs, AV_TIME_BASE_Q, vIn->time_base)) * av_q2d(vIn->time_base)
            : (lastAbsSec >= 0.0 ? lastAbsSec + kMinFrameInterval : nextSeg * kSegmentSeconds);
        lastAbsSec = sec;
        // Frames before the piece being written (or, between pieces, the next one to write): decoded from the
        // keyframe before the seek point
        const int floorSeg = curSeg >= 0 ? curSeg : nextSeg;
        if (sec < floorSeg * kSegmentSeconds - 0.0005) return true;

        const int seg = std::max(floorSeg, static_cast<int>(std::floor(sec / kSegmentSeconds + 1e-9)));
        if (curSeg >= 0 && seg > curSeg) {
            if (!finishSegment()) return false;
            if (!shouldWrite(nextSeg)) {
                stopRun = true;
                return true;
            }
        }
        if (curSeg < 0) {
            // Pieces with no frame of their own (a still picture longer than a piece, a picture that starts
            // late): the previous picture again, or this one. Skipping them would leave a hole the player
            // waits on, and the outer loop would ask for it again and again.
            if (nextSeg < seg && !hasScaled && !scaleInto(f)) return false;
            while (nextSeg < seg) {
                if (!startSegment(nextSeg) || !encodeScaled(0) || !finishSegment()) return false;
                ++nextSeg;
                if (!shouldWrite(nextSeg)) {
                    stopRun = true;
                    return true;
                }
            }
            if (!startSegment(seg)) return false;
            nextSeg = seg + 1;
        }

        const double inSeg = sec - curSeg * kSegmentSeconds;
        if (inSeg + 0.001 < lastKeptSec + kMinFrameInterval) return true; // above 30 fps
        lastKeptSec = inSeg;
        if (!scaleInto(f)) return false;
        // The first frame sits on the piece's start (it is at most one frame later), so pieces join without
        // a gap in the player's buffer
        const int64_t pts = lastPts < 0 ? 0 : std::max<int64_t>(std::llround(inSeg * 1000.0), lastPts + 1);
        lastPts = pts;
        return encodeScaled(pts);
    }

    bool drainDecoder() {
        int ret;
        while ((ret = avcodec_receive_frame(dec, frame)) >= 0) {
            bool ok = feed(frame);
            av_frame_unref(frame);
            if (!ok) return false;
            if (stopRun) return true;
        }
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
        return fail("Video decoding failed", ret);
    }

    // Writes pieces from `start` until the end, a piece that exists, the look-ahead limit, a seek or a stop.
    // false only for a real error (error set).
    bool run(int start) {
        const int64_t target = av_rescale_q(startUs + std::llround(start * kSegmentSeconds * AV_TIME_BASE),
                                            AV_TIME_BASE_Q, vIn->time_base);
        int ret = av_seek_frame(in, vIn->index, target, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) return fail("Could not seek in the video", ret);
        avcodec_flush_buffers(dec);
        nextSeg    = start;
        stopRun    = false;
        hasScaled  = false;
        lastAbsSec = -1.0;

        while ((ret = av_read_frame(in, pkt)) >= 0) {
            if (pkt->stream_index != vIn->index) {
                av_packet_unref(pkt);
                continue;
            }
            bool interrupted = !s.alive();
            if (!interrupted) {
                std::lock_guard lock(s.m);
                interrupted = s.seekPending;
            }
            if (interrupted) {
                av_packet_unref(pkt);
                abortSegment();
                return true;
            }
            ret = avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                ++skippedPackets; // undecodable (corrupt) packet, skipped like players do
                continue;
            }
            if (!drainDecoder()) return false;
            if (stopRun) return true;
        }
        if (ret != AVERROR_EOF) {
            Logger::instance().warn("VideoStreamer", "'{}' stopped reading early ({}); the video ends there",
                                    toUtf8(s.src), avError(ret));
        }

        // End of the video: the last piece, then the real piece count
        avcodec_send_packet(dec, nullptr);
        if (!drainDecoder()) return false;
        if (stopRun) return true;
        if (curSeg >= 0 && !finishSegment()) return false;
        knownLast = std::max(lastWritten, nextSeg - 1);
        s.send({{"event", "VIDEO_STREAM_END"}, {"lastSegment", knownLast}});
        return true;
    }
};

void writeInfo(const std::filesystem::path& dir, const Producer& p, bool complete) {
    const int count = p.knownLast >= 0 ? p.knownLast + 1 : p.segmentCount;
    nlohmann::json info = {
        {"version", kFormatVersion}, {"duration", p.duration}, {"segmentSeconds", kSegmentSeconds},
        {"segmentCount", count}, {"width", p.outW}, {"height", p.outH}, {"hasAudio", p.hasAudio},
        {"complete", complete}
    };
    std::ofstream f(dir / "info.json", std::ios::binary | std::ios::trunc);
    if (f.is_open()) f << info.dump();
    if (!f) Logger::instance().warn("VideoStreamer", "Could not write {}", toUtf8(dir / "info.json"));
}

// Pieces from another format version are not compatible with the player's MediaSource: redone
void dropOtherVersionPieces(const std::filesystem::path& dir) {
    std::error_code ec;
    std::ifstream f(dir / "info.json", std::ios::binary);
    if (!f.is_open()) return;
    auto info = nlohmann::json::parse(f, nullptr, false);
    if (!info.is_discarded() && info.value("version", 0) == kFormatVersion) return;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = toUtf8(entry.path().filename());
        if (name.rfind("v_", 0) == 0) std::filesystem::remove(entry.path(), ec);
    }
    Logger::instance().info("VideoStreamer", "Dropped pieces of another format in {}", toUtf8(dir));
}

} // anonymous namespace

void VideoStreamer::Stream::runVideo() {
    Producer p(*this);
    if (!p.open()) {
        Logger::instance().error("VideoStreamer", "Opening '{}' failed: {}", toUtf8(src), p.error);
        send({{"event", "VIDEO_STREAM_INFO"}, {"status", "error"}, {"error", p.error}});
        return;
    }
    dropOtherVersionPieces(dir);

    nlohmann::json onDisk = nlohmann::json::array();
    for (int i = 0; i < p.segmentCount; ++i) {
        if (segmentExists(i)) onDisk.push_back(i);
    }
    writeInfo(dir, p, false);
    send({{"event", "VIDEO_STREAM_INFO"}, {"status", "success"}, {"duration", p.duration},
          {"segmentSeconds", kSegmentSeconds}, {"segmentCount", p.segmentCount}, {"width", p.outW},
          {"height", p.outH}, {"hasAudio", p.hasAudio}, {"segments", onDisk}});
    Logger::instance().info("VideoStreamer", "Stream '{}': '{}' {:.1f}s, {} piece(s) of {:.0f}s ({} on disk), {}x{}",
                            req.requestId, toUtf8(src), p.duration, p.segmentCount, kSegmentSeconds,
                            onDisk.size(), p.outW, p.outH);

    bool completeWritten = false;
    while (alive()) {
        int from;
        {
            std::lock_guard lock(m);
            from = playSeg;
            seekPending = false;
        }
        const int last = p.knownLast >= 0 ? p.knownLast : p.segmentCount - 1;
        int next = -1;
        for (int i = std::max(0, from); i <= last; ++i) {
            if (!segmentExists(i)) {
                next = i;
                break;
            }
        }

        if (next < 0 || next - from > kAheadSegments) {
            if (next < 0 && !completeWritten && p.knownLast >= 0) {
                bool all = true;
                for (int i = 0; i <= last && all; ++i) all = segmentExists(i);
                if (all) {
                    writeInfo(dir, p, true);
                    completeWritten = true;
                }
            }
            std::unique_lock lock(m);
            cv.wait_for(lock, std::chrono::seconds(1), [&] { return stop.load() || seekPending || playSeg != from; });
            if (std::chrono::steady_clock::now() - lastHeard > kIdleTimeout) {
                Logger::instance().info("VideoStreamer", "Stream '{}': no word from the screen for {} s, stopping",
                                        req.requestId, std::chrono::duration_cast<std::chrono::seconds>(kIdleTimeout).count());
                stop = true;
            }
            continue;
        }

        if (!p.run(next)) {
            Logger::instance().error("VideoStreamer", "Stream '{}' ('{}') failed at piece {}: {}",
                                     req.requestId, toUtf8(src), next, p.error);
            send({{"event", "VIDEO_STREAM_ERROR"}, {"error", p.error}, {"index", next}});
            break;
        }
        if (p.knownLast >= 0) writeInfo(dir, p, false);
    }
    if (p.skippedPackets > 0) {
        Logger::instance().warn("VideoStreamer", "'{}': skipped {} damaged packet(s)", toUtf8(src), p.skippedPackets);
    }
}

// ─── VideoStreamer ───────────────────────────────────────────────────────────

VideoStreamer::VideoStreamer() = default;

VideoStreamer::~VideoStreamer() {
    std::vector<std::shared_ptr<Stream>> all;
    {
        std::lock_guard lock(mutex_);
        for (auto& [id, st] : streams_) all.push_back(st);
        streams_.clear();
    }
    for (auto& st : all) {
        st->stop = true;
        st->cv.notify_all();
        if (st->videoThread.joinable()) st->videoThread.join();
        if (st->audioThread.joinable()) st->audioThread.join();
    }
}

void VideoStreamer::open(const VideoStreamOpenRequest& request, Sink sink) {
    auto st = std::make_shared<Stream>();
    st->req  = request;
    st->sink = std::move(sink);
    st->src  = std::filesystem::path(request.sourcePath);
    if (ShortcutResolver::isShortcut(st->src)) {
        if (auto resolved = ShortcutResolver::resolve(st->src)) st->src = *resolved;
    }
    st->dir     = std::filesystem::path(request.targetDir);
    st->playSeg = std::max(0, static_cast<int>(std::floor(request.startTime / kSegmentSeconds)));

    std::error_code ec;
    if (!std::filesystem::is_regular_file(st->src, ec)) {
        Logger::instance().warn("VideoStreamer", "Source file not found: {}", request.sourcePath);
        st->send({{"event", "VIDEO_STREAM_INFO"}, {"status", "error"}, {"error", "The video file was not found"}});
        return;
    }
    std::filesystem::create_directories(st->dir, ec);
    if (ec) {
        st->send({{"event", "VIDEO_STREAM_INFO"}, {"status", "error"}, {"error", "Could not create the cache folder: " + ec.message()}});
        return;
    }

    // One writer per folder (and per id): an older stream of the same video stops first
    const std::string dirKey = toUtf8(std::filesystem::weakly_canonical(st->dir, ec));
    std::vector<std::string> replaced;
    {
        std::lock_guard lock(mutex_);
        for (auto& [id, other] : streams_) {
            if (id == request.requestId || toUtf8(std::filesystem::weakly_canonical(other->dir, ec)) == dirKey) {
                replaced.push_back(id);
            }
        }
    }
    for (const auto& id : replaced) close(id);

    Logger::instance().info("VideoStreamer", "Opening stream '{}': '{}' -> '{}' from {:.1f}s",
                            request.requestId, request.sourcePath, request.targetDir, request.startTime);
    st->audioThread = std::thread([st]() { st->runAudio(); });
    st->videoThread = std::thread([st]() { st->runVideo(); });
    std::lock_guard lock(mutex_);
    streams_[request.requestId] = st;
}

void VideoStreamer::seek(const std::string& requestId, double time) {
    std::shared_ptr<Stream> st;
    {
        std::lock_guard lock(mutex_);
        auto it = streams_.find(requestId);
        if (it == streams_.end()) return;
        st = it->second;
    }
    const int seg = std::max(0, static_cast<int>(std::floor(time / kSegmentSeconds)));
    const bool missing = !st->segmentExists(seg);
    {
        std::lock_guard lock(st->m);
        st->playSeg   = seg;
        st->lastHeard = std::chrono::steady_clock::now();
        // A piece already on disk needs no restart: the running conversion keeps going (paced by playSeg)
        if (missing) st->seekPending = true;
    }
    st->cv.notify_all();
}

void VideoStreamer::position(const std::string& requestId, double time) {
    std::shared_ptr<Stream> st;
    {
        std::lock_guard lock(mutex_);
        auto it = streams_.find(requestId);
        if (it == streams_.end()) return;
        st = it->second;
    }
    {
        std::lock_guard lock(st->m);
        st->playSeg   = std::max(0, static_cast<int>(std::floor(time / kSegmentSeconds)));
        st->lastHeard = std::chrono::steady_clock::now();
    }
    st->cv.notify_all();
}

void VideoStreamer::close(const std::string& requestId) {
    std::shared_ptr<Stream> st;
    {
        std::lock_guard lock(mutex_);
        auto it = streams_.find(requestId);
        if (it == streams_.end()) return;
        st = it->second;
        streams_.erase(it);
    }
    st->stop = true;
    st->cv.notify_all();
    if (st->videoThread.joinable()) st->videoThread.join();
    if (st->audioThread.joinable()) st->audioThread.join();
    Logger::instance().info("VideoStreamer", "Closed stream '{}'", requestId);
}

} // namespace ifs
