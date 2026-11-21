#include "mail/com_outlook_session.h"

#include "base/strings.h"
#include "base/time.h"
#include "platform/log.h"
#include "platform/win.h"

#include <objbase.h>
#include <oleauto.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace wb::mail::outlook {
namespace {

constexpr char kLog[] = "outlook";
constexpr auto kCallLimit = std::chrono::seconds(60);
constexpr size_t kMaxBody = 1 << 20;
constexpr int kMaxFolders = 300;

// Outlook object model constants (OlDefaultFolders, OlItemType, OlMeetingResponse, OlObjectClass).
enum : int {
    olFolderDeletedItems = 3, olFolderOutbox = 4, olFolderSentMail = 5, olFolderInbox = 6, olFolderCalendar = 9,
    olFolderDrafts = 16, olFolderJunk = 23,
    olMailItem = 0, olAppointmentItem = 1,
    olMeetingTentative = 2, olMeetingAccepted = 3, olMeetingDeclined = 4,
    olTo = 1, olCC = 2, olBCC = 3, olRequired = 1, olOptional = 2, olResource = 3,
    olMeeting = 1, olMailClass = 43, olAppointmentClass = 26,
};

constexpr const wchar_t* kLabelSchema = L"http://schemas.microsoft.com/mapi/string/{00020386-0000-0000-C000-000000000046}/msip_labels";
constexpr const wchar_t* kSmtpSchema = L"http://schemas.microsoft.com/mapi/proptag/0x39FE001F";
constexpr const wchar_t* kTeamsUrlSchema = L"http://schemas.microsoft.com/mapi/string/{00020329-0000-0000-C000-000000000046}/SkypeTeamsMeetingUrl";

// MAPI_E_NOT_FOUND, as Outlook reports an unknown EntryID.
constexpr HRESULT kNotFound = static_cast<HRESULT>(0x8004010FL);

struct ComError {
    HRESULT hr;
    std::string what;
};

bool busy(HRESULT hr) { return hr == RPC_E_CALL_REJECTED || hr == RPC_E_SERVERCALL_RETRYLATER; }
bool gone(HRESULT hr) {
    return hr == RPC_E_DISCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED) ||
           hr == CO_E_OBJNOTCONNECTED;
}

Error to_error(const ComError& e) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08lX", static_cast<unsigned long>(e.hr));
    const std::string detail = e.what + " (" + hex + ")";
    if (busy(e.hr)) return make_error("throttled", "Outlook is busy (a dialog may be open): " + detail);
    if (gone(e.hr)) return make_error("offline", "Outlook is not running: " + detail);
    if (e.hr == E_ABORT || e.hr == E_ACCESSDENIED) return make_error("access_denied", "Outlook refused the request: " + detail);
    if (e.hr == kNotFound) return make_error("not_found", "ErrorItemNotFound: " + detail);
    return make_error("bad_request", "Outlook: " + detail);
}

// ---- VARIANT and IDispatch helpers ----

struct Var {
    VARIANT v;
    Var() { VariantInit(&v); }
    ~Var() { VariantClear(&v); }
    Var(const Var&) = delete;
    Var& operator=(const Var&) = delete;
    Var(Var&& o) noexcept {
        v = o.v;
        VariantInit(&o.v);
    }
};

class Disp {
public:
    Disp() = default;
    explicit Disp(IDispatch* p, bool add_ref = false) : p_(p) {
        if (p_ && add_ref) p_->AddRef();
    }
    ~Disp() {
        if (p_) p_->Release();
    }
    Disp(const Disp& o) : p_(o.p_) {
        if (p_) p_->AddRef();
    }
    Disp& operator=(Disp o) {
        std::swap(p_, o.p_);
        return *this;
    }
    explicit operator bool() const { return p_ != nullptr; }
    IDispatch* get() const { return p_; }

    Var invoke(const wchar_t* name, WORD flags, std::vector<VARIANT> args = {}) const {
        if (!p_) throw ComError{E_POINTER, "no object for " + narrow(name)};
        DISPID id = 0;
        LPOLESTR n = const_cast<LPOLESTR>(name);
        HRESULT hr = p_->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id);
        if (FAILED(hr)) throw ComError{hr, "no member " + narrow(name)};
        std::reverse(args.begin(), args.end());  // IDispatch takes arguments right to left
        DISPPARAMS dp{};
        dp.cArgs = static_cast<UINT>(args.size());
        dp.rgvarg = args.empty() ? nullptr : args.data();
        DISPID put = DISPID_PROPERTYPUT;
        if (flags & DISPATCH_PROPERTYPUT) {
            dp.cNamedArgs = 1;
            dp.rgdispidNamedArgs = &put;
        }
        Var out;
        EXCEPINFO ei{};
        hr = p_->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, &dp, (flags & DISPATCH_PROPERTYPUT) ? nullptr : &out.v, &ei, nullptr);
        std::string desc;
        if (hr == DISP_E_EXCEPTION) {
            if (ei.bstrDescription) desc = narrow(ei.bstrDescription);
            hr = ei.scode ? ei.scode : E_FAIL;
        }
        if (ei.bstrSource) SysFreeString(ei.bstrSource);
        if (ei.bstrDescription) SysFreeString(ei.bstrDescription);
        if (ei.bstrHelpFile) SysFreeString(ei.bstrHelpFile);
        for (auto& a : args) VariantClear(&a);
        if (FAILED(hr)) throw ComError{hr, narrow(name) + (desc.empty() ? "" : ": " + desc)};
        return out;
    }

    Var get_(const wchar_t* name, std::vector<VARIANT> args = {}) const { return invoke(name, DISPATCH_PROPERTYGET | DISPATCH_METHOD, std::move(args)); }
    Var call(const wchar_t* name, std::vector<VARIANT> args = {}) const { return invoke(name, DISPATCH_METHOD, std::move(args)); }
    void put(const wchar_t* name, VARIANT value) const { invoke(name, DISPATCH_PROPERTYPUT, {value}); }

    Disp obj(const wchar_t* name, std::vector<VARIANT> args = {}) const { return as_disp(get_(name, std::move(args))); }
    std::string str(const wchar_t* name) const { return as_str(get_(name)); }
    long num(const wchar_t* name) const { return as_long(get_(name)); }
    bool flag(const wchar_t* name) const { return as_long(get_(name)) != 0; }
    double date(const wchar_t* name) const { return as_date(get_(name)); }

    // A property that may be missing on this item class: the fallback instead of an error.
    std::string try_str(const wchar_t* name) const {
        try {
            return str(name);
        } catch (const ComError& e) {
            if (gone(e.hr) || busy(e.hr)) throw;
            return {};
        }
    }

    static Disp as_disp(Var v) {
        if (v.v.vt == VT_DISPATCH && v.v.pdispVal) return Disp(v.v.pdispVal, true);
        if (v.v.vt == VT_UNKNOWN && v.v.punkVal) {
            IDispatch* d = nullptr;
            if (SUCCEEDED(v.v.punkVal->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&d)))) return Disp(d);
        }
        return Disp();
    }
    static std::string as_str(const Var& v) {
        if (v.v.vt == VT_BSTR) return v.v.bstrVal ? narrow(v.v.bstrVal) : std::string{};
        Var c;
        if (v.v.vt == VT_EMPTY || v.v.vt == VT_NULL) return {};
        if (FAILED(VariantChangeType(&c.v, const_cast<VARIANT*>(&v.v), 0, VT_BSTR)) || !c.v.bstrVal) return {};
        return narrow(c.v.bstrVal);
    }
    static long as_long(const Var& v) {
        if (v.v.vt == VT_I4) return v.v.lVal;
        if (v.v.vt == VT_BOOL) return v.v.boolVal ? 1 : 0;
        Var c;
        if (FAILED(VariantChangeType(&c.v, const_cast<VARIANT*>(&v.v), 0, VT_I4))) return 0;
        return c.v.lVal;
    }
    static double as_date(const Var& v) {
        if (v.v.vt == VT_DATE) return v.v.date;
        Var c;
        if (FAILED(VariantChangeType(&c.v, const_cast<VARIANT*>(&v.v), 0, VT_DATE))) return 0;
        return c.v.date;
    }

private:
    IDispatch* p_ = nullptr;
};

VARIANT vstr(const std::string& s) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(widen(s).c_str());
    return v;
}
VARIANT vint(long n) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = n;
    return v;
}
VARIANT vbool(bool b) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
    return v;
}
VARIANT vdate(double d) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_DATE;
    v.date = d;
    return v;
}

// ---- time: OLE DATE (local or UTC wall clock) <-> unix ms ----

constexpr double kOleEpochToUnixDays = 25569.0;  // 1970-01-01 as an OLE date

long long wall_to_ms(double d) { return static_cast<long long>((d - kOleEpochToUnixDays) * 86'400'000.0 + (d >= 0 ? 0.5 : -0.5)); }
double ms_to_wall(long long ms) { return static_cast<double>(ms) / 86'400'000.0 + kOleEpochToUnixDays; }

// Local wall-clock DATE to UTC unix ms with the rules in force at that moment (DST correct).
long long local_to_utc_ms(double local) {
    if (local == 0) return 0;
    SYSTEMTIME lt{}, ut{};
    if (!VariantTimeToSystemTime(local, &lt)) return 0;
    DYNAMIC_TIME_ZONE_INFORMATION tz{};
    GetDynamicTimeZoneInformation(&tz);
    if (!TzSpecificLocalTimeToSystemTimeEx(&tz, &lt, &ut)) return wall_to_ms(local);
    double u = 0;
    SystemTimeToVariantTime(&ut, &u);
    return wall_to_ms(u);
}

double utc_ms_to_local(long long ms) {
    SYSTEMTIME ut{}, lt{};
    if (!VariantTimeToSystemTime(ms_to_wall(ms), &ut)) return 0;
    DYNAMIC_TIME_ZONE_INFORMATION tz{};
    GetDynamicTimeZoneInformation(&tz);
    if (!SystemTimeToTzSpecificLocalTimeEx(&tz, &ut, &lt)) return ms_to_wall(ms);
    double l = 0;
    SystemTimeToVariantTime(&lt, &l);
    return l;
}

// A local date for Jet Restrict filters, in the user's locale (design D2: Outlook parses filter dates in that locale).
std::string jet_date(double local) {
    BSTR b = nullptr;
    if (FAILED(VarBstrFromDate(local, LOCALE_USER_DEFAULT, 0, &b)) || !b) return {};
    std::string s = narrow(b);
    SysFreeString(b);
    return s;
}

std::string local_stamp(double local) {
    SYSTEMTIME t{};
    VariantTimeToSystemTime(local, &t);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04u%02u%02uT%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    return buf;
}

std::optional<double> parse_stamp(const std::string& s) {
    if (s.size() != 13 || s[8] != 'T') return std::nullopt;
    SYSTEMTIME t{};
    try {
        t.wYear = static_cast<WORD>(std::stoi(s.substr(0, 4)));
        t.wMonth = static_cast<WORD>(std::stoi(s.substr(4, 2)));
        t.wDay = static_cast<WORD>(std::stoi(s.substr(6, 2)));
        t.wHour = static_cast<WORD>(std::stoi(s.substr(9, 2)));
        t.wMinute = static_cast<WORD>(std::stoi(s.substr(11, 2)));
    } catch (...) {
        return std::nullopt;
    }
    double d = 0;
    if (!SystemTimeToVariantTime(&t, &d)) return std::nullopt;
    return d;
}

std::string quote_dasl(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '\'') out += "''";
        else if (c == '%' || c == '[' || c == '_') continue;  // LIKE wildcards are dropped, not escaped
        else out += c;
    }
    return out;
}

std::vector<std::string> split_categories(const std::string& s) {
    std::vector<std::string> out;
    for (auto part : split(s, s.find(';') != std::string::npos ? ';' : ',')) {
        part = trim(part);
        if (!part.empty()) out.push_back(part);
    }
    return out;
}

std::string content_type_of(const std::string& name) {
    const std::string n = to_lower(name);
    const auto ends = [&](const char* s) { return ends_with(n, s); };
    if (ends(".pdf")) return "application/pdf";
    if (ends(".docx")) return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    if (ends(".xlsx")) return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    if (ends(".pptx")) return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
    if (ends(".txt") || ends(".csv") || ends(".md")) return "text/plain";
    if (ends(".png")) return "image/png";
    if (ends(".jpg") || ends(".jpeg")) return "image/jpeg";
    return "application/octet-stream";
}

std::string teams_link_in(const std::string& body) {
    const auto p = body.find("https://teams.microsoft.com/l/meetup-join/");
    if (p == std::string::npos) return {};
    size_t e = p;
    while (e < body.size() && !std::isspace(static_cast<unsigned char>(body[e])) && body[e] != '>' && body[e] != '"') ++e;
    return body.substr(p, e - p);
}

}  // namespace

bool outlook_installed() {
    CLSID clsid;
    return SUCCEEDED(CLSIDFromProgID(L"Outlook.Application", &clsid));
}

// ---- the STA worker ----

struct ComOutlookSession::Impl {
    std::function<bool()> may_start;
    std::mutex mx;
    std::condition_variable cv;
    std::deque<std::function<void()>> queue;
    bool stopping = false;
    std::thread worker;
    // Worker-thread state.
    Disp app, ns;

    explicit Impl(std::function<bool()> start) : may_start(std::move(start)) {
        worker = std::thread([this] { loop(); });
    }
    ~Impl() {
        {
            const std::lock_guard lock(mx);
            stopping = true;
        }
        cv.notify_all();
        // A call stuck inside Outlook cannot be interrupted: the thread is left to finish on its own.
        if (worker.joinable()) worker.detach();
    }

    void loop() {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock lock(mx);
                cv.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping && queue.empty()) break;
                job = std::move(queue.front());
                queue.pop_front();
            }
            job();
        }
        ns = Disp();
        app = Disp();
        if (SUCCEEDED(init)) CoUninitialize();
    }

    // Attaches to Outlook on the worker thread.
    void attach() {
        if (app && ns) return;
        CLSID clsid;
        HRESULT hr = CLSIDFromProgID(L"Outlook.Application", &clsid);
        if (FAILED(hr)) throw ComError{hr, "classic Outlook is not installed"};
        IUnknown* unk = nullptr;
        hr = GetActiveObject(clsid, nullptr, &unk);
        if (FAILED(hr) || !unk) {
            if (!(may_start && may_start())) throw ComError{HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE), "Outlook is not running"};
            hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_IUnknown, reinterpret_cast<void**>(&unk));
            if (FAILED(hr) || !unk) throw ComError{FAILED(hr) ? hr : E_FAIL, "Outlook could not be started"};
        }
        IDispatch* d = nullptr;
        hr = unk->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&d));
        unk->Release();
        if (FAILED(hr)) throw ComError{hr, "Outlook has no automation interface"};
        app = Disp(d);
        ns = app.obj(L"GetNamespace", {vstr("MAPI")});
        log::info(kLog, "attached to Outlook");
    }

    template <typename T>
    Result<T> run(std::function<T()> fn) {
        auto promise = std::make_shared<std::promise<Result<T>>>();
        auto future = promise->get_future();
        {
            const std::lock_guard lock(mx);
            queue.push_back([this, fn = std::move(fn), promise]() mutable {
                long long delay = 1000;
                for (int attempt = 1;; ++attempt) {
                    try {
                        attach();
                        promise->set_value(Result<T>(fn()));
                        return;
                    } catch (const ComError& e) {
                        if (gone(e.hr)) {
                            ns = Disp();
                            app = Disp();
                        }
                        if (busy(e.hr) && attempt < 5) {
                            log::info(kLog, "Outlook is busy; retrying in " + std::to_string(delay) + " ms");
                            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                            delay = std::min<long long>(delay * 2, 30'000);
                            continue;
                        }
                        promise->set_value(Result<T>(to_error(e)));
                        return;
                    } catch (const std::exception& ex) {
                        promise->set_value(Result<T>(make_error("bad_request", std::string("Outlook: ") + ex.what())));
                        return;
                    }
                }
            });
        }
        cv.notify_all();
        if (future.wait_for(kCallLimit) != std::future_status::ready)
            return make_error("offline", "Outlook did not answer within 60 seconds");
        return future.get();
    }

    Status run_status(std::function<void()> fn) {
        auto r = run<bool>([fn = std::move(fn)] {
            fn();
            return true;
        });
        if (!r) return r.error();
        return {};
    }

    // ---- worker-thread helpers ----

    Disp item(const std::string& id) {
        try {
            return ns.obj(L"GetItemFromID", {vstr(id)});
        } catch (const ComError& e) {
            if (gone(e.hr) || busy(e.hr)) throw;
            throw ComError{kNotFound, "no item with that id"};
        }
    }

    std::string smtp_of_entry(const Disp& entry) {
        if (!entry) return {};
        if (to_upper_ascii(entry.try_str(L"Type")) == "EX") {
            try {
                if (Disp u = entry.obj(L"GetExchangeUser")) {
                    const std::string a = u.str(L"PrimarySmtpAddress");
                    if (!a.empty()) return a;
                }
            } catch (const ComError& e) {
                if (gone(e.hr) || busy(e.hr)) throw;
            }
            try {
                Disp pa = entry.obj(L"PropertyAccessor");
                const std::string a = Disp::as_str(pa.call(L"GetProperty", {vstr(narrow(kSmtpSchema))}));
                if (!a.empty()) return a;
            } catch (const ComError& e) {
                if (gone(e.hr) || busy(e.hr)) throw;
            }
        }
        return entry.try_str(L"Address");
    }

    static std::string to_upper_ascii(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    std::string named_prop(const Disp& it, const wchar_t* schema) {
        try {
            Disp pa = it.obj(L"PropertyAccessor");
            return Disp::as_str(pa.call(L"GetProperty", {vstr(narrow(schema))}));
        } catch (const ComError& e) {
            if (gone(e.hr) || busy(e.hr)) throw;
            return {};  // the item has no such property
        }
    }

    OlAddress sender_of(const Disp& it) {
        OlAddress a;
        a.name = it.try_str(L"SenderName");
        const std::string type = to_upper_ascii(it.try_str(L"SenderEmailType"));
        if (type == "SMTP") a.address = it.try_str(L"SenderEmailAddress");
        if (a.address.empty()) {
            try {
                a.address = smtp_of_entry(it.obj(L"Sender"));
            } catch (const ComError& e) {
                if (gone(e.hr) || busy(e.hr)) throw;
            }
        }
        if (a.address.empty()) a.address = it.try_str(L"SenderEmailAddress");
        return a;
    }

    std::vector<OlRecipient> recipients_of(const Disp& it, bool meeting) {
        std::vector<OlRecipient> out;
        Disp rs = it.obj(L"Recipients");
        const long n = rs.num(L"Count");
        for (long i = 1; i <= n && i <= 500; ++i) {
            Disp r = rs.obj(L"Item", {vint(i)});
            OlRecipient x;
            x.who.name = r.try_str(L"Name");
            try {
                x.who.address = smtp_of_entry(r.obj(L"AddressEntry"));
            } catch (const ComError& e) {
                if (gone(e.hr) || busy(e.hr)) throw;
            }
            if (x.who.address.empty()) x.who.address = r.try_str(L"Address");
            const long t = r.num(L"Type");
            if (meeting) {
                x.type = t == olOptional ? "optional" : t == olResource ? "resource" : "required";
                switch (r.num(L"MeetingResponseStatus")) {
                    case 1: x.response = "organizer"; break;
                    case 2: x.response = "tentativelyAccepted"; break;
                    case 3: x.response = "accepted"; break;
                    case 4: x.response = "declined"; break;
                    case 5: x.response = "notResponded"; break;
                    default: x.response = "none";
                }
            } else {
                x.type = t == olCC ? "cc" : t == olBCC ? "bcc" : "to";
            }
            out.push_back(std::move(x));
        }
        return out;
    }

    OlMessage to_message(const Disp& it) {
        OlMessage m;
        m.id = it.str(L"EntryID");
        try {
            m.folder_id = it.obj(L"Parent").str(L"EntryID");
        } catch (const ComError& e) {
            if (gone(e.hr) || busy(e.hr)) throw;
        }
        m.conversation_id = it.try_str(L"ConversationID");
        m.subject = it.try_str(L"Subject");
        m.body = it.try_str(L"Body");
        if (m.body.size() > kMaxBody) m.body = utf8_truncate(m.body, kMaxBody);
        double received = 0;
        try {
            received = it.date(L"ReceivedTime");
        } catch (const ComError& e) {
            if (gone(e.hr) || busy(e.hr)) throw;
        }
        if (received <= 0 || received > 2958465) received = it.date(L"CreationTime");  // drafts have no received time
        m.received = local_to_utc_ms(received);
        m.modified = local_to_utc_ms(it.date(L"LastModificationTime"));
        m.from = sender_of(it);
        m.recipients = recipients_of(it, false);
        try {
            m.has_attachments = it.obj(L"Attachments").num(L"Count") > 0;
        } catch (const ComError& e) {
            if (gone(e.hr) || busy(e.hr)) throw;
        }
        try {
            m.is_read = !it.flag(L"UnRead");
        } catch (const ComError& e) {
            if (gone(e.hr) || busy(e.hr)) throw;
        }
        try {
            m.is_draft = !it.flag(L"Sent");
        } catch (const ComError& e) {
            if (gone(e.hr) || busy(e.hr)) throw;
        }
        m.categories = split_categories(it.try_str(L"Categories"));
        m.msip_labels = named_prop(it, kLabelSchema);
        return m;
    }

    OlEvent to_event(const Disp& it) {
        OlEvent e;
        const std::string entry = it.str(L"EntryID");
        const double start_local = it.date(L"Start");
        const long recurrence_state = it.num(L"RecurrenceState");  // 0 none, 1 master, 2 occurrence, 3 exception
        if (recurrence_state != 0) {
            e.series_id = entry;
            e.id = occurrence_id(entry, local_stamp(start_local));
        } else {
            e.id = entry;
        }
        e.global_id = it.try_str(L"GlobalAppointmentID");
        e.start = wall_to_ms(it.date(L"StartUTC"));
        e.end = wall_to_ms(it.date(L"EndUTC"));
        e.modified = local_to_utc_ms(it.date(L"LastModificationTime"));
        e.all_day = it.flag(L"AllDayEvent");
        const long status = it.num(L"MeetingStatus");  // 0 non-meeting, 1 meeting, 3 received, 5 cancelled, 7 received+cancelled
        e.is_meeting = status != 0;
        e.is_organizer = status == 0 || status == 1 || status == 5;
        e.cancelled = status == 5 || status == 7;
        e.subject = it.try_str(L"Subject");
        e.body = it.try_str(L"Body");
        if (e.body.size() > kMaxBody) e.body = utf8_truncate(e.body, kMaxBody);
        e.location = it.try_str(L"Location");
        e.organizer.name = it.try_str(L"Organizer");
        try {
            e.organizer.address = smtp_of_entry(it.obj(L"GetOrganizer"));
        } catch (const ComError& err) {
            if (gone(err.hr) || busy(err.hr)) throw;
        }
        if (e.is_meeting) e.attendees = recipients_of(it, true);
        switch (it.num(L"ResponseStatus")) {
            case 1: e.response = "organizer"; break;
            case 2: e.response = "tentativelyAccepted"; break;
            case 3: e.response = "accepted"; break;
            case 4: e.response = "declined"; break;
            case 5: e.response = "notResponded"; break;
            default: e.response = "none";
        }
        switch (it.num(L"BusyStatus")) {
            case 0: e.show_as = "free"; break;
            case 1: e.show_as = "tentative"; break;
            case 3: e.show_as = "oof"; break;
            case 4: e.show_as = "workingElsewhere"; break;
            default: e.show_as = "busy";
        }
        switch (it.num(L"Sensitivity")) {
            case 1: e.sensitivity = "personal"; break;
            case 2: e.sensitivity = "private"; break;
            case 3: e.sensitivity = "confidential"; break;
            default: e.sensitivity = "normal";
        }
        e.categories = split_categories(it.try_str(L"Categories"));
        e.msip_labels = named_prop(it, kLabelSchema);
        e.online_url = named_prop(it, kTeamsUrlSchema);
        if (e.online_url.empty()) e.online_url = teams_link_in(e.body);
        return e;
    }

    Disp default_folder(int which) { return ns.obj(L"GetDefaultFolder", {vint(which)}); }

    void set_recipients(const Disp& it, const std::optional<std::vector<std::string>>& to, const std::optional<std::vector<std::string>>& cc,
                        long to_type, long cc_type) {
        if (!to && !cc) return;
        Disp rs = it.obj(L"Recipients");
        for (long i = rs.num(L"Count"); i >= 1; --i) {
            const long t = rs.obj(L"Item", {vint(i)}).num(L"Type");
            if ((to && t == to_type) || (cc && t == cc_type)) rs.call(L"Remove", {vint(i)});
        }
        const auto add = [&](const std::vector<std::string>& list, long type) {
            for (const auto& a : list) {
                Disp r = rs.obj(L"Add", {vstr(a)});
                r.put(L"Type", vint(type));
            }
        };
        if (to) add(*to, to_type);
        if (cc) add(*cc, cc_type);
        rs.call(L"ResolveAll");
    }

    void apply(const Disp& it, const DraftPatch& p) {
        if (p.subject) it.put(L"Subject", vstr(*p.subject));
        if (p.body) it.put(L"Body", vstr(*p.body));
        set_recipients(it, p.to, p.cc, olTo, olCC);
    }

    std::vector<OlFolder> list_folders() {
        std::vector<OlFolder> out;
        std::map<std::string, std::string> known;
        const std::pair<int, const char*> wk[] = {{olFolderInbox, "inbox"},     {olFolderSentMail, "sentitems"}, {olFolderDrafts, "drafts"},
                                                  {olFolderOutbox, "outbox"},   {olFolderDeletedItems, "deleteditems"},
                                                  {olFolderJunk, "junkemail"}};
        for (const auto& [n, name] : wk) {
            try {
                known[default_folder(n).str(L"EntryID")] = name;
            } catch (const ComError& e) {
                if (gone(e.hr) || busy(e.hr)) throw;
            }
        }
        Disp root = default_folder(olFolderInbox).obj(L"Parent");
        std::vector<std::pair<Disp, int>> stack{{root, 0}};
        while (!stack.empty() && static_cast<int>(out.size()) < kMaxFolders) {
            auto [f, depth] = stack.back();
            stack.pop_back();
            Disp subs = f.obj(L"Folders");
            const long n = subs.num(L"Count");
            for (long i = 1; i <= n; ++i) {
                Disp s = subs.obj(L"Item", {vint(i)});
                if (s.num(L"DefaultItemType") != olMailItem) continue;  // calendars, contacts, tasks, notes
                OlFolder of;
                of.id = s.str(L"EntryID");
                of.name = s.str(L"Name");
                if (const auto it = known.find(of.id); it != known.end()) of.well_known = it->second;
                else if (iequals(of.name, "Archive")) of.well_known = "archive";
                out.push_back(of);
                if (depth < 4) stack.push_back({s, depth + 1});
            }
        }
        return out;
    }
};

ComOutlookSession::ComOutlookSession(std::function<bool()> may_start) : impl_(std::make_unique<Impl>(std::move(may_start))) {}
ComOutlookSession::~ComOutlookSession() = default;

std::string ComOutlookSession::state() {
    auto r = impl_->run<bool>([] { return true; });
    if (r) return "running";
    return r.error().code == "throttled" ? "busy" : "not_running";
}

Result<OlAddress> ComOutlookSession::me() {
    return impl_->run<OlAddress>([this] {
        auto& I = *impl_;
        OlAddress a;
        Disp user = I.ns.obj(L"CurrentUser");
        a.name = user.try_str(L"Name");
        try {
            a.address = I.smtp_of_entry(user.obj(L"AddressEntry"));
        } catch (const ComError& e) {
            if (gone(e.hr) || busy(e.hr)) throw;
        }
        if (a.address.empty() || a.address.find('@') == std::string::npos) {
            Disp accounts = I.ns.obj(L"Accounts");
            const long n = accounts.num(L"Count");
            for (long i = 1; i <= n; ++i) {
                const std::string smtp = accounts.obj(L"Item", {vint(i)}).try_str(L"SmtpAddress");
                if (!smtp.empty()) {
                    a.address = smtp;
                    break;
                }
            }
        }
        return a;
    });
}

Result<std::vector<OlFolder>> ComOutlookSession::folders() {
    return impl_->run<std::vector<OlFolder>>([this] { return impl_->list_folders(); });
}

Result<std::vector<OlRow>> ComOutlookSession::rows(const std::string& folder_id, long long received_since) {
    return impl_->run<std::vector<OlRow>>([this, folder_id, received_since] {
        auto& I = *impl_;
        Disp folder = I.ns.obj(L"GetFolderFromID", {vstr(folder_id)});
        // The table reads a whole folder without opening items (design D2).
        const std::string filter = received_since > 0 ? "[ReceivedTime] >= '" + jet_date(utc_ms_to_local(received_since)) + "'" : std::string{};
        Disp table = filter.empty() ? folder.obj(L"GetTable") : folder.obj(L"GetTable", {vstr(filter)});
        Disp cols = table.obj(L"Columns");
        cols.call(L"RemoveAll");
        cols.call(L"Add", {vstr("EntryID")});
        cols.call(L"Add", {vstr("LastModificationTime")});
        cols.call(L"Add", {vstr("ReceivedTime")});
        std::vector<OlRow> out;
        while (!table.flag(L"EndOfTable")) {
            Disp row = table.obj(L"GetNextRow");
            if (!row) break;
            OlRow r;
            r.id = Disp::as_str(row.call(L"Item", {vint(1)}));
            r.modified = local_to_utc_ms(Disp::as_date(row.call(L"Item", {vint(2)})));
            r.received = local_to_utc_ms(Disp::as_date(row.call(L"Item", {vint(3)})));
            if (r.id.empty()) continue;
            if (received_since > 0 && r.received > 0 && r.received < received_since) continue;  // the locale filter, checked in UTC
            out.push_back(std::move(r));
        }
        return out;
    });
}

Result<OlMessage> ComOutlookSession::message(const std::string& id) {
    return impl_->run<OlMessage>([this, id] { return impl_->to_message(impl_->item(id)); });
}

Result<std::vector<OlMessage>> ComOutlookSession::find(const MessageQuery& q) {
    return impl_->run<std::vector<OlMessage>>([this, q] {
        auto& I = *impl_;
        std::vector<Disp> folders;
        if (!q.folder_id.empty()) {
            folders.push_back(I.ns.obj(L"GetFolderFromID", {vstr(q.folder_id)}));
        } else if (!q.conversation_id.empty()) {
            folders.push_back(I.default_folder(olFolderInbox));
            folders.push_back(I.default_folder(olFolderSentMail));
        } else {
            for (const auto& f : I.list_folders())
                if (f.well_known != "deleteditems" && f.well_known != "junkemail" && f.well_known != "outbox")
                    folders.push_back(I.ns.obj(L"GetFolderFromID", {vstr(f.id)}));
        }
        std::vector<std::string> clauses;
        if (q.since) clauses.push_back("\"urn:schemas:httpmail:datereceived\" >= '" + jet_date(utc_ms_to_local(q.since)) + "'");
        if (q.until) clauses.push_back("\"urn:schemas:httpmail:datereceived\" < '" + jet_date(utc_ms_to_local(q.until)) + "'");
        if (q.attachments_only) clauses.push_back("\"urn:schemas:httpmail:hasattachment\" = 1");
        if (!q.text.empty()) {
            const std::string t = quote_dasl(q.text);
            clauses.push_back("(\"urn:schemas:httpmail:subject\" LIKE '%" + t + "%' OR \"urn:schemas:httpmail:textdescription\" LIKE '%" + t +
                              "%' OR \"urn:schemas:httpmail:fromname\" LIKE '%" + t + "%')");
        }
        std::string filter;
        for (const auto& c : clauses) filter += (filter.empty() ? "@SQL=" : " AND ") + c;
        std::vector<OlMessage> out;
        for (const auto& f : folders) {
            Disp items = f.obj(L"Items");
            items.call(L"Sort", {vstr("[ReceivedTime]"), vbool(true)});
            Disp found = filter.empty() ? items : items.obj(L"Restrict", {vstr(filter)});
            Disp it = found.obj(L"GetFirst");
            size_t scanned = 0;
            while (it && out.size() < q.limit * 2 && scanned < 5000) {
                ++scanned;
                if (!q.conversation_id.empty()) {
                    if (it.try_str(L"ConversationID") != q.conversation_id) {
                        it = found.obj(L"GetNext");
                        continue;
                    }
                }
                out.push_back(I.to_message(it));
                if (out.size() >= q.limit && q.conversation_id.empty()) break;
                it = found.obj(L"GetNext");
            }
        }
        return out;
    });
}

Result<std::vector<OlAttachment>> ComOutlookSession::attachments(const std::string& message_id) {
    return impl_->run<std::vector<OlAttachment>>([this, message_id] {
        Disp atts = impl_->item(message_id).obj(L"Attachments");
        std::vector<OlAttachment> out;
        const long n = atts.num(L"Count");
        for (long i = 1; i <= n; ++i) {
            Disp a = atts.obj(L"Item", {vint(i)});
            OlAttachment x;
            x.name = a.try_str(L"FileName");
            if (x.name.empty()) x.name = a.try_str(L"DisplayName");
            x.id = std::to_string(i);
            x.size = a.num(L"Size");
            x.content_type = content_type_of(x.name);
            out.push_back(std::move(x));
        }
        return out;
    });
}

Result<std::string> ComOutlookSession::attachment_bytes(const std::string& message_id, const std::string& attachment_id) {
    return impl_->run<std::string>([this, message_id, attachment_id] {
        Disp atts = impl_->item(message_id).obj(L"Attachments");
        const long index = std::stol(attachment_id);
        if (index < 1 || index > atts.num(L"Count")) throw ComError{kNotFound, "no such attachment"};
        Disp a = atts.obj(L"Item", {vint(index)});
        wchar_t dir[MAX_PATH];
        GetTempPathW(MAX_PATH, dir);
        wchar_t file[MAX_PATH];
        GetTempFileNameW(dir, L"skw", 0, file);
        a.call(L"SaveAsFile", {vstr(narrow(file))});
        std::ifstream in(std::filesystem::path(file), std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        DeleteFileW(file);
        return data;
    });
}

Result<OlMessage> ComOutlookSession::create_reply(const std::string& id, const std::string& kind, const std::string& comment) {
    return impl_->run<OlMessage>([this, id, kind, comment] {
        auto& I = *impl_;
        Disp src = I.item(id);
        Disp r = src.obj(kind == "reply_all" ? L"ReplyAll" : kind == "forward" ? L"Forward" : L"Reply");
        if (!comment.empty()) r.put(L"Body", vstr(comment + "\r\n\r\n" + r.str(L"Body")));
        r.call(L"Save");
        return I.to_message(r);
    });
}

Result<OlMessage> ComOutlookSession::create_draft(const DraftPatch& p) {
    return impl_->run<OlMessage>([this, p] {
        auto& I = *impl_;
        Disp m = I.app.obj(L"CreateItem", {vint(olMailItem)});
        I.apply(m, p);
        m.call(L"Save");
        return I.to_message(m);
    });
}

Result<OlMessage> ComOutlookSession::update_draft(const std::string& id, const DraftPatch& p) {
    return impl_->run<OlMessage>([this, id, p] {
        auto& I = *impl_;
        Disp m = I.item(id);
        I.apply(m, p);
        m.call(L"Save");
        return I.to_message(m);
    });
}

Status ComOutlookSession::send(const std::string& draft_id) {
    return impl_->run_status([this, draft_id] { impl_->item(draft_id).call(L"Send"); });
}

Status ComOutlookSession::send_new(const DraftPatch& p) {
    return impl_->run_status([this, p] {
        auto& I = *impl_;
        Disp m = I.app.obj(L"CreateItem", {vint(olMailItem)});
        I.apply(m, p);
        m.call(L"Send");
    });
}

Result<OlMessage> ComOutlookSession::move(const std::string& id, const std::string& folder_id) {
    return impl_->run<OlMessage>([this, id, folder_id] {
        auto& I = *impl_;
        Disp dest = I.ns.obj(L"GetFolderFromID", {vstr(folder_id)});
        Disp moved = I.item(id).obj(L"Move", {[&] {
                                         VARIANT v;
                                         VariantInit(&v);
                                         v.vt = VT_DISPATCH;
                                         v.pdispVal = dest.get();
                                         dest.get()->AddRef();
                                         return v;
                                     }()});
        return I.to_message(moved);
    });
}

Status ComOutlookSession::remove(const std::string& id) {
    return impl_->run_status([this, id] { impl_->item(id).call(L"Delete"); });
}

Result<std::vector<OlEvent>> ComOutlookSession::events(long long from, long long to, size_t limit) {
    return impl_->run<std::vector<OlEvent>>([this, from, to, limit] {
        auto& I = *impl_;
        Disp items = I.default_folder(olFolderCalendar).obj(L"Items");
        // Recurrences are expanded only on a sorted collection with IncludeRecurrences set before Restrict.
        items.call(L"Sort", {vstr("[Start]")});
        items.put(L"IncludeRecurrences", vbool(true));
        const std::string filter = "[Start] < '" + jet_date(utc_ms_to_local(to)) + "' AND [End] > '" + jet_date(utc_ms_to_local(from)) + "'";
        Disp found = items.obj(L"Restrict", {vstr(filter)});
        std::vector<OlEvent> out;
        Disp it = found.obj(L"GetFirst");
        while (it && out.size() < limit) {
            if (it.num(L"Class") == olAppointmentClass) {
                OlEvent e = I.to_event(it);
                if (e.end > from && e.start < to) out.push_back(std::move(e));
            }
            it = found.obj(L"GetNext");
        }
        return out;
    });
}

Result<OlEvent> ComOutlookSession::event(const std::string& id) {
    return impl_->run<OlEvent>([this, id] {
        auto& I = *impl_;
        if (const auto occ = split_occurrence_id(id)) {
            const auto at = parse_stamp(occ->second);
            if (!at) throw ComError{kNotFound, "malformed occurrence id"};
            Disp pattern = I.item(occ->first).obj(L"GetRecurrencePattern");
            return I.to_event(pattern.obj(L"GetOccurrence", {vdate(*at)}));
        }
        return I.to_event(I.item(id));
    });
}

Result<OlEvent> ComOutlookSession::save_event(const std::string& id, const EventPatch& p) {
    return impl_->run<OlEvent>([this, id, p] {
        auto& I = *impl_;
        Disp e;
        if (id.empty()) {
            e = I.app.obj(L"CreateItem", {vint(olAppointmentItem)});
        } else if (const auto occ = split_occurrence_id(id)) {
            const auto at = parse_stamp(occ->second);
            if (!at) throw ComError{kNotFound, "malformed occurrence id"};
            e = I.item(occ->first).obj(L"GetRecurrencePattern").obj(L"GetOccurrence", {vdate(*at)});
        } else {
            e = I.item(id);
        }
        if (p.subject) e.put(L"Subject", vstr(*p.subject));
        if (p.body) e.put(L"Body", vstr(*p.body));
        if (p.location) e.put(L"Location", vstr(*p.location));
        if (p.start) e.put(L"StartUTC", vdate(ms_to_wall(*p.start)));
        if (p.end) e.put(L"EndUTC", vdate(ms_to_wall(*p.end)));
        const bool attendees = (p.required && !p.required->empty()) || (p.optional && !p.optional->empty());
        if (p.required || p.optional) {
            if (attendees) e.put(L"MeetingStatus", vint(olMeeting));
            I.set_recipients(e, p.required, p.optional, olRequired, olOptional);
        }
        e.call(L"Save");
        // A meeting sends its invitations or updates (calendar.create is risk class external).
        if (e.num(L"MeetingStatus") == olMeeting) e.call(L"Send");
        return I.to_event(e);
    });
}

Status ComOutlookSession::respond(const std::string& id, const std::string& response, const std::string& comment) {
    return impl_->run_status([this, id, response, comment] {
        auto& I = *impl_;
        Disp e;
        if (const auto occ = split_occurrence_id(id)) {
            const auto at = parse_stamp(occ->second);
            if (!at) throw ComError{kNotFound, "malformed occurrence id"};
            e = I.item(occ->first).obj(L"GetRecurrencePattern").obj(L"GetOccurrence", {vdate(*at)});
        } else {
            e = I.item(id);
        }
        const long r = response == "accept" ? olMeetingAccepted : response == "tentative" ? olMeetingTentative : olMeetingDeclined;
        Disp reply = e.obj(L"Respond", {vint(r), vbool(true), vbool(false)});
        if (reply) {
            if (!comment.empty()) reply.put(L"Body", vstr(comment));
            reply.call(L"Send");
        }
    });
}

Result<std::string> ComOutlookSession::free_busy(const std::string& address, long long from, int interval_minutes, int slots) {
    return impl_->run<std::string>([this, address, from, interval_minutes, slots] {
        auto& I = *impl_;
        Disp r = I.ns.obj(L"CreateRecipient", {vstr(address)});
        if (!r.call(L"Resolve").v.boolVal) throw ComError{kNotFound, "the address could not be resolved"};
        const double local = utc_ms_to_local(from);
        const double day = static_cast<double>(static_cast<long long>(local));  // FreeBusy starts at midnight of that day
        const std::string all = Disp::as_str(r.call(L"FreeBusy", {vdate(day), vint(interval_minutes), vbool(true)}));
        const long long offset = static_cast<long long>((local - day) * 1440.0 / interval_minutes + 0.5);
        std::string out;
        for (int i = 0; i < slots; ++i) {
            const size_t k = static_cast<size_t>(offset + i);
            out += k < all.size() ? all[k] : '0';
        }
        return out;
    });
}

Status ComOutlookSession::display(const std::string& id) {
    return impl_->run_status([this, id] { impl_->item(id).call(L"Display", {vbool(false)}); });
}

}  // namespace wb::mail::outlook
