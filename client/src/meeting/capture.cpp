#include "meeting/capture.h"

#include "platform/com.h"
#include "platform/win.h"

#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <uiautomation.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <future>
#include <mutex>

namespace wb::meeting {
namespace {

// MinGW has no audioclientactivationparams.h (Windows 10 2004 and later); these match the SDK definitions.
enum AUDIOCLIENT_ACTIVATION_TYPE { AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0, AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1 };
enum PROCESS_LOOPBACK_MODE { PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0, PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1 };
struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
};
struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
};
constexpr const wchar_t* kProcessLoopbackDevice = L"VAD\\Process_Loopback";

// MinGW declares these without definitions.
constexpr CLSID kMMDeviceEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
constexpr IID kIMMDeviceEnumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
constexpr IID kIAudioClient = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
constexpr IID kIAudioCaptureClient = {0xc8adbd64, 0xe71e, 0x48a0, {0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17}};
constexpr IID kIAudioSessionManager2 = {0x77aa99a0, 0x1bd6, 0x484f, {0x8b, 0xc7, 0x2c, 0x65, 0x4c, 0x9a, 0x9b, 0x6f}};
constexpr IID kIAudioSessionControl2 = {0xbfb7ff88, 0x7239, 0x4fc9, {0x8f, 0xa2, 0x07, 0xc9, 0x50, 0xbe, 0x9c, 0x6d}};
constexpr IID kICompletionHandler = {0x41d949ab, 0x9862, 0x444a, {0x80, 0xf6, 0xc2, 0x61, 0x33, 0x4d, 0xa5, 0xeb}};
constexpr IID kIAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
constexpr CLSID kCUIAutomation = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};

constexpr std::uint32_t kRate = 16000;

struct ComInit {
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComInit() {
        if (SUCCEEDED(hr)) ::CoUninitialize();
    }
};

std::string hr_text(const char* what, HRESULT hr) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s (0x%08lx)", what, static_cast<unsigned long>(hr));
    return buf;
}

// The completion handler of ActivateAudioInterfaceAsync; it must be agile (free-threaded).
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kICompletionHandler)) *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (IsEqualIID(riid, kIAgileObject)) *ppv = static_cast<IAgileObject*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT activate = E_FAIL;
        com_ptr<IUnknown> unk;
        const HRESULT hr = op->GetActivateResult(&activate, unk.put());
        {
            const std::lock_guard lock(mx_);
            result_ = FAILED(hr) ? hr : activate;
            if (SUCCEEDED(result_) && unk) unk.as(client_, kIAudioClient);
            done_ = true;
        }
        cv_.notify_all();
        return S_OK;
    }
    HRESULT wait(com_ptr<IAudioClient>& out) {
        std::unique_lock lock(mx_);
        if (!cv_.wait_for(lock, std::chrono::seconds(5), [&] { return done_; })) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        out = client_;
        return SUCCEEDED(result_) && !client_ ? E_NOINTERFACE : result_;
    }

private:
    std::atomic<ULONG> refs_{1};
    std::mutex mx_;
    std::condition_variable cv_;
    bool done_ = false;
    HRESULT result_ = E_FAIL;
    com_ptr<IAudioClient> client_;
};

Result<com_ptr<IAudioClient>> open_client(const CaptureSource& s, DWORD& flags) {
    com_ptr<IAudioClient> client;
    flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (s.kind == CaptureSource::Kind::process) {
        AUDIOCLIENT_ACTIVATION_PARAMS p{};
        p.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        p.ProcessLoopbackParams.TargetProcessId = s.pid;
        p.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT pv{};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof p;
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&p);
        auto* handler = new ActivationHandler();
        com_ptr<IActivateAudioInterfaceAsyncOperation> op;
        HRESULT hr = ::ActivateAudioInterfaceAsync(kProcessLoopbackDevice, kIAudioClient, &pv, handler, op.put());
        if (SUCCEEDED(hr)) hr = handler->wait(client);
        handler->Release();
        if (FAILED(hr)) return make_error("audio", hr_text("process loopback is not available", hr));
        flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        return client;
    }
    com_ptr<IMMDeviceEnumerator> en;
    HRESULT hr = ::CoCreateInstance(kMMDeviceEnumerator, nullptr, CLSCTX_INPROC_SERVER, kIMMDeviceEnumerator, en.put_void());
    if (FAILED(hr)) return make_error("audio", hr_text("no audio devices", hr));
    com_ptr<IMMDevice> dev;
    const bool mic = s.kind == CaptureSource::Kind::mic;
    hr = en->GetDefaultAudioEndpoint(mic ? eCapture : eRender, mic ? eCommunications : eConsole, dev.put());
    if (FAILED(hr)) return make_error("audio", hr_text(mic ? "no microphone" : "no audio output", hr));
    hr = dev->Activate(kIAudioClient, CLSCTX_INPROC_SERVER, nullptr, client.put_void());
    if (FAILED(hr)) return make_error("audio", hr_text("the audio device cannot be opened", hr));
    if (!mic) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    return client;
}

long long steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// ---- capture ---------------------------------------------------------------------------------------------------------

Result<std::unique_ptr<AudioCapture>> AudioCapture::start(const CaptureSource& source, std::shared_ptr<TrackWriter> writer) {
    std::unique_ptr<AudioCapture> c(new AudioCapture());
    std::mutex m;
    std::condition_variable cv;
    std::optional<Status> started;
    c->thread_ = std::thread([&, raw = c.get(), source, writer] {
        raw->run(source, writer, [&](Status s) {
            {
                const std::lock_guard lock(m);
                started = std::move(s);
            }
            cv.notify_all();
        });
    });
    std::unique_lock lock(m);
    cv.wait(lock, [&] { return started.has_value(); });
    if (!*started) {
        lock.unlock();
        c->stop();
        return started->error();
    }
    return c;
}

AudioCapture::~AudioCapture() { stop(); }

void AudioCapture::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

std::string AudioCapture::error() const {
    const std::lock_guard lock(mx_);
    return error_;
}

void AudioCapture::run(CaptureSource source, std::shared_ptr<TrackWriter> writer, std::function<void(Status)> started) {
    ComInit com;
    DWORD flags = 0;
    auto client = open_client(source, flags);
    if (!client) return started(client.error());
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = kRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = kRate * 2;
    HRESULT hr = (*client)->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2'000'000 /* 200 ms */, 0, &fmt, nullptr);
    if (FAILED(hr)) return started(make_error("audio", hr_text("the audio stream cannot be initialised", hr)));
    unique_handle ready(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    com_ptr<IAudioCaptureClient> cap;
    if (FAILED(hr = (*client)->SetEventHandle(ready.get())) ||
        FAILED(hr = (*client)->GetService(kIAudioCaptureClient, cap.put_void())) || FAILED(hr = (*client)->Start()))
        return started(make_error("audio", hr_text("the audio stream cannot be started", hr)));
    started({});
    const long long t0 = steady_ms();
    std::uint64_t base = writer->samples();
    while (!stop_) {
        ::WaitForSingleObject(ready.get(), 100);
        UINT32 packet = 0;
        while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD bflags = 0;
            if (FAILED(hr = cap->GetBuffer(&data, &frames, &bflags, nullptr, nullptr))) break;
            Status s = (bflags & AUDCLNT_BUFFERFLAGS_SILENT) ? writer->append_silence(frames)
                                                             : writer->append(reinterpret_cast<const std::int16_t*>(data), frames);
            cap->ReleaseBuffer(frames);
            if (!s) {
                const std::lock_guard lock(mx_);
                error_ = s.error().message;
                stop_ = true;
                break;
            }
        }
        if (FAILED(hr)) {
            const std::lock_guard lock(mx_);
            error_ = hr_text("the audio stream failed", hr);
            break;
        }
        // Loopback streams deliver nothing while the app is silent: fill the gap so both tracks keep the meeting's time.
        const auto expected = base + static_cast<std::uint64_t>(steady_ms() - t0) * kRate / 1000;
        if (const auto have = writer->samples(); have + kRate / 2 < expected) (void)writer->append_silence(static_cast<size_t>(expected - have));
    }
    (*client)->Stop();
    (void)writer->close();
}

// ---- detector probe ----------------------------------------------------------------------------------------------------

namespace {

std::vector<Process> process_table() {
    std::vector<Process> out;
    unique_handle snap(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap || snap.get() == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL ok = ::Process32FirstW(snap.get(), &pe); ok; ok = ::Process32NextW(snap.get(), &pe))
        out.push_back({pe.th32ProcessID, pe.th32ParentProcessID, narrow(pe.szExeFile)});
    return out;
}

std::vector<unsigned> active_session_pids() {
    std::vector<unsigned> out;
    com_ptr<IMMDeviceEnumerator> en;
    if (FAILED(::CoCreateInstance(kMMDeviceEnumerator, nullptr, CLSCTX_INPROC_SERVER, kIMMDeviceEnumerator, en.put_void()))) return out;
    com_ptr<IMMDeviceCollection> devices;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put()))) return out;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        com_ptr<IMMDevice> dev;
        com_ptr<IAudioSessionManager2> mgr;
        com_ptr<IAudioSessionEnumerator> sessions;
        if (FAILED(devices->Item(i, dev.put())) ||
            FAILED(dev->Activate(kIAudioSessionManager2, CLSCTX_INPROC_SERVER, nullptr, mgr.put_void())) ||
            FAILED(mgr->GetSessionEnumerator(sessions.put())))
            continue;
        int n = 0;
        sessions->GetCount(&n);
        for (int k = 0; k < n; ++k) {
            com_ptr<IAudioSessionControl> ctl;
            com_ptr<IAudioSessionControl2> ctl2;
            AudioSessionState state = AudioSessionStateInactive;
            DWORD pid = 0;
            if (FAILED(sessions->GetSession(k, ctl.put())) || FAILED(ctl.as(ctl2, kIAudioSessionControl2)) ||
                FAILED(ctl2->GetState(&state)) || state != AudioSessionStateActive || FAILED(ctl2->GetProcessId(&pid)) || pid == 0)
                continue;
            out.push_back(pid);
        }
    }
    return out;
}

// CapabilityAccessManager (design D3): an app is using the microphone while its LastUsedTimeStop is 0. Read only.
bool consent_store_in_use(HKEY parent) {
    for (DWORD i = 0;; ++i) {
        wchar_t name[512];
        DWORD len = 512;
        if (::RegEnumKeyExW(parent, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) return false;
        if (std::wcscmp(name, L"NonPackaged") == 0) {
            unique_hkey sub;
            if (::RegOpenKeyExW(parent, name, 0, KEY_READ, sub.put()) == ERROR_SUCCESS && consent_store_in_use(sub.get())) return true;
            continue;
        }
        ULONGLONG start = 0, stop = 1;
        DWORD sz = sizeof start;
        if (::RegGetValueW(parent, name, L"LastUsedTimeStart", RRF_RT_REG_QWORD, nullptr, &start, &sz) != ERROR_SUCCESS) continue;
        sz = sizeof stop;
        if (::RegGetValueW(parent, name, L"LastUsedTimeStop", RRF_RT_REG_QWORD, nullptr, &stop, &sz) != ERROR_SUCCESS) continue;
        if (start != 0 && stop == 0) return true;
    }
}

bool microphone_in_use() {
    unique_hkey k;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone",
                        0, KEY_READ, k.put()) != ERROR_SUCCESS)
        return false;
    return consent_store_in_use(k.get());
}

// Top-level window titles of a process, read through UI Automation (design D3), with the window text as fallback.
std::vector<std::string> window_titles(unsigned pid) {
    struct Ctx {
        unsigned pid;
        std::vector<HWND> windows;
    } ctx{pid, {}};
    ::EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(p);
            DWORD owner = 0;
            ::GetWindowThreadProcessId(h, &owner);
            if (owner == c->pid && ::IsWindowVisible(h)) c->windows.push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    std::vector<std::string> out;
    if (ctx.windows.empty()) return out;
    com_ptr<IUIAutomation> uia;
    (void)::CoCreateInstance(kCUIAutomation, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), uia.put_void());
    for (HWND h : ctx.windows) {
        std::wstring title;
        com_ptr<IUIAutomationElement> el;
        BSTR name = nullptr;
        if (uia && SUCCEEDED(uia->ElementFromHandle(h, el.put())) && el && SUCCEEDED(el->get_CurrentName(&name)) && name) {
            title = name;
            ::SysFreeString(name);
        }
        if (title.empty()) {
            wchar_t buf[512];
            const int n = ::GetWindowTextW(h, buf, 512);
            title.assign(buf, static_cast<size_t>(std::max(0, n)));
        }
        if (!title.empty()) out.push_back(narrow(title));
    }
    return out;
}

}  // namespace

Observation observe_machine() {
    ComInit com;
    Observation o;
    o.session_pids = active_session_pids();
    o.mic_in_use = microphone_in_use();
    o.processes = process_table();
    o.titles = [](unsigned pid) {
        ComInit c;
        return window_titles(pid);
    };
    return o;
}

bool microphone_present() {
    ComInit com;
    com_ptr<IMMDeviceEnumerator> en;
    if (FAILED(::CoCreateInstance(kMMDeviceEnumerator, nullptr, CLSCTX_INPROC_SERVER, kIMMDeviceEnumerator, en.put_void()))) return false;
    com_ptr<IMMDevice> dev;
    return SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eCommunications, dev.put()));
}

std::vector<Process> audio_processes() {
    ComInit com;
    const auto procs = process_table();
    std::vector<Process> out;
    for (const unsigned pid : active_session_pids())
        for (const auto& p : procs)
            if (p.pid == pid && std::none_of(out.begin(), out.end(), [&](const Process& x) { return x.pid == pid; })) out.push_back(p);
    return out;
}

// ---- indicator ---------------------------------------------------------------------------------------------------------

namespace {

constexpr wchar_t kIndicatorClass[] = L"WorkbenchRecordingIndicator";
constexpr UINT kStopId = 1;

struct IndicatorState {
    std::wstring label;
    long long started = 0;
    std::function<void()> on_stop;
};

LRESULT CALLBACK indicator_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<IndicatorState*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE:
        ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        break;
    case WM_TIMER: ::InvalidateRect(h, nullptr, TRUE); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == kStopId && st && st->on_stop) {
            ::EnableWindow(reinterpret_cast<HWND>(lp), FALSE);
            st->on_stop();
        }
        return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: {
        const LRESULT r = ::DefWindowProcW(h, msg, wp, lp);
        return r == HTCLIENT ? HTCAPTION : r;  // drag the indicator anywhere
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = ::BeginPaint(h, &ps);
        RECT rc;
        ::GetClientRect(h, &rc);
        HBRUSH bg = ::CreateSolidBrush(RGB(32, 32, 32));
        ::FillRect(dc, &rc, bg);
        ::DeleteObject(bg);
        HBRUSH red = ::CreateSolidBrush(RGB(220, 38, 38));
        HGDIOBJ old = ::SelectObject(dc, red);
        ::SelectObject(dc, ::GetStockObject(NULL_PEN));
        ::Ellipse(dc, 10, 13, 22, 25);
        ::SelectObject(dc, old);
        ::DeleteObject(red);
        if (st) {
            const long long s = (::GetTickCount64() - static_cast<ULONGLONG>(st->started)) / 1000;
            wchar_t text[300];
            std::swprintf(text, 300, L"%ls  %02lld:%02lld", st->label.c_str(), s / 60, s % 60);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, RGB(255, 255, 255));
            HGDIOBJ f = ::SelectObject(dc, ::GetStockObject(DEFAULT_GUI_FONT));
            RECT tr{30, 0, rc.right - 76, rc.bottom};
            ::DrawTextW(dc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            ::SelectObject(dc, f);
        }
        ::EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE: return 0;  // only Stop or the capture's end closes it
    case WM_DESTROY: ::PostQuitMessage(0); return 0;
    default: break;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

Indicator::Indicator(Options o, std::function<void()> on_stop) : on_stop_(std::move(on_stop)) {
    started_ = static_cast<long long>(::GetTickCount64());
    std::promise<void> shown;
    auto ready = shown.get_future();
    thread_ = std::thread([this, o = std::move(o), p = std::move(shown)]() mutable { run(std::move(o), p); });
    ready.wait();  // run() publishes the window (or its failure) before its message loop
}

Indicator::~Indicator() {
    if (thread_id_) ::PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    if (thread_.joinable()) thread_.join();
}

void Indicator::run(Options o, std::promise<void>& shown) {
    thread_id_ = ::GetCurrentThreadId();
    MSG msg;
    ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // the thread's message queue exists before anyone posts
    HINSTANCE inst = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = indicator_proc;
    wc.hInstance = inst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kIndicatorClass;
    ::RegisterClassExW(&wc);  // fails harmlessly when already registered
    IndicatorState st{o.label, started_, on_stop_};
    constexpr int w = 300, hgt = 38;
    int x = 0, y = 0;
    if (o.position) {
        x = o.position->first;
        y = o.position->second;
    } else {
        RECT work{};
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        x = work.left + (work.right - work.left - w) / 2;
        y = work.top + 8;
    }
    // Owned by no window: closing or hiding the main window leaves it on screen (Recording Indicator And Stop).
    HWND h = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kIndicatorClass, L"SkoWork recording",
                               WS_POPUP | WS_BORDER, x, y, w, hgt, nullptr, nullptr, inst, &st);
    if (!h) {
        shown.set_value();
        return;
    }
    HWND stop = ::CreateWindowExW(0, L"BUTTON", o.stop_text.c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, w - 72, 6, 64, 24, h,
                                  reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kStopId)), inst, nullptr);
    ::SendMessageW(stop, WM_SETFONT, reinterpret_cast<WPARAM>(::GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    ::SetTimer(h, 1, 1000, nullptr);
    ::ShowWindow(h, SW_SHOWNOACTIVATE);
    stop_ = stop;
    hwnd_ = h;
    shown.set_value();
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    hwnd_ = nullptr;
    stop_ = nullptr;
    ::KillTimer(h, 1);
    ::SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    ::DestroyWindow(h);
}

}  // namespace wb::meeting
