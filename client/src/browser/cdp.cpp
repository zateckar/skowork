#include "browser/cdp.h"

#include "platform/log.h"
#include "platform/win.h"

#include <vector>

namespace wb::browser {

namespace {
constexpr char kLog[] = "browser";
}

CdpConnection::CdpConnection(void* to_chrome, void* from_chrome, EventFn on_event, std::function<void()> on_closed)
    : out_(to_chrome), in_(from_chrome), on_event_(std::move(on_event)), on_closed_(std::move(on_closed)) {
    reader_ = std::thread([this] { read_loop(); });
    dispatcher_ = std::thread([this] { event_loop(); });
}

CdpConnection::~CdpConnection() {
    close();
    if (reader_.joinable()) reader_.join();
    if (dispatcher_.joinable()) dispatcher_.join();
    if (void* h = reader_thread_.load()) ::CloseHandle(static_cast<HANDLE>(h));
    if (out_) ::CloseHandle(static_cast<HANDLE>(out_));
    if (in_) ::CloseHandle(static_cast<HANDLE>(in_));
}

void CdpConnection::close() {
    {
        const std::lock_guard lock(mx_);
        stopping_ = true;
    }
    cv_.notify_all();
    // The reader blocks in ReadFile on an anonymous pipe: cancel that read (until it is running, retry briefly).
    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) {
        for (int i = 0; i < 200 && !reader_done_.load(); ++i) {
            if (void* h = reader_thread_.load()) ::CancelSynchronousIo(static_cast<HANDLE>(h));
            if (reader_done_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    mark_closed();
}

void CdpConnection::mark_closed() {
    alive_ = false;
    {
        const std::lock_guard lock(mx_);
        for (auto& [id, p] : pending_) {
            if (p->done) continue;
            p->done = true;
            p->error = "closed";
        }
    }
    cv_.notify_all();
    std::call_once(closed_once_, [this] {
        if (on_closed_) on_closed_();
    });
}

bool CdpConnection::write(const std::string& bytes) {
    const std::lock_guard lock(write_mx_);
    std::size_t off = 0;
    while (off < bytes.size()) {
        DWORD n = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - off, 1u << 20));
        if (!::WriteFile(static_cast<HANDLE>(out_), bytes.data() + off, chunk, &n, nullptr) || n == 0) return false;
        off += n;
    }
    return true;
}

Result<json> CdpConnection::call(const std::string& method, const json& params, const std::string& session,
                                 std::chrono::milliseconds timeout) {
    if (!alive_) return make_error("closed", "the browser is not running");
    const long long id = next_id_++;
    auto p = std::make_shared<Pending>();
    {
        const std::lock_guard lock(mx_);
        pending_[id] = p;
    }
    if (!write(encode_cdp_command(id, method, params, session))) {
        const std::lock_guard lock(mx_);
        pending_.erase(id);
        return make_error("closed", "the browser pipe is closed");
    }
    std::unique_lock lock(mx_);
    const bool done = cv_.wait_for(lock, timeout, [&] { return p->done; });
    pending_.erase(id);
    if (!done) return make_error("timeout", method + " did not answer within " + std::to_string(timeout.count() / 1000) + " s");
    if (p->error == "closed" && p->result.is_null()) return make_error("closed", "the browser closed");
    if (!p->error.empty()) return make_error("cdp", p->error);
    return std::move(p->result);
}

void CdpConnection::send(const std::string& method, const json& params, const std::string& session) {
    if (!alive_) return;
    (void)write(encode_cdp_command(next_id_++, method, params, session));
}

void CdpConnection::read_loop() {
    HANDLE self = nullptr;
    ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
    reader_thread_ = self;  // closed by the destructor after join, so close() never cancels a reused handle
    struct Done {
        CdpConnection& c;
        ~Done() { c.reader_done_ = true; }
    } done{*this};
    CdpFramer framer;
    std::vector<char> buf(1 << 16);
    std::vector<std::string> messages;
    for (;;) {
        DWORD n = 0;
        if (!::ReadFile(static_cast<HANDLE>(in_), buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) || n == 0) break;
        messages.clear();
        if (!framer.feed(std::string_view(buf.data(), n), messages)) {
            log::warn(kLog, "a CDP message exceeded the size limit; closing the connection");
            break;
        }
        for (const auto& text : messages) {
            CdpMessage m = parse_cdp_message(text);
            if (m.kind == CdpMessage::Kind::response) {
                const std::lock_guard lock(mx_);
                auto it = pending_.find(m.id);
                if (it == pending_.end()) continue;  // a send() whose answer nobody waits for
                it->second->done = true;
                it->second->error = m.error;
                it->second->result = std::move(m.result);
                cv_.notify_all();
            } else if (m.kind == CdpMessage::Kind::event) {
                const std::lock_guard lock(mx_);
                events_.push_back(std::move(m));
                cv_.notify_all();
            }
        }
    }
    mark_closed();
    {
        const std::lock_guard lock(mx_);
        stopping_ = true;
    }
    cv_.notify_all();
}

void CdpConnection::event_loop() {
    for (;;) {
        CdpMessage m;
        {
            std::unique_lock lock(mx_);
            cv_.wait(lock, [&] { return stopping_ || !events_.empty(); });
            if (events_.empty()) return;  // stopping and drained
            m = std::move(events_.front());
            events_.pop_front();
        }
        if (!on_event_) continue;
        try {
            on_event_(m);
        } catch (const std::exception& e) {
            log::warn(kLog, std::string("CDP event handler failed: ") + e.what());
        } catch (...) {
            log::warn(kLog, "CDP event handler failed");
        }
    }
}

}  // namespace wb::browser
