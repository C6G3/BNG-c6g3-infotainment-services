# sherpa-onnx (Kokoro TTS runtime)

The voice assistant's text-to-speech runs on [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx)
(Apache-2.0) through its plain C API, on the CPU. The C interface links from our MinGW build even
though the DLLs are built with MSVC.

| Path | In git | What |
|---|---|---|
| `include/sherpa-onnx/c-api/c-api.h` | yes | Header, pinned to **v1.13.8** (must match the DLLs) |
| `lib/` | no | Import library and runtime DLLs (see below) |

## Setup (developers)

1. Download `sherpa-onnx-v1.13.8-win-x64-shared-MT-Release-lib.tar.bz2` (8 MB) from
   <https://github.com/k2-fsa/sherpa-onnx/releases/tag/v1.13.8>.
2. Copy `sherpa-onnx-c-api.lib`, `sherpa-onnx-c-api.dll` and `onnxruntime.dll` from its `lib/` folder
   into `third_party/sherpa-onnx/lib/`.
3. Configure CMake. It stops with a clear error if `lib/` is missing.

"MT" links the C runtime statically, so users need no Visual C++ redistributable. Every build copies
the two DLLs next to the executable, and `dist` ships them.

## GPU: tried and removed (2026-10-06)

sherpa-onnx was built from source with DirectML and tested on an Intel Iris Xe. Kokoro failed on
DirectML: onnxruntime 1.14.1 failed on the `/Loop` node, and 1.24.4 failed on
`/N.1/pool/ConvTranspose` ("The parameter is incorrect"). sherpa-onnx terminates the process when
inference fails. All GPU code was then removed from the service and the infotainment. Reviving it
would need a DirectML-compatible model (rewriting the grouped ConvTranspose layers) or CUDA on NVIDIA.

## Licensing

sherpa-onnx includes espeak-ng (GPL-3.0) for phonemes. Check its terms before a public release.
