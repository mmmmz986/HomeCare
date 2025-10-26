# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Debug")
  file(REMOVE_RECURSE
  "CMakeFiles/enroll_autogen.dir/AutogenUsed.txt"
  "CMakeFiles/enroll_autogen.dir/ParseCache.txt"
  "enroll_autogen"
  )
endif()
