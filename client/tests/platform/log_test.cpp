// client/desktop-host Diagnostic Logs: rotation under the cap and no token-shaped strings in any log file.
#include "base/encoding.h"
#include "base/redact.h"
#include "platform/crypto.h"
#include "platform/log.h"
#include "platform/win.h"

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <sstream>

using namespace wb;

namespace {

std::filesystem::path temp_dir(const char* tag) {
    auto d = std::filesystem::temp_directory_path() / ("workbench-" + std::string(tag) + "-" + random_urlsafe(6));
    std::filesystem::create_directories(d);
    return d;
}

std::string read_all(const std::filesystem::path& dir) {
    std::string all;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        std::ifstream f(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        all += ss.str();
    }
    return all;
}

}  // namespace

TEST(Log, NoTokenShapedStringsAreWritten) {
    const auto dir = temp_dir("log");
    log::init(log::Options{dir, 1024 * 1024, 64 * 1024, false});
    const std::string jwt = base64url_encode(std::string_view(R"({"alg":"RS256","kid":"k"})")) + "." +
                            base64url_encode(std::string_view(R"({"sub":"user-1","exp":1})")) + "." +
                            random_urlsafe(64);
    const std::string refresh = random_urlsafe(48);
    log::info("test", "access token " + jwt);
    log::info("test", "Authorization: Bearer " + jwt);
    log::warn("test", "refresh_token=" + refresh + "&client_id=workbench-desktop");
    log::error("test", "{\"access_token\":\"" + jwt + "\",\"token_type\":\"Bearer\"}");
    log::info("test", "control plane connected");
    log::shutdown();

    const std::string all = read_all(dir);
    EXPECT_EQ(all.find(jwt), std::string::npos);
    EXPECT_EQ(all.find(refresh), std::string::npos);
    EXPECT_NE(all.find("control plane connected"), std::string::npos);
    // Scanner: nothing that looks like a JWT or a long opaque token survives.
    const std::regex jwt_re(R"(eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,})");
    EXPECT_FALSE(std::regex_search(all, jwt_re));
    const std::regex opaque_re(R"([A-Za-z0-9_-]{40,})");
    EXPECT_FALSE(std::regex_search(all, opaque_re));
    std::filesystem::remove_all(dir);
}

TEST(Log, RotatesAndDeletesOldestFirst) {
    const auto dir = temp_dir("rot");
    log::init(log::Options{dir, 200 * 1024, 32 * 1024, false});
    const std::string line(900, 'x');
    for (int i = 0; i < 1500; ++i) log::info("test", std::to_string(i) + " " + line.substr(0, 100) + " " + std::to_string(i));
    log::shutdown();
    std::uintmax_t total = 0;
    int files = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        total += e.file_size();
        ++files;
    }
    EXPECT_GT(files, 1);
    EXPECT_LE(total, 200u * 1024u + 32u * 1024u);  // the cap, plus at most the file being written
    const std::string all = read_all(dir);
    EXPECT_NE(all.find(" 1499"), std::string::npos);  // newest kept
    EXPECT_EQ(all.find("] 0 x"), std::string::npos);  // oldest gone
    std::filesystem::remove_all(dir);
}
