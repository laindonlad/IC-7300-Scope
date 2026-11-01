#pragma once
// Setting this computer's clock from internet time, for FT8 and the other modes that need it within a
// second or so. It asks NTP servers itself rather than poking the Windows Time service, because that
// service slews small errors out slowly, and an operator pressing a button wants it right now.
#include <stdint.h>
#include <stddef.h>

#include <string>
#include <vector>

namespace clocksync {

const int kPacketSize = 48;
const double kCloseEnough = 0.02;   // seconds: not worth changing the clock, or a permission prompt, for this
const double kMostToStep = 3600;    // seconds: beyond this something's wrong that a person should look at
const double kMostDisagreement = 0.5;  // seconds: servers further apart than this can't both be right

struct Sample {
  double offset = 0;  // seconds to add to this computer's clock to make it right
  double delay = 0;   // the round trip, and so twice the most the offset can be wrong by
};

// The request carries a random tag where its send time would go. A real reply has to quote it back.
void makeRequest(uint8_t out[kPacketSize], uint64_t tag);
// False unless this is a usable answer to that request. sentAt and gotAt are this computer's clock, in Unix seconds.
bool readReply(const uint8_t* p, size_t n, uint64_t tag, double sentAt, double gotAt, Sample& out);
double ntpToUnix(uint64_t ntp);

// The answer to trust out of those from different servers, or false if they disagree. Needs at least one.
bool choose(const std::vector<Sample>& fromEachServer, Sample& best);

enum Verdict { V_CLOSE_ENOUGH, V_STEP, V_TOO_FAR };
Verdict judge(double offset);
std::string describe(double offset);  // "0.312 s slow"

struct Report {
  bool ok = false;
  std::string message;  // a sentence for the status bar
};

// Finds how wrong the clock is and, if change is set, corrects it. Takes seconds, and on Windows can wait
// on a permission prompt, so it belongs on its own thread.
Report jog(bool change);

// What --sync-clock does: the same, in a process that has been given the right to set the clock.
// It takes no time from the command line, so being allowed to run it doesn't allow setting just any time.
int runSyncClock();

}  // namespace clocksync
