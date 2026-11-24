// Meeting-audio probe (diagnostics, not shipped). Run it during a call:
//   audio_probe.exe [seconds-per-target]
// 1. Lists every active render session on every active output device: pid, process, parent chain, device.
// 2. Records each candidate for N seconds and prints its level:
//    - process loopback (include tree) of each session pid and of the outermost ms-teams.exe / Teams.exe,
//    - endpoint loopback of the default console and the default communications output device.
// This shows which process actually plays the call audio and which capture path hears it.
#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <tlhelp32.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

enum AUDIOCLIENT_ACTIVATION_TYPE { AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0, AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1 };
enum PROCESS_LOOPBACK_MODE { PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0, PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1 };
struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS { DWORD TargetProcessId; PROCESS_LOOPBACK_MODE ProcessLoopbackMode; };
struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union { AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams; };
};
static const CLSID kEnum = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const IID kIEnum = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const IID kIClient = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
static const IID kICapture = {0xc8adbd64, 0xe71e, 0x48a0, {0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17}};
static const IID kIMgr2 = {0x77aa99a0, 0x1bd6, 0x484f, {0x8b, 0xc7, 0x2c, 0x65, 0x4c, 0x9a, 0x9b, 0x6f}};
static const IID kICtl2 = {0xbfb7ff88, 0x7239, 0x4fc9, {0x8f, 0xa2, 0x07, 0xc9, 0x50, 0xbe, 0x9c, 0x6d}};
static const IID kIHandler = {0x41d949ab, 0x9862, 0x444a, {0x80, 0xf6, 0xc2, 0x61, 0x33, 0x4d, 0xa5, 0xeb}};
static const IID kIAgile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

struct Handler : IActivateAudioInterfaceCompletionHandler, IAgileObject {
    std::atomic<ULONG> refs{1};
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_FAIL;
    IAudioClient* client = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
        if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, kIHandler)) *p = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (IsEqualIID(r, kIAgile)) *p = static_cast<IAgileObject*>(this);
        else { *p = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --refs; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        IUnknown* unk = nullptr;
        HRESULT act = E_FAIL;
        HRESULT hr = op->GetActivateResult(&act, &unk);
        result = FAILED(hr) ? hr : act;
        if (SUCCEEDED(result) && unk) unk->QueryInterface(kIClient, reinterpret_cast<void**>(&client));
        if (unk) unk->Release();
        ::SetEvent(done);
        return S_OK;
    }
};

struct Proc { DWORD parent; std::string name; };
static std::map<DWORD, Proc> procs() {
    std::map<DWORD, Proc> m;
    HANDLE s = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL ok = ::Process32FirstW(s, &pe); ok; ok = ::Process32NextW(s, &pe)) {
        char n[MAX_PATH];
        ::WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, n, sizeof n, nullptr, nullptr);
        m[pe.th32ProcessID] = {pe.th32ParentProcessID, n};
    }
    ::CloseHandle(s);
    return m;
}
static std::string chain(DWORD pid, const std::map<DWORD, Proc>& p) {
    std::string out;
    std::set<DWORD> seen;
    for (DWORD c = pid; c && !seen.count(c) && p.count(c); c = p.at(c).parent) {
        seen.insert(c);
        if (!out.empty()) out += " <- ";
        out += p.at(c).name + "(" + std::to_string(c) + ")";
    }
    return out;
}
static std::string narrow(const wchar_t* w) {
    char b[512];
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr);
    return b;
}

// Simultaneous mode: every candidate is started first, then all are read in one loop over the same seconds.
struct Stream { std::string label; IAudioClient* c = nullptr; IAudioCaptureClient* cap = nullptr; double sum = 0;
                unsigned long long n = 0, packets = 0; int peak = 0; };
static std::vector<Stream> g_streams;
static bool g_simultaneous = false;

static void add_stream(const std::string& label, IAudioClient* c) {
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 1; f.nSamplesPerSec = 16000; f.wBitsPerSample = 16;
    f.nBlockAlign = 2; f.nAvgBytesPerSec = 32000;
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &f, nullptr);
    if (FAILED(hr)) { std::printf("  %-58s Initialize failed 0x%08lx\n", label.c_str(), hr); return; }
    c->AddRef();
    Stream s; s.label = label; s.c = c;
    c->GetService(kICapture, reinterpret_cast<void**>(&s.cap));
    g_streams.push_back(s);
}

static void run_streams(int secs) {
    for (auto& s : g_streams) s.c->Start();
    const ULONGLONG end = ::GetTickCount64() + secs * 1000ull;
    while (::GetTickCount64() < end) {
        ::Sleep(20);
        for (auto& s : g_streams) {
            UINT32 avail = 0;
            while (SUCCEEDED(s.cap->GetNextPacketSize(&avail)) && avail) {
                BYTE* d; UINT32 fr; DWORD fl;
                if (FAILED(s.cap->GetBuffer(&d, &fr, &fl, nullptr, nullptr))) break;
                ++s.packets;
                if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT))
                    for (UINT32 i = 0; i < fr; ++i) { int v = reinterpret_cast<short*>(d)[i]; s.sum += double(v) * v; if (std::abs(v) > s.peak) s.peak = std::abs(v); }
                s.n += fr;
                s.cap->ReleaseBuffer(fr);
            }
        }
    }
    for (auto& s : g_streams) {
        s.c->Stop();
        const double rms = s.n ? std::sqrt(s.sum / s.n) : 0;
        std::printf("  %-58s packets=%-5llu seconds=%5.1f rms=%6.1f dBFS peak=%d\n", s.label.c_str(), s.packets, s.n / 16000.0,
                    rms < 1 ? -99.0 : 20 * std::log10(rms / 32768), s.peak);
    }
}

// Record from an initialised loopback client for `secs` seconds and report the level.
static void measure(const char* label, IAudioClient* c, bool event_flag, int secs) {
    if (g_simultaneous) { add_stream(label, c); return; }
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 1; f.nSamplesPerSec = 16000; f.wBitsPerSample = 16;
    f.nBlockAlign = 2; f.nAvgBytesPerSec = 32000;
    DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (event_flag) flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &f, nullptr);
    if (FAILED(hr)) { std::printf("  %-58s Initialize failed 0x%08lx\n", label, hr); return; }
    HANDLE ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_flag) c->SetEventHandle(ev);
    IAudioCaptureClient* cap = nullptr;
    c->GetService(kICapture, reinterpret_cast<void**>(&cap));
    c->Start();
    double sum = 0; unsigned long long n = 0, packets = 0, silent = 0; int peak = 0;
    const ULONGLONG end = ::GetTickCount64() + secs * 1000ull;
    while (::GetTickCount64() < end) {
        if (event_flag) ::WaitForSingleObject(ev, 100); else ::Sleep(20);
        UINT32 avail = 0;
        while (SUCCEEDED(cap->GetNextPacketSize(&avail)) && avail) {
            BYTE* d; UINT32 fr; DWORD fl;
            if (FAILED(cap->GetBuffer(&d, &fr, &fl, nullptr, nullptr))) break;
            ++packets;
            if (fl & AUDCLNT_BUFFERFLAGS_SILENT) ++silent;
            else for (UINT32 i = 0; i < fr; ++i) { int v = reinterpret_cast<short*>(d)[i]; sum += double(v) * v; if (std::abs(v) > peak) peak = std::abs(v); }
            n += fr;
            cap->ReleaseBuffer(fr);
        }
    }
    c->Stop();
    const double rms = n ? std::sqrt(sum / n) : 0;
    std::printf("  %-58s packets=%-5llu silent=%-5llu seconds=%5.1f rms=%6.1f dBFS peak=%d\n", label, packets, silent, n / 16000.0,
                rms < 1 ? -99.0 : 20 * std::log10(rms / 32768), peak);
    cap->Release();
    ::CloseHandle(ev);
}

static void probe_process(DWORD pid, const std::string& label, int secs) {
    AUDIOCLIENT_ACTIVATION_PARAMS p{};
    p.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    p.ProcessLoopbackParams.TargetProcessId = pid;
    p.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB; pv.blob.cbSize = sizeof p; pv.blob.pBlobData = reinterpret_cast<BYTE*>(&p);
    auto* h = new Handler();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ::ActivateAudioInterfaceAsync(L"VAD\\Process_Loopback", kIClient, &pv, h, &op);
    if (SUCCEEDED(hr)) { ::WaitForSingleObject(h->done, 5000); hr = h->result; }
    if (FAILED(hr) || !h->client) std::printf("  %-58s activation failed 0x%08lx\n", label.c_str(), hr);
    else { measure(label.c_str(), h->client, true, secs); h->client->Release(); }
    if (op) op->Release();
    h->Release();
}

static void probe_endpoint(IMMDeviceEnumerator* en, ERole role, const char* label, int secs) {
    IMMDevice* d = nullptr;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, role, &d))) { std::printf("  %-58s no device\n", label); return; }
    IPropertyStore* ps = nullptr; PROPVARIANT name; PropVariantInit(&name);
    d->OpenPropertyStore(STGM_READ, &ps);
    if (ps) ps->GetValue(PKEY_Device_FriendlyName, &name);
    std::string l = std::string(label) + " [" + (name.vt == VT_LPWSTR ? narrow(name.pwszVal) : "?") + "]";
    IAudioClient* c = nullptr;
    if (SUCCEEDED(d->Activate(kIClient, CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(&c)))) { measure(l.c_str(), c, false, secs); c->Release(); }
    if (ps) ps->Release();
    d->Release();
}

int main(int argc, char** argv) {
    const int secs = argc > 1 ? std::atoi(argv[1]) : 4;
    g_simultaneous = argc > 2 && std::string(argv[2]) == "--together";
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    ::CoCreateInstance(kEnum, nullptr, CLSCTX_INPROC_SERVER, kIEnum, reinterpret_cast<void**>(&en));
    const auto p = procs();
    std::printf("Render sessions (all active output devices):\n");
    std::set<DWORD> candidates;
    IMMDeviceCollection* devs = nullptr;
    en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devs);
    UINT count = 0;
    devs->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* d = nullptr; devs->Item(i, &d);
        IPropertyStore* ps = nullptr; PROPVARIANT name; PropVariantInit(&name);
        d->OpenPropertyStore(STGM_READ, &ps);
        if (ps) ps->GetValue(PKEY_Device_FriendlyName, &name);
        IAudioSessionManager2* mgr = nullptr; IAudioSessionEnumerator* se = nullptr;
        if (FAILED(d->Activate(kIMgr2, CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(&mgr))) || FAILED(mgr->GetSessionEnumerator(&se))) continue;
        int n = 0; se->GetCount(&n);
        for (int k = 0; k < n; ++k) {
            IAudioSessionControl* ctl = nullptr; IAudioSessionControl2* c2 = nullptr;
            se->GetSession(k, &ctl);
            ctl->QueryInterface(kICtl2, reinterpret_cast<void**>(&c2));
            AudioSessionState st; DWORD pid = 0;
            c2->GetState(&st); c2->GetProcessId(&pid);
            const char* sn = st == AudioSessionStateActive ? "ACTIVE" : st == AudioSessionStateInactive ? "inactive" : "expired";
            if (pid) std::printf("  [%s] %-8s %s\n", name.vt == VT_LPWSTR ? narrow(name.pwszVal).c_str() : "?", sn, chain(pid, p).c_str());
            if (pid && st == AudioSessionStateActive) candidates.insert(pid);
            c2->Release(); ctl->Release();
        }
        se->Release(); mgr->Release(); d->Release();
    }
    // The outermost Teams process, as Skowork's detector picks it.
    for (const auto& [pid, pr] : p)
        if ((_stricmp(pr.name.c_str(), "ms-teams.exe") == 0 || _stricmp(pr.name.c_str(), "Teams.exe") == 0) &&
            !(p.count(pr.parent) && _stricmp(p.at(pr.parent).name.c_str(), pr.name.c_str()) == 0))
            candidates.insert(pid);
    std::printf("\nLevels (%d s each; speech is typically -50..-15 dBFS, -99 = digital silence):\n", secs);
    for (DWORD pid : candidates) probe_process(pid, "process loopback " + chain(pid, p).substr(0, 40), secs);
    probe_endpoint(en, eConsole, "endpoint loopback, default console", secs);
    probe_endpoint(en, eCommunications, "endpoint loopback, default communications", secs);
    if (g_simultaneous) run_streams(secs);
    return 0;
}
