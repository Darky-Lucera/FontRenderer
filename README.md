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

// Set the clipping (optional, but without it the text must fit inside the buffer)
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

- [stb_truetype](https://github.com/nothings/stb/blob/master/stb_truetype.h) (.ttf) :ok:
- [libschrift](https://github.com/tomolt/libschrift) (.ttf, .otf) :ok:
- [FreeType](https://freetype.org/) (not yet) ❌
- [Harfbuzz](https://github.com/harfbuzz/harfbuzz) (not yet) ❌

**Note**: The copy of libschrift in `src/external/libschrift` is version 0.10.2 with some changes. FontSFT needs them, so use this copy and not the upstream one:

- `sft_unitsPerEm()` returns the units per em of the font.
- `SFT_GMetrics::xOffset` gives the horizontal position of the image that `sft_render` produces. It can differ from the left side bearing.
- Extra bounds checks for the pairs of the `kern` table and for the groups of the format 12 and 13 `cmap` subtables.
- The rasterizer keeps its cells on the heap. Upstream reserves 256 KB of stack for them on every glyph, which overflows the default 64 KB stack of Emscripten.
- It builds with DJGPP (DOS). `getu32` has the same return type in its declaration and in its definition, and `sft_loadfile` always fails because DOS cannot map files. FontSFT uses `sft_loadmem`, so this does not affect it.

## Platforms

Tested on Windows, Linux, the web (Emscripten) and DOS (DJGPP, in DOSBox-X). It also builds for Android. macOS and iOS use the same POSIX code, but they are not tested.

DOS support is minimal. DOS cannot map files, so the whole font is loaded into memory, and rasterizing TrueType glyphs is slow on computers of that time. On DOS, a prerendered bitmap font is a better choice. The example also needs long file names (`lfn = true` in DOSBox-X) to find its font file.

## How to use it

You have two options:

- Use CMake and add_subdirectory() where you put fontRenderer as an external dependency.
  Then link against fontRenderer lib as exampleRender does.

- Select the library you want to use into your project (stb_truetype, libschrift,  ~~FreeType~~) and drop the following files in your project:

  - Font.h
  - Font.cpp
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

## Licenses

FontRenderer is distributed under the Boost Software License 1.0, see [LICENSE](LICENSE).

The licenses of the third party code and fonts in this repository are in [LICENSES](LICENSES): stb_truetype, libschrift, MiniFB, doctest, and the Roboto and DejaVu fonts used by the example and the tests.

## Captures

The example shows both backends side by side, the atlas of one of them, and the keys in the status bar.

|Capture|Settings|
|---|---|
|![No antialias](screenshots/capture.png)|No antialias|
|![Gaussian antialias](screenshots/captureAAin.png)|Antialias with Gaussian weights|
|![Gaussian antialias, extended](screenshots/captureAA.png)|Antialias with Gaussian weights, glyphs extended one pixel|
|![Mean antialias, extended](screenshots/captureAAm.png)|Antialias with mean weights, glyphs extended one pixel|
|![Clipping and atlas](screenshots/captureClip.png)|Clipping (the darkened area) and the atlas of stb_truetype|
