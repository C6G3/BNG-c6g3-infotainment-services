#include "resources/WebRtcProbe.hpp"
#include "core/Logger.hpp"
#include "core/UserData.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
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

namespace {

constexpr auto kTimeout      = std::chrono::seconds(90);
constexpr auto kStartTimeout = std::chrono::seconds(20);

std::string dumpJson(const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
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

// Google Chrome, else Microsoft Edge (both speak the DevTools protocol)
std::wstring findBrowser() {
    for (const wchar_t* exe : {L"chrome.exe", L"msedge.exe"}) {
        const std::wstring p = registryAppPath(exe);
        if (!p.empty() && std::filesystem::exists(p)) return p;
    }
    wchar_t buf[MAX_PATH] = {};
    std::vector<std::wstring> guesses;
    for (const wchar_t* var : {L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"}) {
        if (GetEnvironmentVariableW(var, buf, MAX_PATH)) {
            guesses.push_back(std::wstring(buf) + L"\\Google\\Chrome\\Application\\chrome.exe");
            guesses.push_back(std::wstring(buf) + L"\\Microsoft\\Edge\\Application\\msedge.exe");
        }
    }
    for (const auto& g : guesses) {
        if (std::filesystem::exists(g)) return g;
    }
    return L"";
}
#endif

std::string httpGet(int port, const std::string& target) {
    net::io_context ioc;
    beast::tcp_stream stream(ioc);
    stream.expires_after(std::chrono::seconds(5));
    stream.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)));
    http::request<http::empty_body> req{http::verb::get, target, 11};
    req.set(http::field::host, "127.0.0.1:" + std::to_string(port));
    http::write(stream, req);
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);
    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    return res.body();
}

// The page Chrome runs (about:blank): test pattern + tone, offered over WebRTC. It talks back through the
// c6g3Signal binding (Runtime.bindingCalled); the answer arrives as window.__c6g3Answer(sdp).
const char* kProbeScript = R"JS((() => {
  const send = (o) => c6g3Signal(JSON.stringify(o))
  try {
    const W = 800, H = 450
    const c = document.createElement('canvas'); c.width = W; c.height = H; document.body.appendChild(c)
    const g = c.getContext('2d'); let n = 0
    const draw = () => {
      n++
      g.fillStyle = '#0b1220'; g.fillRect(0, 0, W, H)
      const colors = ['#ef4444', '#f59e0b', '#22c55e', '#06b6d4', '#8b5cf6', '#ec4899']
      for (let i = 0; i < 6; i++) { g.fillStyle = colors[i]; g.fillRect(((n * 6 + i * 140) % (W + 140)) - 140, 300, 120, 120) }
      g.fillStyle = '#ffffff'; g.font = 'bold 44px sans-serif'; g.fillText('c6g3 WebRTC check', 36, 80)
      g.font = '36px monospace'; g.fillText('frame ' + n, 36, 150); g.fillText(new Date().toISOString().slice(11, 23), 36, 210)
      g.font = '22px sans-serif'; g.fillText('Powered by Google Chrome', 36, 262)
    }
    draw(); setInterval(draw, 33)
    const stream = window.__c6g3AudioOnly ? new MediaStream() : c.captureStream(30)
    const ac = new AudioContext()
    const osc = ac.createOscillator(); osc.frequency.value = 440
    const gain = ac.createGain(); gain.gain.value = 0.2
    const dst = ac.createMediaStreamDestination()
    osc.connect(gain).connect(dst); osc.start(); ac.resume()
    stream.addTrack(dst.stream.getAudioTracks()[0])
    const pc = new RTCPeerConnection({ iceServers: [] }); window.__pc = pc
    for (const t of stream.getTracks()) pc.addTrack(t, stream)
    pc.onconnectionstatechange = () => send({ type: 'state', state: 'sender-' + pc.connectionState })
    const munge = (sdp) => sdp.replace(/a=rtpmap:(\d+) (VP8|VP9|H264|AV1)\/90000\r\n/g, (line, pt) =>
      new RegExp('a=fmtp:' + pt + ' ').test(sdp) ? line : line + 'a=fmtp:' + pt + ' x-google-start-bitrate=3000\r\n')
      .replace(/a=fmtp:(\d+) (.*)\r\n/g, (line, pt, params) =>
        new RegExp('a=rtpmap:' + pt + ' (VP8|VP9|H264|AV1)/').test(sdp) && !/x-google-start-bitrate/.test(params)
          ? 'a=fmtp:' + pt + ' ' + params + ';x-google-start-bitrate=3000;x-google-min-bitrate=1500;x-google-max-bitrate=4000\r\n' : line)
    ;(async () => {
      for (const s of pc.getSenders()) {
        if (s.track && s.track.kind === 'video') {
          const p = s.getParameters(); p.encodings = p.encodings && p.encodings.length ? p.encodings : [{}]
          p.encodings[0].maxBitrate = 4000000; p.encodings[0].maxFramerate = 30; p.degradationPreference = 'maintain-resolution'
          await s.setParameters(p).catch(() => {})
        }
      }
      const offer = await pc.createOffer(); offer.sdp = munge(offer.sdp)
      await pc.setLocalDescription(offer)
      await new Promise(res => { if (pc.iceGatheringState === 'complete') return res(); pc.addEventListener('icegatheringstatechange', () => pc.iceGatheringState === 'complete' && res()); setTimeout(res, 3000) })
      send({ type: 'offer', sdp: pc.localDescription.sdp })
    })().catch(e => send({ type: 'error', error: 'offer: ' + e }))
    window.__c6g3Answer = (sdp) => pc.setRemoteDescription({ type: 'answer', sdp })
      .then(() => send({ type: 'state', state: 'answer-applied' }))
      .catch(e => send({ type: 'error', error: 'answer: ' + e }))
    setInterval(async () => {
      const out = {}
      ;(await pc.getStats()).forEach(s => {
        if (s.type === 'outbound-rtp') out[s.kind] = { fps: s.framesPerSecond, w: s.frameWidth, h: s.frameHeight, bytes: s.bytesSent, encoder: s.encoderImplementation, limit: s.qualityLimitationReason }
        if (s.type === 'candidate-pair' && s.nominated && s.state === 'succeeded') out.rttMs = Math.round((s.currentRoundTripTime || 0) * 1000)
      })
      send({ type: 'stats', stats: out })
    }, 2000)
  } catch (e) { send({ type: 'error', error: String(e) }) }
})())JS";

} // anonymous namespace

// ─── Session ─────────────────────────────────────────────────────────────────

struct WebRtcProbe::Session : std::enable_shared_from_this<WebRtcProbe::Session> {
    std::string requestId;
    Sink sink;
    bool audioOnly = false;
    std::atomic<bool> stopping{false};
    std::thread thread;
    net::io_context ioc;
    websocket::stream<beast::tcp_stream> ws{ioc};
    net::steady_timer timeout{ioc};
    beast::flat_buffer rbuf;
    std::deque<std::string> wq;
    int nextId = 0;
#if defined(_WIN32)
    HANDLE job = nullptr;
#endif

    void send(nlohmann::json j) {
        j["requestId"] = requestId;
        if (sink.send) sink.send(dumpJson(j));
    }
    void state(const std::string& s, const std::string& detail = "") {
        Logger::instance().info("WebRtcProbe", "'{}': {}{}{}", requestId, s, detail.empty() ? "" : ": ", detail);
        send({{"event", "WEBRTC_PROBE_STATE"}, {"state", s}, {"detail", detail}});
    }
    void error(const std::string& e) {
        Logger::instance().warn("WebRtcProbe", "'{}': {}", requestId, e);
        send({{"event", "WEBRTC_PROBE_ERROR"}, {"error", e}});
    }

    // ── CDP (called on the io thread) ──
    void cdp(const std::string& method, nlohmann::json params = nlohmann::json::object()) {
        queueWrite(dumpJson({{"id", ++nextId}, {"method", method}, {"params", params}}));
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
                if (!self->stopping) self->error("Chrome closed the DevTools connection: " + ec.message());
                return;
            }
            const std::string text = beast::buffers_to_string(self->rbuf.data());
            self->rbuf.consume(self->rbuf.size());
            self->onCdp(text);
            self->doRead();
        });
    }
    void onCdp(const std::string& text) {
        nlohmann::json m;
        try {
            m = nlohmann::json::parse(text);
        } catch (const nlohmann::json::exception& e) {
            return error(std::string("unreadable DevTools message: ") + e.what());
        }
        if (m.contains("error")) return error("DevTools: " + m["error"].dump());
        if (m.contains("result") && m["result"].contains("exceptionDetails")) return error("page script: " + m["result"]["exceptionDetails"].dump().substr(0, 600));
        const std::string method = m.value("method", "");
        if (method == "Runtime.exceptionThrown") return error("page: " + m["params"].dump().substr(0, 600));
        if (method != "Runtime.bindingCalled") return;
        nlohmann::json p;
        try {
            p = nlohmann::json::parse(m["params"].value("payload", "{}"));
        } catch (const nlohmann::json::exception&) {
            return;
        }
        const std::string type = p.value("type", "");
        if (type == "offer") {
            state("offer-sent");
            send({{"event", "WEBRTC_PROBE_OFFER"}, {"sdp", p.value("sdp", "")}});
        } else if (type == "state") {
            state(p.value("state", ""));
        } else if (type == "stats") {
            send({{"event", "WEBRTC_PROBE_STATS"}, {"stats", p.value("stats", nlohmann::json::object())}});
        } else if (type == "error") {
            error(p.value("error", "unknown page error"));
        }
    }

    bool launch(int& port) {
#if defined(_WIN32)
        const std::wstring browser = findBrowser();
        if (browser.empty()) {
            error("Google Chrome (or Microsoft Edge) was not found");
            return false;
        }
        const std::filesystem::path profile = userDataDir() / "webapp" / "probe_profile"; // core/UserData.hpp
        std::error_code ec;
        std::filesystem::create_directories(profile, ec);
        std::filesystem::remove(profile / "DevToolsActivePort", ec);
        std::wstring cmd = L"\"" + browser + L"\" --headless --user-data-dir=\"" + profile.wstring() + L"\""
                           L" --remote-debugging-port=0 --no-first-run --no-default-browser-check --disable-extensions"
                           L" --disable-sync --disable-background-networking --disable-component-update --disable-default-apps"
                           L" --disable-domain-reliability --disable-audio-output --autoplay-policy=no-user-gesture-required"
                           L" --window-size=800,450 about:blank";
        state("starting-chrome", toUtf8(browser));

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
        if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            error("Chrome could not be started (error " + std::to_string(GetLastError()) + ")");
            return false;
        }
        if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
            Logger::instance().warn("WebRtcProbe", "Chrome is not in the job (error {}): it may outlive the service", GetLastError());
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        // Chrome writes its DevTools port to <profile>/DevToolsActivePort once it listens
        const auto deadline = std::chrono::steady_clock::now() + kStartTimeout;
        while (!stopping && std::chrono::steady_clock::now() < deadline) {
            std::ifstream f(profile / "DevToolsActivePort");
            if (f && (f >> port) && port > 0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!stopping) error("Chrome did not open its DevTools port in time");
        return false;
#else
        error("Only on Windows");
        return false;
#endif
    }

    void run() {
        int port = 0;
        if (!launch(port)) return finish();
        try {
            const auto version = nlohmann::json::parse(httpGet(port, "/json/version"));
            state("chrome-ready", version.value("Browser", "") + " (port " + std::to_string(port) + ")");
            std::string wsUrl;
            for (int i = 0; i < 50 && wsUrl.empty() && !stopping; ++i) {
                for (const auto& t : nlohmann::json::parse(httpGet(port, "/json/list"))) {
                    if (t.value("type", "") == "page") {
                        wsUrl = t.value("webSocketDebuggerUrl", "");
                        break;
                    }
                }
                if (wsUrl.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (wsUrl.empty()) {
                error("Chrome has no page to drive");
                return finish();
            }
            const std::string path = wsUrl.substr(wsUrl.find('/', std::string("ws://").size()));
            beast::get_lowest_layer(ws).connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)));
            ws.handshake("127.0.0.1:" + std::to_string(port), path);
        } catch (const std::exception& e) {
            error(std::string("DevTools connection failed: ") + e.what());
            return finish();
        }
        if (stopping) return finish();

        auto self = shared_from_this();
        timeout.expires_after(kTimeout);
        timeout.async_wait([self](beast::error_code ec) {
            if (ec) return;
            self->state("timeout", "the check stops after 90 s");
            self->ioc.stop();
        });
        doRead();
        cdp("Runtime.enable");
        cdp("Runtime.addBinding", {{"name", "c6g3Signal"}});
        if (audioOnly) cdp("Runtime.evaluate", {{"expression", "window.__c6g3AudioOnly = true"}, {"awaitPromise", false}});
        cdp("Runtime.evaluate", {{"expression", kProbeScript}, {"awaitPromise", false}});
        state("page-started");
        ioc.run();
        finish();
    }

    void finish() {
        stopping = true;
        beast::error_code ec;
        timeout.cancel();
        beast::get_lowest_layer(ws).socket().close(ec);
        // The cancelled reads / writes still hold this session: let them finish
        ioc.restart();
        ioc.run();
#if defined(_WIN32)
        if (job) {
            TerminateJobObject(job, 0); // Chrome and every process it started
            CloseHandle(job);
            job = nullptr;
        }
#endif
        Logger::instance().info("WebRtcProbe", "'{}': stopped, Chrome closed", requestId);
        send({{"event", "WEBRTC_PROBE_STOPPED"}});
    }
};

// ─── WebRtcProbe ─────────────────────────────────────────────────────────────

WebRtcProbe::~WebRtcProbe() {
    std::shared_ptr<Session> s;
    {
        std::lock_guard lock(mutex_);
        s = std::move(session_);
    }
    if (s) {
        s->stopping = true;
        net::post(s->ioc, [s]() { s->ioc.stop(); });
        if (s->thread.joinable()) s->thread.join();
    }
}

void WebRtcProbe::start(const std::string& requestId, Sink sink, bool audioOnly) {
    std::shared_ptr<Session> old;
    {
        std::lock_guard lock(mutex_);
        old = std::move(session_);
    }
    if (old) {
        old->stopping = true;
        net::post(old->ioc, [old]() { old->ioc.stop(); });
        if (old->thread.joinable()) old->thread.join();
    }
    auto s = std::make_shared<Session>();
    s->requestId = requestId;
    s->sink = std::move(sink);
    s->audioOnly = audioOnly;
    s->thread = std::thread([s]() { s->run(); });
    std::lock_guard lock(mutex_);
    session_ = s;
}

void WebRtcProbe::answer(const std::string& requestId, const std::string& sdp) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard lock(mutex_);
        s = session_;
    }
    if (!s || s->requestId != requestId) return;
    net::post(s->ioc, [s, sdp]() {
        s->state("answer-received");
        s->cdp("Runtime.evaluate", {{"expression", "window.__c6g3Answer(" + dumpJson(sdp) + ")"}, {"awaitPromise", false}});
    });
}

void WebRtcProbe::stop(const std::string& requestId) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard lock(mutex_);
        if (!session_ || session_->requestId != requestId) return;
        s = std::move(session_);
    }
    s->stopping = true;
    net::post(s->ioc, [s]() { s->ioc.stop(); });
    if (s->thread.joinable()) s->thread.join();
}

} // namespace ifs
