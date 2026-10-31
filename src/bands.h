#pragma once
// The bands the buttons jump to, with common calling frequencies and the rough shape of the IARU Region 1
// band plan (RSGB's sub-bands, simplified). This is a guide for the eye, not a statement of what your
// licence allows: 60m in particular varies by country.
#include "civ.h"

namespace bands {
enum Use { U_CW, U_DIGI, U_SSB, U_FM, U_OTHER };

struct Segment {
  long long lo, hi;
  Use use;
};
struct Preset {
  const char* name;
  long long hz;
  int mode;
  bool data;
};
struct Band {
  const char* name;
  long long lo, hi;
  long long homeHz;   // where the button goes the first time
  int homeMode;
  Preset presets[6];
  Segment segments[7];
};

using civ::CW;
using civ::FM;
using civ::LSB;
using civ::USB;

// The 4m band is on the rig only in some markets: a rig without it refuses and the status bar says so.
inline const Band* table(int& count) {
  static const Band t[] = {
      {"160", 1810000, 2000000, 1840000, LSB,
       {{"CW", 1820000, CW, false}, {"FT8", 1840000, USB, true}, {"WSPR", 1836600, USB, true}, {"SSB", 1900000, LSB, false}},
       {{1810000, 1838000, U_CW}, {1838000, 1843000, U_DIGI}, {1843000, 2000000, U_SSB}}},
      {"80", 3500000, 3800000, 3700000, LSB,
       {{"CW", 3525000, CW, false}, {"FT8", 3573000, USB, true}, {"FT4", 3575000, USB, true}, {"WSPR", 3568600, USB, true}, {"SSB", 3700000, LSB, false}},
       {{3500000, 3570000, U_CW}, {3570000, 3620000, U_DIGI}, {3620000, 3800000, U_SSB}}},
      {"60", 5258500, 5406500, 5357000, USB,
       {{"FT8", 5357000, USB, true}, {"WSPR", 5288700, USB, true}, {"SSB", 5362000, USB, false}},
       {{5258500, 5406500, U_OTHER}}},
      {"40", 7000000, 7200000, 7150000, LSB,
       {{"CW", 7020000, CW, false}, {"FT8", 7074000, USB, true}, {"FT4", 7047500, USB, true}, {"WSPR", 7038600, USB, true}, {"SSB", 7150000, LSB, false}},
       {{7000000, 7040000, U_CW}, {7040000, 7053000, U_DIGI}, {7053000, 7200000, U_SSB}}},
      {"30", 10100000, 10150000, 10136000, USB,
       {{"CW", 10116000, CW, false}, {"FT8", 10136000, USB, true}, {"FT4", 10140000, USB, true}, {"WSPR", 10138700, USB, true}},
       {{10100000, 10130000, U_CW}, {10130000, 10150000, U_DIGI}}},
      {"20", 14000000, 14350000, 14200000, USB,
       {{"CW", 14030000, CW, false}, {"FT8", 14074000, USB, true}, {"FT4", 14080000, USB, true}, {"WSPR", 14095600, USB, true}, {"SSB", 14200000, USB, false}},
       {{14000000, 14070000, U_CW}, {14070000, 14112000, U_DIGI}, {14112000, 14350000, U_SSB}}},
      {"17", 18068000, 18168000, 18140000, USB,
       {{"CW", 18086000, CW, false}, {"FT8", 18100000, USB, true}, {"FT4", 18104000, USB, true}, {"WSPR", 18104600, USB, true}, {"SSB", 18140000, USB, false}},
       {{18068000, 18095000, U_CW}, {18095000, 18120000, U_DIGI}, {18120000, 18168000, U_SSB}}},
      {"15", 21000000, 21450000, 21250000, USB,
       {{"CW", 21030000, CW, false}, {"FT8", 21074000, USB, true}, {"FT4", 21140000, USB, true}, {"WSPR", 21094600, USB, true}, {"SSB", 21250000, USB, false}},
       {{21000000, 21070000, U_CW}, {21070000, 21151000, U_DIGI}, {21151000, 21450000, U_SSB}}},
      {"12", 24890000, 24990000, 24950000, USB,
       {{"CW", 24906000, CW, false}, {"FT8", 24915000, USB, true}, {"FT4", 24919000, USB, true}, {"WSPR", 24924600, USB, true}, {"SSB", 24950000, USB, false}},
       {{24890000, 24915000, U_CW}, {24915000, 24940000, U_DIGI}, {24940000, 24990000, U_SSB}}},
      {"10", 28000000, 29700000, 28500000, USB,
       {{"CW", 28030000, CW, false}, {"FT8", 28074000, USB, true}, {"FT4", 28180000, USB, true}, {"WSPR", 28124600, USB, true}, {"SSB", 28500000, USB, false}, {"FM", 29600000, FM, false}},
       {{28000000, 28070000, U_CW}, {28070000, 28225000, U_DIGI}, {28225000, 29200000, U_SSB}, {29200000, 29510000, U_OTHER}, {29510000, 29700000, U_FM}}},
      {"6", 50000000, 52000000, 50150000, USB,
       {{"CW", 50090000, CW, false}, {"FT8", 50313000, USB, true}, {"FT4", 50318000, USB, true}, {"WSPR", 50293000, USB, true}, {"SSB", 50150000, USB, false}, {"FM", 51510000, FM, false}},
       {{50000000, 50100000, U_CW}, {50100000, 50500000, U_SSB}, {50500000, 51410000, U_DIGI}, {51410000, 52000000, U_FM}}},
      {"4", 70000000, 70500000, 70200000, USB,
       {{"CW", 70030000, CW, false}, {"FT8", 70154000, USB, true}, {"WSPR", 70091000, USB, true}, {"SSB", 70200000, USB, false}, {"FM", 70450000, FM, false}},
       {{70000000, 70100000, U_CW}, {70100000, 70250000, U_SSB}, {70250000, 70500000, U_FM}}},
  };
  count = (int)(sizeof t / sizeof t[0]);
  return t;
}

inline int bandOf(long long hz) {
  int n;
  const Band* t = table(n);
  for (int i = 0; i < n; i++)
    if (hz >= t[i].lo && hz <= t[i].hi) return i;
  return -1;
}
}  // namespace bands
