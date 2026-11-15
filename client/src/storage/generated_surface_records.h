#pragma once

#include "storage/generated_surface_state.h"

namespace wb {

// Internal Thread records use the existing attachment schema but are never exposed as attachments.
std::string generated_surface_record_id(const char* kind, const GeneratedSurfaceBinding& binding,
                                        const std::string& action = {});
Result<std::optional<json>> read_generated_surface_record(Database& db, const std::string& id);
Status write_generated_surface_record(Database& db, const std::string& id, const GeneratedSurfaceBinding& binding,
                                      const json& payload);

}  // namespace wb
