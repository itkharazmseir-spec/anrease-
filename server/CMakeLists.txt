# Dedicated server (Windows + Linux), ENet-based.
add_executable(ao_server
    src/Main.cpp
)

target_link_libraries(ao_server PRIVATE ao_shared enet)
target_compile_features(ao_server PRIVATE cxx_std_17)

target_compile_definitions(ao_server PRIVATE
    TARGET_NAME="AndreasOnlineServer"
)

if(MSVC)
    target_compile_options(ao_server PRIVATE /W3 /utf-8)
else()
    target_compile_options(ao_server PRIVATE -Wall -Wextra)
endif()

set_target_properties(ao_server PROPERTIES
    OUTPUT_NAME "AndreasOnlineServer"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
