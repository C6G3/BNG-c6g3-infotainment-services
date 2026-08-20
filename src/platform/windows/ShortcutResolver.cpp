#include "platform/windows/ShortcutResolver.hpp"
#include "core/Logger.hpp"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shobjidl.h>
#  include <shlguid.h>
#endif

namespace ifs {

bool ShortcutResolver::isShortcut(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(c));
    return ext == ".lnk";
}

std::optional<std::filesystem::path> ShortcutResolver::resolve(const std::filesystem::path& shortcutPath) {
#ifdef _WIN32
    if (!std::filesystem::exists(shortcutPath)) {
        return std::nullopt;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool coInitialized = SUCCEEDED(hr);

    struct ComScope {
        bool initialized;
        ~ComScope() {
            if (initialized) CoUninitialize();
        }
    } comScope{coInitialized};

    IShellLinkW* shellLink = nullptr;
    hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IShellLinkW, reinterpret_cast<void**>(&shellLink));
    if (FAILED(hr) || !shellLink) {
        return std::nullopt;
    }

    IPersistFile* persistFile = nullptr;
    hr = shellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persistFile));
    if (FAILED(hr) || !persistFile) {
        shellLink->Release();
        return std::nullopt;
    }

    std::wstring wpath = shortcutPath.wstring();
    hr = persistFile->Load(wpath.c_str(), STGM_READ);
    if (FAILED(hr)) {
        persistFile->Release();
        shellLink->Release();
        return std::nullopt;
    }

    hr = shellLink->Resolve(nullptr, SLR_NO_UI | SLR_NOUPDATE);

    WCHAR target[MAX_PATH] = {0};
    WIN32_FIND_DATAW wfd;
    hr = shellLink->GetPath(target, MAX_PATH, &wfd, SLGP_RAWPATH);

    persistFile->Release();
    shellLink->Release();

    if (SUCCEEDED(hr) && target[0] != L'\0') {
        std::filesystem::path resolvedPath(target);
        if (std::filesystem::exists(resolvedPath)) {
            return resolvedPath;
        }
    }
#endif
    return std::nullopt;
}

} // namespace ifs
