# Drives the real binary with no display: new, check, render, and failures.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(ENV{QT_QPA_PLATFORM} "")
set(ENV{CLAP_PATH} "${PLUGINS}/clap")
set(ENV{LV2_PATH} "${PLUGINS}/lv2")

function(run expected)
  execute_process(COMMAND ${DOLE} ${ARGN} WORKING_DIRECTORY "${WORK}"
                  RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT code EQUAL expected)
    message(FATAL_ERROR "dole ${ARGN} exited ${code}, not ${expected}\n${out}\n${err}")
  endif()
  set(out "${out}" PARENT_SCOPE)
  set(err "${err}" PARENT_SCOPE)
endfunction()

run(0)
if(NOT out MATCHES "render <song>")
  message(FATAL_ERROR "bare dole should print help:\n${out}")
endif()

run(0 new song.dole --json)
run(1 new song.dole)
if(NOT err MATCHES "already exists")
  message(FATAL_ERROR "new over a file should refuse:\n${err}")
endif()

run(0 check song.dole --json)
if(NOT out MATCHES "\"ok\": true" OR NOT out MATCHES "\"bpm\": 88")
  message(FATAL_ERROR "check --json:\n${out}")
endif()

run(0 render song.dole -o song.wav --json)
if(NOT EXISTS "${WORK}/song.wav")
  message(FATAL_ERROR "render wrote no WAV")
endif()
run(0 render song.dole -o loop.wav --pattern B --bars 2)

file(WRITE "${WORK}/bad.dole" "{\"dole\": 2, \"patterns\": {\"A\": {\"1\": \"x..\"}}}")
run(1 check bad.dole)
if(NOT err MATCHES "needs 16 steps")
  message(FATAL_ERROR "check should say what's wrong:\n${err}")
endif()

file(WRITE "${WORK}/missing.dole" "{\"dole\": 2, \"pads\": {\"1\": {\"sample\": \"nope.wav\"}}}")
run(1 check missing.dole --json)
if(NOT out MATCHES "nope.wav")
  message(FATAL_ERROR "check should report the missing sample:\n${out}")
endif()

run(0 help format)
if(NOT out MATCHES "The .dole song format")
  message(FATAL_ERROR "help format:\n${out}")
endif()
run(0 plugins --json)
if(NOT out MATCHES "clap:dole.test.sine" OR NOT out MATCHES "lv2:urn:dole:test-gain" OR NOT out MATCHES "\"overdrive\"")
  message(FATAL_ERROR "plugins should list the test plugins:\n${out}")
endif()

file(WRITE "${WORK}/plugged.dole" "{\"dole\": 2, \"pads\": {\"1\": {\"effects\": [{\"plugin\": \"overdrive\"}, {\"plugin\": \"lv2:urn:dole:test-gain\"}]}, \"2\": {\"synth\": {\"plugin\": \"clap:dole.test.sine\"}}}, \"patterns\": {\"A\": {\"1\": \"x...............\", \"2\": \"....x...........\"}}}")
run(0 render plugged.dole -o plugged.wav)
file(WRITE "${WORK}/unplugged.dole" "{\"dole\": 2, \"master\": [{\"plugin\": \"clap:not.here\"}]}")
run(1 check unplugged.dole)
if(NOT err MATCHES "isn't installed")
  message(FATAL_ERROR "check should say a plugin is missing:\n${err}")
endif()

run(0 skill)
run(0 --version)

# With no window open, remote and mcp say so instead of hanging.
set(ENV{XDG_RUNTIME_DIR} "${WORK}/runtime")
file(MAKE_DIRECTORY "${WORK}/runtime")
file(CHMOD "${WORK}/runtime" DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
run(1 remote status)
if(NOT err MATCHES "No dole window is open")
  message(FATAL_ERROR "remote with no window:\n${err}")
endif()
set(ENV{DOLE_WINDOW} "999999")
run(1 remote status)
if(NOT err MATCHES "No dole window 999999")
  message(FATAL_ERROR "remote should say which window it couldn't find:\n${err}")
endif()
unset(ENV{DOLE_WINDOW})
file(WRITE "${WORK}/mcp.in"
  "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\"}}\n"
  "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
  "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
  "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"status\",\"arguments\":{}}}\n"
  "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"format\",\"arguments\":{}}}\n")
execute_process(COMMAND ${DOLE} mcp INPUT_FILE "${WORK}/mcp.in" RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0 OR NOT out MATCHES "\"protocolVersion\":\"2025-06-18\"" OR NOT out MATCHES "\"name\":\"edit_song\""
   OR NOT out MATCHES "No dole window is open" OR NOT out MATCHES "The .dole song format")
  message(FATAL_ERROR "mcp:\n${out}\n${err}")
endif()
string(REGEX MATCHALL "\n" answers "${out}")
list(LENGTH answers count)
if(NOT count EQUAL 4)
  message(FATAL_ERROR "mcp should answer the four requests and not the notification:\n${out}")
endif()
