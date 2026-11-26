// Meeting audio as WAV in evaluation exports (add-evaluation-export design D2); the codec is meeting/wav.
#pragma once

#include "meeting/wav.h"

namespace wb::eval {
using meeting::read_wav;
using meeting::write_wav;
}  // namespace wb::eval
