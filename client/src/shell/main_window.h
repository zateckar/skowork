// The native Skowork window (add-native-ui, client/work-inbox): navigation, the Thread list, the open Thread with its
// feed and composer, the Activity rail, banners, the status bar, and the Status, Settings and About views. It
// implements the host's AppWindow interface: open(route), close(), is_open() and emit(event, data)
// with the host events status, navigate, run.event and inbox.changed (design D5). The window and everything in it
// exist only while it is open; closing it releases them.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "shell/host_link.h"
#include "shell/routes.h"
#include "ui/theme.h"

namespace wb::ui {
class Window;
}

namespace wb::shell {

class Shell;

class MainWindow {
public:
    using Post = std::function<void(std::function<void()>)>;
    struct Options {
        std::string lang = "system";  // cs, de, en or system; the ui.language setting overrides it once loaded
        int width = 1180, height = 760;  // DIPs
        bool show = true;
        std::optional<ui::Theme> theme;  // fixed theme (tests)
        // Runs a closure later on the window's thread (closing the window from inside its own message handling).
        Post post;
        // The time (milliseconds since the epoch) for date groups and relative times; tests fix it.
        std::function<long long()> now;
        // The composer's file picker (Attach files); tests replace it. Default: the system Open dialog.
        std::function<std::vector<std::wstring>()> pick_files;
        // The folder picker (Settings > Files); tests replace it. Default: the system Open dialog for folders.
        std::function<std::wstring()> pick_folder;
        // Copies text to the clipboard (About's Copy details); tests replace it. Default: the system clipboard.
        std::function<bool(const std::string&)> copy_text;
        // Runs work off the window's thread (decoding images in the feed). Default: on the window's thread.
        Post background;
        // Save as for an image: the chosen path for the suggested file name, or "" when cancelled. Tests replace it.
        // Default: the system Save dialog.
        std::function<std::wstring(const std::wstring& suggested)> save_file;
    };
    MainWindow(HostLink& host, Options options);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // Opens the window or brings it to the front, at a route such as "#/threads/new" (empty: where it was).
    void open(const std::string& route = {});
    void close();
    bool is_open() const { return shell_ != nullptr; }
    // A host event: status, navigate, run.event, inbox.changed.
    void emit(const std::string& event, const json& data);

    // While open: the window (tests drive it through UI Automation and the element tree) and the current route.
    ui::Window* window() const;
    std::optional<Route> route() const;
    Shell* shell() const { return shell_.get(); }

    std::function<void()> on_closed;

private:
    HostLink& host_;
    Options options_;
    std::unique_ptr<Shell> shell_;
    std::string last_route_;
};

}  // namespace wb::shell
