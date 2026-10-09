# FFmpeg

Used by `codemp/client/cl_cin_video.cpp` to play mp4/webm/mkv/mov cinematics.
The engine only needs the headers to build; it loads the DLLs when the game runs,
and without them every cinematic is a RoQ, as before.

- Version: FFmpeg 9.0.2, LGPL 2.1 (no `--enable-gpl`, no non-free parts), see `COPYING.LGPLv2.1`
- Built with vcpkg, triplet `x86-windows` (shared), port features
  `avcodec avformat swscale swresample`, default features off
- `include/`: the headers of that build
- `bin/x86/`: the DLLs, installed next to the exe by CMake; ship them with it

To rebuild, from a folder with a `vcpkg.json` asking for
`{ "name": "ffmpeg", "default-features": false, "features": [ "avcodec", "avformat", "swscale", "swresample" ] }`:

    vcpkg install --triplet x86-windows

then copy `include/` and the `bin/*.dll` of `packages/ffmpeg_x86-windows`. Linux and
macOS builds use the system's FFmpeg headers (pkg-config) and libraries instead.
