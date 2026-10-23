#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ifs {

/// Parameters for VIDEO_STREAM_OPEN.
struct VideoStreamOpenRequest {
    std::string requestId;   // the stream's id; every later message and event carries it
    std::string sourcePath;  // OS path of the video
    std::string targetDir;   // OS folder of this video in the cache (created when missing)
    double startTime = 0.0;  // seconds: conversion starts at the piece holding this time
};

/// Progressive video for the vehicle screen (YouTube style): instead of converting a whole video before it
/// plays, the picture is written as short standalone WebM pieces "v_00000.webm", "v_00001.webm", ... (VP9,
/// at most 1024x576 and 30 fps, kSegmentSeconds each, every piece starts with a keyframe) that the screen
/// appends to a MediaSource as they appear. Pieces are numbered by position, so one converted by an earlier
/// run (or session) is reused. The sound is converted in parallel to "audio.ogg" for the sound system.
///
/// Conversion runs ahead of the playing position by at most kAheadSegments pieces, so a long video costs
/// CPU only while it is watched; VIDEO_STREAM_SEEK restarts it at the piece the viewer jumped to.
///
/// Events (JSON, "event" + "requestId"):
///  VIDEO_STREAM_INFO   status, error, duration, segmentSeconds, segmentCount, width, height, hasAudio,
///                      segments (pieces already on disk)
///  VIDEO_AUDIO_READY   status, error, hasAudio, file ("audio.ogg")
///  VIDEO_SEGMENT_READY index
///  VIDEO_STREAM_END    lastSegment (the video's real last piece; the container length can be off)
///  VIDEO_STREAM_ERROR  error
class VideoStreamer {
public:
    struct Sink {
        std::function<void(const std::string&)> send;
        std::function<bool()> isConnected;
    };

    VideoStreamer();
    ~VideoStreamer();

    VideoStreamer(const VideoStreamer&)            = delete;
    VideoStreamer& operator=(const VideoStreamer&) = delete;

    /// Starts a stream (a stream already writing to the same folder is stopped first).
    void open(const VideoStreamOpenRequest& request, Sink sink);
    /// The viewer jumped: conversion continues from the piece holding `time`.
    void seek(const std::string& requestId, double time);
    /// Playing position (the page sends it every few seconds): keeps the stream alive and paces conversion.
    void position(const std::string& requestId, double time);
    /// The player closed: conversion stops (pieces written so far stay in the cache).
    void close(const std::string& requestId);

    struct Stream;

private:
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Stream>> streams_;
};

} // namespace ifs
