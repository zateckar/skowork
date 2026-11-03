// client/core-tools Managed Downloads (add-core-tools-and-mcp design D3): files a tool downloads are written to
// <data root>\quarantine with the Windows mark-of-the-web (a Zone.Identifier stream, Internet zone). The data root is
// denied to every tool by client/policy-enforcement, so the agent cannot open them, and shell.run treats any command
// that names the quarantine as destructive, so running one needs the user's approval.
#pragma once

#include "base/result.h"

#include <string>
#include <string_view>

namespace wb::tools {

struct QuarantinedFile {
    std::string path;  // full path of the saved file
    size_t size = 0;
};

// Saves `bytes` under `dir` (created if missing) as "<UTC timestamp>-<safe name>", never overwriting a file, and marks
// it as downloaded from `source_url` (ZoneId=3, HostUrl, ReferrerUrl when given).
Result<QuarantinedFile> quarantine_save(const std::string& dir, std::string_view suggested_name, std::string_view bytes,
                                        std::string_view source_url, std::string_view referrer_url = {});

// A file name that is safe on Windows: no path separators, reserved characters, control characters, trailing dots or
// spaces, or device names; at most 120 characters; "download" when nothing usable is left.
std::string safe_file_name(std::string_view name);

// The file name a download suggests: the Content-Disposition filename, else the last segment of the URL path.
std::string download_name(std::string_view content_disposition, std::string_view url);

}  // namespace wb::tools
