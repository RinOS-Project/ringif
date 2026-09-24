# RinGIF

RinGIF is a small header-only GIF parser and first-frame decoder for RinOS.
It implements a bounded GIF subset and does not provide full animation
playback.

## Supported API and profile

Include `ringif.h`. `rgif_get_info` validates the GIF87a/GIF89a structure and
reports the logical screen size and frame count. `rgif_decode` and
`rgif_decode_with_scratch` decode only the first image descriptor using GIF
LZW. The first frame may use a local or global color table, interlacing, and a
Graphic Control Extension transparency index. The result is numeric
`0xAARRGGBB` pixels; transparent indices leave the opaque logical-screen
background in place.

Later frames, animation timing, disposal compositing, and animation playback
are not exposed. An extension block that the first-frame decoder does not
interpret is skipped as bounded sub-block data. A successful frame count from
`rgif_get_info` must not be read as a promise that every frame can be
rendered.

## Ownership, limits, and errors

The input, output pixels, index buffer, and optional `RGifDecoder` state are
caller-owned. The convenience entry point creates a large decoder-state
object on its stack; use `rgif_decode_with_scratch` when the caller needs to
control that storage. Calls use local or caller-supplied state and are
reentrant when their buffers are independent.

The decoder caps input at 64 MiB, either logical-screen dimension at 8192,
total canvas pixels at 16,777,216, and probed frame count at 1024. `RGIF_OK`,
`RGIF_UNSUPPORTED`, `RGIF_DATA_ERROR`, and `RGIF_ERROR` report success,
unsupported dimensions/profile, malformed data, and invalid/capacity failure.
There is no cancellation or CPU deadline.

## Security, ABI, build, and tests

Treat all input as untrusted, enforce caller-specific limits, and size the
index and output buffers before decoding. Failure may occur during validation
or decoding; discard both output buffers on any non-`RGIF_OK` result. This is
a header-defined C source interface without a separately versioned binary
ABI guarantee. RinOS integrates it through RinImage; this repository has no
standalone build or test target.
