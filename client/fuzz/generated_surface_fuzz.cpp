#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "fuzz_main.h"
#include "ui/generated_surface.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    (void)wb::ui::parse_generated_surface(input);
    (void)wb::ui::discover_generated_surfaces(input);
    const std::string fenced = "```skowork-ui\n" + std::string(input) + "\n```\n";
    (void)wb::ui::discover_generated_surfaces(fenced);
    (void)wb::ui::generated_surface_text(fenced);
    (void)wb::ui::generated_surface_text(input);
    return 0;
}
