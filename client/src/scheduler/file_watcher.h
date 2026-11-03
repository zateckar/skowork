// File Trigger folders (client/scheduler Trigger Kinds, add-scheduled-and-remote-tasks design D9): change notifications
// polled without a thread, a 5-second quiet period before a listing counts as stable, and a full listing at start
// (startup reconciliation) and every few minutes in case a notification was lost.
#pragma once

#include "base/json.h"
#include "base/time.h"
#include "scheduler/scheduler.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace wb::scheduler {

inline constexpr long long kFileQuietMs = 5'000;
inline constexpr long long kFileRescanMs = 5 * 60'000;

// Case-insensitive * and ? over a file name; several patterns separated by ';'.
bool name_matches(const std::string& name, const std::string& patterns);

class FileWatcher {
public:
    using Report = std::function<void(const std::string& trigger_id, const std::vector<FileStat>& files)>;
    FileWatcher(const Clock& clock, Report report);
    ~FileWatcher();
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // The file Triggers to watch: [{id, folder, pattern}] (Scheduler::file_triggers()).
    void set(const json& triggers);
    // Called every tick: reads the notifications and reports listings that have been quiet for kFileQuietMs.
    void poll();

private:
    struct Watch {
        std::string folder, pattern;
        void* handle = nullptr;  // FindFirstChangeNotification, or null while the folder is missing
        bool dirty = true;
        bool pending = false;
        long long changed_at = 0, listed_at = 0, retry_at = 0;
        std::vector<FileStat> listing;
    };
    std::vector<FileStat> list(const Watch& w) const;
    void close(Watch& w);

    const Clock& clock_;
    Report report_;
    std::map<std::string, Watch> watches_;
};

}  // namespace wb::scheduler
