---

name: cpp-architecture
description: Apply modern C++20 engineering practices and c6g3-infotainment-services-specific C++ architecture rules when writing, reviewing, refactoring, or designing C++ code. Use for ownership, lifetime, interfaces, concurrency, error handling, threading, memory management, source/header organization, CMake integration, and general C++20 implementation decisions.
----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

# c6g3-infotainment-services C++20 Architecture Skill

## 1. Purpose

This skill defines how C++ code should be designed and implemented in c6g3-infotainment-services.

It complements `project-overview/SKILL.md`.

The project overview answers:

> What is c6g3-infotainment-services and where does functionality belong?

This skill answers:

> How should that functionality be implemented in modern C++20?

The goal is code that is:

* maintainable
* predictable
* testable
* thread-safe
* resource-safe
* performant
* easy to extend
* explicit about ownership and lifetime

Prefer simple, modern C++ over clever or unnecessarily abstract C++.

---

# 2. Language Standard

c6g3-infotainment-services targets **C++20** unless the project explicitly changes its language standard.

Use C++20 facilities where they provide a meaningful improvement.

Relevant facilities include:

* `std::span`
* `std::string_view`
* `std::filesystem`
* concepts
* ranges
* structured bindings
* `std::jthread`
* `std::stop_token`
* `std::source_location`
* `std::chrono`
* `std::atomic`
* `std::scoped_lock`
* coroutines where justified

Do not use a newer language feature merely because a compiler happens to support it if the project is configured for C++20.

Do not emulate modern facilities with unnecessary custom implementations when the standard library already provides a suitable solution.

When uncertain about exact language or library semantics, consult authoritative C++20 documentation rather than guessing.

---

# 3. General Philosophy

Prefer:

```text
simple
explicit
local
RAII-managed
testable
composable
```

Avoid:

```text
global state
hidden ownership
deep inheritance
large manager classes
implicit thread behavior
manual lifetime management
unnecessary abstractions
premature optimization
```

A good c6g3-infotainment-services component should have a clear responsibility and a clear owner.

---

# 4. Source/Header Organization

Use `.hpp` for public/project-facing declarations and `.cpp` for implementations.

Preferred:

```text
Component.hpp
Component.cpp
```

Keep implementation details private where possible.

Do not put substantial implementation in headers unless there is a reason such as:

* templates
* concepts
* small inline functions
* compile-time utilities
* explicitly required header-only functionality

Avoid including unnecessary headers.

Prefer forward declarations where appropriate to reduce compile-time coupling.

---

# 5. Include Discipline

Each source file should include the header it implements first where practical:

```cpp
#include "VideoDecoder.hpp"

#include <memory>
#include <string>
```

This helps detect missing declarations and dependency mistakes.

Use angle brackets for external/standard dependencies:

```cpp
#include <filesystem>
#include <memory>
#include <string>
```

Use quotes for project headers:

```cpp
#include "VideoFrame.hpp"
#include "Logger.hpp"
```

Avoid umbrella headers when a narrower include exists.

---

# 6. Namespaces

Place c6g3-infotainment-services code inside a project namespace.

Preferred:

```cpp
namespace c6g3-infotainment-services
{
    class VideoDecoder
    {
    };
}
```

Use nested namespaces when useful:

```cpp
namespace c6g3-infotainment-services::video
{
}
```

Avoid global-scope project classes and symbols.

Do not use `using namespace ...;` in headers.

Prefer explicit namespaces in implementation code when ambiguity could matter.

---

# 7. Naming

Prefer descriptive names.

Classes:

```text
VideoDecoder
RadioManager
FrameBuffer
ResourceManager
WebSocketSession
```

Functions:

```text
start()
stop()
open()
close()
decode()
pushFrame()
discardBefore()
```

Variables:

```text
frameCount
bufferSize
sessionId
sourcePath
```

Avoid cryptic abbreviations.

Use project-wide naming consistently once established.

---

# 8. RAII

RAII is a fundamental requirement.

Resources should acquire and release through object lifetime.

Examples:

* files
* sockets
* threads
* mutexes
* FFmpeg contexts
* decoder handles
* temporary resources
* Windows handles

Prefer:

```cpp
class FileHandle
{
public:
    FileHandle();
    ~FileHandle();

private:
    HANDLE handle_{nullptr};
};
```

rather than manually requiring callers to remember cleanup functions.

The invariant should be:

> If an object owns a resource, destroying the object releases the resource.

---

# 9. Ownership

Ownership must be obvious from the type and architecture.

Preferred ownership patterns:

### Sole ownership

```cpp
std::unique_ptr<T>
```

Use when one component owns an object.

### Shared ownership

```cpp
std::shared_ptr<T>
```

Use only when genuinely necessary.

Shared ownership should not be the default way to avoid thinking about lifetime.

### Non-owning reference

```cpp
T&
T*
std::reference_wrapper<T>
std::span<T>
```

Use when the object is owned elsewhere and the lifetime is guaranteed.

Document non-obvious lifetime assumptions.

---

# 10. Smart Pointer Rules

Prefer:

```cpp
auto decoder = std::make_unique<VideoDecoder>();
```

over:

```cpp
VideoDecoder* decoder = new VideoDecoder();
```

Avoid owning raw pointers.

A raw pointer should generally mean:

> nullable, non-owning reference

Do not use raw pointers as an ambiguous ownership mechanism.

---

# 11. `shared_ptr` Discipline

Do not automatically convert everything to `std::shared_ptr`.

Before using shared ownership, determine:

1. Who owns the object?
2. How many owners are actually required?
3. Why can ownership not be unique?
4. Can a stable lifetime be achieved another way?

Use weak references when observers must not extend lifetime:

```cpp
std::weak_ptr<Session>
```

Be especially careful with asynchronous callbacks.

A callback capturing `shared_ptr<this>` can accidentally keep a session alive forever.

---

# 12. Object Lifetime and Asynchronous Operations

c6g3-infotainment-services contains long-running asynchronous systems.

Every asynchronous operation must answer:

```text
Who owns this operation?
Who owns the callback state?
What happens if the object is destroyed?
How is the operation cancelled?
Can the callback execute after shutdown?
```

Never assume asynchronous operations finish before object destruction.

Prefer explicit cancellation and lifetime management.

---

# 13. Threading

Threading should be introduced only where it solves a real problem.

Typical asynchronous components include:

```text
HTTP server
WebSocket sessions
radio acquisition
audio decoding
video decoding
buffer management
file operations
```

Avoid creating a thread per small task.

Prefer managed workers, `std::jthread`, executors, or the concurrency primitives provided by the chosen networking architecture.

---

# 14. `std::jthread`

Prefer `std::jthread` over raw `std::thread` when appropriate.

Example:

```cpp
class Worker
{
public:
    void start();

private:
    void run(std::stop_token stopToken);

    std::jthread thread_;
};
```

This provides a natural ownership model and cooperative cancellation.

Do not rely on forcibly terminating threads.

---

# 15. `std::stop_token`

Long-running tasks should support cooperative cancellation when appropriate.

Example:

```cpp
void decodeLoop(std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        // process work
    }
}
```

Cancellation must propagate to lower-level operations when practical.

A session being stopped should eventually stop:

```text
network acquisition
    ↓
decoder
    ↓
buffer production
```

Do not leave worker threads running after their owning session has stopped.

---

# 16. Thread Safety

Do not assume a class is thread-safe.

Document concurrency expectations where they are not obvious.

For state shared between threads, choose an appropriate mechanism:

```text
std::mutex
std::shared_mutex
std::atomic
std::condition_variable
concurrent queue
single-owner event loop
```

Do not immediately add mutexes everywhere.

Often a better design is to give one thread ownership of mutable state and communicate through messages.

---

# 17. Mutex Rules

Prefer scoped locking:

```cpp
std::scoped_lock lock(mutex_);
```

or:

```cpp
std::unique_lock lock(mutex_);
```

Never manually call:

```cpp
mutex.lock();
...
mutex.unlock();
```

unless there is a very specific reason.

Keep lock scopes small.

Do not perform slow operations while holding a mutex:

```text
network I/O
disk I/O
decoder calls
blocking waits
callbacks into unknown systems
```

This is particularly important for media buffers and sessions.

---

# 18. Deadlock Prevention

When multiple locks are necessary:

* establish a consistent lock ordering, or
* use `std::scoped_lock` for atomic acquisition of multiple mutexes.

Avoid callbacks while holding internal locks.

Avoid calling public methods on other components while holding a lock unless the locking contract explicitly allows it.

---

# 19. Atomics

Use `std::atomic` for small independent state where appropriate:

```cpp
std::atomic_bool running_{false};
```

Do not use atomics as a substitute for understanding shared state.

If multiple related values must remain consistent, a mutex or single-thread ownership model may be more appropriate.

---

# 20. Data Races

Every shared mutable object must have a clear synchronization strategy.

When reviewing asynchronous code, explicitly check:

```text
Who writes this?
Who reads this?
On which thread?
What establishes synchronization?
What happens during shutdown?
```

Never assume that "it usually runs on the same thread."

---

# 21. Containers and Views

Use standard containers wherever practical:

```cpp
std::vector
std::deque
std::array
std::unordered_map
std::map
std::queue
```

Use `std::span` for non-owning contiguous ranges:

```cpp
void process(std::span<const std::byte> data);
```

Use `std::string_view` for non-owning string references when lifetime is guaranteed.

Do not return views to temporary data.

---

# 22. `std::move`

Use move semantics where ownership/value transfer is intended.

Do not sprinkle `std::move` everywhere.

Understand that moving from an object leaves it valid but generally unspecified.

Do not use a moved-from object unless its valid post-move state is explicitly known.

Prefer returning values directly:

```cpp
VideoFrame decode(...)
{
    return frame;
}
```

Modern C++ generally makes this efficient through return value optimization and move semantics.

---

# 23. Copying Media Data

Media data can be large.

Avoid unnecessary copies of:

* decoded video frames
* audio buffers
* network payloads
* packet buffers

Before introducing a copy, ask:

```text
Can ownership move?
Can std::span be used?
Can a shared immutable buffer be used?
Can the consumer process the producer's buffer directly?
Is a copy actually cheaper than the required synchronization?
```

Do not pursue zero-copy architecture prematurely.

Measure first.

---

# 24. Media Frame Ownership

A `VideoFrame` should make ownership explicit.

Conceptually:

```cpp
struct VideoFrame
{
    Timestamp timestamp;
    int width;
    int height;
    PixelFormat format;

    // Explicitly owned frame data or managed reference.
};
```

If FFmpeg owns the underlying memory, do not pretend the frame owns it.

If c6g3-infotainment-services owns it, encode that ownership in the design.

Frame lifetime must remain valid until all consumers are finished.

---

# 25. Buffer Design

Buffers should have explicit limits.

A buffer should define behavior for:

```text
full
empty
overflow
underrun
shutdown
consumer disconnect
```

Avoid unbounded buffering.

A bug that continuously pushes frames into an unbounded queue can turn into a memory-exhaustion failure.

---

# 26. Time Handling

Use `std::chrono` instead of raw integer time values where practical.

Prefer:

```cpp
std::chrono::milliseconds
std::chrono::seconds
std::chrono::steady_clock
```

Use `steady_clock` for elapsed durations and timeout logic.

Use system/calendar clocks only when wall-clock time is actually required.

Avoid mixing units such as milliseconds and seconds without explicit conversions.

---

# 27. Error Handling

Errors must be deliberate.

Use the project's chosen error model consistently.

Potential approaches include:

```text
std::expected
status/result types
exceptions at well-defined boundaries
error codes where interoperability requires them
```

Do not mix five unrelated error-handling styles throughout the project.

Normal expected conditions should not necessarily be represented as exceptions.

Examples:

```text
stream temporarily unavailable
buffer underrun
file not found
session already stopped
```

These may be ordinary state transitions depending on the subsystem.

---

# 28. Exceptions

If exceptions are used:

* establish clear ownership/lifetime guarantees
* never allow exceptions to escape unmanaged background threads
* translate exceptions at API boundaries when necessary
* preserve useful error information
* do not use exceptions for routine loop control

Thread entry points should catch unexpected exceptions before termination can escape the thread.

---

# 29. Assertions

Use assertions for programmer invariants.

Example:

```cpp
assert(frameBuffer_ != nullptr);
```

Do not use assertions for user-controlled or runtime conditions such as:

```text
file does not exist
network unavailable
invalid API request
unsupported codec
```

Those must be handled normally.

---

# 30. `const` Correctness

Prefer `const` when state should not change.

Examples:

```cpp
const Config& config
const std::string& name
const VideoFrame& frame
```

Member functions that do not modify logical object state should be `const`.

Do not remove `const` merely to make code compile without understanding why.

---

# 31. Return Values

Prefer meaningful return types.

Avoid APIs where callers must inspect unrelated member state to discover success.

Prefer:

```cpp
Result<SessionId> createSession(...);
```

over:

```cpp
bool createSession(...);
SessionId lastSessionId();
```

where practical.

Do not expose internal mutable state unnecessarily.

---

# 32. API Objects vs Internal Objects

External request data should not automatically become internal mutable state.

Prefer translating:

```text
HTTP JSON
   ↓
validated request type
   ↓
internal operation
```

This prevents transport-specific types from leaking into the core architecture.

---

# 33. Dependency Direction

Prefer dependencies flowing downward:

```text
Application
    ↓
API / Managers
    ↓
Domain / Media abstractions
    ↓
Platform / Infrastructure
    ↓
External libraries
```

Low-level modules should not know about high-level API routes.

For example:

```text
VideoDecoder
```

should not depend on:

```text
HttpServer
```

and:

```text
HttpRoute
```

should not implement FFmpeg decoding.

---

# 34. Interfaces

Use interfaces where multiple implementations are reasonably expected.

Good examples:

```cpp
class MediaSource;
class VideoDecoder;
class AudioDecoder;
class OutputAdapter;
class ResourceProvider;
```

An interface should represent a meaningful behavioral contract.

Do not create:

```cpp
IApplication
IManager
IHelper
IThing
```

without a concrete reason.

---

# 35. Virtual Destructors

If a class is intended for polymorphic deletion through a base pointer, provide a virtual destructor.

Prefer:

```cpp
class VideoDecoder
{
public:
    virtual ~VideoDecoder() = default;
};
```

Use a virtual destructor only where polymorphism actually exists.

---

# 36. Prefer Composition Over Inheritance

Prefer:

```text
VideoSession
 ├── MediaSource
 ├── VideoDecoder
 └── FrameBuffer
```

over creating a deep inheritance tree.

Use inheritance primarily for genuine substitution relationships.

---

# 37. Avoid God Objects

Do not allow classes to grow into:

```text
c6g3-infotainment-servicesManager
    ├── networking
    ├── decoding
    ├── filesystem
    ├── configuration
    ├── logging
    └── session management
```

Split responsibilities before a class becomes difficult to understand or test.

---

# 38. Dependency Injection

Components should receive their important dependencies rather than constructing everything internally.

Prefer:

```cpp
VideoManager(
    std::shared_ptr<VideoDecoderFactory> decoderFactory,
    std::shared_ptr<Logger> logger
);
```

over:

```cpp
VideoManager()
{
    decoderFactory_ = std::make_shared<...>();
}
```

Do not overuse dependency injection.

Simple value types can be constructed directly.

---

# 39. Configuration Injection

Runtime configuration should be passed explicitly.

Prefer:

```cpp
VideoManager(VideoConfig config);
```

rather than having every component read `config.json` independently.

Only the configuration system should know how configuration is loaded.

---

# 40. Logging Injection

Components should use the project's logging abstraction.

Avoid random:

```cpp
std::cout
printf
OutputDebugString
```

calls scattered throughout production code.

Platform-specific diagnostic mechanisms should be wrapped where necessary.

---

# 41. Asynchronous Callbacks

Be careful about callbacks capturing object ownership.

Potential danger:

```cpp
callback = [self = shared_from_this()] {
    ...
};
```

If the callback is owned by the same object or event loop, this can create a reference cycle.

Use weak references when the callback must not extend lifetime:

```cpp
callback = [weakSelf = weak_from_this()] {
    if (auto self = weakSelf.lock())
    {
        ...
    }
};
```

Only use this pattern when the object is actually managed appropriately.

---

# 42. Boost Integration

Boost is infrastructure, not architecture.

When using Boost.Asio or Boost.Beast:

```text
Application
   ↓
Server
   ↓
Session
   ↓
Asio / Beast
```

Do not spread raw Boost networking operations through unrelated media modules.

Keep networking responsibilities in the server/networking layer.

---

# 43. Asio Design

Prefer clear ownership of:

* `io_context`
* sockets
* acceptors
* timers
* strands/executors
* sessions

The application should control the lifetime of the primary `io_context`.

Do not create unrelated `io_context` instances without a clear architectural reason.

---

# 44. Beast HTTP

HTTP session logic should be isolated from API business logic.

Conceptually:

```text
TCP connection
      ↓
Beast HTTP session
      ↓
HTTP request
      ↓
Router
      ↓
Handler
      ↓
Manager
```

The Beast layer should not know what a radio stream actually does.

---

# 45. Beast WebSocket

WebSocket sessions are long-lived.

Treat them as session objects with explicit:

```text
connect
read
write
close
shutdown
```

Do not allow unbounded outgoing message queues.

Consider backpressure and client disconnects.

---

# 46. Async Error Handling

Every asynchronous operation needs an error path.

Do not write:

```cpp
async_read(..., [](error_code ec, ...) {
    // ignore ec
});
```

without a deliberate reason.

Handle:

```text
success
operation_aborted
connection_closed
timeout
protocol_error
unexpected_error
```

appropriately for the component.

---

# 47. Cancellation

Network operations and media sessions must have a way to stop.

Stopping a session should not simply set:

```cpp
running_ = false;
```

and hope everything exits.

Cancellation must propagate to:

```text
socket
network read
decoder
buffer producer
worker
timers
callbacks
```

where appropriate.

---

# 48. Timers

Use asynchronous timers for network/session timeouts rather than blocking sleeps.

Avoid:

```cpp
std::this_thread::sleep_for(...)
```

inside networking event loops.

Timers should be cancelled as part of session shutdown.

---

# 49. Event Loops

Do not perform CPU-heavy decoding directly on an Asio event loop thread.

Separate:

```text
network I/O
```

from:

```text
CPU-heavy media decoding
```

unless the operation is explicitly non-blocking and sufficiently lightweight.

A likely architecture is:

```text
Asio event loop
        │
        ▼
network packets
        │
        ▼
work queue
        │
        ▼
decoder worker
```

The exact design should be chosen based on measured requirements.

---

# 50. Build System

Use CMake as the authoritative build system.

The source tree should not depend on manually configured IDE projects.

CMake should define:

* language standard
* targets
* include directories
* dependencies
* compiler warnings
* debug/release behavior
* tests
* installation/package behavior

Prefer target-based CMake.

Use:

```cmake
target_link_libraries(...)
target_include_directories(...)
target_compile_features(...)
```

rather than global compiler/linker configuration where possible.

---

# 51. CMake Target Design & DLL Packaging

Prefer one logical target per major component when useful.

Possible future structure:

```text
c6g3-infotainment-services_core
c6g3-infotainment-services_media
c6g3-infotainment-services_server
c6g3-infotainment-services_platform
c6g3-infotainment-services
c6g3-infotainment-servicesTests
```

Do not split targets prematurely. A small project may initially use fewer targets. The goal is dependency clarity, not target-count maximization.

### Dynamic Linking & DLL Distribution Rule

For third-party multimedia and codec libraries (such as FFmpeg):

* Use **dynamic linking** (`.dll` on Windows) rather than monolithic static linking.
* **Licensing Compliance:** Dynamically linking to FFmpeg shared libraries (`avformat-*.dll`, `avcodec-*.dll`, `avutil-*.dll`) maintains clean LGPL compliance without requiring complex relinkable object distribution.
* **Portable Distribution:** The final release package (`dist/`) should bundle the companion DLLs directly alongside `c6g3-infotainment-services.exe` so the application runs out-of-the-box without requiring users to install external packages or configure system `PATH`.

---

# 52. Compiler Warnings

Enable useful warnings.

Warnings should be treated as valuable feedback rather than noise.

Do not blindly enable every warning if doing so creates an unusable development environment, but maintain a high warning standard.

New code should not knowingly introduce warnings.

---

# 53. Debug vs Release

Both configurations must work.

Debug builds should optimize for:

```text
debuggability
assertions
diagnostics
```

Release builds should optimize for:

```text
performance
deployment
```

Do not rely on undefined behavior that only happens to work in one configuration.

---

# 54. Testing

New functionality should be designed so it can be tested without launching BeamNG.

Test:

```text
parsers
buffers
state machines
configuration
resource resolution
session behavior
error handling
```

Then integration-test:

```text
network
FFmpeg
filesystem
Windows APIs
```

Finally test end-to-end with BeamNG.

---

# 55. Testability

Avoid hiding dependencies behind globals because it makes tests difficult.

Prefer injectable dependencies and deterministic components.

A buffer test should not require:

```text
BeamNG
HTTP
network
FFmpeg
```

unless it is specifically an integration test.

---

# 56. Performance Rules

Do not optimize based on intuition alone.

Before optimizing:

1. Identify the suspected bottleneck.
2. Measure it.
3. Change one significant variable.
4. Measure again.
5. Keep the optimization only if it improves the actual target.

Potential media bottlenecks include:

```text
decode CPU
memory copies
color conversion
buffering
synchronization
network throughput
frame upload
```

---

# 57. Allocation Discipline

Frequent allocations can matter in high-rate media paths.

Where profiling shows a problem, consider:

* buffer reuse
* reserved capacity
* object pools
* ring buffers
* reusable frame storage

Do not introduce custom allocators before a measurable problem exists.

---

# 58. Strings

Use:

```cpp
std::string
std::string_view
```

appropriately.

Do not use `std::string_view` when the referenced data may disappear.

Be especially cautious when passing views across asynchronous operations.

Never assume a `string_view` extends the lifetime of its underlying storage.

---

# 59. Filesystem

Use:

```cpp
std::filesystem
```

for ordinary portable filesystem manipulation.

Use Windows APIs only when Windows-specific functionality is actually required.

Keep Windows-specific operations isolated.

---

# 60. Windows Handles

Windows handles should have explicit ownership.

Prefer RAII wrappers over raw `HANDLE` lifetime management.

Never allow leaked handles to accumulate in long-running service processes.

---

# 61. External Resources

Every external resource should have a clear ownership/lifetime boundary.

Examples:

```text
socket
file
FFmpeg context
decoder
thread
timer
buffer
Windows handle
```

Ask:

> Who destroys this?

If the answer isn't obvious from the code, the design should be improved.

---

# 62. Comments

Prefer comments that explain **why**, not what.

Bad:

```cpp
// Increment i
++i;
```

Good:

```cpp
// Keep one extra frame available so the consumer can continue
// while the decoder refills the buffer.
```

Do not use comments to justify poor architecture.

---

# 63. Documentation of Concurrency

When a class has important threading assumptions, document them.

Example:

```text
Threading:
- start() may be called from the control thread.
- decodeLoop() owns decoder state.
- pushFrame() is called only by the decoder worker.
- consume() may be called by the output thread.
```

This is especially important for buffers and sessions.

---

# 64. API Threading Boundaries

Never assume HTTP handlers execute on the same thread as media sessions.

The API should communicate with long-running components through defined thread-safe mechanisms.

Potential approaches:

```text
mutex-protected state
message queue
executor
strand
single-owner event loop
future/promise
```

Choose based on the component.

---

# 65. Shutdown Design

Shutdown is a first-class feature.

The expected sequence is conceptually:

```text
stop accepting new clients
        ↓
stop new sessions
        ↓
cancel network operations
        ↓
stop media acquisition
        ↓
stop decoders
        ↓
flush/discard buffers as appropriate
        ↓
join workers
        ↓
release resources
        ↓
exit
```

Do not rely on process termination to clean up everything.

---

# 66. State Machines

Long-running components should use explicit states rather than many loosely related booleans.

Prefer:

```cpp
enum class SessionState
{
    Created,
    Starting,
    Running,
    Paused,
    Stopping,
    Stopped,
    Failed
};
```

over:

```cpp
bool started;
bool stopped;
bool paused;
bool failed;
```

Multiple booleans can create impossible combinations.

---

# 67. Thread-safe State Machines

If session state is accessed from multiple threads, define synchronization explicitly.

Do not assume an enum is automatically safe merely because it is a single integer.

---

# 68. Resource IDs and Session IDs

IDs should be opaque to callers.

Prefer a project-defined type or clear string/integer representation rather than exposing memory addresses or internal object identifiers.

Example:

```text
session-42
resource-abc123
```

The ID should not encode unnecessary implementation details.

---

# 69. Logging Sensitive Data

Do not log:

* passwords
* authentication tokens
* private credentials
* unnecessary personal filesystem information
* secrets embedded in URLs

Sanitize external input before logging if necessary.

---

# 70. Security Review

Even though the service is local, review potentially dangerous operations carefully.

Particularly:

```text
arbitrary filesystem reads
arbitrary writes
shortcut resolution
process execution
mount operations
URL fetching
path handling
WebSocket commands
```

Never allow a user-controlled path to escape an intended sandbox/root without deliberate authorization.

---

# 71. URL Handling

Treat URLs and paths as untrusted input at API boundaries.

Validate:

```text
scheme
host
path
length
allowed protocols
```

Do not blindly fetch arbitrary URLs unless that behavior is intentionally part of the API.

---

# 72. Future Hardware Acceleration

Hardware decoding may eventually be useful for video.

Do not make software decoding architecture incompatible with hardware decoding.

Prefer an abstraction such as:

```text id="d6lexb"
VideoDecoder
├── FFmpegSoftwareDecoder
└── FutureHardwareDecoder
```

Only implement hardware decoding after real profiling demonstrates the need.

---

# 73. Future Rendering

Decoded frames should not assume a particular rendering API.

Potential outputs:

```text id="vhhj04"
CEF
OpenGL
DirectX
shared memory
network
Android Auto
```

Keep rendering/output concerns separate from decoding.

---

# 74. Future Android Auto

Do not place Android Auto protocol logic into:

```text
video/
radio/
core/
```

A future implementation should likely have its own integration layer.

Conceptually:

```text
src/
└── integrations/
    └── android_auto/
```

The precise structure can be decided when implementation begins.

---

# 75. When to Create a New Module

Create a new module when:

* responsibility is clearly distinct
* the module has a meaningful public contract
* multiple components need the functionality
* the feature has independent lifecycle/testing needs
* separation reduces coupling

Do not create a module simply because there are many lines of code.

---

# 76. When NOT to Abstract

Do not create:

```text
IFooFactory
IBarProvider
IBazManager
```

unless there is a real reason for substitution, testing, or dependency isolation.

Three nearly identical implementations are not always proof that an abstraction is needed.

Prefer simple concrete code until a meaningful abstraction boundary appears.

---

# 77. Refactoring Rule

When modifying existing code:

* preserve behavior unless behavior is intentionally changing
* make the smallest coherent change
* avoid unrelated cleanup
* avoid renaming huge areas without need
* add tests before risky refactors
* verify build and tests after structural changes

Do not turn a small feature request into an unrelated rewrite.

---

# 78. Review Checklist

Before declaring a significant C++ change complete, check:

### Ownership

```text
Who owns every long-lived object?
Are raw pointers non-owning?
Can anything outlive its dependencies?
```

### Concurrency

```text
Which thread touches each mutable object?
Can callbacks execute after destruction?
Can shutdown race with normal operation?
```

### Errors

```text
Are failures observable?
Are expected failures handled?
Can background threads terminate cleanly?
```

### Performance

```text
Are large buffers copied unnecessarily?
Can queues grow without limit?
Is blocking work occurring on event-loop threads?
```

### Architecture

```text
Does the code belong in the correct module?
Is BeamNG unnecessarily coupled?
Are transport and core logic separated?
```

### Testing

```text
Can the important logic be tested without BeamNG?
Are failure paths tested?
```

### Maintainability

```text
Is the class doing one coherent job?
Are names clear?
Are lifetime assumptions understandable?
```

---

# 79. Preferred Development Order

When introducing a new subsystem:

```text
1. Define the responsibility.
2. Define the data types.
3. Define ownership/lifetime.
4. Define concurrency model.
5. Define interface.
6. Implement minimal concrete version.
7. Write tests.
8. Integrate with surrounding system.
9. Add API/adapter.
10. Profile and optimize only when justified.
```

Do not begin by writing the entire subsystem in one large file.

---

# 80. Final Principle

c6g3-infotainment-services should feel like a carefully engineered native service rather than a collection of scripts rewritten in C++.

Prefer:

```text
explicit ownership
clear lifetimes
small responsibilities
clean interfaces
controlled concurrency
testable components
measured performance
```

over:

```text
global state
magic callbacks
shared mutable everything
massive manager classes
blocking event loops
hidden dependencies
premature abstraction
```

The implementation should remain flexible enough that the following path is possible:

```text
BeamNG
  ↓
localhost
  ↓
c6g3-infotainment-services
  ↓
resource/media infrastructure
  ↓
future clients and integrations
```

The long-term goal is not merely to make BeamNG play media it otherwise cannot play.

The goal is to build a reliable **local media and integration bridge** whose first and most important client happens to be BeamNG.

---

# 81. Progress WebSocket Payload Integration

All long-running, streaming, or chunked data operations (filesystem caching/copying, downloading, large directory scanning, transcoding, or buffering) **MUST** implement progress reporting:

* **Progress Model:** Provide a `ProgressCallback` (`std::function<void(const OperationProgress&)>`) to async handlers.
* **WebSocket Event:** Dispatch `OPERATION_PROGRESS` events over the client's `WebSocketSession`.
* **Payload Format:**
  ```json
  {
    "event": "OPERATION_PROGRESS",
    "requestId": "<originating-request-id>",
    "action": "COPYING" | "DOWNLOADING" | "SCANNING" | "PROCESSING",
    "progress": 50.0,
    "current": 5242880,
    "total": 10485760,
    "unit": "bytes" | "items",
    "detail": "<filename-or-step-description>"
  }
  ```

---

# 82. Binary Packaging & DLL Minimization Policy

* **Static Compiler Runtimes:** All MinGW/GCC runtime libraries (`libgcc_s_seh`, `libstdc++`, `libwinpthread`) **MUST** be statically linked into the main executable using `-static-libgcc -static-libstdc++ -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic`.
* **DLL Minimization:** Standalone distribution (`dist/`) **MUST ONLY** bundle external dynamic runtime dependencies (specifically FFmpeg `avformat`, `avcodec`, `avutil`, `swscale`, `swresample`). Never ship compiler runtime DLLs.

