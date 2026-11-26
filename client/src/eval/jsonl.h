// One JSON record per line, for evaluation exports (add-evaluation-export design D2). Text that is not valid UTF-8 (a
// lone surrogate from Outlook, a cut multi-byte character) is replaced with U+FFFD; a write never throws.
#pragma once

#include "base/json.h"
#include "base/result.h"

#include <filesystem>
#include <fstream>
#include <memory>

namespace wb::eval {

class JsonlWriter {
public:
    static Result<JsonlWriter> open(const std::filesystem::path& file);
    Status write(const json& record);
    Status close();
    [[nodiscard]] long long lines() const { return lines_; }

private:
    std::unique_ptr<std::ofstream> out_;
    std::filesystem::path file_;
    long long lines_ = 0;
};

// The record as one line of JSON, with invalid UTF-8 replaced (never throws).
std::string jsonl_line(const json& record);

}  // namespace wb::eval
