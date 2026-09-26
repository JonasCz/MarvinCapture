# third_party/

## FFmpeg (vendored, built from source)

`pinnacle-oss-core` links a small slice of FFmpeg statically: `libavformat`,
`libavcodec` and `libavutil` only (no `libavdevice`, `libavfilter`,
`libswscale` or `libswresample` -- colour conversion happens on the GPU in
the GUI, not in the core).

- **Version:** FFmpeg 8.1.3 (pinned; source tarball sha256 verified in
  `scripts/build-ffmpeg.sh`).
- **License:** LGPL-2.1-or-later. The build has `--enable-gpl` and
  `--enable-nonfree` both **off**, and only LGPL-safe components are
  compiled in, so the resulting static libraries can be linked into the
  AGPL-3.0 `pinnacle-oss-core` without a license conflict.
- **Components enabled** (everything else is `--disable-everything`):
  - Encoders: `ffv1`, `pcm_s16le`
  - Decoders: `ffv1`, `dvvideo`, `mpeg2video`, `pcm_s16le`, `pcm_s16be`,
    `mp2`, `mp2float`
  - Muxers: `matroska`, `mov`, `avi`, `dv`, `mpegts`
  - Demuxers: `dv`, `mpegts`, `matroska`, `mov`, `avi`
  - Parsers: `mpegvideo`, `mpegaudio`
  - Protocol: `file`
  - No network, no autodetected external libraries, no programs/doc/bsfs.
- **Threading:** w32threads on Windows (avoids a winpthread DLL dependency
  for the static libs), pthreads on Linux/macOS. The FFV1 encoder uses
  slice threading (`slices=16`, `thread_count` set by the caller).

### Where the source comes from

Only the *installed* static libraries (`third_party/ffmpeg-<os>-<arch>/`) are
needed to build `pinnacle-oss-core`. The FFmpeg source is **not** kept in the
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
scripts/build-ffmpeg.sh          # fetch, verify sha256, configure, build, install
scripts/build-ffmpeg.sh --clean  # wipe the build/install dirs for this OS/arch first
```

On Windows this must run inside an MSYS2 UCRT64 shell (gcc, make, nasm,
pkgconf, diffutils on `PATH`). The script auto-detects Linux and macOS too.

Output layout (none of this is committed -- see `.gitignore`):

```
third_party/ffmpeg-src/                  downloaded tarball + extracted source
third_party/ffmpeg-<os>-<arch>-build/    out-of-tree configure/build directory
third_party/ffmpeg-<os>-<arch>/          install prefix (include/, lib/*.a, lib/pkgconfig/)
```

`pinnacle-oss-core`'s build picks up the static libs via
`PKG_CONFIG_PATH=third_party/ffmpeg-<os>-<arch>/lib/pkgconfig` and
`pkg-config --static --cflags --libs libavformat libavcodec libavutil`.

### Static linking

The project links these libraries **statically** into `pinnacle-oss-core`
(`pinnacle-oss-core.dll` / `libpinnacle-oss-core.so` / `.dylib`); no FFmpeg
shared libraries are shipped or required at runtime. Per LGPL-2.1 section 6,
the pinned version and this document record exactly what was built and how,
and `scripts/build-ffmpeg.sh` lets anyone reproduce or relink against a
different FFmpeg build from the same (or a compatible) source.
