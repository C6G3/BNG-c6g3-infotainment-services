#include "resources/AuxCapture.hpp"
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
using json          = nlohmann::json;

namespace {

constexpr auto kStartTimeout   = std::chrono::seconds(20);
constexpr auto kWatchdogPeriod = std::chrono::seconds(5);
constexpr int kOrphanChecks    = 4; // the game UI gone this many checks in a row: the capture stops
// The capture page: DevTools answers this URL itself (Fetch), so it is a secure context (audio inputs need one)
// with no server
const char* kPageUrl     = "http://127.0.0.1:38998/c6g3-aux";
const char* kPagePattern = "http://127.0.0.1:38998/*";
const char* kPageHtml    = "<!doctype html><html><head><meta charset=\"utf-8\"><title>c6g3 Aux</title></head><body></body></html>";

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

// Google Chrome, else Microsoft Edge, else Brave (all speak the DevTools protocol)
std::wstring findBrowser() {
    const std::pair<const wchar_t*, const wchar_t*> kinds[] = {
        {L"chrome.exe", L"\\Google\\Chrome\\Application\\chrome.exe"},
        {L"msedge.exe", L"\\Microsoft\\Edge\\Application\\msedge.exe"},
        {L"brave.exe", L"\\BraveSoftware\\Brave-Browser\\Application\\brave.exe"}};
    wchar_t buf[MAX_PATH] = {};
    for (const auto& [exe, folder] : kinds) {
        const std::wstring p = registryAppPath(exe);
        if (!p.empty() && std::filesystem::exists(p)) return p;
        for (const wchar_t* var : {L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"}) {
            if (!GetEnvironmentVariableW(var, buf, MAX_PATH)) continue;
            const std::wstring guess = std::wstring(buf) + folder;
            if (std::filesystem::exists(guess)) return guess;
        }
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

// The capture page's script: opens the input (by its name; "" = the first "CABLE Output") with no voice processing
// and offers it over WebRTC as stereo Opus. It talks back through the c6g3Signal binding; window.__auxStart(label)
// (again: a new link, the same input), window.__auxAnswer(sdp).
const char* kPageScript = R"JS((() => {
  const send = (o) => c6g3Signal(JSON.stringify(o))
  let stream = null
  let pc = null
  const errCode = (e) => {
    if (e && e.code) return e.code
    if (e && (e.name === 'NotAllowedError' || e.name === 'SecurityError')) return 'blocked'
    if (e && (e.name === 'NotFoundError' || e.name === 'OverconstrainedError')) return 'missing'
    return String((e && e.message) || e)
  }
  const iceDone = (p) => new Promise(res => {
    if (p.iceGatheringState === 'complete') return res()
    p.addEventListener('icegatheringstatechange', () => { if (p.iceGatheringState === 'complete') res() })
    setTimeout(res, 3000)
  })
  // Opus in stereo at a music bitrate (WebRTC's default is mono speech)
  const stereo = (sdp) => sdp.replace(/a=fmtp:(\d+) (.*)\r\n/g, (line, pt, params) =>
    new RegExp('a=rtpmap:' + pt + ' opus/', 'i').test(sdp)
      ? 'a=fmtp:' + pt + ' ' + params.replace(/;?(stereo|sprop-stereo|maxaveragebitrate)=\d+/g, '') + ';stereo=1;sprop-stereo=1;maxaveragebitrate=256000\r\n'
      : line)
  async function open (label) {
    const md = navigator.mediaDevices
    if (!md || !md.getUserMedia) { const e = new Error('no audio inputs'); e.code = 'unsupported'; throw e }
    // The names stay hidden until an input was opened once
    const tmp = await md.getUserMedia({ audio: true })
    tmp.getTracks().forEach(t => t.stop())
    const list = (await md.enumerateDevices()).filter(d => d.kind === 'audioinput' && d.deviceId !== 'default' && d.deviceId !== 'communications')
    const dev = label ? list.find(d => d.label === label) : list.find(d => /CABLE Output/i.test(d.label))
    if (!dev) { const e = new Error('input not found'); e.code = label ? 'missing' : 'nocable'; throw e }
    stream = await md.getUserMedia({ audio: { deviceId: { exact: dev.deviceId }, echoCancellation: false, noiseSuppression: false, autoGainControl: false, channelCount: 2, sampleRate: 48000 } })
    const track = stream.getAudioTracks()[0]
    if (track) track.onended = () => { stream = null; send({ type: 'error', error: 'missing', detail: 'the input went away' }) }
    send({ type: 'state', state: 'captured', label: dev.label, detail: JSON.stringify(track ? track.getSettings() : {}) })
  }
  window.__auxStart = async (label) => {
    try {
      if (!stream) await open(label)
      if (pc) { pc.close(); pc = null }
      pc = new RTCPeerConnection({ iceServers: [] })
      for (const t of stream.getAudioTracks()) pc.addTrack(t, stream)
      for (const s of pc.getSenders()) {
        const p = s.getParameters(); p.encodings = p.encodings && p.encodings.length ? p.encodings : [{}]
        p.encodings[0].maxBitrate = 256000
        await s.setParameters(p).catch(() => {})
      }
      const mine = pc
      pc.onconnectionstatechange = () => { if (pc === mine) send({ type: 'state', state: 'link-' + mine.connectionState }) }
      const offer = await pc.createOffer()
      offer.sdp = stereo(offer.sdp)
      await pc.setLocalDescription(offer)
      await iceDone(pc)
      if (pc !== mine) return
      send({ type: 'offer', sdp: pc.localDescription.sdp })
    } catch (e) { send({ type: 'error', error: errCode(e), detail: String(e) }) }
  }
  window.__auxAnswer = (sdp) => {
    if (!pc) return
    pc.setRemoteDescription({ type: 'answer', sdp })
      .then(() => send({ type: 'state', state: 'answer-applied' }))
      .catch(e => send({ type: 'error', error: 'answer: ' + e }))
  }
})())JS";

} // anonymous namespace

// ─── Session ─────────────────────────────────────────────────────────────────

struct AuxCapture::Session : std::enable_shared_from_this<AuxCapture::Session> {
    // Set before the thread starts, then only on the io thread
    std::string requestId;
    std::string deviceLabel;
    Sink sink;

    std::atomic<bool> stopping{false};
    std::thread thread;
    net::io_context ioc;
    websocket::stream<beast::tcp_stream> ws{ioc};
    net::steady_timer watchdog{ioc};
    beast::flat_buffer rbuf;
    std::deque<std::string> wq;
    int nextId = 0;
    bool pageReady = false;
    int orphanChecks = 0;
#if defined(_WIN32)
    HANDLE job = nullptr;
#endif

    void send(json j) {
        j["requestId"] = requestId;
        if (sink.send) sink.send(dumpJson(j));
    }
    void state(const std::string& s, const std::string& detail = "", const std::string& label = "") {
        Logger::instance().info("AuxCapture", "'{}': {}{}{}", requestId, s, detail.empty() ? "" : ": ", detail);
        json j = {{"event", "AUX_STATE"}, {"state", s}, {"detail", detail}};
        if (!label.empty()) j["label"] = label;
        send(std::move(j));
    }
    void error(const std::string& e, const std::string& detail = "") {
        Logger::instance().warn("AuxCapture", "'{}': {}{}{}", requestId, e, detail.empty() ? "" : ": ", detail);
        send({{"event", "AUX_ERROR"}, {"error", e}, {"detail", detail}});
    }

    // ── DevTools (io thread) ──
    void cdp(const std::string& method, json params = json::object()) {
        queueWrite(dumpJson({{"id", ++nextId}, {"method", method}, {"params", std::move(params)}}));
    }
    void evaluate(const std::string& expr) {
        cdp("Runtime.evaluate", {{"expression", expr}, {"awaitPromise", false}});
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
        if (m.contains("error")) return error("DevTools: " + m["error"].dump());
        if (m.contains("result") && m["result"].contains("exceptionDetails")) return error("page script: " + m["result"]["exceptionDetails"].dump().substr(0, 600));
        const std::string method = m.value("method", "");
        const json p = m.contains("params") ? m["params"] : json::object();
        if (method == "Fetch.requestPaused") {
            cdp("Fetch.fulfillRequest", {{"requestId", p.value("requestId", "")}, {"responseCode", 200},
                                         {"responseHeaders", json::array({{{"name", "Content-Type"}, {"value", "text/html; charset=utf-8"}}})},
                                         {"body", base64(kPageHtml)}});
        } else if (method == "Page.loadEventFired") {
            // The script once, in the capture page itself (not in the first about:blank), then the first offer
            evaluate(std::string("if (location.href === ") + dumpJson(kPageUrl) + " && !window.__auxStart) " + kPageScript);
            evaluate(std::string("if (location.href === ") + dumpJson(kPageUrl) + ") window.__auxStart(" + dumpJson(deviceLabel) + ")");
            pageReady = true;
        } else if (method == "Runtime.bindingCalled") {
            onSignal(p.value("payload", "{}"));
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
            Logger::instance().info("AuxCapture", "'{}': offer for the sound system", requestId);
            send({{"event", "AUX_OFFER"}, {"sdp", s.value("sdp", "")}});
        } else if (type == "state") {
            state(s.value("state", ""), s.value("detail", ""), s.value("label", ""));
        } else if (type == "error") {
            error(s.value("error", "capture error"), s.value("detail", ""));
        }
    }

    // A new start while it runs (a reloaded game UI): the same input, a new link
    void restart(const std::string& newRequestId, Sink newSink) {
        requestId = newRequestId;
        sink = std::move(newSink);
        orphanChecks = 0;
        state("reattached");
        if (pageReady) evaluate("window.__auxStart && window.__auxStart(" + dumpJson(deviceLabel) + ")");
    }

    void answer(const std::string& sdp) {
        evaluate("window.__auxAnswer && window.__auxAnswer(" + dumpJson(sdp) + ")");
    }

    // The game UI gone for a while (game closed, sound system not reloaded): nothing would stop the capture
    void armWatchdog() {
        auto self = shared_from_this();
        watchdog.expires_after(kWatchdogPeriod);
        watchdog.async_wait([self](beast::error_code ec) {
            if (ec || self->stopping) return;
            const bool connected = self->sink.isConnected && self->sink.isConnected();
            self->orphanChecks = connected ? 0 : self->orphanChecks + 1;
            if (self->orphanChecks >= kOrphanChecks) {
                Logger::instance().info("AuxCapture", "'{}': the game UI is gone, stopping the capture", self->requestId);
                self->ioc.stop();
                return;
            }
            self->armWatchdog();
        });
    }

    bool launch(int& port) {
#if defined(_WIN32)
        const std::wstring browser = findBrowser();
        if (browser.empty()) {
            error("nobrowser", "Google Chrome, Microsoft Edge or Brave was not found");
            return false;
        }
        // core/UserData.hpp; Brave keeps its own (it and Chrome do not share profiles)
        const bool brave = browser.find(L"brave.exe") != std::wstring::npos;
        const std::filesystem::path profile = userDataDir() / "webapp" / (brave ? "aux_profile_brave" : "aux_profile");
        std::error_code ec;
        std::filesystem::create_directories(profile, ec);
        std::filesystem::remove(profile / "DevToolsActivePort", ec);
        // --use-fake-ui-for-media-stream accepts the browser's own permission prompt (nobody would see it); the
        // devices are the real ones. Its sound output is off: the sound goes over WebRTC only
        std::wstring cmd = L"\"" + browser + L"\" --headless --user-data-dir=\"" + profile.wstring() + L"\""
                           L" --remote-debugging-port=0 --no-first-run --no-default-browser-check --disable-extensions"
                           L" --disable-sync --disable-background-networking --disable-component-update --disable-default-apps"
                           L" --disable-domain-reliability --disable-audio-output --autoplay-policy=no-user-gesture-required"
                           L" --use-fake-ui-for-media-stream"
                           // A headless page counts as background: no throttling of its timers / renderer
                           L" --disable-renderer-backgrounding --disable-background-timer-throttling"
                           L" --disable-backgrounding-occluded-windows --window-size=400,300 about:blank";
        state("starting-browser", toUtf8(browser));

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
        // Normal priority: the service runs below normal and the browser would inherit it (the sound would stutter
        // behind the game)
        if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) {
            error("the browser could not be started (error " + std::to_string(GetLastError()) + ")");
            return false;
        }
        if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
            Logger::instance().warn("AuxCapture", "the browser is not in the job (error {}): it may outlive the service", GetLastError());
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        const auto deadline = std::chrono::steady_clock::now() + kStartTimeout;
        while (!stopping && std::chrono::steady_clock::now() < deadline) {
            std::ifstream f(profile / "DevToolsActivePort");
            if (f && (f >> port) && port > 0) return true;
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
        int port = 0;
        if (!launch(port)) return finish();
        try {
            std::string wsUrl;
            for (int i = 0; i < 50 && wsUrl.empty() && !stopping; ++i) {
                for (const auto& t : json::parse(httpGet(port, "/json/list"))) {
                    if (t.value("type", "") == "page") {
                        wsUrl = t.value("webSocketDebuggerUrl", "");
                        break;
                    }
                }
                if (wsUrl.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (wsUrl.empty()) {
                error("the browser has no page to drive");
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
        state("browser-ready");
        doRead();
        armWatchdog();
        cdp("Fetch.enable", {{"patterns", json::array({{{"urlPattern", kPagePattern}}})}});
        cdp("Page.enable");
        cdp("Runtime.enable");
        cdp("Runtime.addBinding", {{"name", "c6g3Signal"}});
        cdp("Page.navigate", {{"url", kPageUrl}});
        ioc.run();
        finish();
    }

    void finish() {
        stopping = true;
        beast::error_code ec;
        watchdog.cancel();
        beast::get_lowest_layer(ws).socket().close(ec);
        // The cancelled reads / writes still hold this session: let them finish
        ioc.restart();
        ioc.run();
#if defined(_WIN32)
        if (job) {
            TerminateJobObject(job, 0); // the browser and every process it started
            // Asynchronous: a next capture started on the profile while this one still held it would hand itself
            // over and quit
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct{};
            while (QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &acct, sizeof(acct), nullptr) &&
                   acct.ActiveProcesses > 0 && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CloseHandle(job);
            job = nullptr;
        }
#endif
        Logger::instance().info("AuxCapture", "'{}': stopped, browser closed", requestId);
        send({{"event", "AUX_STOPPED"}});
    }
};

// ─── AuxCapture ──────────────────────────────────────────────────────────────

namespace {
void stopSession(std::shared_ptr<AuxCapture::Session> s) {
    if (!s) return;
    s->stopping = true;
    net::post(s->ioc, [s]() { s->ioc.stop(); });
    if (s->thread.joinable()) s->thread.join();
}
} // namespace

AuxCapture::~AuxCapture() {
    std::shared_ptr<Session> s;
    {
        std::lock_guard lock(mutex_);
        s = std::move(session_);
    }
    stopSession(std::move(s));
}

void AuxCapture::start(const std::string& requestId, const std::string& deviceLabel, Sink sink) {
    std::shared_ptr<Session> old;
    {
        std::lock_guard lock(mutex_);
        // Running on the same input: keep the browser, offer the link again
        if (session_ && !session_->stopping && session_->deviceLabel == deviceLabel) {
            auto s = session_;
            net::post(s->ioc, [s, requestId, sink = std::move(sink)]() mutable { s->restart(requestId, std::move(sink)); });
            return;
        }
        old = std::move(session_);
    }
    stopSession(std::move(old));
    auto s = std::make_shared<Session>();
    s->requestId = requestId;
    s->deviceLabel = deviceLabel;
    s->sink = std::move(sink);
    s->thread = std::thread([s]() { s->run(); });
    std::lock_guard lock(mutex_);
    session_ = s;
}

void AuxCapture::answer(const std::string& requestId, const std::string& sdp) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard lock(mutex_);
        s = session_;
    }
    if (!s || s->stopping) return;
    net::post(s->ioc, [s, requestId, sdp]() {
        if (s->requestId == requestId) s->answer(sdp);
    });
}

void AuxCapture::stop(const std::string& requestId) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard lock(mutex_);
        // An old request id (the game UI reloaded and started it again) stops nothing
        if (!session_ || session_->requestId != requestId) return;
        s = std::move(session_);
    }
    stopSession(std::move(s));
}

} // namespace ifs
