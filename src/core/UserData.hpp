#pragma once

#include <filesystem>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ifs {

// Where the service keeps the user's own data (the web apps' browser profiles: sign-ins, cookies, history):
// %LOCALAPPDATA%\C6G3\Infotainment Services, never the service's folder, which gets zipped and passed around.
// Falls back to the current folder only when Windows gives no LocalAppData.
inline std::filesystem::path userDataDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) return std::filesystem::path(std::wstring(buf, n)) / L"C6G3" / L"Infotainment Services";
#endif
    return std::filesystem::current_path() / "userdata";
}

} // namespace ifs
