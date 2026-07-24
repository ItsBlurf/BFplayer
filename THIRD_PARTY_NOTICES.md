# Third-party notices

PS5 Media Center is built with open-source libraries distributed by
PacBrew v0.37. The release ELF statically links the components selected by the
linker from the archives listed below. This file records their upstream project
and license family; the upstream projects remain authoritative for complete
license texts and source.

| Component | License family | Upstream |
| --- | --- | --- |
| SDL2, SDL2_image, SDL2_ttf | Zlib | <https://libsdl.org/> |
| SDL_kitchensink | MIT | <https://github.com/Tuomas56/SDL_kitchensink> |
| FFmpeg 7.0.1 libraries | LGPL-3.0-or-later for this configuration | <https://ffmpeg.org/> |
| libass | ISC | <https://github.com/libass/libass> |
| Fontconfig | MIT-style | <https://www.freedesktop.org/wiki/Software/fontconfig/> |
| HarfBuzz | MIT | <https://github.com/harfbuzz/harfbuzz> |
| FriBidi | LGPL-2.1-or-later | <https://github.com/fribidi/fribidi> |
| FreeType | FTL or GPL-2.0-or-later | <https://freetype.org/> |
| libpng | Libpng-2.0 | <https://libpng.org/> |
| bzip2 | bzip2-1.0.6 | <https://sourceware.org/bzip2/> |
| zlib | Zlib | <https://zlib.net/> |
| XZ Utils liblzma | Public domain/license grant in XZ COPYING | <https://tukaani.org/xz/> |
| OpenSSL | Apache-2.0 | <https://openssl-library.org/> |
| GNU libiconv/libcharset | LGPL-2.1-or-later | <https://www.gnu.org/software/libiconv/> |
| libsamplerate | BSD-2-Clause | <https://libsndfile.github.io/libsamplerate/> |
| Expat | MIT | <https://libexpat.github.io/> |
| SQLite | Public domain | <https://sqlite.org/> |
| libc++, libc++abi, libunwind | Apache-2.0 WITH LLVM-exception | <https://libcxx.llvm.org/> |
| Noto Sans | SIL Open Font License 1.1 | `assets/fonts/OFL.txt` in this package |
| PS5 BigApp/ELF loader core | GPL-3.0-or-later | <https://github.com/ps5-payload-dev/websrv> |

The exact FFmpeg configure string embedded in PacBrew's `libavcodec.a` includes
`--enable-static`, `--disable-shared`, `--enable-openssl`, and
`--enable-version3`. It does not include `--enable-gpl` or
`--enable-nonfree`. Corresponding FFmpeg source for version 7.0.1 is available
from <https://ffmpeg.org/releases/>.

## Standalone BigApp loader

`src/launcher/core/hbldr.c`, `elfldr.c`, and `pt.c` originate from John
Törnblom's ps5-payload-websrv project. The local hbldr copy is modified to
remove websrv-specific headers and to accept the Media Center ELF directly
from the standalone payload image. Those files and the combined standalone
launcher are GPL-3.0-or-later. The complete license is in `LICENSE`.

## Modified SDL_kitchensink image-subtitle renderer

`src/playback/kitsubimage_safe.c` is an altered, project-owned replacement for
SDL_kitchensink's `kitsubimage.c`. It retains the same public factory symbol
while adding bounded allocations, palette/index validation, correct FFmpeg
RGB32 palette conversion, safe callbacks, and repaired cleanup paths. It must
not be represented as the unmodified upstream source.

The upstream file is covered by the following MIT license:

> Copyright (c) 2018 Tuomas Virtanen
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.
