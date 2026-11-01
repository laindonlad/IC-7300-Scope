// Is there a newer ic7300console on GitHub? The build's date is its version: each folder in bin holds a
// BUILD_DATE file, copied there with it, which says when it was built. The other programs don't count.
#pragma once

#include <string>

namespace update {

// When this program was built, as UTC "YYYY-MM-DD HH:MM". Written by the build, in build_stamp.cpp.
extern const char* const kBuildStamp;

// Where people go to get it.
extern const char* const kDownloadPage;

bool validStamp(const std::string& stamp);

// Is `published` a later build than `mine`? Anything that isn't a build date is not.
bool isNewer(const std::string& published, const std::string& mine);

// The date of the build published for this system, or empty if it can't be had. Waits on the network,
// so it belongs on its own thread.
std::string fetchPublished();

}  // namespace update
