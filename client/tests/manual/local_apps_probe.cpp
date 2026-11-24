// Read-only probe of the local Outlook and Teams backends on a real laptop (add-local-outlook-and-teams tasks 1.1, 1.3).
// Not a unit test and not registered with CTest. It never drafts, sends, moves, deletes or answers anything.
//   local_apps_probe.exe [outlook] [teams] [--days N] [--read N]
#include "mail/com_outlook_session.h"
#include "mail/outlook_graph.h"
#include "base/time.h"
#include "base/url.h"
#include "teams/indexeddb.h"
#include "teams/leveldb.h"
#include "teams/snapshot.h"
#include "teams/teams_extract.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

using namespace wb;
using Steady = std::chrono::steady_clock;

namespace {

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
double since(Steady::time_point t) { return std::chrono::duration<double>(Steady::now() - t).count(); }
std::string day(long long ms) {
    const time_t t = static_cast<time_t>(ms / 1000);
    tm lt{};
    localtime_s(&lt, &t);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &lt);
    return b;
}
std::string cut(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n) + "..."; }
size_t peak_mb() {
    PROCESS_MEMORY_COUNTERS pmc{};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof pmc);
    return pmc.PeakWorkingSetSize >> 20;
}

int probe_outlook(int days, int read_n) {
    std::printf("== Outlook (classic, COM)\n");
    if (!mail::outlook::outlook_installed()) {
        std::printf("  classic Outlook is not registered (Outlook.Application); new Outlook has no object model\n");
        return 1;
    }
    mail::outlook::ComOutlookSession ol([] { return false; });  // never start Outlook
    std::printf("  state: %s\n", ol.state().c_str());
    auto t = Steady::now();
    auto me = ol.me();
    if (!me) { std::printf("  me: %s: %s\n", me.error().code.c_str(), me.error().message.c_str()); return 1; }
    std::printf("  me: %s <%s> (%.2fs)\n", me->name.c_str(), me->address.c_str(), since(t));

    t = Steady::now();
    auto folders = ol.folders();
    if (!folders) { std::printf("  folders: %s\n", folders.error().message.c_str()); return 1; }
    std::string inbox;
    std::map<std::string, int> wk;
    for (const auto& f : *folders) {
        if (!f.well_known.empty()) ++wk[f.well_known];
        if (f.well_known == "inbox" && inbox.empty()) inbox = f.id;
    }
    std::printf("  folders: %zu (%.2fs); well-known:", folders->size(), since(t));
    for (const auto& [k, n] : wk) std::printf(" %s", k.c_str());
    std::printf("\n");
    if (inbox.empty()) { std::printf("  no inbox found\n"); return 1; }

    t = Steady::now();
    auto rows = ol.rows(inbox, now_ms() - days * 86'400'000LL);
    if (!rows) { std::printf("  inbox rows: %s\n", rows.error().message.c_str()); return 1; }
    std::printf("  inbox rows, last %d days: %zu (%.2fs)\n", days, rows->size(), since(t));
    std::sort(rows->begin(), rows->end(), [](const auto& a, const auto& b) { return a.received > b.received; });

    t = Steady::now();
    int ok = 0, failed = 0, labelled = 0, with_body = 0;
    size_t body_chars = 0;
    for (int i = 0; i < read_n && i < static_cast<int>(rows->size()); ++i) {
        auto m = ol.message((*rows)[i].id);
        if (!m) { ++failed; if (failed <= 3) std::printf("  read failed: %s: %s\n", m.error().code.c_str(), m.error().message.c_str()); continue; }
        ++ok;
        if (!m->msip_labels.empty()) ++labelled;
        if (!m->body.empty()) ++with_body;
        body_chars += m->body.size();
        if (ok <= 5) std::printf("    %s  %s\n", day(m->received).c_str(), cut(m->subject, 70).c_str());
    }
    const double read_s = since(t);
    std::printf("  full messages read: %d ok, %d failed, %d with body, %d with sensitivity label, %.2fs (%.0f ms/message, %.0f s per 1,000)\n",
                ok, failed, with_body, labelled, read_s, ok ? read_s * 1000 / ok : 0.0, ok ? read_s * 1000 / ok : 0.0);

    t = Steady::now();
    const long long from = now_ms() - 86'400'000LL, to = now_ms() + 14 * 86'400'000LL;
    auto events = ol.events(from, to, 500);
    if (!events) std::printf("  calendar: %s: %s\n", events.error().code.c_str(), events.error().message.c_str());
    else {
        int recurring = 0, online = 0;
        for (const auto& e : *events) { if (!e.series_id.empty()) ++recurring; if (!e.online_url.empty()) ++online; }
        std::printf("  calendar, yesterday..+14 days: %zu events, %d occurrences of series, %d online meetings (%.2fs)\n",
                    events->size(), recurring, online, since(t));
        int shown = 0;
        for (const auto& e : *events)
            if (e.start >= now_ms() - 3'600'000 && shown++ < 5)
                std::printf("    %s  %s%s\n", day(e.start).c_str(), cut(e.subject, 60).c_str(), e.online_url.empty() ? "" : "  [online]");
    }

    // The sync's own path: the adapter's calendarView delta over the default window (90 days back, 365 ahead).
    t = Steady::now();
    {
        mail::outlook::OutlookGraph graph(ol, system_clock());
        const long long day = 86'400'000LL, now_d = now_ms() / day;
        const auto iso = [](long long ms) { return iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(ms))); };
        std::string link = "/me/calendarView/delta?startDateTime=" + url_encode(iso((now_d - 90) * day)) +
                           "&endDateTime=" + url_encode(iso((now_d + 366) * day));
        size_t items = 0;
        int pages = 0;
        while (!link.empty()) {
            auto page = graph.handle("GET", link, std::nullopt, 100);
            if (!page) { std::printf("  sync calendar view: %s: %s (%.1fs)\n", page.error().code.c_str(), page.error().message.c_str(), since(t)); break; }
            ++pages;
            if (const json* v = jobj(*page, "value"); v && v->is_array()) items += v->size();
            link = jstr(*page, "@odata.nextLink");
            if (link.empty()) std::printf("  sync calendar view, -90..+365 days: %zu events in %d pages (%.1fs)\n", items, pages, since(t));
        }
    }

    t = Steady::now();
    auto fb = ol.free_busy(me->address, now_ms(), 60, 24);
    if (!fb) std::printf("  free/busy: %s: %s\n", fb.error().code.c_str(), fb.error().message.c_str());
    else std::printf("  free/busy, next 24 h by hour: %s (%.2fs)\n", fb->c_str(), since(t));
    return failed ? 1 : 0;
}

// What a quiet sync cycle asks Outlook: the folder list, each folder's rows over the mail window, the calendar stamp.
int probe_cycle(int days) {
    std::printf("== Outlook, one quiet sync cycle (rows over %d days)\n", days);
    mail::outlook::ComOutlookSession ol([] { return false; });
    auto t = Steady::now();
    auto folders = ol.folders();
    if (!folders) { std::printf("  folders: %s\n", folders.error().message.c_str()); return 1; }
    std::printf("  folders: %zu (%.2fs)\n", folders->size(), since(t));
    const auto all = Steady::now();
    size_t total = 0;
    double worst = 0;
    for (const auto& f : *folders) {
        t = Steady::now();
        auto rows = ol.rows(f.id, now_ms() - days * 86'400'000LL);
        const double s = since(t);
        worst = std::max(worst, s);
        if (!rows) { std::printf("  %-30s %s\n", cut(f.name, 30).c_str(), rows.error().message.c_str()); continue; }
        total += rows->size();
        if (f.well_known == "inbox" && !rows->empty())
            std::printf("  inbox sample row: id %zu chars, received %s, modified %s\n", rows->front().id.size(),
                        day(rows->front().received).c_str(), day(rows->front().modified).c_str());
        if (s >= 0.5) std::printf("  %-30s %6zu rows %6.2fs\n", cut(f.name, 30).c_str(), rows->size(), s);
    }
    std::printf("  rows of all folders: %zu in %.1fs (slowest folder %.1fs)\n", total, since(all), worst);
    t = Steady::now();
    for (const auto& f : *folders)
        if (auto fs = ol.folder_stamp(f.id); !fs) std::printf("  stamp %s: %s\n", f.name.c_str(), fs.error().message.c_str());
    std::printf("  stamps of all folders: %.2fs\n", since(t));
    t = Steady::now();
    auto st = ol.calendar_stamp();
    std::printf("  calendar stamp: %s (%.2fs)\n", st ? st->c_str() : st.error().message.c_str(), since(t));
    return 0;
}

int probe_teams() {
    std::printf("== Teams (new Teams IndexedDB cache)\n");
    wchar_t buf[MAX_PATH];
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    const std::filesystem::path lad(std::wstring(buf, n));
    const auto profiles = teams::find_teams_profiles(lad);
    std::printf("  profiles found: %zu\n", profiles.size());
    if (profiles.empty()) return 1;
    std::error_code ec;
    uintmax_t bytes = 0;
    for (std::filesystem::recursive_directory_iterator it(profiles[0], ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) bytes += it->file_size(ec);
    std::printf("  cache: %.1f MB\n", bytes / 1048576.0);

    const auto priv = std::filesystem::temp_directory_path() / L"skowork-teams-probe";
    auto t = Steady::now();
    auto snap = teams::snapshot_cache(profiles[0], priv);
    if (!snap) { std::printf("  snapshot: %s: %s\n", snap.error().code.c_str(), snap.error().message.c_str()); return 1; }
    const double snap_s = since(t);
    t = Steady::now();
    auto db = teams::LevelDbReader::open(*snap);
    if (!db) { std::printf("  leveldb: %s: %s\n", db.error().code.c_str(), db.error().message.c_str()); return 1; }
    const auto st = db->stats();
    auto cat = teams::IdbCatalog::build(*db);
    auto x = teams::extract(cat);
    const double read_s = since(t);
    std::printf("  snapshot %.2fs, decode %.2fs, peak working set %zu MB\n", snap_s, read_s, peak_mb());
    std::printf("  leveldb: %d tables (%d missing), %d logs, %zu live keys, bad blocks %d, bad log records %d, bad batches %d\n",
                st.tables, st.missing_tables, st.logs, st.entries, st.bad_blocks, st.bad_log_records, st.bad_batches);
    std::printf("  indexeddb: %zu databases, %zu object stores\n", cat.databases().size(), cat.stores().size());
    std::printf("  extracted: %zu conversations, %zu messages, %zu profiles; skipped %d, blobs %d, decode errors %d; user %s\n",
                x.conversations.size(), x.messages.size(), x.profiles.size(), x.skipped, x.blobs, x.decode_errors,
                x.user_mri.empty() ? "(unknown)" : "known");
    std::map<std::string, int> types;
    for (const auto& c : x.conversations) ++types[c.type];
    std::printf("  conversation types:");
    for (const auto& [k, v] : types) std::printf(" %s=%d", k.c_str(), v);
    std::printf("\n");
    long long newest = 0;
    int mentions = 0, meeting_msgs = 0;
    std::map<std::string, std::string> type_of;
    for (const auto& c : x.conversations) type_of[c.id] = c.type;
    for (const auto& m : x.messages) {
        newest = std::max(newest, m.at_ms);
        if (!x.user_mri.empty() && std::find(m.mention_mris.begin(), m.mention_mris.end(), x.user_mri) != m.mention_mris.end()) ++mentions;
        if (type_of[m.conversation_id] == "meeting") ++meeting_msgs;
    }
    std::printf("  newest message: %s; mentions of me: %d; messages in meeting chats: %d\n", newest ? day(newest).c_str() : "-",
                mentions, meeting_msgs);
    std::filesystem::remove_all(priv, ec);
    return x.decode_errors ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool outlook = false, teams_on = false, cycle = false;
    int days = 14, read_n = 50;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "outlook") outlook = true;
        else if (a == "teams") teams_on = true;
        else if (a == "cycle") cycle = true;
        else if (a == "--days" && i + 1 < argc) days = std::atoi(argv[++i]);
        else if (a == "--read" && i + 1 < argc) read_n = std::atoi(argv[++i]);
    }
    if (cycle) return probe_cycle(days);
    if (!outlook && !teams_on) outlook = teams_on = true;
    int rc = 0;
    if (outlook) rc |= probe_outlook(days, read_n);
    if (teams_on) rc |= probe_teams();
    return rc;
}
