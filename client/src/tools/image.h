// client/core-tools image.generate (add-image-generation design D3, D4): images from a text prompt through
// client/model-access, stored as Thread attachments with the tool as provenance and shown to the user in the feed.
// Risk class read: the prompt goes only to the organization gateway and nothing outside Workbench changes.
#pragma once

#include "model/model_client.h"
#include "runtime/tools.h"

#include <functional>
#include <vector>

namespace wb::tools {

using ImageGenerator = std::function<model::ModelResult<std::vector<std::vector<std::uint8_t>>>(
    const model::ImageRequest& request, const model::RequestContext& ctx)>;

struct ImageToolEnv {
    ImageGenerator generate;               // ModelClient::generate_images in the app
    size_t max_bytes = 20u * 1024 * 1024;  // per image
};

runtime::ToolDef image_generate_tool(ImageToolEnv env);

inline constexpr int kImageMaxPrompt = 4000;
inline constexpr int kImageMaxCount = 4;

}  // namespace wb::tools
