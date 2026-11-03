#include "shell/main_window.h"

#include "shell/shell.h"

namespace wb::shell {

MainWindow::MainWindow(HostLink& host, Options options) : host_(host), options_(std::move(options)) {}

MainWindow::~MainWindow() { shell_.reset(); }

void MainWindow::open(const std::string& route) {
    if (shell_) {
        shell_->bring_to_front();
        if (!route.empty()) shell_->navigate(route);
        return;
    }
    auto s = std::make_unique<Shell>(host_, options_, [this] {
        // The window is being destroyed inside its own message handling: drop it afterwards.
        if (shell_) last_route_ = shell_->route().hash;
        auto done = [this] {
            shell_.reset();
            if (on_closed) on_closed();
        };
        if (options_.post) options_.post(done);
        else done();
    });
    if (!s->ok()) return;
    shell_ = std::move(s);
    shell_->navigate(!route.empty() ? route : !last_route_.empty() ? last_route_ : std::string("#/inbox"));
    shell_->window().render();
}

void MainWindow::close() {
    if (!shell_) return;
    last_route_ = shell_->route().hash;
    shell_.reset();
    if (on_closed) on_closed();
}

void MainWindow::emit(const std::string& event, const json& data) {
    if (shell_) {
        shell_->emit(event, data);
        shell_->window().render();
    }
}

ui::Window* MainWindow::window() const { return shell_ ? &shell_->window() : nullptr; }

std::optional<Route> MainWindow::route() const {
    if (!shell_) return std::nullopt;
    return shell_->route();
}

}  // namespace wb::shell
