#include "host/toast.h"

#include "base/strings.h"
#include "host/dispatch.h"
#include "host/registration.h"
#include "platform/com.h"
#include "platform/log.h"
#include "platform/win.h"

// MinGW-w64: windows.foundation.h specialises IReference<BYTE> and IReference<boolean>, both `unsigned char`; skipping
// the boolean one avoids the redefinition (see the toolchain spike). Skowork code must not use IReference<boolean>.
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#include <windows.foundation.h>
#include <windows.foundation.collections.h>
#include <roapi.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <winstring.h>

#include <vector>

namespace wb {

namespace {

constexpr char kLog[] = "toast";

namespace wun = ABI::Windows::UI::Notifications;
namespace wxd = ABI::Windows::Data::Xml::Dom;
using ActivatedHandler = ABI::Windows::Foundation::ITypedEventHandler<wun::ToastNotification*, IInspectable*>;

class hstring {
public:
    explicit hstring(const std::wstring& s) { ::WindowsCreateString(s.c_str(), static_cast<UINT32>(s.size()), &h_); }
    ~hstring() { ::WindowsDeleteString(h_); }
    hstring(const hstring&) = delete;
    hstring& operator=(const hstring&) = delete;
    [[nodiscard]] HSTRING get() const { return h_; }

private:
    HSTRING h_ = nullptr;
};

std::wstring xml_escape(const std::string& s) {
    std::wstring out;
    for (const wchar_t c : widen(s)) {
        switch (c) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        case L'\'': out += L"&apos;"; break;
        default:
            if (c >= 0x20 || c == L'\n' || c == L'\t') out += c;
        }
    }
    return out;
}

std::string hr_text(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(hr));
    return b;
}

}  // namespace

std::wstring toast_xml(const ToastContent& c) {
    return L"<toast launch=\"thread=" + xml_escape(c.thread_id) + L"\"><visual><binding template=\"ToastGeneric\"><text>" +
           xml_escape(c.title) + L"</text><text>" + xml_escape(c.body) + L"</text></binding></visual></toast>";
}

struct ToastNotifier::Impl {
    std::function<void(const std::string&)> on_activated;
    com_ptr<wun::IToastNotificationManagerStatics> manager;
    com_ptr<wun::IToastNotificationFactory> factory;
    com_ptr<wun::IToastNotifier> notifier;
    // Shown toasts with their Activated registrations, so the handlers live as long as the host.
    std::vector<std::pair<com_ptr<wun::IToastNotification>, EventRegistrationToken>> shown;
    std::vector<std::string> shown_threads;  // parallel to `shown`
    bool ready = false;

    bool init() {
        if (ready) return true;
        const HRESULT ro = ::RoInitialize(RO_INIT_SINGLETHREADED);
        if (FAILED(ro) && ro != RPC_E_CHANGED_MODE) {
            log::warn(kLog, "RoInitialize failed: " + hr_text(ro));
            return false;
        }
        HRESULT hr = ::RoGetActivationFactory(hstring(L"Windows.UI.Notifications.ToastNotificationManager").get(),
                                              __uuidof(wun::IToastNotificationManagerStatics), manager.put_void());
        if (SUCCEEDED(hr))
            hr = ::RoGetActivationFactory(hstring(L"Windows.UI.Notifications.ToastNotification").get(),
                                          __uuidof(wun::IToastNotificationFactory), factory.put_void());
        if (SUCCEEDED(hr)) hr = manager->CreateToastNotifierWithId(hstring(kAppUserModelId).get(), notifier.put());
        if (FAILED(hr)) {
            log::warn(kLog, "toast notifications are unavailable: " + hr_text(hr));
            return false;
        }
        ready = true;
        return true;
    }
};

ToastNotifier::ToastNotifier(std::function<void(const std::string&)> on_activated) : impl_(std::make_unique<Impl>()) {
    impl_->on_activated = std::move(on_activated);
}

ToastNotifier::~ToastNotifier() {
    for (auto& [toast, token] : impl_->shown) toast->remove_Activated(token);
}

bool ToastNotifier::show(const ToastContent& content) {
    if (!impl_->init()) return false;
    com_ptr<IInspectable> inspectable;
    HRESULT hr = ::RoActivateInstance(hstring(L"Windows.Data.Xml.Dom.XmlDocument").get(), inspectable.put());
    com_ptr<wxd::IXmlDocumentIO> io;
    if (SUCCEEDED(hr)) hr = inspectable.as(io, __uuidof(wxd::IXmlDocumentIO));
    if (SUCCEEDED(hr)) hr = io->LoadXml(hstring(toast_xml(content)).get());
    com_ptr<wxd::IXmlDocument> doc;
    if (SUCCEEDED(hr)) hr = inspectable.as(doc, __uuidof(wxd::IXmlDocument));
    com_ptr<wun::IToastNotification> toast;
    if (SUCCEEDED(hr)) hr = impl_->factory->CreateToastNotification(doc.get(), toast.put());
    if (FAILED(hr)) {
        log::warn(kLog, "toast could not be created: " + hr_text(hr));
        return false;
    }
    auto handler = make_callback<ActivatedHandler, HRESULT(wun::IToastNotification*, IInspectable*)>(
        __uuidof(ActivatedHandler), [cb = impl_->on_activated, thread = content.thread_id](wun::IToastNotification*, IInspectable*) {
            // Raised on a WinRT thread; the window opens on the main thread.
            MainThread::post([cb, thread] { if (cb) cb(thread); });
            return S_OK;
        });
    EventRegistrationToken token{};
    hr = toast->add_Activated(handler.get(), &token);
    if (SUCCEEDED(hr)) hr = impl_->notifier->Show(toast.get());
    if (FAILED(hr)) {
        log::warn(kLog, "toast could not be shown: " + hr_text(hr));
        return false;
    }
    impl_->shown.emplace_back(toast, token);
    impl_->shown_threads.push_back(content.thread_id);
    if (impl_->shown.size() > 200) {  // older toasts have long left the Action Center
        impl_->shown.front().first->remove_Activated(impl_->shown.front().second);
        impl_->shown.erase(impl_->shown.begin());
        impl_->shown_threads.erase(impl_->shown_threads.begin());
    }
    return true;
}

std::vector<std::string> ToastNotifier::shown_threads() const { return impl_->shown_threads; }

int ToastNotifier::history_count() {
    if (!impl_->init()) return -1;
    com_ptr<wun::IToastNotificationManagerStatics2> statics2;
    if (FAILED(impl_->manager.as(statics2, __uuidof(wun::IToastNotificationManagerStatics2)))) return -1;
    com_ptr<wun::IToastNotificationHistory> history;
    if (FAILED(statics2->get_History(history.put()))) return -1;
    com_ptr<wun::IToastNotificationHistory2> history2;
    if (FAILED(history.as(history2, __uuidof(wun::IToastNotificationHistory2)))) return -1;
    com_ptr<ABI::Windows::Foundation::Collections::IVectorView<wun::ToastNotification*>> list;
    if (FAILED(history2->GetHistoryWithId(hstring(kAppUserModelId).get(), list.put()))) return -1;
    unsigned size = 0;
    if (FAILED(list->get_Size(&size))) return -1;
    return static_cast<int>(size);
}

std::string ToastNotifier::setting() {
    if (!impl_->init()) return "unavailable";
    wun::NotificationSetting s{};
    if (FAILED(impl_->notifier->get_Setting(&s))) return "unavailable";
    switch (s) {
    case wun::NotificationSetting_Enabled: return "enabled";
    case wun::NotificationSetting_DisabledForApplication: return "disabled_for_application";
    case wun::NotificationSetting_DisabledForUser: return "disabled_for_user";
    case wun::NotificationSetting_DisabledByGroupPolicy: return "disabled_by_group_policy";
    case wun::NotificationSetting_DisabledByManifest: return "disabled_by_manifest";
    default: return "unavailable";
    }
}

void ToastNotifier::activate(const std::string& thread_id) {
    if (impl_->on_activated) impl_->on_activated(thread_id);
}

void ToastNotifier::clear() {
    if (!impl_->ready) return;
    com_ptr<wun::IToastNotificationManagerStatics2> statics2;
    if (FAILED(impl_->manager.as(statics2, __uuidof(wun::IToastNotificationManagerStatics2)))) return;
    com_ptr<wun::IToastNotificationHistory> history;
    if (SUCCEEDED(statics2->get_History(history.put()))) history->ClearWithId(hstring(kAppUserModelId).get());
}

}  // namespace wb
