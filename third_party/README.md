# libwebp 1.6.0

Unmodified decoder and demux sources and headers from Google's libwebp 1.6.0:
https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz

Release archive SHA256:
`e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564`

Only the decoder/demux C files selected by `WebP.props`, upstream headers, and
the upstream AUTHORS, COPYING, and PATENTS notices are retained. Encoder,
command-line tools, and upstream build files are omitted. Sources are unchanged.

`WebP.props` is imported by both Visual Studio projects. It compiles the decoder
directly into the plugin/test binary, using the same static runtime as the host
project. SSE2, SSE4.1, and AVX2 implementations use libwebp's runtime CPU dispatch;
no extra DLL, codec installation, download, or build tool is needed to build.
Keep the upstream notices when redistributing the plugin.
