#include "runtime/tools.h"

#include "base/strings.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace wb::runtime {

// ---- RunControl -----------------------------------------------------------------------------------------------

RunControl::RunControl() : http_(std::make_shared<CancelToken>()) {}

RunControl::~RunControl() {
    if (job_) ::CloseHandle(static_cast<HANDLE>(job_));  // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE ends leftover children
}

std::string RunControl::reason() const {
    const std::lock_guard lock(mx_);
    return reason_;
}

void RunControl::cancel(const std::string& reason) {
    std::vector<std::function<void()>> callbacks;
    {
        const std::lock_guard lock(mx_);
        if (cancelled_.exchange(true)) return;
        reason_ = reason;
        callbacks.swap(callbacks_);
    }
    http_->cancel();
    for (auto& fn : callbacks) {
        try {
            fn();
        } catch (...) {
            // a failing cleanup must not stop the others
        }
    }
    terminate_processes();
}

void RunControl::on_cancel(std::function<void()> fn) {
    {
        const std::lock_guard lock(mx_);
        if (!cancelled_.load()) {
            callbacks_.push_back(std::move(fn));
            return;
        }
    }
    fn();
}

Status RunControl::assign_process(void* process_handle) {
    {
        const std::lock_guard lock(mx_);
        if (!job_ && !cancelled_.load()) {
            HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
            if (!job) return make_error("job", "cannot create the job object for the Run");
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info)) {
                ::CloseHandle(job);
                return make_error("job", "cannot configure the job object for the Run");
            }
            job_ = job;
        }
        if (job_ && !cancelled_.load()) {
            if (!::AssignProcessToJobObject(static_cast<HANDLE>(job_), static_cast<HANDLE>(process_handle)))
                return make_error("job", "cannot assign the process to the Run's job object");
            return {};
        }
    }
    // Cancelled before the process was assigned: it must not survive.
    ::TerminateProcess(static_cast<HANDLE>(process_handle), 1);
    return make_error("cancelled", "the Run was cancelled");
}

void RunControl::terminate_processes() {
    const std::lock_guard lock(mx_);
    if (job_) ::TerminateJobObject(static_cast<HANDLE>(job_), 1);
}

// ---- Risk class -----------------------------------------------------------------------------------------------

bool risk_is_declared(const ToolDef& tool) {
    return tool.risk.has_value() || static_cast<bool>(tool.risk_of) || static_cast<bool>(tool.risk_in_run);
}

Risk declared_risk(const ToolDef& tool) {
    // A tool whose risk depends on its arguments is treated as the worst case until a call exists; one that computes
    // it from the Run declares its highest class in `risk`.
    if (tool.risk_of) return Risk::destructive;
    return tool.risk.value_or(Risk::destructive);
}

Risk lowest_risk(const ToolDef& tool) {
    if (tool.risk_of || tool.risk_in_run) return std::min(tool.min_risk.value_or(Risk::read), declared_risk(tool));
    return declared_risk(tool);
}

Risk effective_risk(const ToolDef& tool, const json& args, const std::string& run_id) {
    try {
        if (tool.risk_in_run) return tool.risk_in_run(run_id, args);
        if (tool.risk_of) return tool.risk_of(args);
    } catch (...) {
        return Risk::destructive;  // cannot tell: assume the worst
    }
    return tool.risk.value_or(Risk::destructive);
}

bool tool_offered(const ToolDef& tool) {
    if (!tool.offered) return true;
    try {
        return tool.offered();
    } catch (...) {
        return false;
    }
}

// ---- Registry -------------------------------------------------------------------------------------------------

void ToolRegistry::add(ToolDef tool) {
    const std::string id = tool.id;
    auto& slot = tools_[id];
    if (slot) retired_.push_back(std::move(slot));
    slot = std::make_unique<ToolDef>(std::move(tool));
}

bool ToolRegistry::remove(const std::string& id) {
    const auto it = tools_.find(id);
    if (it == tools_.end()) return false;
    retired_.push_back(std::move(it->second));
    tools_.erase(it);
    return true;
}

const ToolDef* ToolRegistry::find(const std::string& id) const {
    const auto it = tools_.find(id);
    return it == tools_.end() ? nullptr : it->second.get();
}

std::vector<const ToolDef*> ToolRegistry::all() const {
    std::vector<const ToolDef*> out;
    out.reserve(tools_.size());
    for (const auto& [id, tool] : tools_) {
        (void)id;
        out.push_back(tool.get());
    }
    return out;
}

model::ToolSpec wire_spec(const ToolDef& tool, bool with_rationale) {
    model::ToolSpec spec;
    spec.id = tool.id;
    spec.description = tool.description;
    spec.parameters = tool.parameters.is_object() ? tool.parameters : json{{"type", "object"}};
    if (!spec.parameters.contains("type")) spec.parameters["type"] = "object";
    if (with_rationale) {
        if (!spec.parameters.contains("properties") || !spec.parameters["properties"].is_object())
            spec.parameters["properties"] = json::object();
        spec.parameters["properties"]["rationale"] = {
            {"type", "string"}, {"minLength", 1}, {"description", "Why this call is needed. The user sees this when approving."}};
        json required = spec.parameters.contains("required") && spec.parameters["required"].is_array() ? spec.parameters["required"]
                                                                                                        : json::array();
        if (std::find(required.begin(), required.end(), json("rationale")) == required.end()) required.push_back("rationale");
        spec.parameters["required"] = std::move(required);
    }
    return spec;
}

size_t estimate_tokens(std::string_view text) { return (text.size() + 3) / 4; }

size_t estimate_tool_tokens(const std::vector<model::ToolSpec>& tools) {
    size_t total = 0;
    for (const auto& t : tools) total += estimate_tokens(t.id) + estimate_tokens(t.description) + estimate_tokens(t.parameters.dump()) + 8;
    return total;
}

// ---- find_tools index -----------------------------------------------------------------------------------------

std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> out;
    std::string cur;
    const auto flush = [&] {
        if (cur.empty()) return;
        if (cur.size() > 3 && cur.back() == 's') cur.pop_back();  // calendars -> calendar
        out.push_back(std::move(cur));
        cur.clear();
    };
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) cur += static_cast<char>(std::tolower(u));
        else flush();
    }
    flush();
    return out;
}

ToolIndex::ToolIndex(const std::vector<const ToolDef*>& tools) {
    long long total_length = 0;
    for (const ToolDef* t : tools) {
        Doc doc;
        doc.id = t->id;
        for (const auto& term : tokenize(t->id)) {
            doc.terms[term] += 3;  // the name counts more than the description
            doc.length += 3;
        }
        for (const auto& term : tokenize(t->description)) {
            ++doc.terms[term];
            ++doc.length;
        }
        for (const auto& [term, n] : doc.terms) {
            (void)n;
            ++doc_freq_[term];
        }
        total_length += doc.length;
        docs_.push_back(std::move(doc));
    }
    avg_length_ = docs_.empty() ? 1.0 : std::max(1.0, static_cast<double>(total_length) / static_cast<double>(docs_.size()));
}

std::vector<ToolIndex::Hit> ToolIndex::search(const std::string& query, size_t limit) const {
    constexpr double k1 = 1.2, b = 0.75;
    const auto terms = tokenize(query);
    const double n = static_cast<double>(docs_.size());
    std::vector<Hit> hits;
    for (const auto& doc : docs_) {
        double score = 0;
        for (const auto& term : terms) {
            const auto tf = doc.terms.find(term);
            if (tf == doc.terms.end()) continue;
            const double df = static_cast<double>(doc_freq_.at(term));
            const double idf = std::log((n - df + 0.5) / (df + 0.5) + 1.0);
            const double f = static_cast<double>(tf->second);
            score += idf * f * (k1 + 1) / (f + k1 * (1 - b + b * static_cast<double>(doc.length) / avg_length_));
        }
        if (score > 0) hits.push_back({doc.id, score});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& c) { return a.score != c.score ? a.score > c.score : a.id < c.id; });
    if (hits.size() > limit) hits.resize(limit);
    return hits;
}

}  // namespace wb::runtime
