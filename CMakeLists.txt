cmake_minimum_required(VERSION 3.20)
project(PowerMon VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Uniquement Windows
if(NOT WIN32)
    message(FATAL_ERROR "PowerMon est un outil Windows uniquement.")
endif()

add_executable(powermon WIN32
    src/main.cpp
)

# Subsystem CONSOLE (on veut le terminal)
set_target_properties(powermon PROPERTIES
    WIN32_EXECUTABLE FALSE
    OUTPUT_NAME "powermon"
)

# Libs Windows nécessaires
target_link_libraries(powermon PRIVATE
    pdh
    wbemuuid
    ole32
    oleaut32
    setupapi
    powrprof
    psapi
)

# Optimisations release
target_compile_options(powermon PRIVATE
    $<$<CONFIG:Release>:/O2 /GL /DNDEBUG>
    $<$<CONFIG:Debug>:/Od /Zi>
)

target_link_options(powermon PRIVATE
    $<$<CONFIG:Release>:/LTCG>
)

# Manifeste Windows pour les droits d'accès PDH
configure_file(
    "${CMAKE_SOURCE_DIR}/powermon.manifest"
    "${CMAKE_CURRENT_BINARY_DIR}/powermon.manifest"
    COPYONLY
)
