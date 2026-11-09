#include "channel/machine_config.h"
#include "base/json.h"
#include "platform/crypto.h"
#include "platform/registry.h"
#include "platform/win.h"

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <string_view>

namespace {
std::map<std::wstring, std::wstring> registry;
constexpr char kConfig[] = R"({"control_plane_url":"https://workbench.example.com","idp_issuer":"https://keycloak.example.com/realms/example","idp_client_id":"workbench-desktop"})";

std::string utf16(std::u16string_view text, bool little_endian) {
    std::string out = little_endian ? "\xff\xfe" : "\xfe\xff";
    for (const char16_t c : text) {
        const char lo = static_cast<char>(c & 0xff);
        const char hi = static_cast<char>(c >> 8);
        out += little_endian ? lo : hi;
        out += little_endian ? hi : lo;
    }
    return out;
}

void write_file(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(f.good());
}

class MachineConfig : public testing::Test {
protected:
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("workbench-config-" + wb::random_urlsafe(8));
    std::filesystem::path file = dir / L"workbench.machine.json";

    void SetUp() override {
        registry.clear();
        std::filesystem::create_directories(dir);
    }
    void TearDown() override {
        registry.clear();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

TEST_F(MachineConfig, ReadmeExampleAndDefaults) {
    write_file(file, kConfig);
    const auto c = wb::load_machine_config_file(file);
    ASSERT_TRUE(c) << c.error().message;
    EXPECT_EQ(c->control_plane_url, "https://workbench.example.com");
    EXPECT_EQ(c->idp_issuer, "https://keycloak.example.com/realms/example");
    EXPECT_EQ(c->idp_client_id, "workbench-desktop");
    EXPECT_EQ(c->idp_redirect_uri, "http://localhost:4200/auth/callback");
    EXPECT_EQ(c->idp_scope, "openid profile email");
    EXPECT_EQ(c->channel, "stable");
    auto j = wb::json::parse(kConfig);
    j.erase("idp_client_id");
    write_file(file, j.dump());
    const auto defaults = wb::load_machine_config_file(file);
    ASSERT_TRUE(defaults) << defaults.error().message;
    EXPECT_EQ(defaults->idp_client_id, "workbench-desktop");
}

TEST_F(MachineConfig, WindowsEditorEncodingsPreserveUnicode) {
    const auto j = wb::json::parse(kConfig);
    auto custom = j;
    custom["channel"] = "té🔧";
    const auto text = custom.dump();
    const auto wide = wb::widen(text);
    const std::u16string units(wide.begin(), wide.end());
    for (const auto& bytes : {text, "\xef\xbb\xbf" + text, utf16(units, true), utf16(units, false)}) {
        write_file(file, bytes);
        const auto c = wb::load_machine_config_file(file);
        ASSERT_TRUE(c) << c.error().message;
        EXPECT_EQ(c->channel, "té🔧");
    }
}

TEST_F(MachineConfig, RejectsMalformedJsonAndEncodingWithoutEchoingContents) {
    const std::u16string lone_high = u"{\"channel\":\"" + std::u16string(1, 0xd800) + u"\"}";
    const std::u16string lone_low = u"{\"channel\":\"" + std::u16string(1, 0xdc00) + u"\"}";
    for (const auto& bytes : {std::string{}, std::string("{secret-invalid-json"), std::string("[]"),
                             std::string("\xff\xfe{"), utf16(lone_high, true), utf16(lone_low, false),
                             std::string("{\"channel\":\"\xff\"}")}) {
        write_file(file, bytes);
        const auto c = wb::load_machine_config_file(file);
        ASSERT_FALSE(c);
        EXPECT_EQ(c.error().code, "config");
        EXPECT_NE(c.error().message.find(wb::narrow(file.wstring())), std::string::npos);
        EXPECT_EQ(c.error().message.find("secret-invalid-json"), std::string::npos);
    }
}

TEST_F(MachineConfig, MissingFileNamesAttemptedPathAndNotepadExtension) {
    auto c = wb::load_machine_config_file(file);
    ASSERT_FALSE(c);
    EXPECT_EQ(c.error().code, "not_found");
    EXPECT_NE(c.error().message.find(wb::narrow(file.wstring())), std::string::npos);
    auto txt = file;
    txt += L".txt";
    write_file(txt, kConfig);
    c = wb::load_machine_config_file(file);
    ASSERT_FALSE(c);
    EXPECT_EQ(c.error().code, "not_found");
    EXPECT_NE(c.error().message.find(".json.txt"), std::string::npos);
    EXPECT_NE(c.error().message.find("Rename"), std::string::npos);
}

TEST_F(MachineConfig, UnreadableFileIsNotReportedAsMissing) {
    write_file(file, kConfig);
    wb::unique_file lock(::CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(lock);
    const auto c = wb::load_machine_config_file(file);
    ASSERT_FALSE(c);
    EXPECT_EQ(c.error().code, "config");
    EXPECT_NE(c.error().message.find(wb::narrow(file.wstring())), std::string::npos);
    EXPECT_NE(c.error().message.find("read"), std::string::npos);
}

TEST_F(MachineConfig, MissingRequiredSettingNamesJsonKeyAndPath) {
    auto j = wb::json::parse(kConfig);
    j.erase("idp_issuer");
    write_file(file, j.dump());
    const auto c = wb::load_machine_config_file(file);
    ASSERT_FALSE(c);
    EXPECT_NE(c.error().message.find("idp_issuer"), std::string::npos);
    EXPECT_NE(c.error().message.find(wb::narrow(file.wstring())), std::string::npos);
}

TEST_F(MachineConfig, ValidatesUrlsAndRedirect) {
    for (const auto* key : {"control_plane_url", "idp_issuer", "idp_redirect_uri"}) {
        auto j = wb::json::parse(kConfig);
        j[key] = "http://example.com/unsafe";
        write_file(file, j.dump());
        const auto c = wb::load_machine_config_file(file);
        ASSERT_FALSE(c) << key;
        EXPECT_EQ(c.error().code, "config");
        EXPECT_NE(c.error().message.find(wb::narrow(file.wstring())), std::string::npos);
    }
#ifndef WORKBENCH_DEV_ENDPOINTS
    auto j = wb::json::parse(kConfig);
    j["control_plane_url"] = "http://localhost:8080";
    write_file(file, j.dump());
    EXPECT_FALSE(wb::load_machine_config_file(file));
#endif
}

TEST_F(MachineConfig, RegistryRemainsAuthoritativeEvenWhenInvalid) {
    registry = {{L"ControlPlaneUrl", L"https://registry.example.com"}, {L"IdpIssuer", L"https://identity.example.com"},
                {L"IdpClientId", L"managed-client"}, {L"Channel", L"managed"}};
    auto c = wb::load_machine_config();
    ASSERT_TRUE(c) << c.error().message;
    EXPECT_EQ(c->control_plane_url, "https://registry.example.com");
    EXPECT_EQ(c->idp_client_id, "managed-client");
    EXPECT_EQ(c->channel, "managed");
    registry.erase(L"IdpIssuer");
    c = wb::load_machine_config();
    ASSERT_FALSE(c);
    EXPECT_NE(c.error().message.find("HKLM\\SOFTWARE\\Policies\\Workbench"), std::string::npos);
    registry[L"ControlPlaneUrl"] = L"";
    EXPECT_FALSE(wb::load_machine_config());
}

TEST_F(MachineConfig, DiscoversBesideExecutableFromOtherWorkingDirectory) {
    const auto install = dir / L"Workbench \u017delu\u0165ou\u010dk\u00fd \u6d4b\u8bd5";
    const auto cwd = dir / L"other";
    std::filesystem::create_directories(install);
    std::filesystem::create_directories(cwd);
    const auto exe = install / L"config-probe.exe";
    std::filesystem::copy_file(wb::exe_path(), exe);
    write_file(cwd / L"workbench.machine.json", "invalid working-directory configuration");
    const std::u16string units(kConfig, kConfig + sizeof(kConfig) - 1);
    for (const auto& bytes : {std::string(kConfig), utf16(units, true)}) {
        write_file(install / L"workbench.machine.json", bytes);
        std::wstring command = L"\"" + exe.wstring() + L"\" --probe-machine-config";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION pi{};
        ASSERT_TRUE(::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                     nullptr, cwd.c_str(), &startup, &pi));
        wb::unique_handle process(pi.hProcess);
        wb::unique_handle thread(pi.hThread);
        const DWORD wait = ::WaitForSingleObject(process.get(), 10000);
        if (wait != WAIT_OBJECT_0) {
            ::TerminateProcess(process.get(), 99);
            ::WaitForSingleObject(process.get(), 1000);
        }
        ASSERT_EQ(wait, WAIT_OBJECT_0);
        DWORD code = 99;
        ASSERT_TRUE(::GetExitCodeProcess(process.get(), &code));
        EXPECT_EQ(code, 0u);
    }
}
}  // namespace

namespace wb {
// Link-time double for this standalone test executable only. All file/path/validation code is production code.
std::optional<std::wstring> reg_read_string(HKEY root, const std::wstring& key, const std::wstring& name) {
    EXPECT_EQ(root, HKEY_LOCAL_MACHINE);
    EXPECT_EQ(key, L"SOFTWARE\\Policies\\Workbench");
    const auto it = registry.find(name);
    return it == registry.end() ? std::nullopt : std::optional<std::wstring>(it->second);
}
}  // namespace wb

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--probe-machine-config") {
        const auto c = wb::load_machine_config();
        return c && c->control_plane_url == "https://workbench.example.com" &&
                       c->idp_issuer == "https://keycloak.example.com/realms/example" &&
                       c->idp_client_id == "workbench-desktop" && c->channel == "stable" ? 0 : 1;
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
