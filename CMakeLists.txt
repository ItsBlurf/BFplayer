cmake_minimum_required(VERSION 3.20)
project(bfplayer_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(bfplayer_core STATIC
    src/core/artwork.cpp
    src/core/bulk_import.cpp
    src/core/diagnostics.cpp
    src/core/library_scanner.cpp
    src/core/library_view.cpp
    src/core/media_sources.cpp
    src/core/player_settings.cpp
    src/core/playlist.cpp
    src/core/safe_read_file.cpp
    src/core/source_uri.cpp
    src/core/video_layout.cpp
)
target_include_directories(bfplayer_core PUBLIC include)

if(MSVC)
    target_compile_options(bfplayer_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(bfplayer_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(library_scanner_tests tests/library_scanner_tests.cpp)
    target_link_libraries(library_scanner_tests PRIVATE bfplayer_core)
    add_test(NAME library_scanner_tests COMMAND library_scanner_tests)

    add_executable(artwork_tests tests/artwork_tests.cpp)
    target_link_libraries(artwork_tests PRIVATE bfplayer_core)
    add_test(NAME artwork_tests COMMAND artwork_tests)

    add_executable(bulk_import_tests tests/bulk_import_tests.cpp)
    target_link_libraries(bulk_import_tests PRIVATE bfplayer_core)
    add_test(NAME bulk_import_tests COMMAND bulk_import_tests)

    add_executable(video_layout_tests tests/video_layout_tests.cpp)
    target_link_libraries(video_layout_tests PRIVATE bfplayer_core)
    add_test(NAME video_layout_tests COMMAND video_layout_tests)

    add_executable(library_view_tests tests/library_view_tests.cpp)
    target_link_libraries(library_view_tests PRIVATE bfplayer_core)
    add_test(NAME library_view_tests COMMAND library_view_tests)

    add_executable(media_sources_tests tests/media_sources_tests.cpp)
    target_link_libraries(media_sources_tests PRIVATE bfplayer_core)
    add_test(NAME media_sources_tests COMMAND media_sources_tests)

    add_executable(player_settings_tests tests/player_settings_tests.cpp)
    target_link_libraries(player_settings_tests PRIVATE bfplayer_core)
    add_test(NAME player_settings_tests COMMAND player_settings_tests)

    add_executable(source_uri_tests tests/source_uri_tests.cpp)
    target_link_libraries(source_uri_tests PRIVATE bfplayer_core)
    add_test(NAME source_uri_tests COMMAND source_uri_tests)

    add_executable(playlist_tests tests/playlist_tests.cpp)
    target_link_libraries(playlist_tests PRIVATE bfplayer_core)
    add_test(NAME playlist_tests COMMAND playlist_tests)

    add_executable(safe_read_file_tests tests/safe_read_file_tests.cpp)
    target_link_libraries(safe_read_file_tests PRIVATE bfplayer_core)
    add_test(NAME safe_read_file_tests COMMAND safe_read_file_tests)

    add_executable(
        standalone_route_tests
        tests/standalone_route_tests.cpp
        src/launcher/standalone_route.c
    )
    target_include_directories(standalone_route_tests PRIVATE include)
    add_test(NAME standalone_route_tests COMMAND standalone_route_tests)

    find_program(BFPLAYER_NODE_EXECUTABLE node)
    if(BFPLAYER_NODE_EXECUTABLE)
        add_test(
            NAME standalone_launcher_tests
            COMMAND "${BFPLAYER_NODE_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/standalone_launcher_tests.js"
        )
        add_test(
            NAME naming_tests
            COMMAND "${BFPLAYER_NODE_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/naming_tests.js"
        )
        add_test(
            NAME startup_diagnostics_tests
            COMMAND "${BFPLAYER_NODE_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/startup_diagnostics_tests.js"
        )
    endif()

    set(BFPLAYER_SQLITE_AMALGAMATION
        "${CMAKE_CURRENT_SOURCE_DIR}/build/host-deps/sqlite-amalgamation-3530300"
        CACHE PATH "Path containing sqlite3.c and sqlite3.h for host tests")
    if(EXISTS "${BFPLAYER_SQLITE_AMALGAMATION}/sqlite3.c")
        add_library(sqlite3_host STATIC "${BFPLAYER_SQLITE_AMALGAMATION}/sqlite3.c")
        target_include_directories(sqlite3_host PUBLIC "${BFPLAYER_SQLITE_AMALGAMATION}")
        target_compile_definitions(sqlite3_host PRIVATE
            SQLITE_THREADSAFE=1
            SQLITE_OMIT_LOAD_EXTENSION=1
        )
        add_executable(library_database_tests
            tests/library_database_tests.cpp
            src/core/library_database.cpp
        )
        target_include_directories(library_database_tests PRIVATE include)
        target_link_libraries(library_database_tests PRIVATE sqlite3_host)
        add_test(NAME library_database_tests COMMAND library_database_tests)
    endif()
endif()
