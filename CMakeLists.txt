cmake_minimum_required(VERSION 3.21)

project(AndreasOnline
    VERSION 0.1.0
    DESCRIPTION "Independent online multiplayer for GTA San Andreas 1.0 US"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(AO_BUILD_CLIENT "Build the in-game ASI client (Windows x86 only)" ON)
option(AO_BUILD_SERVER "Build the dedicated server (Windows + Linux)"    OFF)

# Static MSVC runtime (/MT, /MTd) - plugin-sdk's Plugin.lib is built with
# staticruntime "On", so the client MUST match or the link fails. enet (below)
# picks this up too since it's fetched/added after this is set.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Header-only code shared by client and server (versions, protocol)
add_library(ao_shared INTERFACE)
target_include_directories(ao_shared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/shared/include)
target_compile_definitions(ao_shared INTERFACE
    AO_VERSION_STRING="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# ENet - used by both the client (connecting out) and the server (listening).
# Fetched from source rather than vendored: it's a tiny, rarely-updated C
# library, and this keeps it off the dependency-tracking burden of a git
# submodule. Builds as a static lib on both Windows and Linux.
# ---------------------------------------------------------------------------
if(AO_BUILD_CLIENT OR AO_BUILD_SERVER)
    include(FetchContent)
    FetchContent_Declare(
        enet
        GIT_REPOSITORY https://github.com/lsalzman/enet.git
        GIT_TAG        master
    )
    FetchContent_MakeAvailable(enet)
    # enet's own CMakeLists.txt uses the old-style include_directories(),
    # which is NOT transitive to targets outside that directory - add it
    # explicitly so `target_link_libraries(<tgt> PRIVATE enet)` elsewhere is
    # enough to get enet/enet.h on the include path.
    target_include_directories(enet INTERFACE ${enet_SOURCE_DIR}/include)
endif()

if(AO_BUILD_CLIENT)
    if(NOT WIN32)
        message(STATUS "AO: client skipped (Windows only)")
    elseif(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR
            "AO: the client must be built as 32-bit. Configure with  -A Win32  "
            "(or use the preset: cmake --preset vs2022-x86)")
    else()
        add_subdirectory(client)
    endif()
endif()

if(AO_BUILD_SERVER)
    add_subdirectory(server)
endif()
