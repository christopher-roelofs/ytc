# Third-Party Notices

YTC ("Your Tube Client") is licensed under the PolyForm Noncommercial License
1.0.0 (see `LICENSE`). That license covers YTC's own source code **only**.

YTC incorporates and/or redistributes the third-party components listed below.
Each remains under its own license, which is **not** superseded by YTC's license.
The permissive and LGPL terms here impose no non-commercial restriction; YTC's
own non-commercial terms apply to YTC's code, not to these components.

---

## Bundled at runtime (shipped in the PortMaster zip, `ytc/libs.aarch64/`)

These are dynamically loaded shared libraries. They are shipped as separate,
unmodified `.so` files (dynamic linking); YTC's code links against their public
APIs. Under the LGPL this permits YTC's own code to carry a different license,
provided these libraries remain replaceable and their notices are preserved.

### libmpv (`libmpv.so.2`)
- Project: mpv — https://mpv.io/  /  https://github.com/mpv-player/mpv
- License: GNU Lesser General Public License, version 2.1 or later (LGPL-2.1-or-later).
  (mpv can also be built under GPL when GPL-only components are enabled; the
  build bundled here is the LGPL, render-only, software-decode configuration.)
- Full text: https://www.gnu.org/licenses/lgpl-2.1.html

### FFmpeg (`libavcodec.so.60`, `libavformat.so.60`, `libavfilter.so.9`,
`libavutil.so.58`, `libswscale.so.7`, `libswresample.so.4`)
- Project: FFmpeg — https://ffmpeg.org/  /  https://github.com/FFmpeg/FFmpeg
- License: GNU Lesser General Public License, version 2.1 or later
  (LGPL-2.1-or-later) for the configuration bundled here. FFmpeg contains
  optional GPL-only components; those are not enabled in this build.
- Full text: https://www.gnu.org/licenses/lgpl-2.1.html

> To exercise your LGPL rights, you may replace the bundled libmpv/FFmpeg
> shared objects in `ytc/libs.aarch64/` with your own compatible builds.

---

## Bundled data assets (shipped in the PortMaster zip, `ytc/data/`)

### DejaVu Sans (`DejaVuSans.ttf`)
- Project: DejaVu Fonts — https://dejavu-fonts.github.io/
- License: DejaVu Fonts License (a permissive Bitstream Vera–derived license).
  Free to use, embed, and redistribute; the font software may not be sold on its
  own. Full text: https://dejavu-fonts.github.io/License.html

### SDL_GameControllerDB (`gamecontrollerdb.txt`)
- Project: https://github.com/mdqinc/SDL_GameControllerDB
- License: zlib/libpng license (same terms as SDL). Permissive.
- Full text: https://opensource.org/license/zlib

---

## Bundled in source (`third_party/`, compiled into the binary)

### nlohmann/json (`json.hpp`)
- Project: https://github.com/nlohmann/json
- License: MIT License.
- Copyright (c) 2013–2025 Niels Lohmann.

### stb single-file libraries (`stb_image.h`, `stb_image_write.h`, `stb_truetype.h`)
- Project: https://github.com/nothings/stb
- License: dual-licensed — Public Domain (Unlicense) **or** MIT License, at your option.
- Author: Sean Barrett.

---

## MIT License (applies to nlohmann/json and, at your option, the stb libraries)

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
