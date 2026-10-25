# liblzma for dd reimagined for Windows

This is liblzma from **XZ Utils 5.8.4**. dd uses it for multithreaded .xz
(LZMA2) compression of the images it reads, and for decompressing the images
it writes.

- **Source:** https://github.com/tukaani-project/xz/releases/tag/v5.8.4,
  `xz-5.8.4.tar.gz`, SHA-256
  `0014C7886930454FE8BD4228665B51AF55EEAE560EA135C9C4CD33F55B2591D9`.
- **Signature:** `xz-5.8.4.tar.gz.sig` was verified on 2026-10-10. It is a
  good signature from Lasse Collin, key fingerprint
  `3690 C240 CE51 B467 0D30 AD1C 38EE 757D 6918 4620`.
- **Licence:** liblzma is under the BSD Zero Clause License (COPYING.0BSD),
  which is compatible with dd's GPL v2 or later.

## What was kept

- From `src/liblzma`: the 77 `.c` files that XZ Utils' own CMake build
  compiles for MSVC on x64, and every header.
- From `src/common`: `tuklib_physmem.c`, `tuklib_cpucores.c` and the
  headers.

The generators, the `*_small.c` variants, the build scripts, the tools and
the tests were removed.

## How it is built

`liblzma.rsp` holds the flags and defines that XZ Utils' CMake configuration
produced with MSVC 18 for x64. They were taken from compile_commands.json,
minus the `PACKAGE_*` strings, which liblzma does not use. Warnings are
turned off with /W0 because this is upstream code.

`build.cmd` compiles every `.c` file here into `build\lzma.lib` when that
file is missing; delete it to force a rebuild. CMakeLists.txt builds the
same files as the `lzma` target.

## To update

1. Download and verify a newer release.
2. Configure it once with CMake (`-G "NMake Makefiles"
   -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
   -DBUILD_SHARED_LIBS=OFF`).
3. Copy the files listed in compile_commands.json for the liblzma target.
4. Refresh the defines in `liblzma.rsp`.
