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
    // Programmatic closes must consult the same unsaved-state guard as WM_CLOSE.
    if (shell_->window().on_close_request && !shell_->window().on_close_request()) return;
    close_for_shutdown();
}

void MainWindow::close_for_shutdown() {
    if (!shell_) return;
    last_route_ = shell_->route().hash;
    shell_.reset();
    if (on_closed) on_closed();
}

bool MainWindow::has_unsaved_generated_state() const { return shell_ && shell_->has_unsaved_generated_state(); }

void MainWindow::confirm_shutdown(std::function<void(bool)> done) {
    if (!shell_) return done(true);
    shell_->confirm_shutdown(std::move(done));
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
