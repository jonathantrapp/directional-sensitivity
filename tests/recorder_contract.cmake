string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(bitmap "${BINARY}/recorder-test-${suffix}.bmp")
execute_process(COMMAND "${RECORDER}" --render-demo "${bitmap}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Offscreen recorder render failed")
endif()
file(READ "${bitmap}" magic LIMIT 2 HEX)
file(SIZE "${bitmap}" bytes)
if(NOT magic STREQUAL "424d" OR NOT bytes EQUAL 3213654)
  message(FATAL_ERROR "Invalid offscreen bitmap export")
endif()
execute_process(COMMAND "${RECORDER}" --render-demo "${bitmap}" RESULT_VARIABLE result)
if(NOT result EQUAL 2)
  message(FATAL_ERROR "Recorder overwrote an existing render")
endif()
# Every case exits before device enumeration, window creation or registration.
foreach(arguments IN ITEMS "--dpi;0" "--dpi;1600;--sensitivity;nan"
    "--dpi;1600;--sensitivity;1;--output;${BINARY}"
    "--dpi;1600;--sensitivity;1;--pixels-per-count;0"
    "--dpi;1600;--sensitivity;1;--output;unused-wide;--directions;24"
    "--dpi;1600;--sensitivity;1;--output;unused-wide;--directions;25")
  execute_process(COMMAND "${RECORDER}" ${arguments} OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE result)
  if(NOT result EQUAL 2)
    message(FATAL_ERROR "Recorder failed to reject invalid arguments safely: ${arguments}")
  endif()
endforeach()
