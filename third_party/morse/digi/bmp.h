#pragma once
// The header of an 8-bit grey BMP, top-down (rows in the order they're written), as WEFAX charts
// are saved. Rows are padded to 4 bytes. Plain C++ (tools/digi_test).
#include <stdint.h>
#include <string.h>

namespace Bmp {
constexpr int HEADER = 14 + 40 + 256 * 4;

inline int stride(int w) { return (w + 3) & ~3; }

inline void header(uint8_t* b, int w, int h) {
  auto put = [&](int at, uint32_t v, int n) {
    for (int i = 0; i < n; i++) b[at + i] = v >> (8 * i) & 255;
  };
  memset(b, 0, HEADER);
  uint32_t image = (uint32_t)stride(w) * h;
  b[0] = 'B';
  b[1] = 'M';
  put(2, HEADER + image, 4);
  put(10, HEADER, 4);
  put(14, 40, 4);
  put(18, w, 4);
  put(22, (uint32_t)-h, 4);  // negative: top-down
  put(26, 1, 2);
  put(28, 8, 2);
  put(34, image, 4);
  put(46, 256, 4);
  for (int i = 0; i < 256; i++) put(54 + 4 * i, i | i << 8 | i << 16, 4);
}
}  // namespace Bmp
