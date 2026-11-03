# Never clear or write the user's shared shader cache. Each test owns a fresh
# directory; two separate processes prove persistence beyond in-memory caches.
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef nonce)
set(cache_dir "${CACHE_ROOT}/shader_cache_${nonce}")
file(MAKE_DIRECTORY "${cache_dir}")
foreach(mode cold warm)
  execute_process(COMMAND "${EXE}" ${mode} "${cache_dir}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${mode} cache run failed (${result}):\n${output}\n${errors}")
  endif()
  string(REGEX MATCH "shader cache ${mode}: [^\n]+" summary "${output}")
  message("${summary}")
endforeach()
