// Kept apart from the rest so that a new date only recompiles this file. build_stamp.inc comes from cmake/build_stamp.cmake.
#include "update_check.h"

namespace update {
const char* const kBuildStamp =
#include "build_stamp.inc"
    ;
}  // namespace update
