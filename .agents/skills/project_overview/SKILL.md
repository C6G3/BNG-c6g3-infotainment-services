---

name: project-overview
description: Understand and work within the c6g3-infotainment-services project architecture, scope, design principles, module boundaries, development conventions, and roadmap. Use when planning features, modifying architecture, creating new modules, reviewing code, making technology decisions, or determining where new functionality belongs.
--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

# c6g3-infotainment-services Project Overview

## 1. Project Identity

**Project name:** c6g3-infotainment-services

**Project type:** Native Windows C++ local media and integration service.

c6g3-infotainment-services is a standalone local service designed primarily to extend the capabilities of BeamNG.drive by providing functionality that is difficult or unavailable inside BeamNG's built-in CEF environment.

The service communicates with BeamNG over localhost and provides reusable media, resource, streaming, and integration capabilities.

c6g3-infotainment-services must be designed as a **general-purpose local bridge service**, not as a collection of BeamNG-specific hacks.

BeamNG is the first major client of the service, not the definition of the entire architecture.

---

# 2. Core Purpose

c6g3-infotainment-services exists to provide capabilities such as:

* Local HTTP communication with BeamNG.
* WebSocket communication for real-time events and control.
* Radio stream acquisition and decoding.
* Audio decoding and buffering.
* Video decoding for formats unsupported by BeamNG CEF.
* Streaming decoded video frames to BeamNG.
* Windows shortcut (`.lnk`) resolution.
* Local resource exposure.
* Temporary resource/cache management.
* Media buffering and streaming.
* Future integration with Android Auto.

The service should provide these capabilities through clean, reusable abstractions.

---

# 3. Architectural Philosophy

Follow these principles throughout development.

## 3.1 Separate BeamNG from the Core

Do not make the core media engine depend directly on BeamNG.

Prefer:

```text
BeamNG
   ↓
Adapter / API
   ↓
c6g3-infotainment-services Core
   ↓
Media / Resource Infrastructure
```

Avoid:

```text
BeamNG-specific logic
        ↓
decoder
        ↓
buffer
```

The same media infrastructure should eventually be usable by other clients.

---

## 3.2 Separate Transport from Functionality

HTTP and WebSocket servers are interfaces to the service.

They are not the media engine.

Prefer:

```text
HTTP/WebSocket
      ↓
API Routes
      ↓
Manager / Session
      ↓
Pipeline
      ↓
Decoder / Resource
```

Avoid putting decoding, filesystem logic, or long-running processing directly inside HTTP handlers.

---

## 3.3 Separate Sources, Processing, and Outputs

Media systems should conceptually follow:

```text
Source
  ↓
Demux / Acquisition
  ↓
Decoder
  ↓
Processing
  ↓
Buffer
  ↓
Output
```

Examples:

```text
HTTP radio stream
  ↓
AAC/MP3 acquisition
  ↓
Audio decoder
  ↓
PCM
  ↓
Audio buffer
  ↓
BeamNG
```

```text
Video file
  ↓
Container demux
  ↓
Video decoder
  ↓
Video frames
  ↓
Frame buffer
  ↓
BeamNG CEF
```

Future:

```text
Android Auto
  ↓
Projection input
  ↓
Media pipeline
  ↓
c6g3-infotainment-services output
```

---

# 4. Major Architectural Layers

The intended architecture is:

```text
┌──────────────────────────────────────────────┐
│                  Clients                     │
│                                              │
│ BeamNG │ CEF │ Lua │ Future  │
└──────────────────────┬───────────────────────┘
                       │
                 API / Protocol
                       │
┌──────────────────────▼───────────────────────┐
│                    Sessions                  │
│                                              │
│ VideoSession │ RadioSession │ Future         │
│ ProjectionSession                            │
└──────────────────────┬───────────────────────┘
                       │
                    Pipelines
                       │
┌──────────────────────▼───────────────────────┐
│                     Media                   │
│                                              │
│ Acquisition │ Demux │ Decode │ Buffer       │
│ Transform │ Output                           │
└──────────────────────┬───────────────────────┘
                       │
                     Sources
                       │
┌──────────────────────▼───────────────────────┐
│                    Resources                │
│                                              │
│ File │ Shortcut │ HTTP │ Radio │ Virtual     │
│ Resource                                      │
└──────────────────────────────────────────────┘
```

New features should be placed into the appropriate layer rather than bypassing the architecture.

---

# 5. Repository Structure

The preferred repository structure is:

```text
c6g3-infotainment-services/
│
├── .agents/
│   └── skills/
│       └── project-overview/
│           └── SKILL.md
│
├── src/
│   ├── main.cpp
│   │
│   ├── app/
│   │   ├── Application.cpp
│   │   ├── Application.hpp
│   │   ├── Config.cpp
│   │   └── Config.hpp
│   │
│   ├── server/
│   │   ├── HttpServer.cpp
│   │   ├── HttpServer.hpp
│   │   ├── WebSocketServer.cpp
│   │   └── WebSocketServer.hpp
│   │
│   ├── api/
│   │   ├── SystemRoutes.cpp
│   │   ├── RadioRoutes.cpp
│   │   ├── VideoRoutes.cpp
│   │   └── ResourceRoutes.cpp
│   │
│   ├── media/
│   │   ├── MediaSource.cpp
│   │   ├── MediaSource.hpp
│   │   ├── MediaBuffer.cpp
│   │   └── MediaBuffer.hpp
│   │
│   ├── radio/
│   │   ├── RadioManager.cpp
│   │   ├── RadioManager.hpp
│   │   ├── RadioStream.cpp
│   │   ├── RadioStream.hpp
│   │   ├── AudioDecoder.cpp
│   │   └── AudioDecoder.hpp
│   │
│   ├── video/
│   │   ├── VideoManager.cpp
│   │   ├── VideoManager.hpp
│   │   ├── VideoDecoder.cpp
│   │   ├── VideoDecoder.hpp
│   │   ├── VideoFrame.cpp
│   │   ├── VideoFrame.hpp
│   │   ├── FrameBuffer.cpp
│   │   └── FrameBuffer.hpp
│   │
│   ├── resources/
│   │   ├── Resource.cpp
│   │   ├── Resource.hpp
│   │   ├── FileResource.cpp
│   │   ├── ShortcutResource.cpp
│   │   ├── ResourceManager.cpp
│   │   └── ResourceManager.hpp
│   │
│   ├── core/
│   │   ├── Logger.cpp
│   │   ├── Logger.hpp
│   │   ├── Error.hpp
│   │   └── Types.hpp
│   │
│   └── platform/
│       └── windows/
│           ├── WindowsFilesystem.cpp
│           ├── WindowsFilesystem.hpp
│           ├── ShortcutResolver.cpp
│           └── ShortcutResolver.hpp
│
├── include/
│
├── tests/
│   ├── core/
│   ├── media/
│   ├── radio/
│   ├── video/
│   ├── resources/
│   ├── server/
│   └── integration/
│
├── config/
│
├── resources/
│
├── third_party/
│
├── build/
│
├── dist/
│
├── CMakeLists.txt
├── README.md
└── .gitignore
```

This is a target architecture, not a requirement to create every file immediately.

Create directories and modules when they become necessary.

---

# 6. Build Directory

`build/` is generated build output.

It belongs at the repository root:

```text
build/
```

It may contain:

```text
build/
├── Debug/
├── Release/
├── CMakeFiles/
├── CMakeCache.txt
└── ...
```

Never depend on manually editing files inside `build/`.

`build/` should normally be ignored by Git.

It must be safe to delete and regenerate.

---

# 7. Distribution Directory

`dist/` is for distributable builds.

Example:

```text
dist/
└── c6g3-infotainment-services/
    ├── c6g3-infotainment-services.exe
    ├── required DLLs
    ├── config/
    └── resources/
```

`build/` is for developers.

`dist/` is for packaging/release.

Do not confuse the two.

---

# 8. `main.cpp`

`main.cpp` must remain minimal.

Its responsibility should primarily be application startup.

Preferred conceptual structure:

```cpp
int main()
{
    Application app;

    if (!app.initialize())
        return 1;

    return app.run();
}
```

Do not place server implementation, media decoding, configuration parsing, or large amounts of business logic inside `main.cpp`.

---

# 9. Application Layer

`Application` coordinates major subsystems.

It may own or initialize:

* configuration
* logging
* HTTP server
* WebSocket server
* resource manager
* radio manager
* video manager
* session manager
* shutdown handling

It should coordinate systems rather than implement their internals.

---

# 10. Server Layer

The server layer provides communication mechanisms.

Primary protocols:

* HTTP
* WebSocket

Use HTTP primarily for:

* metadata
* commands
* resource discovery
* session creation
* status
* configuration endpoints

Use WebSocket primarily for:

* real-time events
* playback state
* buffer state
* asynchronous notifications
* streaming control
* future projection communication

Do not perform expensive decoding directly inside network request handlers.

---

# 11. API Design

The API should be versioned.

Preferred base path:

```text
/api/v1/
```

Potential endpoints:

```text
GET  /api/v1/status

GET  /api/v1/radio
GET  /api/v1/radio/{id}

POST /api/v1/radio/{id}/start
POST /api/v1/radio/{id}/stop

POST /api/v1/video
GET  /api/v1/video/{id}

POST /api/v1/video/{id}/play
POST /api/v1/video/{id}/pause
POST /api/v1/video/{id}/seek

POST /api/v1/resources/mount
POST /api/v1/resources/unmount
```

These are architectural examples, not fixed API contracts.

Do not add endpoints arbitrarily.

Keep routes thin and delegate functionality to managers/services.

---

# 12. Sessions

Long-lived operations should use sessions.

Examples:

```text
VideoSession
RadioSession
ProjectionSession
```

A session may contain:

```text
source
decoder
buffer
state
position
configuration
statistics
```

Example conceptual state:

```text
VideoSession
├── source
├── decoder
├── frameBuffer
├── position
├── duration
├── state
└── statistics
```

Sessions should have clear lifecycle states such as:

```text
Created
Starting
Running
Paused
Stopping
Stopped
Failed
```

Long-running resources must support explicit cleanup.

---

# 13. Resource System

Treat files, shortcuts, streams, and future sources as resources.

Potential resource types:

```text
FileResource
ShortcutResource
HttpResource
RadioResource
VirtualResource
```

The resource abstraction should answer questions such as:

* Where does the resource come from?
* Can it be opened?
* Can it be streamed?
* What metadata does it provide?
* How is it released?

Do not make BeamNG aware of how a resource is internally resolved.

---

# 14. Windows Shortcut Handling

Windows `.lnk` files should be handled by a Windows-specific resource/provider layer.

Preferred conceptual flow:

```text
.lnk
 ↓
ShortcutResolver
 ↓
Target path
 ↓
Resource
```

Do not scatter Windows Shell API calls throughout unrelated modules.

Keep Windows-specific functionality under:

```text
src/platform/windows/
```

where practical.

---

# 15. Media Architecture

Audio and video should be treated as separate but related pipelines.

## Audio

```text
Source
 ↓
Demux / stream acquisition
 ↓
AudioDecoder
 ↓
PCM
 ↓
AudioBuffer
 ↓
Output
```

## Video

```text
Source
 ↓
Demux
 ↓
VideoDecoder
 ↓
VideoFrame
 ↓
FrameBuffer
 ↓
OutputAdapter
```

The decoder should not know that BeamNG exists.

---

# 16. FFmpeg Integration

FFmpeg is the preferred candidate for general-purpose audio/video decoding unless a later technical evaluation establishes a better solution.

Keep FFmpeg behind project-level abstractions.

Do not allow FFmpeg-specific types to spread unnecessarily throughout the entire codebase.

Prefer:

```text
c6g3-infotainment-services
    ↓
VideoDecoder interface
    ↓
FFmpeg implementation
```

rather than making every subsystem directly call FFmpeg APIs.

Potential FFmpeg responsibilities include:

* container demuxing
* codec detection
* audio decoding
* video decoding
* resampling
* pixel format conversion
* stream metadata

The exact FFmpeg architecture should be designed when the multimedia implementation begins.

---

# 17. Video Buffering

The project may use rolling/ring buffering for video.

Do not hard-code a rolling-window implementation into the entire video architecture.

Instead, expose a buffer abstraction.

Conceptually:

```cpp
class FrameBuffer
{
public:
    void push(VideoFrame frame);
    VideoFrame get(Timestamp timestamp);
    void discardBefore(Timestamp timestamp);
};
```

Possible implementations may include:

```text
RingBuffer
RollingBuffer
MemoryBuffer
DiskBackedBuffer
```

The buffering strategy should remain replaceable.

---

# 18. Why Ring/Rolling Buffers Exist

The primary purpose of buffering is to avoid decoding an entire media file before playback.

For example:

```text
Video
──────────────────────────────────────────────

                 playback
                    ↓
             ┌──────────────┐
             │ buffered data│
             └──────────────┘
```

The buffer should normally contain a configurable amount of media around the playback position.

For live streams, old data can be discarded.

For files, seeking and decoding behavior may require different buffering strategies.

Do not assume one buffering strategy works equally well for every source.

---

# 19. CEF Integration

CEF is an output/client environment, not the decoder architecture.

The project exists partly because BeamNG's CEF environment may lack codecs or capabilities needed by the desired media experience.

Therefore:

```text
VideoDecoder
     ↓
VideoFrame
     ↓
FrameBuffer
     ↓
CEF Adapter
```

Do not create a decoder whose internal architecture is fundamentally tied to CEF.

This makes future outputs possible:

```text
CEF
OpenGL
DirectX
Android Auto
File
Network
```

---

# 20. Radio Architecture

Radio streams should be treated as streaming media sources.

Conceptually:

```text
Internet
   ↓
RadioStream
   ↓
Demux / decoder
   ↓
PCM
   ↓
AudioBuffer
   ↓
Output
```

The radio system should eventually support:

* stream connection
* reconnection
* buffering
* codec detection
* metadata
* playback state
* errors
* graceful shutdown

Do not assume all radio stations use the same codec or transport.

---

# 21. Concurrency

The service is expected to perform concurrent operations.

Examples:

* HTTP requests
* WebSocket connections
* network streams
* media decoding
* frame buffering
* file access
* cache management

Do not block network threads with long-running media operations.

Prefer asynchronous/concurrent architecture where appropriate.

Thread ownership and lifecycle must be explicit.

Every background task must have a defined shutdown path.

Avoid detached threads for core service operations unless there is a strong reason.

Prefer managed workers, executors, thread pools, or equivalent controlled mechanisms.

---

# 22. Memory Management

Prefer modern C++ ownership semantics:

```text
std::unique_ptr
std::shared_ptr
std::weak_ptr
std::vector
std::span
std::string_view
```

Use RAII.

Avoid manual `new`/`delete` ownership unless required by an external API.

Be especially careful with:

* decoded video frames
* audio buffers
* FFmpeg resources
* network buffers
* session lifetime
* asynchronous callbacks

Media buffers can become large, so memory limits must eventually be configurable.

---

# 23. Error Handling

Errors should be explicit and diagnosable.

Differentiate between:

* invalid user input
* unavailable resource
* network failure
* decoder failure
* unsupported codec
* buffer underrun
* internal error
* shutdown/cancellation

Do not silently swallow failures.

Avoid using exceptions as a substitute for normal control flow where a result/error model is clearer.

---

# 24. Logging

Use a centralized logging system.

Logs should provide useful operational information such as:

```text
[INFO] Video session created
[INFO] Opening source
[INFO] Decoder selected
[INFO] Buffer initialized
[WARN] Buffer underrun
[ERROR] Decoder failed
```

Do not rely exclusively on `std::cout` throughout the application.

Logging should eventually support levels such as:

```text
TRACE
DEBUG
INFO
WARN
ERROR
CRITICAL
```

---

# 25. Configuration

Runtime configuration should be externalized.

Potential configuration:

```json
{
    "server": {
        "host": "127.0.0.1",
        "port": 38123
    },
    "video": {
        "buffer_seconds": 30,
        "max_memory_mb": 512
    },
    "audio": {
        "buffer_ms": 500
    },
    "cache": {
        "directory": "cache"
    }
}
```

Do not hard-code values that users are reasonably expected to configure.

Configuration should be validated at startup.

---

# 26. Localhost Security

The initial service is intended primarily for local machine communication.

Prefer binding to:

```text
127.0.0.1
```

rather than exposing the service to the entire network by default.

Do not assume that localhost automatically makes every design safe.

Validate requests and avoid dangerous arbitrary filesystem or process operations.

If remote access is ever required, design authentication and authorization explicitly.

---

# 27. Android Auto Future Scope

Android Auto is a future integration target.

It is **not part of the initial implementation**.

The architecture should allow future projection-related sessions and adapters without making the current service dependent on Android Auto.

Potential future architecture:

```text
Android Device
      ↓
Android Auto / Projection
      ↓
Projection Adapter
      ↓
ProjectionSession
      ↓
c6g3-infotainment-services Media Infrastructure
      ↓
BeamNG
```

Do not implement Android Auto merely because it appears in this document.

Do not allow future Android Auto requirements to unnecessarily complicate the initial BeamNG implementation.

---

# 28. Future Extensibility

Potential future capabilities include:

* additional audio codecs
* additional video codecs
* live radio metadata
* local music libraries
* video libraries
* media caching
* hardware acceleration
* GPU frame transfer
* OpenGL output
* DirectX output
* Android Auto projection
* additional clients
* telemetry streaming
* device integration
* virtual files/resources
* remote/local media sources

Future functionality should be added through adapters, providers, managers, or pipelines rather than special-case logic.

---

# 29. Technology Direction

Initial target:

```text
Platform:
Windows

Language:
Modern C++

Build:
CMake

Communication:
HTTP + WebSocket

Media:
FFmpeg candidate/preferred

Testing:
GoogleTest or equivalent

Source control:
Git
```

Use modern C++ practices.

Prefer C++20 or the project's explicitly configured standard.

Do not introduce large frameworks without evaluating whether they solve a real architectural problem.

---

# 30. Dependency Philosophy

Prefer well-maintained, focused libraries.

Potential categories:

```text
HTTP/WebSocket
    Boost.Beast / Boost.Asio or equivalent

Media
    FFmpeg

Testing
    GoogleTest

Logging
    dedicated logging library or lightweight internal abstraction

Configuration
    JSON/TOML/etc. depending on project requirements
```

Do not add dependencies simply because they are convenient.

Before adding a dependency, consider:

1. Does the project actually need it?
2. Does it solve a substantial problem?
3. Is it maintained?
4. Does it complicate deployment?
5. Does it introduce licensing concerns?
6. Can the functionality reasonably be implemented internally?

---

# 31. Module Boundaries

Each module should have a clear responsibility.

Examples:

```text
server/
    Communication

api/
    API routing

radio/
    Radio-specific media behavior

video/
    Video-specific media behavior

media/
    Generic media abstractions

resources/
    Resource acquisition and management

platform/windows/
    Windows-specific implementation

core/
    Cross-cutting infrastructure

app/
    Application orchestration
```

Avoid circular dependencies.

Avoid making low-level modules depend on high-level application modules.

Prefer dependency direction such as:

```text
Application
    ↓
Managers
    ↓
Core abstractions
    ↓
Platform / external libraries
```

---

# 32. Interfaces and Abstractions

Use abstractions when they provide meaningful replaceability.

Good candidates include:

```text
AudioDecoder
VideoDecoder
MediaSource
Resource
OutputAdapter
FrameBuffer
AudioBuffer
```

Do not create interfaces for every class merely for the sake of abstraction.

The goal is to isolate things that are likely to change.

---

# 33. Testing Strategy

Important components should be testable without BeamNG.

Unit-test:

```text
FrameBuffer
AudioBuffer
ResourceManager
ShortcutResolver
Configuration
API validation
Session state
```

Integration-test:

```text
HTTP API
WebSocket API
FFmpeg pipeline
radio stream handling
video decoding
resource resolution
```

BeamNG should primarily be used for end-to-end validation.

Do not require BeamNG to run for every unit test.

---

# 34. Development Workflow

When implementing a feature:

1. Understand which architectural layer owns it.
2. Identify the smallest appropriate abstraction.
3. Implement the core functionality independently.
4. Add tests.
5. Add the API/adapter layer.
6. Integrate with BeamNG.
7. Test failure cases.
8. Profile if performance matters.
9. Update documentation when architecture changes.

Do not start by modifying the BeamNG integration unless the feature genuinely requires it.

---

# 35. Feature Development Rule

For every new feature, answer:

```text
What is the source?
What processing occurs?
Where does state live?
Where is buffering required?
What is the output?
Which module owns it?
Does it need a session?
Does it need an API endpoint?
Can it be tested without BeamNG?
```

If these questions cannot be answered, clarify the architecture before implementing.

---

# 36. Avoid Premature Implementation

Do not implement future features merely because they are mentioned in this skill.

Examples:

```text
Android Auto
GPU zero-copy
DirectX rendering
remote clients
advanced caching
hardware acceleration
```

These are architectural considerations unless explicitly moved into the active roadmap.

Do not create speculative abstractions that have no current use.

Design for extension, but implement only justified functionality.

---

# 37. Avoid Monolithic Classes

Do not create classes such as:

```text
c6g3-infotainment-servicesManager
MediaManagerEverything
ServiceManager
```

containing unrelated responsibilities.

Prefer focused components:

```text
RadioManager
VideoManager
ResourceManager
SessionManager
```

Managers should coordinate operations, not become dumping grounds for business logic.

---

# 38. Avoid Global State

Avoid global mutable objects such as:

```cpp
globalDecoder
globalServer
globalConfig
globalBuffer
```

Prefer explicit ownership through `Application`, managers, sessions, or dependency injection.

Global constants are acceptable where appropriate.

---

# 39. Lifecycle Management

Every long-running subsystem must have a clear lifecycle:

```text
initialize
start
run
stop
shutdown
```

Sessions should similarly support:

```text
create
start
pause/resume
stop
destroy
```

Shutdown must be graceful.

The service should be able to terminate without leaving decoder threads, network connections, or temporary resources running indefinitely.

---

# 40. Performance Principles

Performance matters, especially for video.

Avoid unnecessary:

* memory copies
* frame conversions
* allocations
* synchronization
* blocking operations
* thread context switches

However, do not optimize prematurely.

First establish:

```text
correctness
→
measurable bottleneck
→
optimization
```

Do not introduce complex zero-copy/GPU architectures without profiling evidence.

---

# 41. Media Latency

For real-time media, consider:

```text
network latency
decode latency
buffer latency
conversion latency
output latency
```

A larger buffer can improve stability but increase latency.

A smaller buffer reduces latency but increases the risk of underruns.

Buffer sizes should therefore be configurable where appropriate.

---

# 42. API Stability

The API is a contract.

Avoid breaking existing endpoints unnecessarily.

Use versioning:

```text
/api/v1/
```

When incompatible changes become necessary, introduce a new API version rather than silently changing behavior.

---

# 43. Documentation

Maintain:

```text
README.md
```

for human-facing project documentation.

Keep architecture-specific agent behavior in this skill.

Do not turn `SKILL.md` into a user manual.

The skill exists to tell an AI agent how to reason about and modify the project.

---

# 44. Git Rules

Do not commit generated artifacts.

Normally ignore:

```text
build/
dist/
*.dll
*.exe
*.obj
*.pdb
*.lib
*.exp
CMakeCache.txt
CMakeFiles/
```

unless a specific release or project requirement says otherwise.

Source code, CMake configuration, tests, documentation, and intentional project resources belong in Git.

---

# 45. Current Scope

The initial project scope is:

### Phase 1 — Foundation

```text
CMake
C++ application
logging
configuration
localhost HTTP server
basic API
clean shutdown
```

### Phase 2 — Resource System

```text
Resource abstraction
File resources
Windows shortcut resolution
temporary resource/cache handling
```

### Phase 3 — Radio

```text
Radio streams
stream acquisition
audio decoding
audio buffering
radio metadata where practical
```

### Phase 4 — Video

```text
Video source
demuxing
video decoding
VideoFrame abstraction
FrameBuffer
rolling/ring buffering
CEF output integration
```

### Phase 5 — Hardening

```text
testing
error handling
performance profiling
memory limits
logging improvements
release packaging
```

### Future Phase

```text
Android Auto / projection experiments
```

---

# 46. Definition of a Good Architecture

A successful implementation should allow this:

```text
Replace the HTTP server
without rewriting the decoder.

Replace FFmpeg
without rewriting BeamNG integration.

Replace BeamNG
without rewriting the media pipeline.

Replace the buffering strategy
without rewriting the video decoder.

Add Android Auto
without rewriting the core resource/media infrastructure.
```

If adding a feature requires modifying unrelated subsystems, reconsider the architecture.

---

# 47. Agent Behavior

When working on c6g3-infotainment-services:

* Read the existing architecture before adding new modules.
* Prefer extending existing abstractions over duplicating functionality.
* Keep module responsibilities clear.
* Do not introduce speculative complexity.
* Do not place large amounts of logic in `main.cpp`.
* Do not couple media decoders directly to BeamNG.
* Do not couple resource providers directly to HTTP.
* Do not block network threads with long-running media operations.
* Do not introduce global mutable state.
* Do not silently ignore errors.
* Do not create unnecessary abstractions.
* Do not implement future roadmap items without explicit instruction.
* Preserve existing API contracts unless a breaking change is intentional.
* Add tests for significant new logic.
* Keep platform-specific code isolated.
* Prefer RAII and modern C++ ownership.
* Consider memory usage when handling media.
* Consider cancellation and shutdown for every asynchronous operation.

---

# 48. When Modifying Architecture

Before making a substantial architectural change:

1. Identify the current dependency direction.
2. Explain what problem the change solves.
3. Identify affected modules.
4. Check whether an existing abstraction can be extended.
5. Avoid breaking unrelated functionality.
6. Prefer incremental changes.
7. Update this skill only when the architectural decision becomes an established project rule.

Do not modify this document for every implementation detail.

It should describe stable architectural decisions.

---

# 49. Guiding Principle

The central design principle of c6g3-infotainment-services is:

> **Build a reusable local media and integration infrastructure, with BeamNG as its first client—not a collection of BeamNG-specific workarounds.**

When uncertain where functionality belongs, favor:

```text
generic core capability
        ↓
adapter
        ↓
BeamNG
```

rather than:

```text
BeamNG-specific implementation
        ↓
everything else
```
