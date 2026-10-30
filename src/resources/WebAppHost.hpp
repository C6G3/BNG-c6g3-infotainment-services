#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace ifs {

/// Web apps (YouTube first) for the infotainment: the page runs in the user's own browser, hidden, and its picture
/// and sound reach the game over WebRTC.
///
/// The user's Google Chrome (else Microsoft Edge) runs headless with its own profile next to the service
/// (webapp/profile: the login stays there, separate from the user's own browser; the disk cache is capped) and its
/// sound output off. Two tabs, driven over one DevTools connection:
///   the app tab   - the web app itself (WEBAPP_OPEN url), 1280 wide and exactly as tall as the screen's shape asks
///   the controller - a page DevTools serves itself on a localhost URL (a secure context, no HTTP server), which
///                   captures the app tab (picked by its title) and offers the WebRTC links:
///                   "screen" (the picture, to the vehicle screen), "sound" (to the game UI's sound system) and
///                   "cast:<screen>" (the picture again, one per passenger screen; signalled through the vehicle
///                   screen, which relays it over GE)
/// One web app at a time: opening another replaces it; opening the same one again (a reloaded screen) re-offers
/// the links without restarting the browser.
///
/// Screen -> service:  WEBAPP_OPEN {requestId, url, titleMatch, siteMode, height}, WEBAPP_ANSWER {requestId, to, sdp},
///                     WEBAPP_INPUT {requestId, kind: touch|text|key|wheel, phase, x, y, text, key, dy},
///                     WEBAPP_NAV {requestId, nav: back|forward|reload|home|url|playpause|nexttrack|previoustrack|
///                                 site_mode (url: mobile|desktop)|forget (url: an https origin: its data is cleared),
///                                 url},
///                     WEBAPP_CLOSE {requestId},
///                     WEBAPP_CAST {requestId, screen, on} (answered with WEBAPP_ANSWER to: "cast:<screen>")
/// Sound system -> service: WEBAPP_SOUND_ATTACH {requestId}, WEBAPP_ANSWER {requestId, to: "sound", sdp}
/// Service -> pages:   WEBAPP_STATE {state, detail, browser, title, url; "focus": editable; "video": video;
///                     "media": media {title, artist, album, art (JPEG data URL), playing}}, WEBAPP_OFFER {to, sdp},
///                     WEBAPP_ERROR {error}, WEBAPP_CLOSED
class WebAppHost {
public:
    struct Sink {
        std::function<void(const std::string&)> send;
        std::function<bool()> isConnected;
    };

    WebAppHost() = default;
    ~WebAppHost();

    WebAppHost(const WebAppHost&)            = delete;
    WebAppHost& operator=(const WebAppHost&) = delete;

    // height: the page's height for its 1280 width (the screen's own shape); 0 = 720
    // siteMode: "mobile" = the site's phone layout (Spotify only), else "desktop"
    // fill: a CSS selector whose element fills the page (Poki's game frame), "" = the whole page
    // chromeFirst: Chrome before Brave (Spotify: Widevine; Poki: Brave blocks the ad its games wait for)
    // press: a CSS selector whose button is pressed once it appears (Poki's Profile button), "" = none
    // mouse: a desktop game (GTA, Minecraft): taps go as the left mouse button, no touchscreen, the page kept focused
    // incognito: a private session (the Chrome app): its own empty profile, deleted when it closes
    void open(const std::string& requestId, const std::string& url, const std::string& titleMatch, const std::string& siteMode, int height, bool chromeFirst, const std::string& fill, const std::string& press, bool mouse, bool incognito, Sink screen);
    void attachSound(const std::string& requestId, Sink sound);
    void cast(const std::string& requestId, const std::string& screenName, bool on);
    void answer(const std::string& requestId, const std::string& to, const std::string& sdp);
    void input(const std::string& requestId, const std::string& inputJson);
    void navigate(const std::string& requestId, const std::string& action, const std::string& url);
    // The picture's codec ("vp9", "h264", "vp8", "av1") and text hint; reoffer: a running app's links start again
    void picture(const std::string& requestId, const std::string& codec, bool textHint, bool reoffer);
    void close(const std::string& requestId);
    // The app lock's reset: the web app closes (its browser too), so its profile can be deleted
    void closeAll();
    // The address the running web app was opened with ("" = none): a reopened screen may take it back unlocked
    std::string runningUrl();
    // A visible window of the web apps' browser with their profile, on the user's desktop (the web app closes first:
    // the profile is one browser's at a time). Signing in there counts for the web apps; Google refuses its sign-in
    // in a browser a program controls, as the hidden one is. "" = opened, else why not
    std::string openOnDesktop(const std::string& url, bool chromeFirst, bool* focused = nullptr);
    // That window is still open: no web app can start until it is closed (it holds the profile)
    bool desktopWindowOpen();

    struct Session;

private:
    std::shared_ptr<Session> current(const std::string& requestId);
    void stopSession(std::shared_ptr<Session> s);

    std::mutex mutex_;
    std::shared_ptr<Session> session_;
    std::string requestId_; // the screen request that owns the session now
    std::string openedUrl_; // what the session was opened with (runningUrl)
    void* desktopProcess_ = nullptr; // the desktop window's browser process (a HANDLE), while it may run
};

} // namespace ifs
