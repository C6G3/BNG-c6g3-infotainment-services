#include "platform/windows/ProcessPriority.hpp"
#include "core/Logger.hpp"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace ifs {

bool ProcessPriority::apply(const std::string& level) {
    auto& log = Logger::instance();
#ifdef _WIN32
    DWORD priorityClass = 0;
    if (level == "normal") {
        priorityClass = NORMAL_PRIORITY_CLASS;
    } else if (level == "below_normal") {
        priorityClass = BELOW_NORMAL_PRIORITY_CLASS;
    } else if (level == "idle") {
        priorityClass = IDLE_PRIORITY_CLASS;
    } else {
        log.warn("App", "Unknown process.priority '{}' (use normal, below_normal or idle); keeping the current priority",
                 level);
        return false;
    }
    if (!SetPriorityClass(GetCurrentProcess(), priorityClass)) {
        log.warn("App", "Could not set the process priority to {} (Windows error {})", level, GetLastError());
        return false;
    }
    log.info("App", "Process priority: {}", level);
    return true;
#else
    log.warn("App", "process.priority '{}' is only supported on Windows; ignored", level);
    return false;
#endif
}

} // namespace ifs
