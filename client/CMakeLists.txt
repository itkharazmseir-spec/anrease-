include(${PROJECT_SOURCE_DIR}/cmake/PluginSdkSA.cmake)

set(GTA_SA_DIR "$ENV{GTA_SA_DIR}" CACHE PATH
    "GTA San Andreas install folder (optional: the .asi is copied there after build)")

add_library(ao_client SHARED
    src/Main.cpp
    src/Log.cpp
    src/Log.h
    src/LocalPlayer.cpp
    src/LocalPlayer.h
    src/DebugOverlay.cpp
    src/DebugOverlay.h
    src/NetworkConfig.cpp
    src/NetworkConfig.h
    src/NetworkClient.cpp
    include/ao/NetworkClient.h
)

target_include_directories(ao_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(ao_client PRIVATE PluginSDK::SA ao_shared enet)

target_compile_definitions(ao_client PRIVATE
    TARGET_NAME="AndreasOnline"
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(ao_client PRIVATE /W3 /utf-8)

set_target_properties(ao_client PROPERTIES
    OUTPUT_NAME "AndreasOnline"
    SUFFIX ".asi"
    PREFIX ""
    # plugin-sdk's Release Plugin.lib is built with link-time code generation
    INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VS_DEBUGGER_WORKING_DIRECTORY "${GTA_SA_DIR}"
    VS_DEBUGGER_COMMAND "${GTA_SA_DIR}/gta_sa.exe"
)

if(GTA_SA_DIR)
    file(TO_CMAKE_PATH "${GTA_SA_DIR}" _gta)
    add_custom_command(TARGET ao_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_gta}/scripts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:ao_client>" "${_gta}/scripts/"
        COMMENT "Copying AndreasOnline.asi to ${_gta}/scripts")
endif()
