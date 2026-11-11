#pragma once

#include "resource/ByteReader.h"

namespace dhsh {

// FormatError is the existing archive/resource error type. Reuse it instead of
// introducing a second, unrelated exception hierarchy.
using ResourceError = FormatError;

} // namespace dhsh
