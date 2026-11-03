// Build-time check of the UI string bundles (add-native-ui design D9): every language has every key, parameter
// and plural form of the English bundle. Usage: ui_strings_check <dir> [stamp]. Exit code 1 fails the build.
#include <cstdio>
#include <fstream>
#include <sstream>

#include "ui/strings.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ui_strings_check <dir> [stamp]\n");
        return 2;
    }
    std::vector<wb::ui::Strings> bundles;
    int rc = 0;
    for (const char* lang : {"cs", "de", "en"}) {
        const std::string path = std::string(argv[1]) + "/" + lang + ".json";
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "%s: cannot read\n", path.c_str());
            rc = 1;
            continue;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        auto s = wb::ui::Strings::parse(lang, ss.str());
        if (!s) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), s.error().message.c_str());
            rc = 1;
            continue;
        }
        bundles.push_back(std::move(s).value());
    }
    for (const std::string& issue : wb::ui::check_bundles(bundles)) {
        std::fprintf(stderr, "ui strings: %s\n", issue.c_str());
        rc = 1;
    }
    if (rc == 0 && argc > 2) std::ofstream(argv[2]) << "ok\n";
    return rc;
}
