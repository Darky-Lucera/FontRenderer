# FontRenderer

FontRenderer is a simple tool for rendering text from a ttf font into a buffer (with clipping).

As an example I have used the Mini Frame Buffer (MiniFB) library to create a window and render some text inside.

## API

The API is simple:

```cpp
const char *text = "Hello World!";
uint32_t   fontSize = 32;
uint32_t   color32 = 0xffff0000;   // ARGB. With alpha 0 the text is invisible

// Load a font
MindShake::FontSTB font("resources/Roboto-Regular.ttf");

// Set the clipping (optional, but without it the user has the responsibility to fit the text inside the buffer)
font.SetClipping(left, top, right, bottom);

// Enable antialias
font.SetAntialias(true);
font.SetAntialiasWeights(20, 4, 1); // Gaussian
font.SetAntialiasWeights(1, 1, 1);  // Mean

// Allow antialias to extend one border pixel per glyph
font.SetAntialiasAllowEx(true);

// In case you need the dimensions of the future rendered text. For instance to horizontal align text...
Rect rect;
font.GetTextBox(text, fontSize, &rect);

// Draw a colored text of height fontSize in your buffer at pos (posX, posY)
font.DrawText(text, fontSize, color32, bufferDest, bufferDestStride, posX, posY);
```

**Note**: Each glyph is rendered only once per fontSize, so changing any antialias setting discards the glyphs already rendered.

## Font Renderer external dependencies

For getting the font glyphs the following libraries are used:

- [stb_truetype](https://github.com/nothings/stb/blob/master/stb_truetype.h) (.ttf, .otf) :ok:
- [libschrift](https://github.com/tomolt/libschrift) (.ttf, and .otf with TrueType outlines: it cannot read CFF outlines) :ok:
- [FreeType](https://freetype.org/) (.ttf, .otf, and the other scalable formats it reads) :ok: (optional). `FontFT` also has hinting, a monochrome mode and stem darkening, which make small text sharper. See `FontFT.h`.

**Note**: The copy of libschrift in `src/external/libschrift` is version 0.10.2 with some changes. FontSFT needs them, so use this copy and not the upstream one:

- `sft_unitsPerEm()` returns the units per em of the font.
- `SFT_GMetrics::xOffset` gives the horizontal position of the image that `sft_render` produces. It can differ from the left side bearing.
- Extra bounds checks for the pairs of the `kern` table and for the groups of the format 12 and 13 `cmap` subtables.
- The rasterizer keeps its cells on the heap. Upstream reserves 256 KB of stack for them on every glyph, which overflows the default 64 KB stack of Emscripten.
- It builds with DJGPP (DOS). `getu32` has the same return type in its declaration and in its definition, and `sft_loadfile` always fails because DOS cannot map files. FontSFT uses `sft_loadmem`, so this does not affect it.

## Kerning

Every backend reads the kerning of pairs of glyphs from the `kern` feature of the OpenType `GPOS` table with `GposKerning`. stb_truetype only reads part of `GPOS`, and libschrift does not read it. `GposKerning` reads pair adjustments in both formats and with any value format, also inside extension lookups, in `GPOS` 1.0 and 1.1. As in HarfBuzz, a font without that kerning uses its `kern` table instead.

FontRenderer does not know the script of the text, and a font can kern the same pair differently in each script. `GposKerning` uses the first script whose kerning has the pair: Latin, then the default script, then the rest in the order of the font.

Kerning that depends on more than two glyphs needs a text shaper like HarfBuzz, so FontRenderer does not apply it.

## Platforms

Tested on Windows, Linux, the web (Emscripten) and DOS (DJGPP, in DOSBox-X). It also builds for Android. macOS and iOS use the same POSIX code, but they are not tested.

DOS support is minimal. DOS cannot map files, so the whole font is loaded into memory, and rasterizing TrueType glyphs is slow on computers of that time. On DOS, a prerendered bitmap font is a better choice. The example also needs long file names (`lfn = true` in DOSBox-X) to find its font file.

## How to use it

You have two options:

- Use CMake. Add FontRenderer with `add_subdirectory()` or `FetchContent`, and link against the `fontRenderer` target, as the examples do.
  These options choose what is built:

  |Option|Default|What it builds|
  |---|---|---|
  |`FONTRENDERER_USE_STB`|`ON`|The stb_truetype backend, `FontSTB`|
  |`FONTRENDERER_USE_LIBSCHRIFT`|`ON`|The libschrift backend, `FontSFT`|
  |`FONTRENDERER_USE_FREETYPE`|`OFF`|The FreeType backend, `FontFT`. CMake uses the FreeType installed in the system, and downloads it if there is none|
  |`FONTRENDERER_BUILD_EXAMPLES`|`ON` only when FontRenderer is the main project|The examples, which show every backend that is on. They download MiniFB, unless the project that adds FontRenderer already has a `minifb` target|
  |`FONTRENDERER_BUILD_TESTS`|`ON` only when FontRenderer is the main project|The unit tests|

  At least one backend must be on. The library defines `FONTRENDERER_USE_STB`, `FONTRENDERER_USE_LIBSCHRIFT` and `FONTRENDERER_USE_FREETYPE` for each backend that is on, also for the code that uses the library. The header of each backend, like `FontSTB.h`, stops the build if its macro is not defined.

- Select the library you want to use into your project (stb_truetype, libschrift, FreeType) and drop the following files in your project.
  Define the macro of each backend you choose, `FONTRENDERER_USE_STB`, `FONTRENDERER_USE_LIBSCHRIFT` or `FONTRENDERER_USE_FREETYPE`, for every file you compile: the header of the backend and the C API need it.

  - Font.h
  - Font.cpp
  - GposKerning.h
  - GposKerning.cpp
  - MappedFile.h
  - MappedFile.cpp
  - SkylineBinPack.h
  - SkylineBinPack.cpp
  - UTF8_Utils.h

  ---

  **If you choose to use libschrift**:

  - FontSFT.h
  - FontSFT.cpp
  - schrift.h (the modified copy, see above)
  - schrift.c (the modified copy, see above)

  ---

  **If you choose to use stb_truetype**:

  - FontSTB.h
  - FontSTB.cpp
  - stb_truetype.h

  ---

  **If you choose to use FreeType**:

  - FontFT.h
  - FontFT.cpp
  - FreeType itself, which you build and link on your own

  ---

  **If you also want the C API**:

  - FontC.h
  - FontC.cpp, which only creates fonts with the backends whose macro is defined

## Licenses

FontRenderer is distributed under the Boost Software License 1.0, see [LICENSE](LICENSE).

The licenses of the third party code and fonts in this repository are in [LICENSES](LICENSES): stb_truetype, libschrift, doctest, and the Roboto and DejaVu fonts used by the examples and the tests.

CMake downloads MiniFB for the examples, and FreeType when `FONTRENDERER_USE_FREETYPE` is on and the system has none. They are not part of this repository and have their own licenses. If your program uses the FreeType backend, it includes FreeType, whose license (the FreeType License or the GPLv2, as you choose) asks you to credit FreeType in your documentation.

## Captures

The example shows every backend that is on side by side, the atlas of one of them, and the keys in the status bar.

|Capture|Settings|
|---|---|
|![No antialias](screenshots/capture.png)|No antialias|
|![Gaussian antialias](screenshots/captureAAin.png)|Antialias with Gaussian weights|
|![Gaussian antialias, extended](screenshots/captureAA.png)|Antialias with Gaussian weights, glyphs extended one pixel|
|![Mean antialias, extended](screenshots/captureAAm.png)|Antialias with mean weights, glyphs extended one pixel|
|![Clipping and atlas](screenshots/captureClip.png)|Clipping (the darkened area) and the atlas of stb_truetype|
