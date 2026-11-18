execute_process(COMMAND "${REPLAY}" --ne 1.5 1.5
  INPUT_FILE "${SOURCE}/examples/motions.csv"
  OUTPUT_VARIABLE output RESULT_VARIABLE result)
string(REPLACE "\r\n" "\n" output "${output}")
set(expected "x,y,out_x,out_y,overflow_bypass\n10,0,10,0,0\n0,-10,0,-10,0\n10,-10,15,-15,0\n1,-1,2,-2,0\n1,0,1,0,0\n0,-1,0,-1,0\n-10,-10,-10,-10,0\n")
if(NOT result EQUAL 0 OR NOT output STREQUAL expected)
  message(FATAL_ERROR "Replay output mismatch: ${output}")
endif()
foreach(value IN ITEMS nan inf -1 0 4.1 1junk)
  execute_process(COMMAND "${REPLAY}" --ne "${value}" 1
    OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE result)
  if(NOT result EQUAL 2)
    message(FATAL_ERROR "CLI did not reject gain: ${value}")
  endif()
endforeach()
execute_process(COMMAND "${REPLAY}" --ne 4 .25 OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE result)
if(NOT result EQUAL 2)
  message(FATAL_ERROR "CLI accepted fold-prone profile")
endif()
execute_process(COMMAND "${REPLAY}" INPUT_FILE "${SOURCE}/tests/invalid.csv"
  OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE result)
if(NOT result EQUAL 2)
  message(FATAL_ERROR "CLI accepted out-of-range coordinate")
endif()

# Diagnostic fractions are integer Q24 units; cardinal/zero rows hold both axes.
execute_process(COMMAND "${REPLAY}" --gen micro --count 4 --csv-diag --ne 1.25 1.25
  OUTPUT_VARIABLE output RESULT_VARIABLE result)
string(REPLACE "\r\n" "\n" output "${output}")
set(expected "raw_x,raw_y,out_x,out_y,res_x,res_y\n1,-1,1,-1,4194304,-4194304\n1,-1,2,-2,-8388608,8388608\n0,0,0,0,-8388608,8388608\n1,0,1,0,-8388608,8388608\n")
if(NOT result EQUAL 0 OR NOT output STREQUAL expected)
  message(FATAL_ERROR "Diagnostic carry contract mismatch: ${output}")
endif()
foreach(generator IN ITEMS circle flick micro cardinal reversal mixed)
  execute_process(COMMAND "${REPLAY}" --gen "${generator}" --count 128
    OUTPUT_VARIABLE first RESULT_VARIABLE result)
  execute_process(COMMAND "${REPLAY}" --gen "${generator}" --count 128
    OUTPUT_VARIABLE second RESULT_VARIABLE second_result)
  string(REPLACE "\r\n" "\n" first "${first}")
  string(REPLACE "\r\n" "\n" second "${second}")
  string(REGEX MATCHALL "[^\n]+" rows "${first}")
  list(LENGTH rows row_count)
  if(NOT result EQUAL 0 OR NOT second_result EQUAL 0 OR NOT first STREQUAL second OR NOT row_count EQUAL 129)
    message(FATAL_ERROR "Non-deterministic or wrong-size generator: ${generator}")
  endif()
endforeach()
foreach(arguments IN ITEMS "--gen;bad" "--gen;circle;--count;0" "--count;5"
    "--bench;--count;999999" "--bench;--csv-diag" "--bench;--bench-packet"
    "--gen;circle;--gen;micro" "--gen;circle;--count;5;--count;6")
  execute_process(COMMAND "${REPLAY}" ${arguments} OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE result)
  if(NOT result EQUAL 2)
    message(FATAL_ERROR "CLI accepted incompatible/invalid arguments: ${arguments}")
  endif()
endforeach()

# No hardware or driver access: loading a file must enforce setup and exclusivity.
execute_process(COMMAND "${REPLAY}" --profile "${SOURCE}/examples/manual.dsp" --dpi 800 --sensitivity 1
  OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE result)
if(NOT result EQUAL 3)
  message(FATAL_ERROR "Profile DPI mismatch not rejected")
endif()
execute_process(COMMAND "${REPLAY}" --profile "${SOURCE}/examples/manual.dsp" --dpi 1600 --sensitivity 1 --ne 1 1
  OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE result)
if(NOT result EQUAL 2)
  message(FATAL_ERROR "Profile/manual gain combination not rejected")
endif()
execute_process(COMMAND "${REPLAY}" --profile "${SOURCE}/examples/manual.dsp" --dpi 1600 --sensitivity 1
  INPUT_FILE "${SOURCE}/examples/motions.csv" OUTPUT_VARIABLE output RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT output MATCHES "10,-10,13,-13,0")
  message(FATAL_ERROR "Matching saved profile did not replay correctly")
endif()
