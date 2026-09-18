cmake_minimum_required(VERSION 3.16)

project(libgodot_elixir_nif LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Locate Erlang/ERTS NIF headers.
execute_process(
  COMMAND erl -noshell -eval "io:format(\"~s\", [lists:concat([code:root_dir(), \"/erts-\", erlang:system_info(version), \"/include\"])])" -s init stop
  OUTPUT_VARIABLE ERL_INCLUDE_PATH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT ERL_INCLUDE_PATH)
  message(FATAL_ERROR "Could not determine ERL_INCLUDE_PATH; ensure 'erl' is on PATH")
endif()

set(GODOT_CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../godot-cpp)
set(LIBGODOT_BUILD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../build)

# Ensure godot-cpp uses the same GDExtension API/header that libgodot was built with.
set(GODOTCPP_GDEXTENSION_DIR "${LIBGODOT_BUILD_DIR}/gdextension" CACHE PATH "" FORCE)
set(GODOTCPP_CUSTOM_API_FILE "${LIBGODOT_BUILD_DIR}/gdextension/extension_api.json" CACHE FILEPATH "" FORCE)

# The engine bundle and godot-cpp each carry a gdextension_interface.h whose
# typedef casing differs; godot-cpp's loader only matches its own. Expose just
# libgodot.h from the bundle so the generated header is the one that resolves.
set(LIBGODOT_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/libgodot_include)
file(MAKE_DIRECTORY ${LIBGODOT_INCLUDE_DIR})
file(COPY ${LIBGODOT_BUILD_DIR}/gdextension/libgodot.h DESTINATION ${LIBGODOT_INCLUDE_DIR})

message(STATUS "Building godot-cpp via add_subdirectory (incremental; first build may take a while)...")
# Keep the setup similar to the C++ sample (add_subdirectory), but isolate build artifacts
# in this project's binary tree so we don't reuse/overwrite ../../godot-cpp/build.
add_subdirectory(${GODOT_CPP_DIR} ${CMAKE_CURRENT_BINARY_DIR}/godot-cpp)

add_library(libgodot_nif MODULE src/libgodot_nif.cpp)

target_include_directories(libgodot_nif PRIVATE
  ${ERL_INCLUDE_PATH}
  ${GODOT_CPP_DIR}/include
  ${GODOT_CPP_DIR}/gen/include
  ${LIBGODOT_INCLUDE_DIR}
)

target_link_libraries(libgodot_nif PRIVATE godot-cpp)

# When building via `mix compile` (elixir_make), write artefacts to the Mix priv dir.
# When building standalone, fall back to a local priv/ directory.
if(DEFINED ENV{MIX_APP_PATH})
  set(PRIV_DIR "$ENV{MIX_APP_PATH}/priv")
else()
  set(PRIV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/priv")
endif()

file(MAKE_DIRECTORY "${PRIV_DIR}")

if(WIN32)
  # On Windows, NIFs are DLLs.
  set_target_properties(libgodot_nif PROPERTIES
    OUTPUT_NAME "libgodot_nif"
    PREFIX ""
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY "${PRIV_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${PRIV_DIR}"
  )
else()
  # NIF is expected to be a .so on both macOS and Linux.
  set_target_properties(libgodot_nif PROPERTIES
    OUTPUT_NAME "libgodot_nif"
    PREFIX ""
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${PRIV_DIR}"
  )
endif()

# When distributing precompiled artefacts, we want libgodot to live next to the NIF in priv/.
if(WIN32)
  set(LIBGODOT_SOURCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../build/libgodot.dll")
  set(LIBGODOT_DEST_PATH "${PRIV_DIR}/libgodot.dll")
elseif(APPLE)
  set(LIBGODOT_SOURCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../build/libgodot.dylib")
  set(LIBGODOT_DEST_PATH "${PRIV_DIR}/libgodot.dylib")
else()
  set(LIBGODOT_SOURCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../build/libgodot.so")
  set(LIBGODOT_DEST_PATH "${PRIV_DIR}/libgodot.so")
endif()

if(NOT EXISTS "${LIBGODOT_SOURCE_PATH}")
  message(FATAL_ERROR "Missing libgodot at '${LIBGODOT_SOURCE_PATH}'. Build it first (from repo root run ./build_libgodot.sh) or use precompiled artefacts via elixir_make.")
endif()

if(APPLE)
  add_custom_command(
    TARGET libgodot_nif POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBGODOT_SOURCE_PATH}" "${LIBGODOT_DEST_PATH}"
    # Normalize the install name so it doesn't refer to Godot's bin/ path.
    COMMAND install_name_tool -id "@rpath/libgodot.dylib" "${LIBGODOT_DEST_PATH}"
    VERBATIM
  )
else()
  add_custom_command(
    TARGET libgodot_nif POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBGODOT_SOURCE_PATH}" "${LIBGODOT_DEST_PATH}"
    VERBATIM
  )
endif()

if(APPLE)
  # Equivalent to: -undefined dynamic_lookup
  target_link_options(libgodot_nif PRIVATE "-undefined" "dynamic_lookup")
elseif(NOT WIN32)
  target_link_libraries(libgodot_nif PRIVATE dl)
endif()

# libgodot_host: standalone executable whose main() owns thread 0 on macOS.
# Reuses the godot-cpp already built above so builds are incremental.
# weft::harness lives vendored under thirdparty/ so the connector builds
# standalone from a plain `git clone` (see git-subtree add in this branch's
# history). QCBOR is nested beneath it at the path weft-harness's own
# CMakeLists expects (libs/qcbor). iceoryx2 is NOT vendored — it is
# dlopen'd at runtime via WEFT_ICEORYX2_PATH per the bus contract.
set(WEFT_HARNESS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/weft-harness)
if(EXISTS ${WEFT_HARNESS_DIR}/CMakeLists.txt AND EXISTS ${WEFT_HARNESS_DIR}/libs/qcbor/CMakeLists.txt)
  add_subdirectory(${WEFT_HARNESS_DIR} ${CMAKE_CURRENT_BINARY_DIR}/weft-harness EXCLUDE_FROM_ALL)
  set(HOST_HAS_WEFT_HARNESS TRUE)
else()
  message(WARNING "weft::harness subtree missing at ${WEFT_HARNESS_DIR}; libgodot_host will build without the bus loop.")
  set(HOST_HAS_WEFT_HARNESS FALSE)
endif()

enable_testing()

# weft_client_nif: elixir side of the weft::harness lifecycle channel. Mirrors
# thirdparty/weft-harness/proof/command_publisher.cpp — publishes commands and
# waits on request-id-matched replies. Only builds when the weft-harness
# subtree is present; without it there is no client protocol to embed.
if(HOST_HAS_WEFT_HARNESS)
  add_library(weft_client_nif MODULE ${CMAKE_CURRENT_SOURCE_DIR}/src/weft_client_nif.cpp)
  target_include_directories(weft_client_nif PRIVATE
    ${ERL_INCLUDE_PATH}
    ${WEFT_HARNESS_DIR}/include
  )
  target_link_libraries(weft_client_nif PRIVATE weft::harness)
  # BEAM's :erlang.load_nif calls dlopen with a .so extension on every
  # unix, including macOS — force the suffix rather than trust
  # CMAKE_SHARED_MODULE_SUFFIX (which is .dylib on darwin).
  if(WIN32)
    set_target_properties(weft_client_nif PROPERTIES
      OUTPUT_NAME "weft_client_nif" PREFIX "" SUFFIX ".dll"
      RUNTIME_OUTPUT_DIRECTORY "${PRIV_DIR}"
      LIBRARY_OUTPUT_DIRECTORY "${PRIV_DIR}"
    )
  else()
    set_target_properties(weft_client_nif PROPERTIES
      OUTPUT_NAME "weft_client_nif" PREFIX "" SUFFIX ".so"
      LIBRARY_OUTPUT_DIRECTORY "${PRIV_DIR}"
    )
  endif()
  if(APPLE)
    target_link_options(weft_client_nif PRIVATE "-undefined" "dynamic_lookup")
  elseif(NOT WIN32)
    target_link_libraries(weft_client_nif PRIVATE dl)
  endif()
endif()

add_executable(libgodot_host ${CMAKE_CURRENT_SOURCE_DIR}/../libgodot_host/host.cpp)
target_include_directories(libgodot_host PRIVATE
  ${GODOT_CPP_DIR}/include
  ${GODOT_CPP_DIR}/gen/include
  ${LIBGODOT_INCLUDE_DIR}
)
target_link_libraries(libgodot_host PRIVATE godot-cpp)
if(HOST_HAS_WEFT_HARNESS)
  target_link_libraries(libgodot_host PRIVATE weft::harness)
  target_compile_definitions(libgodot_host PRIVATE LIBGODOT_HOST_HAS_WEFT_HARNESS=1)
endif()
if(NOT WIN32)
  target_link_libraries(libgodot_host PRIVATE dl)
endif()
set_target_properties(libgodot_host PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PRIV_DIR}"
)

# Smoke tests with paired negative controls (CLAUDE.md rule 2). The suite
# is a bash script rather than a cmake test-per-case because each case
# depends on running the built host with a real libgodot.dylib next to it;
# ctest just wraps the script. Skip on Windows where bash may not be on PATH.
if(NOT WIN32)
  add_test(
    NAME libgodot_host_smoke
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/../libgodot_host/test/smoke_test.sh
    WORKING_DIRECTORY ${PRIV_DIR}
  )
  # DEMO_DIR points at a manifest-side godot-demo-projects checkout so
  # the demo cases (demo_project_iterates, demo_project_no_modal_alert)
  # can run under ctest. When the checkout is absent the suite skips
  # those cases quietly.
  set(DEMO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../4-entities/godot-demo-projects/misc/matrix_transform")
  set_tests_properties(libgodot_host_smoke PROPERTIES
    ENVIRONMENT "HOST=${PRIV_DIR}/libgodot_host;LIBGODOT=${LIBGODOT_DEST_PATH};PROJECT_DIR=${PRIV_DIR};DEMO_DIR=${DEMO_DIR}"
  )
endif()
