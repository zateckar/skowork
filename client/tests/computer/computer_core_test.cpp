// client/computer-use, portable part (add-computer-use tasks 2.2, 3.2, 3.3, 4.4): the destructive-action classifier
// over a labelled element set in Czech, German and English, the Risk class of each action, credential detection,
// application scoping, hotkeys and keys, and the screenshot coordinate transform at 100, 150 and 200 % scaling.
#include "computer/classify.h"

#include <gtest/gtest.h>

using namespace wb::computer;

namespace {

ElementInfo el(std::string type, std::string name, std::string id = {}) {
    ElementInfo e;
    e.control_type = std::move(type);
    e.name = std::move(name);
    e.automation_id = std::move(id);
    return e;
}

}  // namespace

TEST(ComputerClassify, LabelledElementSet) {
    struct Case {
        ElementInfo e;
        bool destructive;
    };
    const Case cases[] = {
        // en
        {el("button", "Send"), true},
        {el("button", "Submit application"), true},
        {el("button", "Delete"), true},
        {el("menuitem", "Remove from list"), true},
        {el("button", "Pay now"), true},
        {el("button", "Don't Save"), true},
        {el("button", "Close without saving"), true},
        {el("button", "Discard draft"), true},
        {el("button", "Save"), false},
        {el("button", "Cancel"), false},
        {el("button", "OK"), false},
        {el("button", "Open"), false},
        {el("tabitem", "Sender details"), true},  // a stem at a word start: classified conservatively as destructive
        {el("button", "Resend code"), false},     // not at a word start
        // cs
        {el("button", "Odeslat"), true},
        {el("button", "Odeslání žádosti"), true},
        {el("button", "Smazat"), true},
        {el("menuitem", "Odstranit řádek"), true},
        {el("button", "Zaplatit"), true},
        {el("button", "Neukládat"), true},
        {el("button", "Zavřít bez uložení"), true},
        {el("button", "Podat žádost"), true},
        {el("button", "Uložit"), false},
        {el("button", "Zrušit"), false},
        {el("button", "Otevřít"), false},
        // de
        {el("button", "Senden"), true},
        {el("button", "Absenden"), true},
        {el("button", "Löschen"), true},
        {el("menuitem", "Entfernen"), true},
        {el("button", "Jetzt bezahlen"), true},
        {el("button", "Nicht speichern"), true},
        {el("button", "Verwerfen"), true},
        {el("button", "Einreichen"), true},
        {el("button", "Speichern"), false},
        {el("button", "Abbrechen"), false},
        {el("button", "Öffnen"), false},
        // automation ids
        {el("button", "", "btnSendMail"), true},
        {el("button", "", "delete_button"), true},
        {el("button", "", "SaveButton"), false},
    };
    for (const auto& c : cases)
        EXPECT_EQ(is_destructive_control(c.e), c.destructive) << c.e.name << " / " << c.e.automation_id;
}

TEST(ComputerClassify, RiskClassOfActions) {
    const auto send = el("button", "Odeslat");
    const auto save = el("button", "Speichern");
    const auto edit = el("edit", "Subject");
    EXPECT_EQ(action_risk({"computer.observe"}, nullptr, "excel.exe"), "read");
    EXPECT_EQ(action_risk({"computer.screenshot"}, nullptr, "excel.exe"), "read");
    EXPECT_EQ(action_risk({"computer.click"}, &save, "excel.exe"), "external");
    EXPECT_EQ(action_risk({"computer.click"}, &send, "excel.exe"), "destructive");
    EXPECT_EQ(action_risk({"computer.click"}, nullptr, "excel.exe"), "external");
    EXPECT_EQ(action_risk({"computer.select"}, &send, "excel.exe"), "destructive");
    // Typing only fills a field; with submit it also activates the target.
    EXPECT_EQ(action_risk({"computer.type"}, &send, "excel.exe"), "external");
    EXPECT_EQ(action_risk({"computer.type", "", true}, &send, "excel.exe"), "destructive");
    // Enter and Ctrl+Enter in mail and chat clients send.
    EXPECT_EQ(action_risk({"computer.keys", "Ctrl+Enter"}, &edit, "OUTLOOK.EXE"), "destructive");
    EXPECT_EQ(action_risk({"computer.keys", "Enter"}, &edit, "ms-teams.exe"), "destructive");
    EXPECT_EQ(action_risk({"computer.type", "", true}, &edit, "slack.exe"), "destructive");
    EXPECT_EQ(action_risk({"computer.keys", "Enter"}, &edit, "notepad.exe"), "external");
    EXPECT_EQ(action_risk({"computer.keys", "Ctrl+S"}, &edit, "outlook.exe"), "external");
    // Delete removes the selected item outside a text field; Shift+Delete always.
    EXPECT_EQ(action_risk({"computer.keys", "Delete"}, &edit, "explorer.exe"), "external");
    EXPECT_EQ(action_risk({"computer.keys", "Delete"}, nullptr, "explorer.exe"), "destructive");
    EXPECT_EQ(action_risk({"computer.keys", "Shift+Delete"}, &edit, "explorer.exe"), "destructive");
}

TEST(ComputerClassify, CredentialFields) {
    ElementInfo pw = el("edit", "Heslo");
    pw.is_password = true;
    EXPECT_TRUE(is_credential_element(pw, "sapgui.exe"));
    EXPECT_FALSE(is_credential_element(el("edit", "User name"), "sapgui.exe"));
    EXPECT_TRUE(is_credential_element(el("edit", "User name"), "CredentialUIBroker.exe"));
    EXPECT_TRUE(is_credential_element(el("edit", ""), "consent.exe"));
    ElementInfo xaml = el("pane", "");
    xaml.class_name = "Credential Dialog Xaml Host";
    EXPECT_TRUE(is_credential_element(xaml, "explorer.exe"));
    EXPECT_TRUE(is_credential_process("C:\\Windows\\System32\\LogonUI.exe"));
    EXPECT_TRUE(is_credential_process("lockapp"));
    EXPECT_FALSE(is_credential_process("excel.exe"));
}

TEST(ComputerClassify, ApplicationScoping) {
    const std::vector<std::string> own = {"Skowork.exe", "skowork-worker.exe"};
    EXPECT_FALSE(app_refusal("EXCEL.EXE", {}, {}, own));
    EXPECT_TRUE(app_refusal("Skowork.exe", {}, {}, own));
    EXPECT_TRUE(app_refusal("skowork.exe", {"skowork.exe"}, {}, own)) << "own windows stay out of scope even when allowed";
    EXPECT_TRUE(app_refusal("CredentialUIBroker.exe", {}, {}, own));
    EXPECT_TRUE(app_refusal("SecHealthUI.exe", {}, {}, own));
    const auto denied = app_refusal("KeePass.exe", {}, {"keepass.exe"}, own);
    ASSERT_TRUE(denied);
    EXPECT_NE(denied->find("keepass.exe"), std::string::npos) << "the refusal names the application";
    EXPECT_NE(denied->find("denylist"), std::string::npos);
    EXPECT_TRUE(app_refusal("excel.exe", {"excel"}, {"EXCEL.EXE"}, own)) << "the denylist wins";
    EXPECT_FALSE(app_refusal("excel.exe", {"excel"}, {}, own)) << "allowlist entries may omit .exe";
    const auto not_listed = app_refusal("notepad.exe", {"excel.exe", "saplogon.exe"}, {}, own);
    ASSERT_TRUE(not_listed);
    EXPECT_NE(not_listed->find("allowlist"), std::string::npos);
    EXPECT_FALSE(app_refusal("notepad.exe", {"", " "}, {}, own)) << "blank entries are no allowlist";
    EXPECT_TRUE(app_refusal("", {}, {}, own));
}

TEST(ComputerClassify, LaunchPrograms) {
    EXPECT_EQ(launch_executable("notepad.exe"), "notepad.exe");
    EXPECT_EQ(launch_executable("notepad"), "notepad.exe") << "a bare name gets .exe, as CreateProcess adds it";
    EXPECT_EQ(launch_executable("C:\\Program Files\\App\\app.EXE"), "app.EXE");
    EXPECT_EQ(launch_executable("  calc.exe  "), "calc.exe");
    EXPECT_EQ(launch_executable("report.docx"), "") << "documents are not launched";
    EXPECT_EQ(launch_executable("https://example.com"), "") << "URLs are not launched";
    EXPECT_EQ(launch_executable("ms-settings:display"), "");
    EXPECT_EQ(launch_executable("notepad.exe /p x"), "") << "a command line is not an executable";
    EXPECT_EQ(launch_executable("notepad.exe /p"), "") << "an argument after a slash is not a path";
    EXPECT_EQ(launch_executable("C:\\foo\\app.exe /p"), "") << "arguments after an absolute path are refused";
    EXPECT_EQ(launch_executable("my app.exe"), "") << "a relative name with a space is refused";
    EXPECT_EQ(launch_executable("C:\\Program Files\\App\\app.exe"), "app.exe") << "spaces in directories of an absolute path are fine";
    EXPECT_EQ(launch_executable("notepad.exe\" /p"), "") << "quotes are refused";
    EXPECT_EQ(launch_executable(""), "");
    EXPECT_EQ(launch_executable("C:\\"), "") << "a bare drive has no program";
    EXPECT_TRUE(is_command_interpreter("cmd.exe"));
    EXPECT_TRUE(is_command_interpreter("PowerShell.exe"));
    EXPECT_TRUE(is_command_interpreter("rundll32.exe"));
    EXPECT_FALSE(is_command_interpreter("notepad.exe"));
}

TEST(ComputerClassify, Hotkeys) {
    const auto h = parse_hotkey("Ctrl+Alt+End");
    ASSERT_TRUE(h);
    EXPECT_EQ(h->modifiers, 2u | 1u);
    EXPECT_EQ(h->vk, 0x23u);
    EXPECT_EQ(h->text, "Ctrl+Alt+End");
    const auto p = parse_hotkey("shift + win + pause");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->modifiers, 4u | 8u);
    EXPECT_EQ(p->vk, 0x13u);
    EXPECT_EQ(p->text, "Shift+Win+Pause");
    EXPECT_FALSE(parse_hotkey("Ctrl+End")) << "at least two modifiers";
    EXPECT_FALSE(parse_hotkey("End"));
    EXPECT_FALSE(parse_hotkey("Ctrl+Alt"));
    EXPECT_FALSE(parse_hotkey("Ctrl+Alt+Nope"));
    EXPECT_FALSE(parse_hotkey("Ctrl+Alt+End+Home"));
}

TEST(ComputerClassify, Keys) {
    const auto k = parse_keys("Ctrl+S");
    ASSERT_TRUE(k);
    ASSERT_EQ(k->size(), 1u);
    EXPECT_EQ((*k)[0].modifiers, 2u);
    EXPECT_EQ((*k)[0].vk, static_cast<unsigned>('S'));
    const auto seq = parse_keys("Down Down  Enter");
    ASSERT_TRUE(seq);
    ASSERT_EQ(seq->size(), 3u);
    EXPECT_EQ((*seq)[2].vk, 0x0Du);
    const auto spaced = parse_keys("Ctrl + Shift + Tab F5");
    ASSERT_TRUE(spaced);
    ASSERT_EQ(spaced->size(), 2u);
    EXPECT_EQ((*spaced)[0].modifiers, 2u | 4u);
    EXPECT_EQ((*spaced)[0].vk, 0x09u);
    EXPECT_EQ((*spaced)[1].vk, 0x74u);
    const auto alt = parse_keys("Alt");
    ASSERT_TRUE(alt);
    EXPECT_EQ((*alt)[0].vk, 0x12u);
    EXPECT_FALSE(parse_keys("Ctrl+Blah"));
    EXPECT_FALSE(parse_keys(""));
    EXPECT_FALSE(parse_keys("F25"));
}

// The model sees a downscaled image of a window captured in physical pixels. At 100 %, 150 % and 200 % display
// scaling the same 800x600 (logical) window covers 800x600, 1200x900 and 1600x1200 physical pixels; a point the model
// picks in the image must land on the same place of the window.
TEST(ComputerClassify, CoordinateTransformAtScalings) {
    for (const double scale : {1.0, 1.5, 2.0}) {
        const int left = static_cast<int>(300 * scale), top = static_cast<int>(200 * scale);
        const int w = static_cast<int>(800 * scale), h = static_cast<int>(600 * scale);
        // Downscaled to a longest side of 1568 at most (client/model-access Image Input).
        const double down = std::min(1.0, 1568.0 / std::max(w, h));
        const int iw = static_cast<int>(w * down + 0.5), ih = static_cast<int>(h * down + 0.5);
        const CaptureTransform t{left, top, w, h, iw, ih};
        // A button at logical (400, 300) inside the window, 40x20 logical.
        const double bx = 400 * scale, by = 300 * scale;
        const double ix = (bx + 20 * scale) * iw / w, iy = (by + 10 * scale) * ih / h;  // its centre in the image
        const auto s = t.to_screen(ix, iy);
        ASSERT_TRUE(s) << scale;
        EXPECT_NEAR(s->first, left + bx + 20 * scale, 1 + w / static_cast<double>(iw)) << scale;
        EXPECT_NEAR(s->second, top + by + 10 * scale, 1 + h / static_cast<double>(ih)) << scale;
        // Corners stay inside the window; points outside the image are refused.
        EXPECT_EQ(t.to_screen(0, 0)->first, left + static_cast<int>(0.5 * w / iw));
        EXPECT_LT(t.to_screen(iw - 0.01, ih - 0.01)->first, left + w);
        EXPECT_FALSE(t.to_screen(-1, 5));
        EXPECT_FALSE(t.to_screen(iw, 5));
    }
}
