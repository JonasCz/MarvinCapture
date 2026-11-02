# third_party/

## FFmpeg (vendored, built from source)

`marvin-core` links a small slice of FFmpeg statically: `libavformat`,
`libavcodec` and `libavutil` only (no `libavdevice`, `libavfilter`,
`libswscale` or `libswresample` -- colour conversion happens on the GPU in
the GUI, not in the core).

- **Version:** FFmpeg 8.1.3 (pinned; source tarball sha256 verified in
  `scripts/build-ffmpeg.sh`).
- **License:** LGPL-2.1-or-later. The build has `--enable-gpl` and
  `--enable-nonfree` both **off**, and only LGPL-safe components are
  compiled in, so the resulting static libraries can be linked into the
  AGPL-3.0 `marvin-core` without a license conflict.
- **Components enabled** (everything else is `--disable-everything`):
  - Encoders: `ffv1`, `flac`, `pcm_s16le`
  - Decoders: `ffv1`, `flac`, `dvvideo`, `mpeg2video`, `pcm_s16le`, `pcm_s16be`,
    `mp2`, `mp2float`
  - Muxers: `matroska`, `mov`, `avi`, `dv`, `mpegts`
  - Demuxers: `dv`, `mpegts`, `matroska`, `mov`, `avi`
  - Parsers: `mpegvideo`, `mpegaudio`, `flac`
  - Protocol: `file`
  - No network, no autodetected external libraries, no programs/doc/bsfs.
- **Threading:** w32threads on Windows (avoids a winpthread DLL dependency
  for the static libs), pthreads on Linux/macOS. The FFV1 encoder uses
  slice threading (`slices=16`, `thread_count` set by the caller).

### Where the source comes from

Only the *installed* static libraries (`third_party/ffmpeg-<os>-<arch>/`) are
needed to build `marvin-core`. The FFmpeg source is **not** kept in the
repo or needed afterwards; `scripts/build-ffmpeg.sh` downloads it on demand:

- Tarball: <https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz> (release list and
  signatures at <https://ffmpeg.org/download.html>)
- sha256: `7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3`
- It is unpacked to `third_party/ffmpeg-src/`, built out of tree in
  `third_party/ffmpeg-<os>-<arch>-build/`, and both can be deleted once the
  install directory exists. `scripts/build.ps1` runs the script automatically
  if the install directory is missing.
- To use a different FFmpeg, edit `FFMPEG_VERSION` and `FFMPEG_SHA256` at the
  top of the script, or point CMake at an existing build with
  `-DPIN_FFMPEG_PREFIX=<prefix>`.

### Rebuilding

```sh
scripts/build-ffmpeg.sh                     # fetch, verify sha256, configure, build, install
scripts/build-ffmpeg.sh --clean             # wipe the build/install dirs for this OS/arch first
scripts/build-ffmpeg.sh --android arm64-v8a # Android (or x86_64), with the NDK; prefix
                                            # third_party/ffmpeg-android-arm64 (-x86_64)
```

The Android build (run by `scripts/build-android-native.sh`) uses the same
component list, cross-compiled with the NDK version and API level of
`scripts/android-ndk.sh`.

On Windows this must run inside an MSYS2 UCRT64 shell (gcc, make, nasm,
pkgconf, diffutils on `PATH`). The script auto-detects Linux and macOS too.

Output layout (none of this is committed -- see `.gitignore`):

```
third_party/ffmpeg-src/                  downloaded tarball + extracted source
third_party/ffmpeg-<os>-<arch>-build/    out-of-tree configure/build directory
third_party/ffmpeg-<os>-<arch>/          install prefix (include/, lib/*.a, lib/pkgconfig/)
```

`marvin-core`'s build picks up the static libs via
`PKG_CONFIG_PATH=third_party/ffmpeg-<os>-<arch>/lib/pkgconfig` and
`pkg-config --static --cflags --libs libavformat libavcodec libavutil`.

### Static linking

The project links these libraries **statically** into `marvin-core`
(`marvin-core.dll` / `libmarvin-core.so` / `.dylib`); no FFmpeg
shared libraries are shipped or required at runtime. Per LGPL-2.1 section 6,
the pinned version and this document record exactly what was built and how,
and `scripts/build-ffmpeg.sh` lets anyone reproduce or relink against a
different FFmpeg build from the same (or a compatible) source.

## libusb (Android only, built from source)

On Windows, macOS and Linux the core uses the system's (or Homebrew's, or MSYS2's)
libusb-1.0. Android has none, so `scripts/build-android-native.sh` builds it and
links it **statically** into `libmarvin-core.so`:

- **Version:** libusb 1.0.30 (it has the Android fixes for devices opened from a
  file descriptor, `libusb_wrap_sys_device()`).
- **License:** LGPL-2.1-or-later, like FFmpeg; the same relinking reasoning
  applies (the version, source and build steps are recorded here and in the
  script, so anyone can rebuild or relink against a modified libusb).
- **Source:** <https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2>,
  sha256 `fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf`
  (pinned in the script), unpacked to `third_party/libusb-src/`.
- **Build:** autotools `configure --host=<triple> --disable-udev --enable-static
  --disable-shared --with-pic` with the NDK's clang for API 30, out of tree in
  `third_party/libusb-android-<arch>-build/`, installed to
  `third_party/libusb-android-<arch>/` (`<arch>` is `arm64` or `x86_64`). Not
  committed (`.gitignore`).

