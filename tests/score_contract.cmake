string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(root "${BINARY}/score-${suffix}")
file(MAKE_DIRECTORY "${root}")
file(WRITE "${root}/profile.dsp" "directional-profile=1\ndpi=1600\nreference_sensitivity=1\nne=1.25,1.25\nse=1,1\nsw=1,1\nnw=1,1\n")
file(WRITE "${root}/trials.csv" "trial,split,target_x,target_y,dx,dy\na,validation,10,-10,8,-8\nb,validation,10,0,10,0\nc,validation,8,8,8,8\nd,validation,10,-10,8,-8\n")
execute_process(COMMAND "${SCORE}" "${root}/profile.dsp" "${root}/trials.csv" 1600 1
  OUTPUT_VARIABLE scored RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT scored MATCHES "a,10,-10,8,-8,10,-10,0" OR
   NOT scored MATCHES "b,10,0,10,0,10,0,0" OR NOT scored MATCHES "c,8,8,8,8,8,8,0" OR
   NOT scored MATCHES "d,10,-10,8,-8,10,-10,0")
  message(FATAL_ERROR "Fixed-point profile scoring failed: ${scored}")
endif()
execute_process(COMMAND "${SCORE}" "${root}/profile.dsp" "${root}/trials.csv" 800 1 RESULT_VARIABLE result)
if(NOT result EQUAL 2)
  message(FATAL_ERROR "Scorer accepted incompatible setup")
endif()
