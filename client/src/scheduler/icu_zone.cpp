#include "scheduler/icu_zone.h"

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <vector>

namespace wb::scheduler {

namespace {

// The few ICU C entry points used, declared here: icu.dll exports them without version suffixes.
using UChar = char16_t;
using UErrorCode = int;
using UDate = double;
struct UCalendar;
constexpr int UCAL_GREGORIAN = 1;
constexpr int UCAL_YEAR = 1, UCAL_MONTH = 2, UCAL_DATE = 5, UCAL_DAY_OF_WEEK = 7, UCAL_HOUR_OF_DAY = 11, UCAL_MINUTE = 12,
              UCAL_SECOND = 13;
constexpr int UCAL_REPEATED_WALL_TIME = 3, UCAL_SKIPPED_WALL_TIME = 4;
constexpr int UCAL_WALLTIME_LAST = 0, UCAL_WALLTIME_FIRST = 1;
bool failed(UErrorCode e) { return e > 0; }

struct Icu {
    UCalendar* (*open)(const UChar*, int32_t, const char*, int, UErrorCode*) = nullptr;
    void (*close)(UCalendar*) = nullptr;
    void (*set_millis)(UCalendar*, UDate, UErrorCode*) = nullptr;
    UDate (*get_millis)(const UCalendar*, UErrorCode*) = nullptr;
    int32_t (*get)(const UCalendar*, int, UErrorCode*) = nullptr;
    void (*set_date_time)(UCalendar*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, UErrorCode*) = nullptr;
    void (*set_attribute)(UCalendar*, int, int32_t) = nullptr;
    void (*clear)(UCalendar*) = nullptr;
    int32_t (*canonical)(const UChar*, int32_t, UChar*, int32_t, int8_t*, UErrorCode*) = nullptr;
    int32_t (*default_zone)(UChar*, int32_t, UErrorCode*) = nullptr;
    bool ok = false;
};

const Icu& icu() {
    static const Icu lib = [] {
        Icu l;
        HMODULE h = ::LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!h) return l;
        auto get = [h](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(reinterpret_cast<void*>(::GetProcAddress(h, name)));
            return fn != nullptr;
        };
        l.ok = get(l.open, "ucal_open") && get(l.close, "ucal_close") && get(l.set_millis, "ucal_setMillis") &&
               get(l.get_millis, "ucal_getMillis") && get(l.get, "ucal_get") && get(l.set_date_time, "ucal_setDateTime") &&
               get(l.set_attribute, "ucal_setAttribute") && get(l.clear, "ucal_clear") && get(l.canonical, "ucal_getCanonicalTimeZoneID") &&
               get(l.default_zone, "ucal_getDefaultTimeZone");
        return l;
    }();
    return lib;
}

std::u16string u16(const std::string& s) { return std::u16string(s.begin(), s.end()); }  // zone ids are ASCII
std::string u8(const std::u16string& s) {
    std::string out;
    for (const char16_t c : s) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

class IcuZone final : public TimeZone {
public:
    IcuZone(std::string id, UCalendar* cal) : id_(std::move(id)), cal_(cal) {}
    ~IcuZone() override { icu().close(cal_); }
    IcuZone(const IcuZone&) = delete;
    IcuZone& operator=(const IcuZone&) = delete;

    [[nodiscard]] std::string id() const override { return id_; }

    [[nodiscard]] LocalTime local(long long utc_ms) const override {
        const std::lock_guard lock(mx_);
        UErrorCode e = 0;
        icu().set_millis(cal_, static_cast<UDate>(utc_ms), &e);
        return fields(e);
    }

    [[nodiscard]] std::optional<long long> utc(const LocalTime& t) const override {
        const std::lock_guard lock(mx_);
        UErrorCode e = 0;
        icu().clear(cal_);  // no milliseconds left over from an earlier time
        icu().set_date_time(cal_, t.year, t.month - 1, t.day, t.hour, t.minute, t.second, &e);
        const UDate ms = icu().get_millis(cal_, &e);
        if (failed(e)) return std::nullopt;
        // A time in a DST gap is moved by ICU; reading the fields back shows that it does not exist.
        icu().set_millis(cal_, ms, &e);
        const LocalTime back = fields(e);
        if (failed(e) || back.year != t.year || back.month != t.month || back.day != t.day || back.hour != t.hour ||
            back.minute != t.minute)
            return std::nullopt;
        return static_cast<long long>(ms);
    }

private:
    LocalTime fields(UErrorCode& e) const {
        LocalTime t;
        t.year = icu().get(cal_, UCAL_YEAR, &e);
        t.month = icu().get(cal_, UCAL_MONTH, &e) + 1;
        t.day = icu().get(cal_, UCAL_DATE, &e);
        t.hour = icu().get(cal_, UCAL_HOUR_OF_DAY, &e);
        t.minute = icu().get(cal_, UCAL_MINUTE, &e);
        t.second = icu().get(cal_, UCAL_SECOND, &e);
        t.dow = icu().get(cal_, UCAL_DAY_OF_WEEK, &e) - 1;
        return t;
    }

    std::string id_;
    UCalendar* cal_;
    mutable std::mutex mx_;
};

}  // namespace

Result<std::shared_ptr<TimeZone>> icu_zone(const std::string& iana_id) {
    if (iana_id == "UTC" || iana_id == "Etc/UTC") return std::shared_ptr<TimeZone>(std::make_shared<FixedZone>(0, iana_id));
    const Icu& l = icu();
    if (!l.ok) return make_error("unavailable", "the Windows time-zone data (icu.dll) is not available");
    const std::u16string id = u16(iana_id);
    UChar canon[128] = {};
    int8_t system = 0;
    UErrorCode e = 0;
    l.canonical(id.data(), static_cast<int32_t>(id.size()), canon, 128, &system, &e);
    if (failed(e) || !system) return make_error("invalid_time_zone", "unknown time zone " + iana_id);
    e = 0;
    UCalendar* cal = l.open(id.data(), static_cast<int32_t>(id.size()), "en_US", UCAL_GREGORIAN, &e);
    if (failed(e) || !cal) return make_error("invalid_time_zone", "unknown time zone " + iana_id);
    l.set_attribute(cal, UCAL_REPEATED_WALL_TIME, UCAL_WALLTIME_FIRST);
    l.set_attribute(cal, UCAL_SKIPPED_WALL_TIME, UCAL_WALLTIME_LAST);
    return std::shared_ptr<TimeZone>(std::make_shared<IcuZone>(iana_id, cal));
}

std::string local_zone_id() {
    const Icu& l = icu();
    if (!l.ok) return "UTC";
    UChar buf[128] = {};
    UErrorCode e = 0;
    const int32_t n = l.default_zone(buf, 128, &e);
    if (failed(e) || n <= 0) return "UTC";
    return u8(std::u16string(buf, static_cast<size_t>(n)));
}

}  // namespace wb::scheduler
