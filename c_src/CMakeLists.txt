cmake_minimum_required(VERSION 3.10)
project(ducktape_nif C)

# Output directory
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/../priv)

# Set module path for FindErlang
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/CMake)

# Find Erlang
find_package(Erlang REQUIRED)

# Platform-specific settings
if(APPLE)
    set(CMAKE_SHARED_MODULE_CREATE_C_FLAGS
        "${CMAKE_SHARED_MODULE_CREATE_C_FLAGS} -flat_namespace -undefined suppress")
    if(EXISTS "/opt/homebrew")
        # Apple Silicon
        include_directories(/opt/homebrew/include)
        link_directories(/opt/homebrew/lib)
    elseif(EXISTS "/usr/local/opt")
        # Intel Mac with Homebrew
        include_directories(/usr/local/include)
        link_directories(/usr/local/lib)
    endif()
endif()

# C standard
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler flags
if(MSVC)
    add_compile_options(/W3)
else()
    add_compile_options(-Wall -Wextra -pedantic)
    # Duktape may have some warnings we need to suppress
    add_compile_options(-Wno-unused-parameter)
endif()

# Include Erlang headers
include_directories(${ERLANG_EI_INCLUDE_PATH})
include_directories(${ERLANG_ERTS_INCLUDE_PATH})

# Library paths
link_directories(${ERLANG_EI_LIBRARY_PATH})

# Duktape source (will be added in step 2)
# For now, create a stub NIF

# Source files
set(DUCKTAPE_NIF_SRC
    ducktape_nif.c
)

# Create the shared library
add_library(ducktape MODULE ${DUCKTAPE_NIF_SRC})

# Set output name without 'lib' prefix on some platforms
set_target_properties(ducktape PROPERTIES PREFIX "")

# Link libraries
if(WIN32)
    target_link_libraries(ducktape ei)
elseif(APPLE)
    # macOS doesn't need explicit linking for NIF
else()
    target_link_libraries(ducktape ei pthread)
endif()
