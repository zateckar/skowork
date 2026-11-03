// Fuzz target for the policy matchers that see untrusted tool arguments (client/policy-enforcement Filesystem Path
// Rules, Network Egress Rules For Client Tools): the path syntax checks, final-path normalisation, the glob matcher and
// the URL egress check. Any input must give an answer, never a crash, hang or unbounded memory use.
#include "fuzz_main.h"
#include "policy/core.h"

using namespace wb::policy;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    // The whole input as a path and as a URL.
    (void)path_syntax_problem(text);
    const std::string norm = normalize_final_path(text);
    (void)path_within(norm, text);
    (void)check_egress_url(text, {"*.corp.local", "**"}, {"*.pastebin.com"});

    // "pattern\ntext" as a glob and its subject.
    const size_t nl = text.find('\n');
    const std::string_view pattern = text.substr(0, nl), subject = nl == std::string_view::npos ? text : text.substr(nl + 1);
    (void)glob_match(pattern, subject);
    (void)pattern_specificity(pattern);
    // A pattern without wildcards matches itself.
    if (pattern.find_first_of("*?") == std::string_view::npos && !glob_match(pattern, pattern)) __builtin_trap();
    // `**` matches everything.
    if (!glob_match("**", subject)) __builtin_trap();

    PathRules rules;
    rules.workspace = "C:\\Data\\workspace";
    rules.data_root = "C:\\Data";
    rules.read = {std::string(pattern)};
    rules.deny = {"**\\.ssh\\**"};
    const RuleVerdict v = check_path(subject, Access::write, rules);
    // A deny rule always wins.
    if (v.allowed && glob_match("**\\.ssh\\**", subject)) __builtin_trap();
    return 0;
}
