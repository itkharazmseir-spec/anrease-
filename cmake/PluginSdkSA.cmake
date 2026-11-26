# ---------------------------------------------------------------------------
# Imports plugin-sdk (GTA San Andreas part) as the CMake target  PluginSDK::SA
#
# Requirements (one-time, done inside the plugin-sdk checkout):
#   Build the "Plugin_SA" project of plugin-sdk in BOTH configurations:
#       Release -> <sdk>/output/lib/Plugin.lib
#       zDebug  -> <sdk>/output/lib/Plugin_d.lib
#   (Use the plugin-sdk installer / premake to generate its solution first.)
#
# The settings below mirror what plugin-sdk's own premake5.lua uses for
# Plugin_SA and for generated ASI projects (verified against commit 15f15b6):
#   defines  GTASA PLUGIN_SGV_10US RW _CRT_NON_CONFORMING_SWPRINTFS
#   runtime  static (/MT, /MTd)   charset MBCS   C++latest   /sdl-
# ---------------------------------------------------------------------------

set(PLUGIN_SDK_DIR "$ENV{PLUGIN_SDK_DIR}" CACHE PATH "Path to plugin-sdk checkout")

if(NOT PLUGIN_SDK_DIR OR NOT EXISTS "${PLUGIN_SDK_DIR}/plugin_sa/plugin_sa.h")
    message(FATAL_ERROR
        "AO: plugin-sdk not found.\n"
        "Set the PLUGIN_SDK_DIR environment variable (the plugin-sdk installer does this) "
        "or pass -DPLUGIN_SDK_DIR=C:/path/to/plugin-sdk")
endif()

file(TO_CMAKE_PATH "${PLUGIN_SDK_DIR}" _sdk)

set(_lib_release "${_sdk}/output/lib/Plugin.lib")
set(_lib_debug   "${_sdk}/output/lib/Plugin_d.lib")

if(NOT EXISTS "${_lib_release}")
    message(WARNING
        "AO: ${_lib_release} does not exist yet. "
        "Build plugin-sdk's Plugin_SA project (Release) before building the client.")
endif()
if(NOT EXISTS "${_lib_debug}")
    message(STATUS "AO: ${_lib_debug} not found - Debug client build will not link "
                   "until plugin-sdk Plugin_SA is built in its zDebug configuration.")
endif()

add_library(PluginSDK_SA INTERFACE)
add_library(PluginSDK::SA ALIAS PluginSDK_SA)

target_include_directories(PluginSDK_SA SYSTEM INTERFACE
    "${_sdk}/plugin_sa"
    "${_sdk}/plugin_sa/game_sa"
    "${_sdk}/plugin_sa/game_sa/enums"
    "${_sdk}/plugin_sa/game_sa/rw"
    "${_sdk}/shared"
    "${_sdk}/shared/game"
)

target_compile_definitions(PluginSDK_SA INTERFACE
    GTASA
    PLUGIN_SGV_10US
    RW
    _CRT_NON_CONFORMING_SWPRINTFS
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
    _MBCS
    $<$<CONFIG:Debug>:DEBUG>
)

target_compile_features(PluginSDK_SA INTERFACE cxx_std_23)   # -> /std:c++latest on VS2022

target_compile_options(PluginSDK_SA INTERFACE
    /sdl-
    /MP
    /wd4073        # "initializers put in library initialization area" (plugin-sdk)
)

target_link_libraries(PluginSDK_SA INTERFACE
    $<IF:$<CONFIG:Debug>,${_lib_debug},${_lib_release}>
)
