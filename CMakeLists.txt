cmake_minimum_required(VERSION 3.20)
# The version itself is set in include/dd_version.h; keep this in step.
project(dd_windows VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# A single self-contained dd.exe, as 0.6 was.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# liblzma from XZ Utils 5.8.4 (third_party/xz, see README.dd.md), for .xz
# images.  The defines are the ones XZ Utils' own CMake build uses on x64.
file(GLOB_RECURSE LZMA_SOURCES CONFIGURE_DEPENDS third_party/xz/src/*.c)
add_library(lzma STATIC ${LZMA_SOURCES})
target_include_directories(lzma
    PUBLIC  third_party/xz/src/liblzma/api
    PRIVATE third_party/xz/src/liblzma/common third_party/xz/src/liblzma/check
            third_party/xz/src/liblzma/lz third_party/xz/src/liblzma/rangecoder
            third_party/xz/src/liblzma/lzma third_party/xz/src/liblzma/delta
            third_party/xz/src/liblzma/simple third_party/xz/src/common)
target_compile_definitions(lzma
    PUBLIC  LZMA_API_STATIC
    PRIVATE HAVE_CHECK_CRC32 HAVE_CHECK_CRC64 HAVE_CHECK_SHA256
            HAVE_DECODERS HAVE_DECODER_ARM HAVE_DECODER_ARM64 HAVE_DECODER_ARMTHUMB HAVE_DECODER_DELTA
            HAVE_DECODER_IA64 HAVE_DECODER_LZMA1 HAVE_DECODER_LZMA2 HAVE_DECODER_POWERPC HAVE_DECODER_RISCV
            HAVE_DECODER_SPARC HAVE_DECODER_X86
            HAVE_ENCODERS HAVE_ENCODER_ARM HAVE_ENCODER_ARM64 HAVE_ENCODER_ARMTHUMB HAVE_ENCODER_DELTA
            HAVE_ENCODER_IA64 HAVE_ENCODER_LZMA1 HAVE_ENCODER_LZMA2 HAVE_ENCODER_POWERPC HAVE_ENCODER_RISCV
            HAVE_ENCODER_SPARC HAVE_ENCODER_X86
            HAVE_IMMINTRIN_H HAVE_INTTYPES_H HAVE_LZIP_DECODER
            HAVE_MF_BT2 HAVE_MF_BT3 HAVE_MF_BT4 HAVE_MF_HC3 HAVE_MF_HC4
            HAVE_STDBOOL_H HAVE_STDINT_H HAVE_USABLE_CLMUL HAVE_VISIBILITY=0 HAVE__BOOL
            HAVE__MM_MOVEMASK_EPI8 HAVE___BUILTIN_ASSUME_ALIGNED
            MYTHREAD_VISTA TUKLIB_FAST_UNALIGNED_ACCESS TUKLIB_SYMBOL_PREFIX=lzma_ NDEBUG)

# The copy engine.  A GUI links this library and supplies its own callbacks.
add_library(ddcore STATIC
    lib/copy.c
    lib/device.c
    lib/endpoint.c
    lib/format.c
    lib/fs_exfat.c
    lib/fs_ext.c
    lib/fs_fat.c
    lib/fs_ntfs.c
    lib/fs_swap.c
    lib/fsmap.c
    lib/hash.c
    lib/layout.c
    lib/options.c
    lib/xz.c
)
target_include_directories(ddcore PUBLIC include)
target_compile_definitions(ddcore PUBLIC
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601)   # Windows 7 or later
target_link_libraries(ddcore PUBLIC lzma setupapi bcrypt)

add_executable(dd cli/main.c cli/dd.rc)
target_link_libraries(dd PRIVATE ddcore)

# The GUI.  Its manifest (common controls 6, elevation, DPI) is in gui.rc.
add_executable(dd-gui WIN32 gui/gui.c gui/gui.rc)
target_link_libraries(dd-gui PRIVATE ddcore user32 gdi32 comctl32 comdlg32 shell32 uxtheme)

if(MSVC)
    target_compile_options(ddcore PRIVATE /W4)
    target_compile_options(dd PRIVATE /W4)
    target_compile_options(dd-gui PRIVATE /W4)
    target_link_options(dd-gui PRIVATE /MANIFEST:NO)
else()
    target_compile_options(ddcore PRIVATE -Wall -Wextra)
    target_compile_options(dd PRIVATE -Wall -Wextra)
    target_compile_options(dd-gui PRIVATE -Wall -Wextra)
    target_link_options(dd PRIVATE -municode)
    target_link_options(dd-gui PRIVATE -municode)
endif()
