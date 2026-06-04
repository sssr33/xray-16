include_guard()

# The MSVC compiler settings:
# Set properties:
set(CMAKE_VS_USE_DEBUG_LIBRARIES "$<CONFIG:Debug>")

# Clear predefined flags which we going to define ourselves
string(REGEX REPLACE "/EH[a-z]+" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS}) # exceptions
string(REGEX REPLACE "/Z(7|i|I)" "" CMAKE_CXX_FLAGS_DEBUG ${CMAKE_CXX_FLAGS_DEBUG}) # debug information format

# Enable standard C++ exceptions everywhere except ReleaseMasterGold
add_compile_options($<$<NOT:$<CONFIG:ReleaseMasterGold>>:/EHsc>)

# Disable MS STL exceptions on ReleaseMasterGold
add_compile_definitions($<$<CONFIG:ReleaseMasterGold>:_HAS_EXCEPTIONS=0>)

# Enable debug information for all configurations
add_compile_options(/Zi)

# Enable SSE2 for 32-bit build
# (on x64 it's always enabled and produces error if try to to enable it)
add_compile_options($<$<EQUAL:${CMAKE_SIZEOF_VOID_P},4>:/arch:SSE2>)

# Disable specific warnings
add_compile_options(
    /wd4201 # nonstandard extension used : nameless struct/union
    /wd4251 # class 'x' needs to have dll-interface to be used by clients of class 'y'
    /wd4275 # non dll-interface class 'x' used as base for dll-interface class 'y'
)

# The MSVC linker settings:
add_link_options("/LARGEADDRESSAWARE")

set(XRAY_DISABLE_WARNINGS "/w")

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(XRAY_SDK_PLATFORM_DIR x64)
else()
    set(XRAY_SDK_PLATFORM_DIR x86)
endif()

set(XRAY_SDK_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/sdk/include")
set(XRAY_SDK_LIBRARY_DIR "${CMAKE_SOURCE_DIR}/sdk/libraries/${XRAY_SDK_PLATFORM_DIR}")

function(xray_add_sdk_imported_library target library)
    if (NOT TARGET ${target})
        add_library(${target} UNKNOWN IMPORTED)
        set_target_properties(${target} PROPERTIES
            IMPORTED_LOCATION "${XRAY_SDK_LIBRARY_DIR}/${library}"
            INTERFACE_INCLUDE_DIRECTORIES "${XRAY_SDK_INCLUDE_DIR}"
        )
    endif()
endfunction()

xray_add_sdk_imported_library(OpenAL::OpenAL OpenAL32.lib)
xray_add_sdk_imported_library(Ogg::Ogg libogg_static.lib)
xray_add_sdk_imported_library(Vorbis::Vorbis libvorbis_static.lib)
xray_add_sdk_imported_library(Vorbis::VorbisFile libvorbisfile.lib)
xray_add_sdk_imported_library(Theora::Theora libtheora_static.lib)
xray_add_sdk_imported_library(LZO::LZO lzo.lib)
xray_add_sdk_imported_library(JPEG::JPEG jpeg-static.lib)

set(JPEG_FOUND TRUE)
set(MEMORY_ALLOCATOR "standard" CACHE STRING "Use specific memory allocator (mimalloc/standard)")
set_property(CACHE MEMORY_ALLOCATOR PROPERTY STRINGS "mimalloc" "standard")

find_package(SDL2 2.0.18 CONFIG QUIET)
if (NOT TARGET SDL2::SDL2)
    find_package(SDL2 2.0.18 QUIET)
endif()
if (NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 INTERFACE IMPORTED)
    message(WARNING "SDL2 was not found. Visual Studio project generation will continue, but targets that include SDL headers require SDL2 to build.")
endif()

unset(XRAY_SDK_INCLUDE_DIR)
unset(XRAY_SDK_LIBRARY_DIR)
unset(XRAY_SDK_PLATFORM_DIR)
