#include "tools/image.h"

#include "base/strings.h"
#include "tools/image_info.h"

#include <algorithm>
#include <cstdio>

namespace wb::tools {

namespace {

const char* const kSizes[] = {"1024x1024", "1024x1536", "1536x1024"};

std::string mb(size_t bytes) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return b;
}

// What the agent is told when the gateway refuses or fails (client/model-access Typed Error Codes).
std::string failure_text(const model::ModelFailure& f) {
    if (f.code == model::code::model_unavailable && f.reason == "role not granted")
        return "Image generation is not available for this user: their policy does not include the image model.";
    if (f.code == model::code::quota_exceeded)
        return "The daily image budget is used up" + (f.reset_at.empty() ? std::string(".") : "; it resets at " + f.reset_at + ".");
    if (f.code == model::code::cancelled) return "Image generation was cancelled.";
    return "Image generation failed (" + f.code + "): " + f.message;
}

}  // namespace

runtime::ToolDef image_generate_tool(ImageToolEnv env) {
    runtime::ToolDef t;
    t.id = "image.generate";
    t.description =
        "Generate images from a text description with the organization image model. The images are stored in the Thread and "
        "shown to the user; you cannot see them. Describe the picture fully in the prompt (subject, style, composition, "
        "text to include). Use it only when the user asks for an image.";
    t.parameters = json::parse(R"({"type":"object","properties":{
        "prompt":{"type":"string","minLength":1,"maxLength":4000,"description":"What the image shows, in detail"},
        "size":{"type":"string","enum":["1024x1024","1024x1536","1536x1024"],"description":"Square, portrait or landscape; default 1024x1024"},
        "count":{"type":"integer","minimum":1,"maximum":4,"description":"How many images; default 1"}},
        "required":["prompt"],"additionalProperties":false})");
    t.risk = runtime::Risk::read;
    t.run = [env = std::move(env)](runtime::ToolContext& ctx, const json& args) -> runtime::ToolOutput {
        model::ImageRequest req;
        req.prompt = jstr(args, "prompt");
        req.size = args.contains("size") ? jstr(args, "size") : std::string("1024x1024");
        req.count = static_cast<int>(jint(args, "count", 1));
        if (trim(req.prompt).empty() || req.prompt.size() > static_cast<size_t>(kImageMaxPrompt))
            return runtime::ToolOutput::failure("prompt must be 1 to 4000 characters.");
        if (std::find(std::begin(kSizes), std::end(kSizes), req.size) == std::end(kSizes))
            return runtime::ToolOutput::failure("size must be 1024x1024, 1024x1536 or 1536x1024.");
        if (req.count < 1 || req.count > kImageMaxCount) return runtime::ToolOutput::failure("count must be from 1 to 4.");
        if (!env.generate) return runtime::ToolOutput::failure("Image generation is not available: the agent is not connected to the model gateway.");

        model::RequestContext rc;
        rc.run_id = ctx.run_id;
        rc.thread_id = ctx.thread_id;
        rc.feature = "image.generate";
        rc.cancel = ctx.control ? ctx.control->http_token() : nullptr;
        auto r = env.generate(req, rc);
        if (ctx.cancelled()) return runtime::ToolOutput::failure("Image generation was cancelled.");
        if (!r.ok()) return runtime::ToolOutput::failure(failure_text(r.failure()));

        // Every image is checked before anything is stored: all or nothing.
        runtime::ToolOutput out = runtime::ToolOutput::success({});
        int i = 0;
        for (const auto& bytes : r.value()) {
            ++i;
            const std::string_view view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (bytes.size() > env.max_bytes)
                return runtime::ToolOutput::failure("The image model returned an image of " + mb(bytes.size()) +
                                                    ", more than the limit of " + mb(env.max_bytes) + "; nothing was stored.");
            const auto info = image_info(view);
            if (!info)
                return runtime::ToolOutput::failure("The image model returned data that is not a PNG, JPEG or WebP image; nothing was stored.");
            out.files.push_back({"image-" + std::to_string(i) + "." + info->extension, info->mime, std::string(view), info->width,
                                 info->height});
        }
        out.content = "Generated " + std::to_string(out.files.size()) + " image" + (out.files.size() == 1 ? "" : "s") + " (" +
                      req.size + ").";
        return out;
    };
    return t;
}

}  // namespace wb::tools
