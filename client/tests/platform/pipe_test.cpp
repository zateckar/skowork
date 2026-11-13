#include "platform/crypto.h"
#include "platform/pipe.h"
#include "platform/win.h"

#include <windows.h>
#include <aclapi.h>
#include <iphlpapi.h>
#include <sddl.h>

#include <gtest/gtest.h>

using namespace wb;

namespace {
std::wstring unique_pipe() { return L"\\\\.\\pipe\\skowork-test-" + widen(random_urlsafe(8)); }
}  // namespace

TEST(Pipe, EchoAndSingleOwner) {
    const auto name = unique_pipe();
    auto server = PipeServer::create(name);
    ASSERT_TRUE(server);
    (*server)->start([](const std::string& req) { return "echo:" + req; });

    auto r = pipe_call(name, "hello");
    ASSERT_TRUE(r);
    EXPECT_EQ(*r, "echo:hello");

    // A second owner is refused: this is the single-instance check.
    auto second = PipeServer::create(name);
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, "exists");

    // Concurrent clients are served.
    std::vector<std::thread> ts;
    std::atomic<int> ok{0};
    for (int i = 0; i < 8; ++i)
        ts.emplace_back([&, i] {
            auto x = pipe_call(name, std::to_string(i));
            if (x && *x == "echo:" + std::to_string(i)) ++ok;
        });
    for (auto& t : ts) t.join();
    EXPECT_EQ(ok.load(), 8);
    (*server)->stop();
}

TEST(Pipe, DaclGrantsOnlyTheCurrentUser) {
    const auto name = unique_pipe();
    auto server = PipeServer::create(name);
    ASSERT_TRUE(server);
    (*server)->start([](const std::string& req) { return req; });
    unique_file h(::CreateFileW(name.c_str(), READ_CONTROL, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(h);
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    ASSERT_EQ(::GetSecurityInfo(h.get(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd),
              ERROR_SUCCESS);
    unique_hlocal sd_guard(sd);
    ASSERT_NE(dacl, nullptr);
    ASSERT_EQ(dacl->AceCount, 1);
    void* ace = nullptr;
    ASSERT_TRUE(::GetAce(dacl, 0, &ace));
    auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
    EXPECT_EQ(allowed->Header.AceType, ACCESS_ALLOWED_ACE_TYPE);
    LPWSTR sid = nullptr;
    ASSERT_TRUE(::ConvertSidToStringSidW(reinterpret_cast<PSID>(&allowed->SidStart), &sid));
    EXPECT_EQ(std::wstring(sid), current_user_sid());
    ::LocalFree(sid);
    h.reset();
    (*server)->stop();
}

TEST(Pipe, RefusesOversizedFrames) {
    const auto name = unique_pipe();
    auto server = PipeServer::create(name);
    ASSERT_TRUE(server);
    (*server)->start([](const std::string& req) { return req; });
    unique_file h(::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(h);
    const std::uint32_t huge = 0x7FFFFFFF;
    DWORD w = 0;
    ASSERT_TRUE(::WriteFile(h.get(), &huge, 4, &w, nullptr));
    std::string resp;
    EXPECT_FALSE(pipe_read_frame(h.get(), resp));  // the server drops the connection
    h.reset();
    (*server)->stop();
}
