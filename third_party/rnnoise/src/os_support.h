/* Not part of the RNNoise release: vec.h and vec_neon.h include this Opus header, which the release leaves
   out. Only the generic and ARM NEON paths use it (x86 takes vec_avx.h), so this gives them what they need
   from common.h. */
#ifndef OS_SUPPORT_H
#define OS_SUPPORT_H

#include "common.h"

#ifndef OPUS_CLEAR
#define OPUS_CLEAR(dst, n) RNN_CLEAR(dst, n)
#endif
#ifndef OPUS_COPY
#define OPUS_COPY(dst, src, n) RNN_COPY(dst, src, n)
#endif
#ifndef OPUS_MOVE
#define OPUS_MOVE(dst, src, n) RNN_MOVE(dst, src, n)
#endif

#endif
