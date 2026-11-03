// The native window (add-native-ui) as the host's UI window: the shell's MainWindow, calling the RPC methods on the
// thread pool and receiving their replies on the main thread (design D5). No other process is started to show it.
#pragma once

#include "host/app_window.h"
#include "host/dispatch.h"
#include "host/rpc.h"
#include "shell/main_window.h"

namespace wb {

class NativeAppWindow final : public AppWindow {
public:
    NativeAppWindow(RpcRegistry& rpc, ThreadPool& pool)
        : link_(
              [&rpc](const std::string& method) -> shell::AsyncLink::Handler {
                  const RpcRegistry::Handler* h = rpc.find(method, false);
                  return h ? *h : shell::AsyncLink::Handler{};
              },
              [&pool](std::function<void()> f) { pool.post(std::move(f)); },
              [](std::function<void()> f) { MainThread::post(std::move(f)); }),
          main_(link_, options(pool)) {}

    void open(const std::string& route = {}) override { main_.open(route); }
    void close() override { main_.close(); }
    [[nodiscard]] bool is_open() const override { return main_.is_open(); }
    void emit(const std::string& event, const json& data) override { main_.emit(event, data); }

private:
    static shell::MainWindow::Options options(ThreadPool& pool) {
        shell::MainWindow::Options o;
        o.post = [](std::function<void()> f) { MainThread::post(std::move(f)); };
        o.background = [&pool](std::function<void()> f) { pool.post(std::move(f)); };
        return o;
    }
    shell::AsyncLink link_;
    shell::MainWindow main_;
};

}  // namespace wb
