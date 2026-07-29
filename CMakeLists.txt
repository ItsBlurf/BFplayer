cmake_minimum_required(VERSION 3.20)
project(bfplayer_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(bfplayer_core STATIC
    src/core/artwork.cpp
    src/core/bulk_import.cpp
    src/core/diagnostics.cpp
    src/core/dlna_protocol.cpp
    src/core/library_scanner.cpp
    src/core/library_view.cpp
    src/core/list_navigation.cpp
    src/core/media_sources.cpp
    src/core/player_settings.cpp
    src/core/playlist.cpp
    src/core/safe_read_file.cpp
    src/core/source_uri.cpp
    src/core/subtitle_browser.cpp
    src/core/subtitle_provider.cpp
    src/core/video_layout.cpp
)
target_include_directories(bfplayer_core PUBLIC include)

if(MSVC)
    target_compile_options(bfplayer_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(bfplayer_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)

option(
    BFPLAYER_BUILD_DLNA_INTEGRATION_TESTS
    "Build POSIX DLNA socket/HTTP integration tests"
    OFF
)

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

    add_executable(dlna_protocol_tests tests/dlna_protocol_tests.cpp)
    target_link_libraries(dlna_protocol_tests PRIVATE bfplayer_core)
    add_test(NAME dlna_protocol_tests COMMAND dlna_protocol_tests)

    add_executable(subtitle_provider_tests tests/subtitle_provider_tests.cpp)
    target_link_libraries(subtitle_provider_tests PRIVATE bfplayer_core)
    add_test(NAME subtitle_provider_tests COMMAND subtitle_provider_tests)

    add_executable(subtitle_browser_tests tests/subtitle_browser_tests.cpp)
    target_link_libraries(subtitle_browser_tests PRIVATE bfplayer_core)
    add_test(NAME subtitle_browser_tests COMMAND subtitle_browser_tests)

    add_executable(list_navigation_tests tests/list_navigation_tests.cpp)
    target_link_libraries(list_navigation_tests PRIVATE bfplayer_core)
    add_test(NAME list_navigation_tests COMMAND list_navigation_tests)

    add_executable(playlist_tests tests/playlist_tests.cpp)
    target_link_libraries(playlist_tests PRIVATE bfplayer_core)
    add_test(NAME playlist_tests COMMAND playlist_tests)

    add_executable(safe_read_file_tests tests/safe_read_file_tests.cpp)
    target_link_libraries(safe_read_file_tests PRIVATE bfplayer_core)
    add_test(NAME safe_read_file_tests COMMAND safe_read_file_tests)

    add_executable(
        kitchensink_audio_clock_tests
        tests/kitchensink_audio_clock_tests.cpp
        src/playback/kitchensink_audio_clock.c
    )
    target_include_directories(kitchensink_audio_clock_tests PRIVATE include)
    add_test(
        NAME kitchensink_audio_clock_tests
        COMMAND kitchensink_audio_clock_tests
    )

    if(BFPLAYER_BUILD_DLNA_INTEGRATION_TESTS)
        if(NOT UNIX)
            message(
                FATAL_ERROR
                "DLNA integration tests require POSIX sockets"
            )
        endif()
        find_package(Threads REQUIRED)
        find_package(tinyxml2 CONFIG REQUIRED)
        add_executable(
            dlna_client_integration_tests
            tests/dlna_client_integration_tests.cpp
            src/network/dlna_client.cpp
        )
        target_include_directories(
            dlna_client_integration_tests
            PRIVATE include
        )
        target_link_libraries(
            dlna_client_integration_tests
            PRIVATE
                bfplayer_core
                tinyxml2::tinyxml2
                Threads::Threads
        )
        add_test(
            NAME dlna_client_integration_tests
            COMMAND dlna_client_integration_tests
        )
        set_tests_properties(
            dlna_client_integration_tests
            PROPERTIES TIMEOUT 15
        )
    endif()

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
        add_test(
            NAME websrv_package_tests
            COMMAND "${BFPLAYER_NODE_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/websrv_package_tests.js"
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
