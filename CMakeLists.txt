cmake_minimum_required(VERSION 3.21)
project(dole VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 DBus Multimedia Concurrent Test)

# Everything but main.cpp, shared by the app and the tests, so each source
# compiles once and a new file in src/ needs no line here.
file(GLOB dole_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM dole_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(dole_core STATIC ${dole_sources})
target_include_directories(dole_core PUBLIC src)
target_link_libraries(dole_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus
  Qt6::Multimedia Qt6::Concurrent)
target_compile_options(dole_core PUBLIC -Wall -Wextra)
target_compile_definitions(dole_core PUBLIC DOLE_VERSION="${PROJECT_VERSION}")

add_executable(dole src/main.cpp src/resources.qrc)
target_link_libraries(dole PRIVATE dole_core)

# The tests load the real Main.qml, so they carry the resources too.
enable_testing()
add_executable(dole_tests tests/dole_tests.cpp src/resources.qrc)
target_link_libraries(dole_tests PRIVATE dole_core Qt6::Test)
add_test(NAME dole_tests COMMAND dole_tests)
set_tests_properties(dole_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# The command line runs the real binary, with no display.
add_test(NAME dole_cli COMMAND ${CMAKE_COMMAND} -DDOLE=$<TARGET_FILE:dole> -DWORK=${CMAKE_BINARY_DIR}/cli-test
  -P ${CMAKE_SOURCE_DIR}/tests/cli_test.cmake)

# `cmake --install build --prefix ~/.local` puts the app in the launcher
# before it is packaged; a package installs the same files under /usr.
include(GNUInstallDirs)
install(TARGETS dole RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/dole.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES share/dole.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/dole)
