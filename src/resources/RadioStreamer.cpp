#include "resources/RadioStreamer.hpp"
#include "core/Logger.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <thread>

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

constexpr int     kSampleRate      = 48000;  // Opus
constexpr int     kChunkMs         = 2000;   // one piece
constexpr int64_t kMinBufferBytes  = 512 * 1024;
constexpr size_t  kMinPiecesKept   = 4;      // never fewer on disk, whatever maxBufferBytes says
constexpr int     kOpenAttempts    = 3;      // a station that never answered: error after this many tries
constexpr int     kMaxBackoffSec   = 10;
constexpr auto    kIdleTimeout     = std::chrono::seconds(30); // no word from the page: stream stops
constexpr auto    kMetadataEvery   = std::chrono::seconds(1);
constexpr const char* kHeaderName  = "init.hdr";
constexpr const char* kMimeType    = "audio/webm; codecs=\"opus\"";
constexpr const char* kUserAgent   = "Mozilla/5.0 (c6g3 infotainment radio)";

std::string chunkName(int index) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "r_%05d.chk", index);
    return buf;
}

bool isBufferFile(const std::string& name) {
    return name == kHeaderName || (name.rfind("r_", 0) == 0 && name.size() > 4 && name.substr(name.size() - 4) == ".chk");
}

// "StreamTitle='Artist - Song';StreamUrl='';" -> "Artist - Song"
std::string icyTitle(const std::string& packet) {
    const std::string key = "StreamTitle='";
    size_t start = packet.find(key);
    if (start == std::string::npos) return "";
    start += key.size();
    size_t end = packet.find("';", start);
    if (end == std::string::npos) end = packet.rfind('\'');
    if (end == std::string::npos || end < start) return "";
    return packet.substr(start, end - start);
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // anonymous namespace

// ─── Stream ──────────────────────────────────────────────────────────────────

struct RadioStreamer::Stream {
    RadioStreamOpenRequest req;
    Sink sink;
    std::filesystem::path dir;
    std::atomic<bool> stop{false};
    std::atomic<int64_t> lastHeardMs{nowMs()};
    std::mutex m;
    std::condition_variable cv;
    std::thread thread;

    // Output: one Opus encoder and one webm_chunk muxer for the whole session (continuous across reconnects)
    AVCodecContext*  enc      = nullptr;
    AVFormatContext* out      = nullptr;
    AVStream*        oSt      = nullptr;
    AVAudioFifo*     fifo     = nullptr;
    AVFrame*         encFrame = nullptr;
    AVPacket*        outPkt   = nullptr;
    int64_t samplesOut = 0;
    int     nextChunk  = 0;
    std::deque<std::pair<int, int64_t>> onDisk; // (index, bytes), oldest first
    int64_t bytesOnDisk = 0;
    std::string lastTitle;
    std::string error;

    ~Stream() {
        if (out) avformat_free_context(out);
        avcodec_free_context(&enc);
        if (fifo) av_audio_fifo_free(fifo);
        av_frame_free(&encFrame);
        av_packet_free(&outPkt);
    }

    void send(nlohmann::json j) {
        j["requestId"] = req.requestId;
        // Station titles can be Latin-1: replaced, not thrown (a throw here would end the service)
        if (sink.send) sink.send(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }

    bool idle() const {
        return nowMs() - lastHeardMs.load() > std::chrono::duration_cast<std::chrono::milliseconds>(kIdleTimeout).count();
    }

    bool alive() const {
        return !stop.load() && !idle() && (!sink.isConnected || sink.isConnected());
    }

    bool fail(const std::string& msg, int code = 0) {
        error = code ? msg + ": " + avError(code) : msg;
        return false;
    }

    // Waits up to `seconds` (wakes on close)
    void sleepFor(int seconds) {
        std::unique_lock lock(m);
        cv.wait_for(lock, std::chrono::seconds(seconds), [&] { return stop.load(); });
    }

    void clearFolder() {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (isBufferFile(toUtf8(entry.path().filename()))) std::filesystem::remove(entry.path(), ec);
        }
    }

    bool openOutput();
    bool writePackets();
    bool encodeFifo();
    void collectChunks();
    bool listen(bool& played);
    void run();
};

bool RadioStreamer::Stream::openOutput() {
    const AVCodec* encoder = avcodec_find_encoder_by_name("libopus");
    if (!encoder) return fail("This build has no Opus encoder (libopus)");
    enc = avcodec_alloc_context3(encoder);
    if (!enc) return fail("Out of memory");
    enc->sample_rate = kSampleRate;
    enc->sample_fmt  = AV_SAMPLE_FMT_FLT;
    enc->bit_rate    = std::clamp<int64_t>(req.bitRate, 32000, 256000);
    enc->time_base   = AVRational{1, kSampleRate};
    av_channel_layout_default(&enc->ch_layout, 2);

    // The muxer's file pattern: a '%' in the folder would be read as a number field
    std::string folder = toUtf8(dir);
    std::string escaped;
    for (char c : folder) {
        escaped += c;
        if (c == '%') escaped += '%';
    }
    const std::string pattern = escaped + "/r_%05d.chk";
    int ret = avformat_alloc_output_context2(&out, nullptr, "webm_chunk", pattern.c_str());
    if (ret < 0 || !out) return fail("Could not create the radio buffer", ret);
    if (out->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if ((ret = avcodec_open2(enc, encoder, nullptr)) < 0) return fail("Could not open the Opus encoder", ret);

    oSt = avformat_new_stream(out, nullptr);
    if (!oSt) return fail("Could not add the audio stream");
    if ((ret = avcodec_parameters_from_context(oSt->codecpar, enc)) < 0) return fail("Could not set up the audio stream", ret);
    oSt->time_base = enc->time_base;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "header", toUtf8(dir / kHeaderName).c_str(), 0);
    av_dict_set(&opts, "chunk_start_index", "0", 0);
    av_dict_set(&opts, "audio_chunk_duration", std::to_string(kChunkMs).c_str(), 0);
    if (!(out->oformat->flags & AVFMT_NOFILE) && (ret = avio_open(&out->pb, pattern.c_str(), AVIO_FLAG_WRITE)) < 0) {
        av_dict_free(&opts);
        return fail("Could not create the radio buffer", ret);
    }
    ret = avformat_write_header(out, &opts);
    av_dict_free(&opts);
    if (ret < 0) return fail("Could not write the radio stream header", ret);

    fifo     = av_audio_fifo_alloc(enc->sample_fmt, enc->ch_layout.nb_channels, enc->frame_size > 0 ? enc->frame_size * 4 : 4096);
    encFrame = av_frame_alloc();
    outPkt   = av_packet_alloc();
    if (!fifo || !encFrame || !outPkt) return fail("Out of memory");
    encFrame->format      = enc->sample_fmt;
    encFrame->sample_rate = enc->sample_rate;
    encFrame->nb_samples  = enc->frame_size > 0 ? enc->frame_size : 960;
    av_channel_layout_copy(&encFrame->ch_layout, &enc->ch_layout);
    if ((ret = av_frame_get_buffer(encFrame, 0)) < 0) return fail("Out of memory", ret);
    return true;
}

// A piece is written whole when the next one starts (webm_chunk): every file that exists after a write is done
void RadioStreamer::Stream::collectChunks() {
    std::error_code ec;
    for (;;) {
        const std::filesystem::path p = dir / chunkName(nextChunk);
        if (!std::filesystem::exists(p, ec)) break;
        const int64_t bytes = static_cast<int64_t>(std::filesystem::file_size(p, ec));
        onDisk.emplace_back(nextChunk, bytes);
        bytesOnDisk += bytes;
        send({{"event", "RADIO_CHUNK_READY"}, {"index", nextChunk}, {"file", chunkName(nextChunk)}, {"bytes", bytes}});
        ++nextChunk;
    }
    const int64_t limit = std::max(kMinBufferBytes, req.maxBufferBytes);
    while (bytesOnDisk > limit && onDisk.size() > kMinPiecesKept) {
        std::filesystem::remove(dir / chunkName(onDisk.front().first), ec);
        bytesOnDisk -= onDisk.front().second;
        onDisk.pop_front();
    }
}

bool RadioStreamer::Stream::writePackets() {
    int ret;
    while ((ret = avcodec_receive_packet(enc, outPkt)) >= 0) {
        av_packet_rescale_ts(outPkt, enc->time_base, oSt->time_base);
        outPkt->stream_index = oSt->index;
        ret = av_write_frame(out, outPkt);
        av_packet_unref(outPkt);
        if (ret < 0) return fail("Could not write the radio piece", ret);
        collectChunks();
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
    return fail("Opus encoding failed", ret);
}

// Encodes every whole Opus frame waiting in the fifo; the stream's clock is the count of samples written
bool RadioStreamer::Stream::encodeFifo() {
    const int frameSize = encFrame->nb_samples;
    while (av_audio_fifo_size(fifo) >= frameSize) {
        int ret = av_frame_make_writable(encFrame);
        if (ret < 0) return fail("Out of memory", ret);
        if (av_audio_fifo_read(fifo, reinterpret_cast<void**>(encFrame->data), frameSize) < frameSize) return fail("Audio buffer read failed");
        encFrame->pts = samplesOut;
        samplesOut += frameSize;
        if ((ret = avcodec_send_frame(enc, encFrame)) < 0) return fail("Opus encoding failed", ret);
        if (!writePackets()) return false;
    }
    return true;
}

static int interruptCallback(void* opaque) {
    auto* s = static_cast<RadioStreamer::Stream*>(opaque);
    return (s->stop.load() || s->idle()) ? 1 : 0;
}

// One connection to the station: plays until it drops (true) or a real error (false, error set).
// played: set once sound came through on this connection.
bool RadioStreamer::Stream::listen(bool& played) {
    played = false;
    AVFormatContext* in = avformat_alloc_context();
    AVCodecContext* dec = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* converted = av_frame_alloc();
    AVChannelLayout swrInLayout{};
    int swrInRate = 0;
    int swrInFormat = -1;
    bool ok = true;

    auto cleanup = [&]() {
        avformat_close_input(&in);
        avcodec_free_context(&dec);
        swr_free(&swr);
        av_packet_free(&pkt);
        av_frame_free(&frame);
        av_frame_free(&converted);
        av_channel_layout_uninit(&swrInLayout);
    };
    if (!in || !pkt || !frame || !converted) {
        cleanup();
        return fail("Out of memory");
    }
    in->interrupt_callback.callback = interruptCallback;
    in->interrupt_callback.opaque = this;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "user_agent", kUserAgent, 0);
    av_dict_set(&opts, "icy", "1", 0);
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_on_network_error", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "5", 0);
    av_dict_set(&opts, "rw_timeout", "15000000", 0); // 15 s without data: the connection is dead
    // A radio stream has one audio stream: a short probe starts it sooner (the default reads ~5 s first)
    av_dict_set(&opts, "probesize", "65536", 0);
    av_dict_set(&opts, "analyzeduration", "1000000", 0);
    int ret = avformat_open_input(&in, req.url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        cleanup(); // avformat_open_input freed `in` on failure
        return fail("Could not reach the station", ret);
    }
    if ((ret = avformat_find_stream_info(in, nullptr)) < 0) {
        cleanup();
        return fail("Could not read the station's stream", ret);
    }
    const int ai = av_find_best_stream(in, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (ai < 0) {
        cleanup();
        return fail("The station sends no audio");
    }
    // Only the audio is read (HLS stations may also carry other programs / streams)
    for (unsigned i = 0; i < in->nb_streams; ++i) in->streams[i]->discard = (static_cast<int>(i) == ai) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;

    const AVCodec* decoder = avcodec_find_decoder(in->streams[ai]->codecpar->codec_id);
    if (!decoder) {
        cleanup();
        return fail(std::string("No decoder for the station's ") + avcodec_get_name(in->streams[ai]->codecpar->codec_id));
    }
    dec = avcodec_alloc_context3(decoder);
    if (!dec || avcodec_parameters_to_context(dec, in->streams[ai]->codecpar) < 0 || avcodec_open2(dec, decoder, nullptr) < 0) {
        cleanup();
        return fail("Could not open the station's audio decoder");
    }
    Logger::instance().info("RadioStreamer", "Stream '{}': listening to {} ({}, {} Hz, {} ch)", req.requestId, req.url,
                            avcodec_get_name(dec->codec_id), dec->sample_rate, dec->ch_layout.nb_channels);

    auto lastMetaCheck = std::chrono::steady_clock::now() - kMetadataEvery;
    while (ok && alive()) {
        ret = av_read_frame(in, pkt);
        if (ret == AVERROR(EAGAIN)) continue;
        if (ret < 0) {
            if (alive()) Logger::instance().warn("RadioStreamer", "Stream '{}': the station stopped sending ({})", req.requestId, avError(ret));
            break;
        }
        if (pkt->stream_index != ai) {
            av_packet_unref(pkt);
            continue;
        }
        ret = avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) continue; // a damaged packet: skipped like players do

        while (ok && (ret = avcodec_receive_frame(dec, frame)) >= 0) {
            // The station's format can change (HLS discontinuity): the resampler follows the frames
            AVChannelLayout inLayout{};
            if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || frame->ch_layout.nb_channels <= 0) {
                av_channel_layout_default(&inLayout, std::max(1, frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : dec->ch_layout.nb_channels));
            } else {
                av_channel_layout_copy(&inLayout, &frame->ch_layout);
            }
            if (!swr || swrInRate != frame->sample_rate || swrInFormat != frame->format || av_channel_layout_compare(&swrInLayout, &inLayout) != 0) {
                swr_free(&swr);
                if (swr_alloc_set_opts2(&swr, &enc->ch_layout, enc->sample_fmt, enc->sample_rate,
                                        &inLayout, static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr) < 0 ||
                    swr_init(swr) < 0) {
                    av_channel_layout_uninit(&inLayout);
                    ok = fail("Could not convert the station's audio");
                    break;
                }
                av_channel_layout_uninit(&swrInLayout);
                av_channel_layout_copy(&swrInLayout, &inLayout);
                swrInRate = frame->sample_rate;
                swrInFormat = frame->format;
            }
            av_channel_layout_uninit(&inLayout);

            const int maxOut = swr_get_out_samples(swr, frame->nb_samples);
            av_frame_unref(converted);
            converted->format      = enc->sample_fmt;
            converted->sample_rate = enc->sample_rate;
            converted->nb_samples  = std::max(1, maxOut);
            av_channel_layout_copy(&converted->ch_layout, &enc->ch_layout);
            if (av_frame_get_buffer(converted, 0) < 0) {
                ok = fail("Out of memory");
                break;
            }
            const int n = swr_convert(swr, converted->data, converted->nb_samples,
                                      reinterpret_cast<const uint8_t* const*>(frame->extended_data), frame->nb_samples);
            av_frame_unref(frame);
            if (n < 0) continue;
            if (n > 0 && av_audio_fifo_write(fifo, reinterpret_cast<void**>(converted->data), n) < n) {
                ok = fail("Audio buffer write failed");
                break;
            }
            if (!encodeFifo()) {
                ok = false;
                break;
            }
            if (!played && n > 0) {
                played = true;
                send({{"event", "RADIO_STREAM_STATE"}, {"state", "playing"}});
            }
        }

        // "Now playing" (Icecast / Shoutcast metadata)
        const auto now = std::chrono::steady_clock::now();
        if (now - lastMetaCheck >= kMetadataEvery) {
            lastMetaCheck = now;
            uint8_t* meta = nullptr;
            if (av_opt_get(in, "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN, &meta) >= 0 && meta) {
                const std::string title = icyTitle(reinterpret_cast<const char*>(meta));
                if (!title.empty() && title != lastTitle) {
                    lastTitle = title;
                    send({{"event", "RADIO_METADATA"}, {"title", title}});
                }
            }
            av_free(meta);
        }
    }
    cleanup();
    return ok;
}

void RadioStreamer::Stream::run() {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        send({{"event", "RADIO_STREAM_INFO"}, {"status", "error"}, {"error", "Could not create the radio buffer folder: " + ec.message()}});
        return;
    }
    clearFolder();
    if (!openOutput()) {
        Logger::instance().error("RadioStreamer", "Stream '{}': {}", req.requestId, error);
        send({{"event", "RADIO_STREAM_INFO"}, {"status", "error"}, {"error", error}});
        clearFolder();
        return;
    }
    send({{"event", "RADIO_STREAM_INFO"}, {"status", "success"}, {"header", kHeaderName},
          {"chunkSeconds", kChunkMs / 1000.0}, {"mimeType", kMimeType}});

    bool everPlayed = false;
    int failures = 0;
    while (alive()) {
        send({{"event", "RADIO_STREAM_STATE"}, {"state", everPlayed ? "reconnecting" : "connecting"}});
        bool played = false;
        const bool ok = listen(played);
        if (!alive()) break;
        everPlayed = everPlayed || played;
        if (played) {
            failures = 0;
        } else {
            ++failures;
            Logger::instance().warn("RadioStreamer", "Stream '{}': {} (try {})", req.requestId, ok ? "no sound from the station" : error, failures);
            if (!everPlayed && failures >= kOpenAttempts) {
                send({{"event", "RADIO_STREAM_ERROR"}, {"error", ok ? "The station sends no sound" : error}});
                break;
            }
        }
        sleepFor(std::min(kMaxBackoffSec, 1 << std::min(failures, 4)));
    }
    if (idle()) {
        Logger::instance().info("RadioStreamer", "Stream '{}': no word from the page for {} s, stopping", req.requestId,
                                std::chrono::duration_cast<std::chrono::seconds>(kIdleTimeout).count());
    }
    if (out) {
        avformat_free_context(out);
        out = nullptr;
    }
    clearFolder();
}

// ─── RadioStreamer ───────────────────────────────────────────────────────────

RadioStreamer::RadioStreamer() = default;

RadioStreamer::~RadioStreamer() {
    std::vector<std::shared_ptr<Stream>> all;
    {
        std::lock_guard lock(mutex_);
        for (auto& [id, st] : streams_) all.push_back(st);
        streams_.clear();
    }
    for (auto& st : all) {
        st->stop = true;
        st->cv.notify_all();
        if (st->thread.joinable()) st->thread.join();
    }
}

void RadioStreamer::open(const RadioStreamOpenRequest& request, Sink sink) {
    auto st = std::make_shared<Stream>();
    st->req  = request;
    st->sink = std::move(sink);
    st->dir  = std::filesystem::path(request.targetDir);

    if (request.url.rfind("http://", 0) != 0 && request.url.rfind("https://", 0) != 0) {
        Logger::instance().warn("RadioStreamer", "Not a station address: '{}'", request.url);
        st->send({{"event", "RADIO_STREAM_INFO"}, {"status", "error"}, {"error", "The station's address must start with http:// or https://"}});
        return;
    }

    // One writer per folder (and per id): an older stream of the same station stops first
    std::error_code ec;
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

    Logger::instance().info("RadioStreamer", "Opening stream '{}': {} -> '{}' (buffer {} KB)", request.requestId,
                            request.url, request.targetDir, std::max(kMinBufferBytes, request.maxBufferBytes) / 1024);
    st->thread = std::thread([st]() { st->run(); });
    std::lock_guard lock(mutex_);
    streams_[request.requestId] = st;
}

void RadioStreamer::keepAlive(const std::string& requestId) {
    std::lock_guard lock(mutex_);
    auto it = streams_.find(requestId);
    if (it != streams_.end()) it->second->lastHeardMs = nowMs();
}

void RadioStreamer::close(const std::string& requestId) {
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
    if (st->thread.joinable()) st->thread.join();
    Logger::instance().info("RadioStreamer", "Closed stream '{}'", requestId);
}

} // namespace ifs
