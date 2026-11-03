// The computer.* tool definitions without a desktop: the target checks that run before the policy decides a call, the
// launch refusals, and the provenance of failed results. Nothing here sends input or starts a program.
#include "computer/service.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace wb::computer {
namespace {

std::shared_ptr<ComputerService> service() {
    ComputerEnv env;
    env.unreachable_surface = [] { return std::optional<std::string>{}; };
    return ComputerService::create(std::move(env));
}

const runtime::ToolDef& tool(const std::vector<runtime::ToolDef>& tools, const std::string& id) {
    for (const auto& t : tools)
        if (t.id == id) return t;
    throw std::runtime_error("no tool " + id);
}

TEST(ComputerService, ActionWithoutTargetFailsBeforeApproval) {
    auto s = service();
    const auto tools = computer_tools(s);

    const auto& click = tool(tools, "computer.click");
    ASSERT_TRUE(click.precheck);
    const auto no_ref = click.precheck("run-1", json::object());
    ASSERT_TRUE(no_ref);
    EXPECT_NE(no_ref->find("ref"), std::string::npos);

    const auto no_window = tool(tools, "computer.type").precheck("run-1", json{{"text", "hello"}});
    ASSERT_TRUE(no_window);
    EXPECT_NE(no_window->find("computer.observe"), std::string::npos) << "the message says how to get a window";
}

TEST(ComputerService, ObserveWithoutWindowIsNotPrechecked) {
    auto s = service();
    const auto tools = computer_tools(s);
    EXPECT_FALSE(tool(tools, "computer.observe").precheck("run-1", json::object()));
}

TEST(ComputerService, LaunchRefusesInterpreterBeforeApproval) {
    auto s = service();
    const auto tools = computer_tools(s);
    const auto& launch = tool(tools, "computer.launch");
    ASSERT_TRUE(launch.precheck);
    const auto refusal = launch.precheck("run-1", json{{"program", "powershell.exe"}});
    ASSERT_TRUE(refusal);
    EXPECT_NE(refusal->find("interpreter"), std::string::npos);
    EXPECT_FALSE(launch.precheck("run-1", json{{"program", "notepad.exe"}})) << "an ordinary program passes the check";
    EXPECT_TRUE(launch.precheck("run-1", json{{"program", "report.docx"}})) << "documents are refused";
}

TEST(ComputerService, FailedResultsCarryTheDesktopProvenance) {
    auto s = service();
    const auto tools = computer_tools(s);
    for (const auto& t : tools) {
        EXPECT_EQ(t.provenance.source_kind, "computer") << t.id;
        EXPECT_EQ(t.provenance.source_id, "desktop") << t.id << ": a result with no window names the desktop, not an empty source";
        EXPECT_FALSE(t.provenance.trusted) << t.id;
    }
}

}  // namespace
}  // namespace wb::computer
