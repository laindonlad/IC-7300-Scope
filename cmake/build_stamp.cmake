# Runs at every build. Records when it was, for the program and for the BUILD_DATE file that goes in bin.
# The files are left alone within the same minute, so that doesn't relink anything.
string(TIMESTAMP stamp "%Y-%m-%d %H:%M" UTC)
set(old "")
if(EXISTS "${OUT_DIR}/BUILD_DATE")
  file(READ "${OUT_DIR}/BUILD_DATE" old)
endif()
if(NOT old STREQUAL "${stamp}\n")
  file(WRITE "${OUT_DIR}/BUILD_DATE" "${stamp}\n")
  file(WRITE "${OUT_DIR}/build_stamp.inc" "\"${stamp}\"\n")
endif()
