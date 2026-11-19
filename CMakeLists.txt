cmake_minimum_required(VERSION 3.21)
project(dole VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 DBus Multimedia Concurrent Test)
# LV2 plugins load through lilv; CLAP plugin windows are X11, so xcb, and
# RandR to find the monitor dole is on.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LILV REQUIRED IMPORTED_TARGET lilv-0)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-randr)

# Everything but main.cpp, shared by the app and the tests, so each source
# compiles once and a new file in src/ needs no line here.
file(GLOB dole_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM dole_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(dole_core STATIC ${dole_sources})
target_include_directories(dole_core PUBLIC src)
# The CLAP headers (MIT) are carried here; they're all a host needs.
target_include_directories(dole_core SYSTEM PUBLIC third_party/clap/include)
target_link_libraries(dole_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus
  Qt6::Multimedia Qt6::Concurrent PkgConfig::LILV PkgConfig::XCB)
target_compile_options(dole_core PUBLIC -Wall -Wextra)
target_compile_definitions(dole_core PUBLIC DOLE_VERSION="${PROJECT_VERSION}")

add_executable(dole src/main.cpp src/resources.qrc)
target_link_libraries(dole PRIVATE dole_core)

# The tests load the real Main.qml, so they carry the resources too.
enable_testing()
# Tiny CLAP and LV2 plugins of the tests' own, so plugin hosting is tested
# with nothing installed.
set(TEST_PLUGINS ${CMAKE_BINARY_DIR}/test-plugins)
add_library(dole_test_clap MODULE tests/plugins/testclap.cpp)
target_include_directories(dole_test_clap PRIVATE third_party/clap/include)
set_target_properties(dole_test_clap PROPERTIES PREFIX "" OUTPUT_NAME dole-test SUFFIX .clap
  LIBRARY_OUTPUT_DIRECTORY ${TEST_PLUGINS}/clap)
add_library(dole_test_lv2 MODULE tests/plugins/testlv2.c)
set_target_properties(dole_test_lv2 PROPERTIES PREFIX "" OUTPUT_NAME dole-test
  LIBRARY_OUTPUT_DIRECTORY ${TEST_PLUGINS}/lv2/dole-test.lv2)
configure_file(tests/plugins/manifest.ttl ${TEST_PLUGINS}/lv2/dole-test.lv2/manifest.ttl COPYONLY)
configure_file(tests/plugins/dole-test.ttl ${TEST_PLUGINS}/lv2/dole-test.lv2/dole-test.ttl COPYONLY)

add_executable(dole_tests tests/dole_tests.cpp src/resources.qrc)
target_link_libraries(dole_tests PRIVATE dole_core Qt6::Test)
add_dependencies(dole_tests dole dole_test_clap dole_test_lv2)
target_compile_definitions(dole_tests PRIVATE DOLE_BINARY="$<TARGET_FILE:dole>" TEST_PLUGINS="${TEST_PLUGINS}")
add_test(NAME dole_tests COMMAND dole_tests)
set_tests_properties(dole_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# The command line runs the real binary, with no display.
add_test(NAME dole_cli COMMAND ${CMAKE_COMMAND} -DDOLE=$<TARGET_FILE:dole> -DWORK=${CMAKE_BINARY_DIR}/cli-test
  -DPLUGINS=${TEST_PLUGINS} -P ${CMAKE_SOURCE_DIR}/tests/cli_test.cmake)

# `cmake --install build --prefix ~/.local` puts the app in the launcher
# before it is packaged; a package installs the same files under /usr.
include(GNUInstallDirs)
install(TARGETS dole RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/dole.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES share/dole.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/dole)
