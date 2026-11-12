#include "meeting/speech_installer.h"

#include "platform/http.h"
#include "platform/win.h"

namespace wb::meeting {

namespace fs = std::filesystem;

SpeechInstaller::SpeechInstaller(Env env) : env_(std::move(env)) {}

SpeechInstaller::~SpeechInstaller() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

Status SpeechInstaller::start_download() {
    if (env_.download_allowed && !env_.download_allowed())
        return make_error("policy", "Your organization has turned speech package downloads off. Import a package instead.");
    const SpeechSource source = env_.source ? env_.source() : SpeechSource{};
    if (source.url.empty() || source.sha256.empty())
        return make_error("unavailable", "Your administrator has not published a speech package. Import a package instead.");
    return run(
        [this, source](const std::shared_ptr<CancelToken>& token) {
            const auto progress = [this](std::uint64_t done, std::uint64_t) {
                // One event per 16 MB keeps Settings informed without flooding it.
                const std::uint64_t step = done >> 24;
                if (step == progress_step_.exchange(step)) return;
                if (env_.changed) env_.changed({{"state", "downloading"}, {"bytes", done}});
            };
            return download_speech_package(env_.data_root, source, token, progress, env_.limits);
        },
        "downloading");
}

Status SpeechInstaller::start_import(const fs::path& archive) {
    const std::string digest = env_.source ? env_.source().sha256 : std::string{};
    return run(
        [this, archive, digest](const std::shared_ptr<CancelToken>& token) {
            return install_speech_archive(env_.data_root, archive, digest, env_.limits, [token] { return token->cancelled(); });
        },
        "verifying");
}

void SpeechInstaller::cancel() {
    std::lock_guard lk(mx_);
    if (cancel_) cancel_->cancel();
}

Result<bool> SpeechInstaller::remove(const std::string& version) {
    // Held across the removal so that no setup starts while a folder is deleted.
    std::lock_guard lk(mx_);
    if (running_) return make_error("busy", "A speech setup is running.");
    return remove_speech_version(env_.data_root, version, env_.in_use && env_.in_use());
}

Status SpeechInstaller::run(std::function<Result<std::string>(const std::shared_ptr<CancelToken>&)> work, const char* first_state) {
    std::thread old;
    {
        std::lock_guard lk(mx_);
        if (running_) return make_error("busy", "A speech setup is already running.");
        old = std::move(thread_);
        running_ = true;
        cancel_ = std::make_shared<CancelToken>();
        state_ = first_state;
        version_.clear();
        error_.clear();
        thread_ = std::thread([this, work = std::move(work), token = cancel_] {
            // The package active before this setup: a version that is already active is never removed on failure.
            std::string previous;
            const bool had_previous = active_speech_dir(env_.data_root, &previous).ok();
            Result<std::string> r = work(token);
            std::string state, version, error;
            if (!r) {
                state = r.error().code == "cancelled" ? "cancelled" : "failed";
                error = r.error().message;
                version = had_previous ? previous : std::string{};
            } else {
                version = *r;
                state = "probing";
                emit(state, version, error);
                // The engine must initialize from the new folder before the package counts as ready.
                const fs::path dir = speech_root(env_.data_root) / L"packages" / widen(version) / L"models" / L"stt";
                const Status probe = env_.probe ? env_.probe(dir) : Status(make_error("unavailable", "no engine probe"));
                if (probe.ok()) {
                    state = "ready";
                } else {
                    state = "failed";
                    error = "The speech engine could not start: " + probe.error().message;
                    if (!had_previous || previous != version) remove_speech_version(env_.data_root, version, false);
                    version = had_previous ? previous : std::string{};
                }
            }
            {
                std::lock_guard done(mx_);
                running_ = false;
                state_ = state;
                version_ = version;
                error_ = error;
            }
            emit(state, version, error);
            if (state == "ready" && env_.activated) env_.activated();
        });
    }
    if (old.joinable()) old.join();
    emit(first_state, std::string{}, std::string{});
    return {};
}

void SpeechInstaller::emit(const std::string& state, const std::string& version, const std::string& error) {
    if (env_.changed) env_.changed(json{{"state", state}, {"version", version}, {"error", error}});
}

json SpeechInstaller::status() {
    std::string state, version, error;
    {
        std::lock_guard lk(mx_);
        state = state_;
        version = version_;
        error = error_;
    }
    const SpeechSource source = env_.source ? env_.source() : SpeechSource{};
    std::string active;
    const bool installed = active_speech_dir(env_.data_root, &active).ok();
    return json{{"state", state},
                {"installed", installed},
                {"version", installed ? active : version},
                {"error", error},
                {"download", !source.url.empty() && !source.sha256.empty() && (!env_.download_allowed || env_.download_allowed())},
                {"available_version", source.version}};
}

}  // namespace wb::meeting
