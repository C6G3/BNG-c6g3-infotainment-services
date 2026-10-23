#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ifs {

/// Parameters for RADIO_STREAM_OPEN.
struct RadioStreamOpenRequest {
    std::string requestId;             // the stream's id; every later message and event carries it
    std::string url;                   // the station's stream (http/https: Icecast / Shoutcast, HLS .m3u8, ...)
    std::string targetDir;             // OS folder of this station's buffer (created when missing, emptied on open)
    int64_t maxBufferBytes = 4 << 20;  // pieces on disk are dropped oldest first above this
    int64_t bitRate        = 96000;    // Opus bit rate of the pieces
};

/// Live radio for the Radio app. The vehicle screens and the game UI cannot reach the internet, so the service
/// listens to the station and writes it to the station's buffer folder as one continuous Opus / WebM stream cut
/// in short pieces (webm_chunk): "init.hdr" (the stream's header) and "r_00000.chk", "r_00001.chk", ...
/// (kChunkSeconds each, numbered in order). The sound system appends them to a MediaSource, header first, so
/// the station plays without a gap between pieces.
///
/// The buffer stays small: a live station only grows at its own pace, and the oldest pieces are deleted once the
/// folder holds more than maxBufferBytes (the player is a few pieces behind the newest). A dropped connection is
/// opened again (the stream's timeline goes on); the stream stops on RADIO_STREAM_CLOSE, when the page that
/// opened it disconnects, or when it hears nothing from it for kIdleTimeout.
///
/// Events (JSON, "event" + "requestId"):
///  RADIO_STREAM_INFO   status, error, header ("init.hdr"), chunkSeconds, mimeType   (the header is written)
///  RADIO_CHUNK_READY   index, file, bytes                                            (a piece is complete)
///  RADIO_STREAM_STATE  state: "connecting" | "playing" | "reconnecting"
///  RADIO_METADATA      title (the station's "now playing" text, Icecast / Shoutcast stations)
///  RADIO_STREAM_ERROR  error (the station could not be reached; the stream has stopped)
class RadioStreamer {
public:
    struct Sink {
        std::function<void(const std::string&)> send;
        std::function<bool()> isConnected;
    };

    RadioStreamer();
    ~RadioStreamer();

    RadioStreamer(const RadioStreamer&)            = delete;
    RadioStreamer& operator=(const RadioStreamer&) = delete;

    /// Starts a stream (a stream already writing to the same folder is stopped first).
    void open(const RadioStreamOpenRequest& request, Sink sink);
    /// The page is still listening (sent every few seconds): keeps the stream alive.
    void keepAlive(const std::string& requestId);
    /// Stops a stream; its folder is emptied.
    void close(const std::string& requestId);

    struct Stream;

private:
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Stream>> streams_;
};

} // namespace ifs
