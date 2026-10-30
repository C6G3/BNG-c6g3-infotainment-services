#include "resources/WebAppHost.hpp"
#include "core/Logger.hpp"
#include "core/UserData.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ifs {

namespace beast     = boost::beast;
namespace http      = boost::beast::http;
namespace websocket = boost::beast::websocket;
namespace net       = boost::asio;
using tcp           = boost::asio::ip::tcp;
using json          = nlohmann::json;

namespace {

constexpr int kWidth  = 1280;
constexpr int kHeight = 720;   // the page's height unless the screen asks for its own shape (WEBAPP_OPEN height)
constexpr int kMinHeight = 480;
constexpr int kMaxHeight = 1080;
constexpr auto kStartTimeout   = std::chrono::seconds(20);
constexpr auto kReconnectWindow = std::chrono::seconds(8); // a refused DevTools port: read it again this long (run)
constexpr auto kWatchdogPeriod = std::chrono::seconds(5);
constexpr int kOrphanChecks    = 4;  // the screen gone this many checks in a row: the app closes
constexpr int kSizeAttempts    = 3;
constexpr auto kSizeSettle     = std::chrono::milliseconds(500);
constexpr auto kControllerCheck = std::chrono::milliseconds(1500);
constexpr long long kDiskCacheBytes = 256LL * 1024 * 1024;
// The controller page: DevTools answers this URL itself (Fetch), so it is a secure context with no server
const char* kControllerUrl  = "http://127.0.0.1:38999/c6g3-webapp-controller";
const char* kControllerPattern = "http://127.0.0.1:38999/*";

std::string dumpJson(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string toUtf8(const std::wstring& ws) {
#if defined(_WIN32)
    if (ws.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), n, nullptr, nullptr);
    return out;
#else
    return std::string(ws.begin(), ws.end());
#endif
}

std::wstring toWide(const std::string& s) {
#if defined(_WIN32)
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

struct Browser {
    std::wstring path;
    std::string name;    // shown on the screen: "Powered by <name>"
    std::string profile; // its folder under <user data>/webapp: Brave cannot read Chrome's sign-ins (and back)
};

#if defined(_WIN32)
std::wstring registryAppPath(const wchar_t* exe) {
    const std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + exe;
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD size = sizeof(buf);
        if (RegGetValueW(root, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS && buf[0]) return buf;
    }
    return L"";
}

struct BrowserKind {
    const wchar_t* exe;
    const wchar_t* folder; // under Program Files / LocalAppData
    const char* name;
    const char* profile;
};
const BrowserKind kBrave  = {L"brave.exe", L"\\BraveSoftware\\Brave-Browser\\Application\\brave.exe", "Brave", "profile_brave"};
const BrowserKind kChrome = {L"chrome.exe", L"\\Google\\Chrome\\Application\\chrome.exe", "Google Chrome", "profile"};
const BrowserKind kEdge   = {L"msedge.exe", L"\\Microsoft\\Edge\\Application\\msedge.exe", "Microsoft Edge", "profile"};

Browser findKind(const BrowserKind& k) {
    const std::wstring p = registryAppPath(k.exe);
    if (!p.empty() && std::filesystem::exists(p)) return {p, k.name, k.profile};
    wchar_t buf[MAX_PATH] = {};
    for (const wchar_t* var : {L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"}) {
        if (!GetEnvironmentVariableW(var, buf, MAX_PATH)) continue;
        const std::wstring guess = std::wstring(buf) + k.folder;
        if (std::filesystem::exists(guess)) return {guess, k.name, k.profile};
    }
    return {};
}

// Google Chrome, else Microsoft Edge, else Brave (all speak the DevTools protocol). With kPreferBrave the apps take
// Brave, else Chrome, else Edge, except a chromeFirst app: Spotify's player needs Widevine, which headless Brave does
// not have, and Poki's games wait for their ad, which Brave's Shields block (Fruit Ninja stayed on its loading bar)
constexpr bool kPreferBrave = false; // user, 2026-10-28: back to Chrome first (Brave's web app sound was silent in game)
Browser findBrowser(bool chromeFirst) {
    const BrowserKind* braveOrder[] = {&kBrave, &kChrome, &kEdge};
    const BrowserKind* chromeOrder[] = {&kChrome, &kEdge, &kBrave};
    for (const BrowserKind* k : kPreferBrave && !chromeFirst ? braveOrder : chromeOrder) {
        Browser b = findKind(*k);
        if (!b.path.empty()) return b;
    }
    return {};
}
#endif

// The controller page's script: captures the app tab (picked by title by the browser flag) on the first offer and
// offers one WebRTC link per receiver. It talks back through the c6g3Signal binding.
const char* kControllerScript = R"JS((() => {
  const send = (o) => c6g3Signal(JSON.stringify(o))
  let stream = null
  let capturing = null // one capture for both links (the two offers come together)
  let motionHint = false
  const pcs = {}
  window.__c6g3Hint = (motion) => {
    motionHint = !!motion
    const v = stream && stream.getVideoTracks()[0]
    if (v) v.contentHint = motionHint ? 'motion' : 'detail'
  }
  const iceDone = (pc) => new Promise(res => {
    if (pc.iceGatheringState === 'complete') return res()
    pc.addEventListener('icegatheringstatechange', () => { if (pc.iceGatheringState === 'complete') res() })
    setTimeout(res, 3000)
  })
  // A local link: start the picture at a high bitrate instead of ramping up from the internet default
  const munge = (sdp) => sdp.replace(/a=rtpmap:(\d+) (VP8|VP9|H264|AV1)\/90000\r\n/g, (line, pt) =>
    new RegExp('a=fmtp:' + pt + ' ').test(sdp) ? line : line + 'a=fmtp:' + pt + ' x-google-start-bitrate=5000\r\n')
    .replace(/a=fmtp:(\d+) (.*)\r\n/g, (line, pt, params) =>
      new RegExp('a=rtpmap:' + pt + ' (VP8|VP9|H264|AV1)/').test(sdp) && !/x-google-start-bitrate/.test(params)
        ? 'a=fmtp:' + pt + ' ' + params + ';x-google-start-bitrate=5000;x-google-min-bitrate=1500;x-google-max-bitrate=8000\r\n' : line)
  async function capture () {
    stream = await navigator.mediaDevices.getDisplayMedia({
      video: { frameRate: 30, width: { max: 1280 }, height: { max: 1080 } }, // the tab's own size (the screen's shape)
      audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false },
      selfBrowserSurface: 'exclude', surfaceSwitching: 'exclude'
    })
    const v = stream.getVideoTracks()[0]
    if (v) {
      // 'detail' (screen content): still parts are sharpened over the next frames. 'motion' never went back to
      // them, so gradients and flat areas stayed blocky; it is used only while a big video plays (__c6g3Hint)
      v.contentHint = motionHint ? 'motion' : 'detail'
      v.onended = () => { stream = null; capturing = null; send({ type: 'error', error: 'the capture of the web app ended' }) }
    }
    send({ type: 'state', state: 'captured', detail: JSON.stringify(v ? v.getSettings() : {}) + ' audio ' + stream.getAudioTracks().length })
  }
  window.__c6g3Offer = async (to) => {
    try {
      if (!stream) {
        capturing = capturing || capture().catch(e => { capturing = null; throw e })
        await capturing
      }
      if (pcs[to]) { pcs[to].close(); delete pcs[to] }
      const pc = new RTCPeerConnection({ iceServers: [] })
      pcs[to] = pc
      const picture = to === 'screen' || to.startsWith('cast:') // 'cast:<screen>': a passenger screen
      const tracks = picture ? stream.getVideoTracks() : stream.getAudioTracks()
      for (const t of tracks) pc.addTrack(t, stream)
      if (picture) {
        for (const s of pc.getSenders()) {
          const p = s.getParameters(); p.encodings = p.encodings && p.encodings.length ? p.encodings : [{}]
          p.encodings[0].maxBitrate = 8000000; p.encodings[0].maxFramerate = 30
          // Keep it sharp: 'balanced' dropped to ~330x156 while the link warmed up (the prototype found the same)
          p.degradationPreference = 'maintain-resolution'
          await s.setParameters(p).catch(() => {})
        }
      }
      pc.onconnectionstatechange = () => send({ type: 'state', state: to + '-' + pc.connectionState })
      const offer = await pc.createOffer()
      if (picture) offer.sdp = munge(offer.sdp)
      await pc.setLocalDescription(offer)
      await iceDone(pc)
      send({ type: 'offer', to, sdp: pc.localDescription.sdp })
    } catch (e) { send({ type: 'error', error: 'offer ' + to + ': ' + e }) }
  }
  window.__c6g3Answer = (to, sdp) => {
    const pc = pcs[to]
    if (!pc) return
    pc.setRemoteDescription({ type: 'answer', sdp })
      .then(() => send({ type: 'state', state: to + '-answer-applied' }))
      .catch(e => send({ type: 'error', error: 'answer ' + to + ': ' + e }))
  }
  window.__c6g3Drop = (to) => {
    if (pcs[to]) { pcs[to].close(); delete pcs[to] }
  }
  setInterval(async () => {
    const pc = pcs.screen
    if (!pc) return
    const out = {}
    ;(await pc.getStats()).forEach(s => {
      if (s.type === 'outbound-rtp' && s.kind === 'video') Object.assign(out, { fps: s.framesPerSecond, w: s.frameWidth, h: s.frameHeight, limit: s.qualityLimitationReason, encoder: s.encoderImplementation })
    })
    send({ type: 'stats', stats: out })
  }, 5000)
  send({ type: 'state', state: 'controller-ready' })
})())JS";

// The app tab, every page (top frame only), through the c6g3Page binding:
//   {editable}  a text field has focus, so the screen's keyboard opens by itself (the screen has no keyboard button);
//               also after each tap, so tapping an already focused field opens it again
//   {video}     the biggest video on the page ({x, y, w, h} of the page, 0..1, the visible part; playing) or null:
//               the passenger screens show just that part of the picture (cast)
//   {call}      true while the page is in a call (a connected WebRTC link receiving sound: Discord's voice channels)
//   {media}     what plays (the site's media info: title, artist, album, artwork as a JPEG data URL; playing): the
//               screen's home card and the gauges. window.__c6g3PlayPause() = WEBAPP_NAV playpause,
//               window.__c6g3Media('nexttrack' | 'previoustrack') = WEBAPP_NAV nexttrack / previoustrack
// Full screen stays inside the page: the element fills the page and the site is told it is full screen. The
// browser's own full screen changed the page's shape (960x810, 711x600, ...) and the picture shrank on the screen.
// YouTube's "Something went wrong" (videos it stops after ~45 s) is covered by a message + Next video button.
const char* kPageScript = R"JS((() => {
  if (window !== window.top) return
  const tell = (o) => { try { window.c6g3Page(JSON.stringify(o)) } catch (e) {} }

  let fsEl = null
  const fsStyle = document.createElement('style')
  // (+ YouTube: its full screen layout kept the player 932 px wide on a 1280 px page without the browser's own
  // full screen; its boxes fill the page instead)
  const FILL = '{width:100vw!important;height:100vh!important;max-width:none!important;max-height:none!important}'
  fsStyle.textContent = '[data-c6g3-fs]{position:fixed!important;inset:0!important;width:100vw!important;height:100vh!important;' +
    'max-width:none!important;max-height:none!important;margin:0!important;z-index:2147483647!important;background:#000!important}' +
    ['#player-full-bleed-container', '#player-container', 'ytd-player#ytd-player', 'ytd-player #container', '#movie_player']
      .map(s => 'html[data-c6g3-fs] ' + s + FILL).join('')
  const fsChanged = (el) => {
    el.dispatchEvent(new Event('fullscreenchange', { bubbles: true }))
    el.dispatchEvent(new Event('webkitfullscreenchange', { bubbles: true }))
    window.dispatchEvent(new Event('resize'))
  }
  const enterFs = function () {
    if (fsEl !== this) {
      if (fsEl) fsEl.removeAttribute('data-c6g3-fs')
      fsEl = this
      if (!fsStyle.isConnected) (document.head || document.documentElement).appendChild(fsStyle)
      this.setAttribute('data-c6g3-fs', '')
      fsChanged(this)
    }
    return Promise.resolve()
  }
  const exitFs = function () {
    if (fsEl) {
      const el = fsEl
      fsEl = null
      el.removeAttribute('data-c6g3-fs')
      fsChanged(el)
    }
    return Promise.resolve()
  }
  Element.prototype.requestFullscreen = enterFs
  Element.prototype.webkitRequestFullscreen = enterFs
  Element.prototype.webkitRequestFullScreen = enterFs
  Document.prototype.exitFullscreen = exitFs
  Document.prototype.webkitExitFullscreen = exitFs
  Document.prototype.webkitCancelFullScreen = exitFs
  for (const name of ['fullscreenElement', 'webkitFullscreenElement', 'webkitCurrentFullScreenElement'])
    Object.defineProperty(Document.prototype, name, { get: () => fsEl, configurable: true })
  for (const name of ['fullscreen', 'webkitIsFullScreen'])
    Object.defineProperty(Document.prototype, name, { get: () => !!fsEl, configurable: true })
  for (const name of ['fullscreenEnabled', 'webkitFullscreenEnabled'])
    Object.defineProperty(Document.prototype, name, { get: () => true, configurable: true })
  // An app that shows one element of its page (webApps.js fill: Poki's game frame) fills the page with it, as full
  // screen does. Its ancestors get the top z-index too: each positioned one is its own stacking layer, and the frame
  // can only rise to the top of that layer. Poki (told it is full screen) makes its game container position: fixed,
  // a layer at z 0 under the page's tiles and ad boxes (z 2); before that a z 1 one kept it under the menu bubble
  // (z 10). Checked twice a second: the site builds the element late and may build it again
  // "selector@1280x830/990x610": a game that never draws larger than its own size (Quenq's Angry Birds: 990 x 610,
  // centred, in a 1280 x 830 frame; smaller in a smaller one): its frame keeps the first size and is scaled so the
  // second (the game, at the frame's centre) fills the page; "@WxH" alone = the whole frame
  const [fillSel, fillSizeText] = String(window.__c6g3Fill || '').split('@')
  const fillSize = /^(\d+)x(\d+)(?:\/(\d+)x(\d+))?$/.exec(fillSizeText || '')
  if (fillSel) {
    const upStyle = document.createElement('style')
    // (+ no page scrollbar beside it)
    upStyle.textContent = '[data-c6g3-fs-up]{z-index:2147483647!important}html{overflow:hidden!important}'
    // The game's frame keeps the keyboard focus (the bypass's keys go to the focused frame): given again when the
    // page took it back (a tap outside it, the site's own script)
    const focusGame = (el) => {
      const f = el.tagName === 'IFRAME' ? el : el.querySelector('iframe')
      if (f && document.activeElement !== f) f.focus()
    }
    // Inline !important beats a site's own !important rules on the element's id (Quenq pins its game frame to
    // 840 x 480), and a scaled / filtered ancestor would hold a fixed element inside it (Quenq scales the frame's box
    // with transform): those go. The site sets its scale again on every resize, so this runs on every check
    const FILL_INLINE = { position: 'fixed', inset: '0', width: '100vw', height: '100vh', 'max-width': 'none',
      'max-height': 'none', 'min-width': '0', 'min-height': '0', margin: '0', 'z-index': '2147483647' }
    const sizedInline = () => {
      const w = Number(fillSize[1]), h = Number(fillSize[2])
      const s = Math.min(innerWidth / Number(fillSize[3] || w), innerHeight / Number(fillSize[4] || h))
      return { ...FILL_INLINE, inset: 'auto', top: '50%', left: '50%', width: w + 'px', height: h + 'px',
        'transform-origin': 'center center', transform: 'translate(-50%, -50%) scale(' + s.toFixed(4) + ')' }
    }
    const pin = (el) => {
      const want = fillSize ? sizedInline() : FILL_INLINE
      for (const k in want) if (el.style.getPropertyValue(k) !== want[k] || el.style.getPropertyPriority(k) !== 'important') el.style.setProperty(k, want[k], 'important')
      for (let p = el.parentElement; p && p !== document.documentElement; p = p.parentElement) {
        for (const k of ['transform', 'filter', 'perspective']) if (p.style.getPropertyPriority(k) !== 'important') p.style.setProperty(k, 'none', 'important')
      }
    }
    setInterval(() => {
      if (fsEl && fsEl.isConnected) {
        pin(fsEl)
        if (!document.activeElement || document.activeElement === document.body) focusGame(fsEl)
        return
      }
      const el = document.querySelector(fillSel)
      if (!el) return
      if (!upStyle.isConnected) (document.head || document.documentElement).appendChild(upStyle)
      for (let p = el.parentElement; p && p !== document.documentElement; p = p.parentElement) p.setAttribute('data-c6g3-fs-up', '')
      fsEl = null
      enterFs.call(el)
      pin(el)
      focusGame(el)
    }, 500)
  }
  // An app that opens on a panel of its site (webApps.js press: Poki's Profile button, its Register / Login panel)
  // presses that button once, when it appears after the page has loaded and its script has had 1.5 s to start (the
  // server-drawn button is there at once, but a press before the site's script runs does nothing)
  const pressSel = window.__c6g3Press
  if (pressSel) {
    const startPressing = () => setTimeout(() => {
      let tries = 0
      const presser = setInterval(() => {
        const el = document.querySelector(pressSel)
        if (el) el.click()
        if (el || ++tries >= 40) clearInterval(presser)
      }, 500)
    }, 1500)
    if (document.readyState === 'complete') startPressing()
    else window.addEventListener('load', startPressing, { once: true })
  }
  const NOT_TEXT = ['button', 'checkbox', 'radio', 'range', 'submit', 'reset', 'file', 'image', 'color']
  const isText = () => {
    let a = document.activeElement
    while (a && a.shadowRoot && a.shadowRoot.activeElement) a = a.shadowRoot.activeElement
    if (!a) return false
    if (a.isContentEditable || a.tagName === 'TEXTAREA') return true
    return a.tagName === 'INPUT' && !NOT_TEXT.includes((a.type || '').toLowerCase())
  }
  let timer = null
  const report = () => {
    clearTimeout(timer)
    timer = setTimeout(() => tell({ editable: isText() }), 120)
  }
  document.addEventListener('focusin', report, true)
  document.addEventListener('focusout', report, true)
  document.addEventListener('touchend', report, true)

  const round = (v) => Math.round(v * 1000) / 1000
  const pickVideo = () => {
    const W = innerWidth, H = innerHeight
    let best = null, area = W * H * 0.05 // smaller ones (previews, ads in a corner) are not the video
    for (const v of document.querySelectorAll('video')) {
      if (v.readyState < 2) continue
      const r = v.getBoundingClientRect()
      const x0 = Math.max(0, r.left), y0 = Math.max(0, r.top), x1 = Math.min(W, r.right), y1 = Math.min(H, r.bottom)
      const a = Math.max(0, x1 - x0) * Math.max(0, y1 - y0)
      if (a <= area) continue
      area = a
      best = { x: round(x0 / W), y: round(y0 / H), w: round((x1 - x0) / W), h: round((y1 - y0) / H), playing: !v.paused && !v.ended }
    }
    return best
  }
  let lastVideo = 'unset'
  setInterval(() => {
    const now = JSON.stringify(pickVideo())
    if (now === lastVideo) return
    lastVideo = now
    tell({ video: JSON.parse(now) })
  }, 250)

  // In a call (Discord's voice channels): a connected WebRTC link of the page that receives sound. The screen pauses
  // its other sources for it and plays them again after. Every link the page makes is known through a subclass
  const peers = new Set()
  if (window.RTCPeerConnection) {
    const PC = window.RTCPeerConnection
    class C6g3PeerConnection extends PC {
      constructor (...args) { super(...args); peers.add(this) }
    }
    window.RTCPeerConnection = C6g3PeerConnection
    if (window.webkitRTCPeerConnection) window.webkitRTCPeerConnection = C6g3PeerConnection
  }
  const inCall = () => {
    for (const pc of peers) {
      const st = pc.connectionState
      if (st === 'closed' || st === 'failed') { peers.delete(pc); continue }
      if (st === 'connected' && pc.getReceivers().some(r => r.track && r.track.kind === 'audio' && r.track.readyState === 'live')) return true
    }
    return false
  }
  let lastCall = false
  setInterval(() => {
    const now = inCall()
    if (now === lastCall) return
    lastCall = now
    tell({ call: now })
  }, 500)

  // What plays, for the screen's home card and the gauges: the site's media info (navigator.mediaSession, which
  // YouTube and Spotify fill in) + whether anything plays. The artwork goes as a small JPEG (the screen loads no
  // web pictures); fetched once per picture.
  // The media elements: the page's and every one it starts, also outside the document (Spotify plays its music in
  // one that is never added to the page). A muted one (a looping background clip: Spotify's Canvas) is not what
  // plays, unless nothing else is there
  const startedMedia = new Set()
  {
    const play = HTMLMediaElement.prototype.play
    HTMLMediaElement.prototype.play = function () {
      startedMedia.add(this)
      return play.apply(this, arguments)
    }
  }
  const mediaElements = () => {
    for (const m of startedMedia) if (!m.isConnected && m.ended) startedMedia.delete(m)
    const all = Array.from(new Set([...startedMedia, ...document.querySelectorAll('video, audio')]))
    const audible = all.filter(m => !m.muted)
    return audible.length ? audible : all
  }
  // Playing: the site's own word (mediaSession.playbackState, set by YouTube), else a media element that is not
  // paused (buffering counts as playing: Spotify's stream often dips below "enough data" mid-song). A change is
  // reported once it has held: PLAY_HOLD_MS to playing, PAUSE_HOLD_MS to paused (a song change pauses Spotify's
  // player for ~2 s while the next one loads; the screen's card shows its own presses at once)
  const PLAY_HOLD_MS = 600
  const PAUSE_HOLD_MS = 2500
  const rawPlaying = () => {
    const ps = navigator.mediaSession && navigator.mediaSession.playbackState
    if (ps === 'playing') return true
    if (ps === 'paused') return false
    return mediaElements().some(m => !m.paused && !m.ended)
  }
  let playing = false, playingSeen = false, playingSince = 0
  const isPlaying = () => {
    const now = rawPlaying()
    if (now === playing) { playingSince = 0; return playing }
    if (!playingSeen) { playingSeen = true; playing = now; return playing }
    if (!playingSince) playingSince = performance.now()
    if (performance.now() - playingSince >= (now ? PLAY_HOLD_MS : PAUSE_HOLD_MS)) { playing = now; playingSince = 0 }
    return playing
  }
  const artCache = { src: '', data: '' }
  const artData = async (src) => {
    if (src === artCache.src) return artCache.data
    artCache.src = src
    artCache.data = ''
    try {
      const img = await createImageBitmap(await (await fetch(src)).blob())
      const scale = Math.min(1, 320 / Math.max(img.width, img.height))
      const c = document.createElement('canvas')
      c.width = Math.round(img.width * scale)
      c.height = Math.round(img.height * scale)
      c.getContext('2d').drawImage(img, 0, 0, c.width, c.height)
      if (artCache.src === src) artCache.data = c.toDataURL('image/jpeg', 0.82)
    } catch (e) {}
    return artCache.data
  }
  let lastMedia = ''
  let mediaBusy = false
  setInterval(async () => {
    if (mediaBusy) return
    mediaBusy = true
    try {
      const md = navigator.mediaSession && navigator.mediaSession.metadata
      const art = md && md.artwork && md.artwork.length ? md.artwork[md.artwork.length - 1].src : ''
      const now = JSON.stringify({ title: md ? md.title || '' : '', artist: md ? md.artist || '' : '', album: md ? md.album || '' : '',
        art: art ? await artData(art) : '', playing: isPlaying() })
      if (now !== lastMedia) {
        lastMedia = now
        tell({ media: JSON.parse(now) })
      }
    } finally {
      mediaBusy = false
    }
  }, 500)

  // Play / pause from the screen (home card): the site's own media-key handlers (navigator.mediaSession, caught as
  // the site sets them: Spotify ignores a scripted click on its button), else its button, else the media element
  const mediaActions = {}
  if (window.MediaSession && MediaSession.prototype.setActionHandler) {
    const setActionHandler = MediaSession.prototype.setActionHandler
    MediaSession.prototype.setActionHandler = function (action, handler) {
      mediaActions[action] = handler
      return setActionHandler.call(this, action, handler)
    }
  }
  window.__c6g3PlayPause = () => {
    const action = rawPlaying() ? 'pause' : 'play'
    if (typeof mediaActions[action] === 'function') {
      try { return mediaActions[action]({ action }) } catch (e) {}
    }
    const button =document.querySelector('#movie_player .ytp-play-button, [data-testid="control-button-playpause"], [data-testid="play-button"], [data-encore-id="buttonPrimary"]')
    if (button) return button.click()
    const all = mediaElements()
    const playing = all.filter(m => !m.paused && !m.ended)
    if (playing.length) return playing.forEach(m => m.pause())
    const m = all.find(m => m.readyState > 0)
    if (m) m.play().catch(() => {})
  }
  // Next / previous from the screen (home card, the global next / previous keys): the site's own media-key
  // handlers, else its skip buttons (YouTube's player, Spotify's bar)
  const SKIP_BUTTONS = {
    nexttrack: '#movie_player .ytp-next-button, [data-testid="control-button-skip-forward"]',
    previoustrack: '#movie_player .ytp-prev-button, [data-testid="control-button-skip-back"]'
  }
  window.__c6g3Media = (action) => {
    if (!SKIP_BUTTONS[action]) return
    if (typeof mediaActions[action] === 'function') {
      try { return mediaActions[action]({ action }) } catch (e) {}
    }
    const button = document.querySelector(SKIP_BUTTONS[action])
    if (button) button.click()
  }

  // YouTube: a video it stops ("Something went wrong": it streams about a minute to a browser it cannot vouch for)
  // gets our message and a Next video button over the player instead of its error. Next = YouTube's own next
  // (playlist / autoplay), else the first suggested video. (No innerHTML: YouTube only allows trusted HTML.)
  if (/(^|\.)youtube\.com$/.test(location.hostname)) {
    const css = document.createElement('style')
    css.textContent = '#c6g3-blocked{position:absolute;inset:0;z-index:2147483646;display:flex;flex-direction:column;' +
      'align-items:center;justify-content:center;gap:14px;background:#0f0f0f;color:#fff;text-align:center;' +
      'font:500 22px Roboto,Arial,sans-serif}' +
      '#c6g3-blocked small{font-size:15px;font-weight:400;color:#aaa}' +
      '#c6g3-blocked button{margin-top:8px;padding:12px 28px;border:0;border-radius:24px;background:#f1f1f1;' +
      'color:#0f0f0f;font:500 17px Roboto,Arial,sans-serif;cursor:pointer}'
    let box = null
    const playNext = () => {
      const a = document.querySelector('#movie_player .ytp-next-button[href]') ||
        document.querySelector('#related a#thumbnail[href*="/watch"], ytd-watch-next-secondary-results-renderer a[href*="/watch"]')
      if (a) { a.click(); return }
      const p = document.querySelector('#movie_player')
      if (p && p.nextVideo) p.nextVideo()
    }
    const makeBox = () => {
      const el = document.createElement('div')
      el.id = 'c6g3-blocked'
      const title = document.createElement('div')
      title.textContent = 'This video can’t play here'
      const why = document.createElement('small')
      why.textContent = 'YouTube stopped it on this screen.'
      const next = document.createElement('button')
      next.type = 'button'
      next.textContent = 'Next video'
      // The screen's taps are touches, and YouTube's player keeps a touch inside it from becoming a click: the
      // button acts on the release itself (click stays for a mouse); one press = one Next
      let pressedAt = 0
      const press = (e) => {
        e.stopPropagation()
        if (e.type === 'touchend' && e.cancelable) e.preventDefault()
        const now = Date.now()
        if (now - pressedAt < 800) return
        pressedAt = now
        playNext()
      }
      for (const type of ['pointerup', 'touchend', 'click']) next.addEventListener(type, press)
      // Presses on the message are ours, not the player's (it would show its controls, or toggle play)
      for (const type of ['pointerdown', 'touchstart', 'mousedown']) el.addEventListener(type, (e) => e.stopPropagation())
      el.append(title, why, next)
      return el
    }
    setInterval(() => {
      const p = document.querySelector('#movie_player')
      const err = p && p.querySelector('.ytp-error')
      const shown = !!(err && err.getClientRects().length && getComputedStyle(err).display !== 'none')
      if (!shown) {
        if (box) { box.remove(); box = null }
        return
      }
      if (!css.isConnected) (document.head || document.documentElement).appendChild(css)
      if (!box) box = makeBox()
      if (box.parentNode !== p) p.appendChild(box)
    }, 500)
  }
})())JS";

const char* kControllerHtml = "<!doctype html><html><head><meta charset=\"utf-8\"><title>c6g3 web app controller</title></head><body></body></html>";

std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(t[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(t[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// Windows virtual key codes for the keys the screen sends
struct KeyInfo { const char* key; const char* code; int vk; const char* text; };
const KeyInfo kKeys[] = {
    {"Enter", "Enter", 13, "\r"}, {"Backspace", "Backspace", 8, ""}, {"Escape", "Escape", 27, ""},
    {"Tab", "Tab", 9, ""}, {"Delete", "Delete", 46, ""}, {"ArrowLeft", "ArrowLeft", 37, ""},
    {"ArrowUp", "ArrowUp", 38, ""}, {"ArrowRight", "ArrowRight", 39, ""}, {"ArrowDown", "ArrowDown", 40, ""},
    {" ", "Space", 32, " "}};

} // anonymous namespace

// ─── Session ─────────────────────────────────────────────────────────────────

struct WebAppHost::Session : std::enable_shared_from_this<WebAppHost::Session> {
    // Set before the thread starts, then only on the io thread
    std::string requestId;
    std::string url;
    std::string titleMatch;
    std::string siteMode = "desktop"; // "mobile": the site's phone layout (applyEmulation)
    bool chromeFirst = false;         // Chrome before Brave (findBrowser)
    std::string fill;                 // a CSS selector: that element fills the page (kPageScript)
    std::string press;                // a CSS selector: that button is pressed once it appears (kPageScript)
    bool mouse = false;               // a desktop game: taps go as a mouse, the page has no touchscreen (input)
    Sink screen;
    Sink sound;
    bool soundAttached = false;

    std::atomic<bool> stopping{false};
    std::thread thread;
    net::io_context ioc;
    websocket::stream<beast::tcp_stream> ws{ioc};
    net::steady_timer watchdog{ioc};
    net::steady_timer sizeTimer{ioc};
    net::steady_timer ctlTimer{ioc};
    net::steady_timer titleTimer{ioc};
    beast::flat_buffer rbuf;
    std::deque<std::string> wq;
    int nextId = 0;
    std::map<int, std::pair<std::string, std::function<void(const json&)>>> pending;
    std::set<int> bestEffort; // pending ids sent with callBestEffort

    Browser browser;
    std::filesystem::path profileDir; // the browser's profile (launch)
    std::string appTargetId, appSession, ctlSession;
    bool appLoaded = false, sized = false, ctlReady = false;
    bool titleSeen = false; // the app tab's title has shown titleMatch once (maybeOffer)
    bool isMobileEmulation = false;
    bool motionHint = false; // a big video plays: the picture's encoder favours movement (__c6g3Hint)
    bool offeredScreen = false, offeredSound = false;
    std::map<std::string, bool> castScreens; // passenger screens with a picture link (true: waiting for its offer)
    int pageW = kWidth, pageH = kHeight;
    int wantH = kHeight; // kWidth x wantH: the screen's own shape, so the site fills it with no bars
    int orphanChecks = 0;
    std::string lastTitle, lastUrl;
    json lastMedia; // the page's last media report: a reopened screen gets it again
    bool inCall = false; // the page's last call report (same)
#if defined(_WIN32)
    HANDLE job = nullptr;
#endif

    // ── To the pages ──
    void toScreen(json j) {
        j["requestId"] = requestId;
        if (screen.send) screen.send(dumpJson(j));
    }
    void toSound(json j) {
        j["requestId"] = requestId;
        if (soundAttached && sound.send) sound.send(dumpJson(j));
    }
    void state(const std::string& s, const std::string& detail = "") {
        Logger::instance().info("WebAppHost", "'{}': {}{}{}", requestId, s, detail.empty() ? "" : ": ", detail);
        json j = {{"event", "WEBAPP_STATE"}, {"state", s}, {"detail", detail}, {"browser", browser.name}};
        toScreen(j);
        toSound(j);
    }
    void error(const std::string& e) {
        Logger::instance().warn("WebAppHost", "'{}': {}", requestId, e);
        toScreen({{"event", "WEBAPP_ERROR"}, {"error", e}});
    }

    // ── DevTools (io thread) ──
    void call(const std::string& method, json params, const std::string& sessionId = "",
              std::function<void(const json&)> done = {}) {
        const int id = ++nextId;
        json m = {{"id", id}, {"method", method}, {"params", std::move(params)}};
        if (!sessionId.empty()) m["sessionId"] = sessionId;
        pending[id] = {method, std::move(done)};
        queueWrite(dumpJson(m));
    }
    // A call whose failure only gets logged (the web app still opens)
    void callBestEffort(const std::string& method, json params, const std::string& sessionId = "") {
        bestEffort.insert(nextId + 1);
        call(method, std::move(params), sessionId);
    }
    void evaluate(const std::string& sessionId, const std::string& expr, std::function<void(const json&)> done = {},
                  bool userGesture = false) {
        call("Runtime.evaluate", {{"expression", expr}, {"returnByValue", true}, {"userGesture", userGesture}}, sessionId,
             [this, done = std::move(done)](const json& r) {
                 if (r.contains("exceptionDetails")) return error("page script: " + r["exceptionDetails"].dump().substr(0, 500));
                 if (done) done(r.contains("result") && r["result"].contains("value") ? r["result"]["value"] : json());
             });
    }
    void queueWrite(std::string msg) {
        wq.push_back(std::move(msg));
        if (wq.size() == 1) doWrite();
    }
    void doWrite() {
        auto self = shared_from_this();
        ws.async_write(net::buffer(wq.front()), [self](beast::error_code ec, std::size_t) {
            self->wq.pop_front();
            if (ec) {
                if (!self->stopping) self->error("DevTools write: " + ec.message());
                return;
            }
            if (!self->wq.empty()) self->doWrite();
        });
    }
    void doRead() {
        auto self = shared_from_this();
        ws.async_read(rbuf, [self](beast::error_code ec, std::size_t) {
            if (ec) {
                if (!self->stopping) {
                    self->error("the browser closed: " + ec.message());
                    self->ioc.stop();
                }
                return;
            }
            const std::string text = beast::buffers_to_string(self->rbuf.data());
            self->rbuf.consume(self->rbuf.size());
            self->onCdp(text);
            self->doRead();
        });
    }
    void onCdp(const std::string& text) {
        json m;
        try {
            m = json::parse(text);
        } catch (const json::exception&) {
            return;
        }
        if (m.contains("id")) {
            const int id = m["id"].get<int>();
            auto it = pending.find(id);
            if (it == pending.end()) return;
            auto [method, done] = std::move(it->second);
            pending.erase(it);
            const bool optional = bestEffort.erase(id) > 0;
            if (m.contains("error")) {
                if (!optional) return error(method + ": " + m["error"].dump());
                Logger::instance().warn("WebAppHost", "'{}': {} failed (ignored): {}", requestId, method, m["error"].dump());
                return;
            }
            if (done) done(m.contains("result") ? m["result"] : json::object());
            return;
        }
        onEvent(m.value("method", ""), m.contains("params") ? m["params"] : json::object(), m.value("sessionId", ""));
    }

    void onEvent(const std::string& method, const json& p, const std::string& sessionId) {
        if (method == "Fetch.requestPaused") {
            call("Fetch.fulfillRequest", {{"requestId", p.value("requestId", "")}, {"responseCode", 200},
                                          {"responseHeaders", json::array({{{"name", "Content-Type"}, {"value", "text/html; charset=utf-8"}}})},
                                          {"body", base64(kControllerHtml)}},
                 sessionId);
        } else if (method == "Page.loadEventFired" || method == "Page.domContentEventFired") {
            if (sessionId == ctlSession && method == "Page.loadEventFired") ensureController();
            // The app's picture starts once its page is parsed: waiting for load (every thumbnail, ad and font)
            // took 10-20 s longer on busy sites; what still loads then shows in the picture. Load sizes again if the
            // first measure missed (a page still redirecting)
            if (sessionId == appSession) {
                appLoaded = true;
                if (!sized) sizePage();
            }
        } else if (method == "Runtime.bindingCalled" && sessionId == ctlSession) {
            onSignal(p.value("payload", "{}"));
        } else if (method == "Runtime.bindingCalled" && sessionId == appSession && p.value("name", "") == "c6g3Page") {
            json page;
            try {
                page = json::parse(p.value("payload", "{}"));
            } catch (const json::exception&) {
                return;
            }
            if (page.contains("editable"))
                toScreen({{"event", "WEBAPP_STATE"}, {"state", "focus"}, {"editable", page.value("editable", false)}});
            if (page.contains("video")) {
                toScreen({{"event", "WEBAPP_STATE"}, {"state", "video"}, {"video", page["video"]}});
                // The picture's encoder favours smooth movement only while a big video plays (a quarter of the page)
                const json& v = page["video"];
                const bool motion = v.is_object() && v.value("playing", false) && v.value("w", 0.0) * v.value("h", 0.0) >= 0.25;
                if (motion != motionHint) {
                    motionHint = motion;
                    if (ctlReady) evaluate(ctlSession, std::string("window.__c6g3Hint && window.__c6g3Hint(") + (motion ? "true" : "false") + ")");
                }
            }
            if (page.contains("call")) {
                inCall = page.value("call", false);
                toScreen({{"event", "WEBAPP_STATE"}, {"state", "call"}, {"call", inCall}});
            }
            if (page.contains("media")) {
                lastMedia = page["media"];
                toScreen({{"event", "WEBAPP_STATE"}, {"state", "media"}, {"media", lastMedia}});
            }
        } else if (method == "Target.targetInfoChanged") {
            const json& info = p.contains("targetInfo") ? p["targetInfo"] : json::object();
            if (info.value("targetId", "") != appTargetId) return;
            const std::string title = info.value("title", ""), u = info.value("url", "");
            if (title == lastTitle && u == lastUrl) return;
            lastTitle = title;
            lastUrl = u;
            toScreen({{"event", "WEBAPP_STATE"}, {"state", "page"}, {"title", title}, {"url", u}, {"browser", browser.name}});
        } else if (method == "Target.targetDestroyed") {
            if (p.value("targetId", "") == appTargetId && !stopping) {
                error("the web app's tab closed");
                ioc.stop();
            }
        }
    }

    void onSignal(const std::string& payload) {
        json s;
        try {
            s = json::parse(payload);
        } catch (const json::exception&) {
            return;
        }
        const std::string type = s.value("type", "");
        if (type == "offer") {
            const std::string to = s.value("to", "");
            json j = {{"event", "WEBAPP_OFFER"}, {"to", to}, {"sdp", s.value("sdp", "")}};
            Logger::instance().info("WebAppHost", "'{}': offer for the {}", requestId, to);
            if (to == "sound") toSound(j);
            else toScreen(j);
        } else if (type == "state") {
            const std::string st = s.value("state", "");
            if (st == "controller-ready") {
                ctlReady = true;
                if (motionHint) evaluate(ctlSession, "window.__c6g3Hint(true)");
                maybeOffer();
            }
            // A captured tab gets the browser's "sharing this tab" bar, which takes 56 px from the page: sized again
            if (st == "captured") {
                sizeTimer.expires_after(kSizeSettle);
                sizeTimer.async_wait([self = shared_from_this()](beast::error_code ec) {
                    if (!ec && !self->stopping) self->sizePage();
                });
            }
            state(st, s.value("detail", ""));
        } else if (type == "stats") {
            toScreen({{"event", "WEBAPP_STATS"}, {"stats", s.contains("stats") ? s["stats"] : json::object()}});
        } else if (type == "error") {
            error(s.value("error", "controller error"));
        }
    }

    // The app tab's page exactly kWidth x wantH: the captured frame then maps 1:1 to the screen's taps. The window
    // is grown by what its frame takes; a resize applies a moment later, so it is measured again and corrected
    void sizePage(int attempt = 0) {
        evaluate(appSession, "[innerWidth, innerHeight]", [this, attempt](const json& v) {
            if (!v.is_array() || v.size() != 2) return;
            pageW = v[0].get<int>();
            pageH = v[1].get<int>();
            if ((pageW == kWidth && pageH == wantH) || attempt >= kSizeAttempts) {
                sized = true;
                state("sized", std::to_string(pageW) + "x" + std::to_string(pageH));
                return maybeOffer();
            }
            call("Browser.getWindowForTarget", {{"targetId", appTargetId}}, "", [this, attempt](const json& r) {
                const int windowId = r.value("windowId", 0);
                const json& b = r.contains("bounds") ? r["bounds"] : json::object();
                const int w = b.value("width", kWidth) + (kWidth - pageW), h = b.value("height", wantH) + (wantH - pageH);
                call("Browser.setWindowBounds", {{"windowId", windowId}, {"bounds", {{"width", w}, {"height", h}}}}, "",
                     [this, attempt](const json&) {
                         sizeTimer.expires_after(kSizeSettle);
                         sizeTimer.async_wait([self = shared_from_this(), attempt](beast::error_code ec) {
                             if (!ec && !self->stopping) self->sizePage(attempt + 1);
                         });
                     });
            });
        });
    }

    // Both tabs ready: each receiver gets its offer (the first one starts the capture: a user gesture for it)
    // The capture picks its tab by title (--auto-select-tab-capture-source-by-title) and looks only once: started
    // while the tab still showed its address (a page parsed before its title was set), it waited for good. The
    // title is asked of the page (targetInfoChanged does not report title changes)
    void maybeOffer() {
        if (!ctlReady || !appLoaded || !sized) return;
        if (!titleSeen) {
            evaluate(appSession, "document.title", [this](const json& v) {
                if (titleSeen || stopping) return;
                if (v.is_string() && v.get<std::string>().find(titleMatch) != std::string::npos) {
                    titleSeen = true;
                    return maybeOffer();
                }
                titleTimer.expires_after(std::chrono::milliseconds(250));
                titleTimer.async_wait([self = shared_from_this()](beast::error_code ec) {
                    if (!ec && !self->stopping) self->maybeOffer();
                });
            });
            return;
        }
        if (!offeredScreen) {
            offeredScreen = true;
            evaluate(ctlSession, "window.__c6g3Offer('screen')", {}, true);
        }
        if (soundAttached && !offeredSound) {
            offeredSound = true;
            evaluate(ctlSession, "window.__c6g3Offer('sound')", {}, true);
        }
        for (auto& [name, pending] : castScreens) {
            if (!pending) continue;
            pending = false;
            evaluate(ctlSession, "window.__c6g3Offer(" + dumpJson("cast:" + name) + ")", {}, true);
        }
    }

    // The site's mobile layout (the screen's setting, sent with WEBAPP_OPEN and WEBAPP_NAV site_mode): only for
    // a site that asks for it (Spotify); every other site gets the browser's own identity back
    void applyEmulation() {
        if (appSession.empty()) return;
        const bool isSpotify = (url.find("spotify.com") != std::string::npos || titleMatch.find("Spotify") != std::string::npos);
        const bool mobile = isSpotify && siteMode == "mobile";
        if (mobile == isMobileEmulation) return;
        isMobileEmulation = mobile;
        if (!mobile) {
            call("Emulation.setUserAgentOverride", {{"userAgent", ""}}, appSession);
            return;
        }
        call("Emulation.setUserAgentOverride", {
            {"userAgent", "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Mobile Safari/537.36"},
            {"platform", "Android"},
            {"userAgentMetadata", {
                {"brands", json::array({
                    {{"brand", "Google Chrome"}, {"version", "130"}},
                    {{"brand", "Chromium"}, {"version", "130"}}
                })},
                {"fullVersion", "130.0.0.0"},
                {"platform", "Android"},
                {"platformVersion", "14"},
                {"architecture", ""},
                {"model", "Pixel 8"},
                {"mobile", true}
            }}
        }, appSession);
    }

    // The toggle on the screen: the page loads again in the other layout (nothing to do when it already has it)
    void setSiteMode(const std::string& mode) {
        siteMode = mode == "mobile" ? "mobile" : "desktop";
        const bool was = isMobileEmulation;
        applyEmulation();
        if (isMobileEmulation == was) return;
        appLoaded = false;
        call("Page.reload", json::object(), appSession);
    }

    // ── From the pages (posted to the io thread) ──
    void reopen(const std::string& newRequestId, const std::string& newUrl, const std::string& newSiteMode, int newHeight, Sink newScreen) {
        requestId = newRequestId;
        screen = std::move(newScreen);
        soundAttached = false; // the sound system attaches again with the new request id
        offeredScreen = false;
        offeredSound = false;
        orphanChecks = 0;
        // The passenger screens ask again for the new request id
        for (const auto& entry : castScreens) evaluate(ctlSession, "window.__c6g3Drop(" + dumpJson("cast:" + entry.first) + ")");
        castScreens.clear();
        state("reopened", browser.name);
        if (!lastMedia.is_null()) toScreen({{"event", "WEBAPP_STATE"}, {"state", "media"}, {"media", lastMedia}});
        if (inCall) toScreen({{"event", "WEBAPP_STATE"}, {"state", "call"}, {"call", true}});
        if (!newUrl.empty() && newUrl != url) {
            url = newUrl;
            siteMode = newSiteMode;
            appLoaded = false;
            applyEmulation();
            call("Page.navigate", {{"url", url}}, appSession);
        } else if (newSiteMode != siteMode) {
            setSiteMode(newSiteMode);
        }
        if (newHeight != wantH) {
            wantH = newHeight;
            sized = false;
            return sizePage(); // offers when sized
        }
        maybeOffer();
    }
    void attachSound(Sink s) {
        sound = std::move(s);
        soundAttached = true;
        offeredSound = false;
        state("sound-attached");
        maybeOffer();
    }
    // A passenger screen (its material name) wants the picture (again: a new link replaces its old one), or not
    void cast(const std::string& name, bool on) {
        if (on) {
            castScreens[name] = true;
            return maybeOffer();
        }
        castScreens.erase(name);
        evaluate(ctlSession, "window.__c6g3Drop(" + dumpJson("cast:" + name) + ")");
    }
    void answer(const std::string& to, const std::string& sdp) {
        evaluate(ctlSession, "window.__c6g3Answer(" + dumpJson(to) + ", " + dumpJson(sdp) + ")");
    }
    void input(const json& in) {
        if (appSession.empty()) return;
        const std::string kind = in.value("kind", "");
        const double x = in.value("x", 0.0) * pageW, y = in.value("y", 0.0) * pageH;
        if (kind == "touch" && mouse) {
            // The finger as the left mouse button: down, drag (held), up; a tap = one click
            const std::string phase = in.value("phase", "");
            json ev = {{"x", x}, {"y", y}, {"button", "left"}, {"pointerType", "mouse"}};
            if (phase == "start") {
                call("Input.dispatchMouseEvent", {{"type", "mouseMoved"}, {"x", x}, {"y", y}, {"button", "none"}, {"buttons", 0}}, appSession);
                ev["type"] = "mousePressed"; ev["buttons"] = 1; ev["clickCount"] = 1;
            } else if (phase == "move") {
                ev["type"] = "mouseMoved"; ev["buttons"] = 1;
            } else {
                ev["type"] = "mouseReleased"; ev["buttons"] = 0; ev["clickCount"] = 1;
            }
            call("Input.dispatchMouseEvent", ev, appSession);
        } else if (kind == "touch") {
            const std::string phase = in.value("phase", "");
            const std::string type = phase == "start" ? "touchStart" : phase == "move" ? "touchMove" : phase == "cancel" ? "touchCancel" : "touchEnd";
            json points = json::array();
            if (type == "touchStart" || type == "touchMove") points.push_back({{"x", x}, {"y", y}});
            call("Input.dispatchTouchEvent", {{"type", type}, {"touchPoints", points}}, appSession);
        } else if (kind == "text") {
            call("Input.insertText", {{"text", in.value("text", "")}}, appSession);
        } else if (kind == "key") {
            const std::string key = in.value("key", "");
            for (const auto& k : kKeys) {
                if (key != k.key) continue;
                json down = {{"type", "keyDown"}, {"key", k.key}, {"code", k.code}, {"windowsVirtualKeyCode", k.vk}, {"nativeVirtualKeyCode", k.vk}};
                if (k.text[0]) down["text"] = k.text;
                call("Input.dispatchKeyEvent", down, appSession);
                call("Input.dispatchKeyEvent", {{"type", "keyUp"}, {"key", k.key}, {"code", k.code}, {"windowsVirtualKeyCode", k.vk}, {"nativeVirtualKeyCode", k.vk}}, appSession);
                break;
            }
        } else if (kind == "wheel") {
            call("Input.dispatchMouseEvent", {{"type", "mouseWheel"}, {"x", x}, {"y", y}, {"deltaX", in.value("dx", 0.0)}, {"deltaY", in.value("dy", 0.0)}}, appSession);
        } else if (kind == "rawkey") {
            // A key of the real keyboard (the keyboard bypass) for a game: down and up come apart, so a held key
            // stays held. A printable one goes down with its text (keypress); the frame with focus gets it
            const std::string key = in.value("key", "");
            const int vk = in.value("keyCode", 0);
            const int mods = (in.value("alt", false) ? 1 : 0) | (in.value("ctrl", false) ? 2 : 0) | (in.value("shift", false) ? 8 : 0);
            json ev = {{"key", key}, {"code", in.value("code", "")}, {"windowsVirtualKeyCode", vk}, {"nativeVirtualKeyCode", vk}, {"modifiers", mods}};
            if (in.value("phase", "") == "up") {
                ev["type"] = "keyUp";
            } else {
                const auto chars = std::count_if(key.begin(), key.end(), [](unsigned char c) { return (c & 0xC0) != 0x80; });
                const bool printable = chars == 1 && !(mods & 3);
                ev["type"] = printable ? "keyDown" : "rawKeyDown";
                if (printable) ev["text"] = key;
                ev["autoRepeat"] = in.value("repeat", false);
            }
            call("Input.dispatchKeyEvent", ev, appSession);
        }
    }
    void navigate(const std::string& action, const std::string& to) {
        if (appSession.empty()) return;
        if (action == "back") evaluate(appSession, "history.back()");
        else if (action == "forward") evaluate(appSession, "history.forward()");
        else if (action == "reload") call("Page.reload", json::object(), appSession);
        else if (action == "home") call("Page.navigate", {{"url", url}}, appSession);
        else if (action == "url" && !to.empty()) call("Page.navigate", {{"url", to}}, appSession);
        else if (action == "playpause") evaluate(appSession, "window.__c6g3PlayPause && window.__c6g3PlayPause()");
        else if (action == "nexttrack" || action == "previoustrack")
            evaluate(appSession, "window.__c6g3Media && window.__c6g3Media(" + dumpJson(action) + ")");
        else if (action == "site_mode") setSiteMode(to);
        // A site's sign-in and stored data leave the profile (Discord once the invite is accepted: nobody can use
        // the account from the bus afterwards)
        else if (action == "forget" && to.rfind("https://", 0) == 0)
            callBestEffort("Storage.clearDataForOrigin", {{"origin", to}, {"storageTypes", "all"}}, appSession);
    }

    // The screen gone for a while (vehicle deleted, game closed): nothing would close the app
    void armWatchdog() {
        auto self = shared_from_this();
        watchdog.expires_after(kWatchdogPeriod);
        watchdog.async_wait([self](beast::error_code ec) {
            if (ec || self->stopping) return;
            const bool connected = self->screen.isConnected && self->screen.isConnected();
            self->orphanChecks = connected ? 0 : self->orphanChecks + 1;
            if (self->orphanChecks >= kOrphanChecks) {
                Logger::instance().info("WebAppHost", "'{}': the screen is gone, closing the web app", self->requestId);
                self->ioc.stop();
                return;
            }
            // Bars come and go (sharing, downloads, ...): the page kept at its size, so taps stay 1:1
            if (self->sized) {
                self->evaluate(self->appSession, "[innerWidth, innerHeight]", [self](const json& v) {
                    if (v.is_array() && v.size() == 2 && (v[0].get<int>() != kWidth || v[1].get<int>() != self->wantH)) self->sizePage();
                });
            }
            self->armWatchdog();
        });
    }

    // ── Browser ──
    bool launch(std::string& browserPath) {
#if defined(_WIN32)
        browser = findBrowser(chromeFirst);
        if (browser.path.empty()) {
            error("Brave, Google Chrome or Microsoft Edge was not found");
            return false;
        }
        // The user's sign-ins live in their own app data, not next to the service (core/UserData.hpp); one profile
        // per browser family
        const std::filesystem::path profile = userDataDir() / "webapp" / browser.profile;
        profileDir = profile;
        std::error_code ec;
        std::filesystem::create_directories(profile, ec);
        std::filesystem::remove(profile / "DevToolsActivePort", ec);
        // (Component updates stay on: they bring Widevine, which Spotify's player needs; without them it is
        // unavailable even when the profile already has it)
        std::wstring cmd = L"\"" + browser.path + L"\" --headless --user-data-dir=\"" + profile.wstring() + L"\""
                           L" --remote-debugging-port=0 --no-first-run --no-default-browser-check --disable-extensions"
                           L" --disable-sync --disable-background-networking --disable-default-apps"
                           L" --disable-domain-reliability --disable-audio-output --autoplay-policy=no-user-gesture-required"
                           // Headless pages count as background to Chrome, which drops their renderers to idle
                           // priority and throttles their timers: behind the game they barely ran
                           L" --disable-renderer-backgrounding --disable-background-timer-throttling"
                           L" --disable-backgrounding-occluded-windows"
                           L" --disk-cache-size=" + std::to_wstring(kDiskCacheBytes) +
                           // The page's screen is the page's own size (headless Chrome's default is 800x600); full
                           // screen itself stays inside the page (kPageScript)
                           L" \"--screen-info={0,0 " + std::to_wstring(kWidth) + L"x" + std::to_wstring(wantH) + L"}\"" +
                           L" --window-size=" + std::to_wstring(kWidth) + L"," + std::to_wstring(wantH) +
                           L" \"--auto-select-tab-capture-source-by-title=" + toWide(titleMatch) + L"\" about:blank";
        state("starting-browser", toUtf8(browser.path));

        job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');
        // Normal priority: the service itself runs below normal (config "priority") and the browser would
        // inherit that, losing every slice to the game in the foreground
        if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) {
            error("the browser could not be started (error " + std::to_string(GetLastError()) + ")");
            return false;
        }
        if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
            Logger::instance().warn("WebAppHost", "the browser is not in the job (error {}): it may outlive the service", GetLastError());
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        // The browser writes "<port>\n/devtools/browser/<id>" to <profile>/DevToolsActivePort once it listens
        const auto deadline = std::chrono::steady_clock::now() + kStartTimeout;
        while (!stopping && std::chrono::steady_clock::now() < deadline) {
            std::ifstream f(profile / "DevToolsActivePort");
            int port = 0;
            std::string path;
            if (f && (f >> port) && (f >> path) && port > 0 && !path.empty()) {
                browserPath = std::to_string(port) + path;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!stopping) error("the browser did not open its DevTools port in time");
        return false;
#else
        error("Only on Windows");
        return false;
#endif
    }

    void run() {
        std::string portAndPath;
        if (!launch(portAndPath)) return finish();
        try {
            auto slash = portAndPath.find('/');
            std::string port = portAndPath.substr(0, slash), path = portAndPath.substr(slash);
            // The first start after a browser update restarts the browser once to move the profile over: the port
            // it wrote closes and a new one follows. Refused: read the port again, for a few seconds
            const auto until = std::chrono::steady_clock::now() + kReconnectWindow;
            for (;;) {
                beast::error_code ec;
                beast::get_lowest_layer(ws).socket().close(ec);
                beast::get_lowest_layer(ws).connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), static_cast<unsigned short>(std::stoi(port))), ec);
                if (!ec) break;
                if (stopping || std::chrono::steady_clock::now() >= until) throw beast::system_error(ec);
                Logger::instance().info("WebAppHost", "DevTools port {} refused ({}); reading the port again", port, ec.message());
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                std::ifstream f(profileDir / "DevToolsActivePort");
                int p = 0;
                std::string pathNow;
                if (f && (f >> p) && (f >> pathNow) && p > 0 && !pathNow.empty()) {
                    port = std::to_string(p);
                    path = pathNow;
                }
            }
            ws.read_message_max(64 * 1024 * 1024);
            ws.handshake("127.0.0.1:" + port, path);
        } catch (const std::exception& e) {
            error(std::string("DevTools connection failed: ") + e.what());
            return finish();
        }
        if (stopping) return finish();
        state("browser-ready", browser.name);
        doRead();
        armWatchdog();
        call("Target.setDiscoverTargets", {{"discover", true}});
        call("Target.getTargets", json::object(), "", [this](const json& r) {
            for (const auto& t : r.value("targetInfos", json::array())) {
                if (t.value("type", "") != "page") continue;
                appTargetId = t.value("targetId", "");
                break;
            }
            if (appTargetId.empty()) {
                error("the browser has no tab for the web app");
                return ioc.stop();
            }
            call("Target.attachToTarget", {{"targetId", appTargetId}, {"flatten", true}}, "", [this](const json& a) {
                appSession = a.value("sessionId", "");
                // No site gets the PC's microphone or camera (Discord voice channels are listen-only), and no
                // notification prompt waits unseen in the hidden window: refused at once, for every site
                // (permission names as in navigator.permissions; a browser that refuses one still opens the app)
                for (const char* perm : {"microphone", "camera", "notifications"})
                    callBestEffort("Browser.setPermission", {{"permission", {{"name", perm}}}, {"setting", "denied"}});
                call("Page.enable", json::object(), appSession);
                call("Runtime.enable", json::object(), appSession);
                if (mouse) {
                    // A desktop game (webApps.js pointer 'mouse': GTA, Minecraft) waits for mouse clicks and ignores
                    // or blocks touches, so taps only got through now and then; and the hidden page is never the
                    // focused window, which games check before they take a click or the pointer (Minecraft)
                    callBestEffort("Emulation.setFocusEmulationEnabled", {{"enabled", true}}, appSession);
                } else {
                    call("Emulation.setTouchEmulationEnabled", {{"enabled", true}, {"maxTouchPoints", 5}}, appSession);
                }
                applyEmulation();
                call("Runtime.addBinding", {{"name", "c6g3Page"}}, appSession);
                if (!fill.empty())
                    call("Page.addScriptToEvaluateOnNewDocument", {{"source", "window.__c6g3Fill = " + dumpJson(fill)}}, appSession);
                if (!press.empty())
                    call("Page.addScriptToEvaluateOnNewDocument", {{"source", "window.__c6g3Press = " + dumpJson(press)}}, appSession);
                call("Page.addScriptToEvaluateOnNewDocument", {{"source", kPageScript}}, appSession);
                call("Page.navigate", {{"url", url}}, appSession);
                openController();
            });
        });
        ioc.run();
        finish();
    }

    // In a window of its own: in the app's window it would be the active tab, and a background app tab does not take
    // the window's new size (sizePage)
    void openController() {
        call("Target.createTarget", {{"url", "about:blank"}, {"newWindow", true}, {"background", true}}, "", [this](const json& r) {
            const std::string targetId = r.value("targetId", "");
            call("Target.attachToTarget", {{"targetId", targetId}, {"flatten", true}}, "", [this](const json& a) {
                ctlSession = a.value("sessionId", "");
                call("Fetch.enable", {{"patterns", json::array({{{"urlPattern", kControllerPattern}}})}}, ctlSession);
                call("Page.enable", json::object(), ctlSession);
                call("Runtime.enable", json::object(), ctlSession);
                call("Runtime.addBinding", {{"name", "c6g3Signal"}}, ctlSession);
                call("Page.navigate", {{"url", kControllerUrl}}, ctlSession, [this](const json&) {
                    // In case its load event came before Page.enable took effect
                    ctlTimer.expires_after(kControllerCheck);
                    ctlTimer.async_wait([self = shared_from_this()](beast::error_code ec) {
                        if (!ec && !self->stopping) self->ensureController();
                    });
                });
            });
        });
    }

    // The controller script, once, in the controller page itself. The tab's first page (about:blank) fires a load
    // event too: injected there, the script said "ready" and was then wiped by the navigation, so the real page never
    // got it (no offers: the screen waited at its splash for good)
    // (check and injection in one evaluation: the load event and the timer cannot both inject it)
    void ensureController() {
        evaluate(ctlSession, std::string("if (location.href === ") + dumpJson(kControllerUrl) + " && !window.__c6g3Offer) " +
                                 kControllerScript);
    }

    void finish() {
        stopping = true;
        beast::error_code ec;
        watchdog.cancel();
        sizeTimer.cancel();
        ctlTimer.cancel();
        titleTimer.cancel();
        beast::get_lowest_layer(ws).socket().close(ec);
        // The cancelled reads / writes still hold this session: let them finish
        ioc.restart();
        ioc.run();
#if defined(_WIN32)
        if (job) {
            TerminateJobObject(job, 0); // the browser and every process it started
            // Termination is asynchronous: a next browser started on the profile while this one still held it
            // handed itself over and quit ("did not open its DevTools port", "connection aborted")
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct{};
            while (QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &acct, sizeof(acct), nullptr) &&
                   acct.ActiveProcesses > 0 && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CloseHandle(job);
            job = nullptr;
        }
#endif
        Logger::instance().info("WebAppHost", "'{}': closed, browser stopped", requestId);
        json j = {{"event", "WEBAPP_CLOSED"}};
        toScreen(j);
        toSound(j);
    }
};

// ─── WebAppHost ──────────────────────────────────────────────────────────────

WebAppHost::~WebAppHost() {
    std::shared_ptr<Session> s;
    {
        std::lock_guard lock(mutex_);
        s = std::move(session_);
    }
    stopSession(std::move(s));
#if defined(_WIN32)
    // (the desktop window is the user's: it stays open)
    if (desktopProcess_) CloseHandle(static_cast<HANDLE>(desktopProcess_));
#endif
}

void WebAppHost::stopSession(std::shared_ptr<Session> s) {
    if (!s) return;
    s->stopping = true;
    net::post(s->ioc, [s]() { s->ioc.stop(); });
    if (s->thread.joinable()) s->thread.join();
}

// The running session, if this request id is still its own (a reloaded screen reopens it with a new one)
std::shared_ptr<WebAppHost::Session> WebAppHost::current(const std::string& requestId) {
    std::lock_guard lock(mutex_);
    if (!session_ || session_->stopping || requestId != requestId_) return nullptr;
    return session_;
}

void WebAppHost::open(const std::string& requestId, const std::string& url, const std::string& titleMatch, const std::string& siteMode, int height, bool chromeFirst, const std::string& fill, const std::string& press, bool mouse, Sink screen) {
    const std::string match = titleMatch.empty() ? "YouTube" : titleMatch;
    const std::string mode = siteMode == "mobile" ? "mobile" : "desktop";
    height = height > 0 ? std::clamp(height, kMinHeight, kMaxHeight) : kHeight;
    std::shared_ptr<Session> old;
    {
        std::lock_guard lock(mutex_);
        // The same web app again (a reloaded screen): keep the browser, offer the links again
        if (session_ && !session_->stopping && session_->titleMatch == match) {
            requestId_ = requestId;
            if (!url.empty()) openedUrl_ = url;
            auto s = session_;
            net::post(s->ioc, [s, requestId, url, mode, height, screen = std::move(screen)]() mutable { s->reopen(requestId, url, mode, height, std::move(screen)); });
            return;
        }
        old = std::move(session_);
        requestId_.clear();
    }
    stopSession(std::move(old)); // one web app at a time
    auto s = std::make_shared<Session>();
    s->requestId = requestId;
    s->url = url;
    s->titleMatch = match;
    s->siteMode = mode;
    s->chromeFirst = chromeFirst;
    s->fill = fill;
    s->press = press;
    s->mouse = mouse;
    s->wantH = height;
    s->screen = std::move(screen);
    s->thread = std::thread([s]() { s->run(); });
    std::lock_guard lock(mutex_);
    session_ = s;
    requestId_ = requestId;
    openedUrl_ = url;
}

void WebAppHost::attachSound(const std::string& requestId, Sink sound) {
    auto s = current(requestId);
    if (!s) return;
    net::post(s->ioc, [s, requestId, sound = std::move(sound)]() mutable {
        if (s->requestId == requestId) s->attachSound(std::move(sound));
    });
}

void WebAppHost::answer(const std::string& requestId, const std::string& to, const std::string& sdp) {
    auto s = current(requestId);
    if (!s) return;
    net::post(s->ioc, [s, requestId, to, sdp]() {
        if (s->requestId == requestId) s->answer(to, sdp);
    });
}

void WebAppHost::cast(const std::string& requestId, const std::string& screenName, bool on) {
    auto s = current(requestId);
    if (!s || screenName.empty()) return;
    net::post(s->ioc, [s, requestId, screenName, on]() {
        if (s->requestId == requestId) s->cast(screenName, on);
    });
}

void WebAppHost::input(const std::string& requestId, const std::string& inputJson) {
    auto s = current(requestId);
    if (!s) return;
    json in;
    try {
        in = json::parse(inputJson);
    } catch (const json::exception&) {
        return;
    }
    net::post(s->ioc, [s, requestId, in = std::move(in)]() {
        if (s->requestId == requestId) s->input(in);
    });
}

void WebAppHost::navigate(const std::string& requestId, const std::string& action, const std::string& url) {
    auto s = current(requestId);
    if (!s) return;
    net::post(s->ioc, [s, requestId, action, url]() {
        if (s->requestId == requestId) s->navigate(action, url);
    });
}

void WebAppHost::close(const std::string& requestId) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard lock(mutex_);
        // A request id the session no longer has (the screen reloaded and reopened it) closes nothing
        if (!session_ || requestId != requestId_) return;
        s = std::move(session_);
        requestId_.clear();
        openedUrl_.clear();
    }
    stopSession(std::move(s));
}

void WebAppHost::closeAll() {
    std::shared_ptr<Session> s;
    {
        std::lock_guard lock(mutex_);
        s = std::move(session_);
        requestId_.clear();
        openedUrl_.clear();
    }
    stopSession(std::move(s));
}

std::string WebAppHost::runningUrl() {
    std::lock_guard lock(mutex_);
    return session_ && !session_->stopping ? openedUrl_ : std::string();
}

bool WebAppHost::desktopWindowOpen() {
#if defined(_WIN32)
    std::lock_guard lock(mutex_);
    if (!desktopProcess_) return false;
    if (WaitForSingleObject(static_cast<HANDLE>(desktopProcess_), 0) == WAIT_TIMEOUT) return true;
    CloseHandle(static_cast<HANDLE>(desktopProcess_));
    desktopProcess_ = nullptr;
#endif
    return false;
}

std::string WebAppHost::openOnDesktop(const std::string& url, bool chromeFirst) {
#if defined(_WIN32)
    if (desktopWindowOpen()) return "The browser window is already open on your desktop.";
    closeAll();
    const Browser browser = findBrowser(chromeFirst);
    if (browser.path.empty()) return "Brave, Google Chrome or Microsoft Edge was not found";
    const std::filesystem::path profile = userDataDir() / "webapp" / browser.profile;
    std::error_code ec;
    std::filesystem::create_directories(profile, ec);
    // The hidden browser's processes end a moment after its job closes: its lockfile goes with the last one
    for (int i = 0; i < 50 && std::filesystem::exists(profile / "lockfile", ec); i++) {
        if (std::filesystem::remove(profile / "lockfile", ec)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // A plain window: no DevTools port, nothing controls it (what Google's sign-in asks for)
    std::wstring cmd = L"\"" + browser.path + L"\" --user-data-dir=\"" + profile.wstring() + L"\""
                       L" --no-first-run --no-default-browser-check --new-window \"" + toWide(url) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi))
        return "The browser could not be started (error " + std::to_string(GetLastError()) + ")";
    CloseHandle(pi.hThread);
    Logger::instance().info("WebAppHost", "Opened {} on the desktop: {}", browser.name, url);
    std::lock_guard lock(mutex_);
    desktopProcess_ = pi.hProcess;
    return "";
#else
    (void)url; (void)chromeFirst;
    return "Only on Windows";
#endif
}

} // namespace ifs
