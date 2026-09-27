# FontRenderer

FontRenderer is a simple tool for rendering text from a ttf font into a buffer (with clipping).

It can also save the glyphs it renders and draw text with them later, without the font file and without a rasterizer (see [Baked fonts](#baked-fonts)), and it gives where each glyph goes, to draw the text with the GPU (see [Drawing with the GPU](#drawing-with-the-gpu)).

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

### Classes

- `FontBase` lays out and draws text with the glyphs a font already has. `DrawText`, `GetTextBox` and `GetGlyphQuads` are here.
- `Font` renders its glyphs from a font file the first time they are drawn. `FontSTB`, `FontSFT` and `FontFT` are its backends.
- `FontBaked` only reads the glyphs that a `Font` saved before. See [Baked fonts](#baked-fonts).

## Baked fonts

A `Font` can save the glyphs it has rendered, and `FontBaked` can draw text with them later, without the font file and without a rasterizer. This is useful on computers that are too slow to rasterize glyphs, like DOS computers, or to ship a program without the font and its rasterizer.

`FontBaked` cannot render new glyphs. It does not draw a code point or a height that was not saved.

```cpp
MindShake::FontSTB font("resources/Roboto-Regular.ttf");
font.SetAntialias(true);

// Render every glyph the program will draw, at every height it will use.
font.Preload(u8"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 .,:;!?¿¡áéíóúñÑ", 16);
font.Preload(u8"0123456789", 32);

// Look up the kerning of every pair of rendered glyphs. It can be very slow with many glyphs.
font.LoadAllKerningPairs();

font.SaveBaked("roboto.frb", "roboto.tga");
```

And later, maybe in another program:

```cpp
MindShake::FontBaked font("roboto.frb", "roboto.tga");
font.DrawText("Hello", 16, color32, bufferDest, bufferDestStride, posX, posY);
```

`Preload` packs all the glyphs of the text together, which fills the texture better than drawing them one by one. `SaveBaked` saves every glyph rendered so far, also the ones rendered by `DrawText` or `GetTextBox`. It only saves the kerning pairs already looked up, so call `LoadAllKerningPairs` first.

`SaveBaked` writes two files:

- The metrics file has the metrics of each height, the position of each glyph in the texture, and the kerning. It is a little-endian binary file, so it works the same on every platform.
- The texture is an 8-bit grayscale TGA compressed with RLE, which most image programs read. It only has the used area of the texture. `FontBaked` reads it compressed or not, with its rows from the top down or from the bottom up.

You can convert the texture to another format, like PNG, and decode it yourself. Then pass the pixels to `FontBaked(metricsFile, texture, width, height)`. The size must be the same. If the texture is only in the GPU, pass `nullptr`: `DrawText` then draws nothing, but `GetGlyphQuads` still works.

## Drawing with the GPU

`GetGlyphQuads` gives the glyphs that `DrawText` would draw: where each one goes, relative to the position of the text, and the area of the texture it uses. Draw a textured quad for each one. The clipping does not apply to the quads.

The packer can rotate a glyph to fit it better. A rotated glyph is stored transposed: the pixel (x, y) of the quad is the texel (textureRect.x + y, textureRect.y + x).

With `Font`, drawing or measuring text can render new glyphs and change the texture. `GetTextureVersion` changes every time the texels change, so compare it with the version you uploaded to know when to upload the texture again.

## Font Renderer external dependencies

For getting the font glyphs the following libraries are used:

- [stb_truetype](https://github.com/nothings/stb/blob/master/stb_truetype.h) (.ttf, .otf) :ok:
- [libschrift](https://github.com/tomolt/libschrift) (.ttf, and .otf with TrueType outlines: it cannot read CFF outlines) :ok:
- [FreeType](https://freetype.org/) (.ttf, .otf, and the other scalable formats it reads) :ok: (optional). `FontFT` also has hinting, a monochrome mode and stem darkening, which make small text sharper. See `FontFT.h`.

`FontBaked` does not need any of them: it only reads the files that a `Font` saved.

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

DOS support is minimal. DOS cannot map files, so the whole font is loaded into memory, and rasterizing TrueType glyphs is slow on computers of that time. On DOS, a [baked font](#baked-fonts) is a better choice: build only `FONTRENDERER_USE_BAKED`, and save the font on another computer. The example also needs long file names (`lfn = true` in DOSBox-X) to find its font file.

## How to use it

You have two options:

- Use CMake. Add FontRenderer with `add_subdirectory()` or `FetchContent`, and link against the `fontRenderer` target, as the examples do.
  These options choose what is built:

  |Option|Default|What it builds|
  |---|---|---|
  |`FONTRENDERER_USE_STB`|`ON`|The stb_truetype backend, `FontSTB`|
  |`FONTRENDERER_USE_LIBSCHRIFT`|`ON`|The libschrift backend, `FontSFT`|
  |`FONTRENDERER_USE_FREETYPE`|`OFF`|The FreeType backend, `FontFT`. CMake uses the FreeType installed in the system, and downloads it if there is none|
  |`FONTRENDERER_USE_BAKED`|`ON`|The backend that reads baked fonts, `FontBaked`|
  |`FONTRENDERER_BUILD_EXAMPLES`|`ON` only when FontRenderer is the main project|The examples, which show every backend that is on. They download MiniFB, unless the project that adds FontRenderer already has a `minifb` target|
  |`FONTRENDERER_BUILD_TESTS`|`ON` only when FontRenderer is the main project|The unit tests|

  At least one backend must be on. The library defines `FONTRENDERER_USE_STB`, `FONTRENDERER_USE_LIBSCHRIFT`, `FONTRENDERER_USE_FREETYPE` and `FONTRENDERER_USE_BAKED` for each backend that is on, also for the code that uses the library. The header of each backend, like `FontSTB.h`, stops the build if its macro is not defined.

  With only `FONTRENDERER_USE_BAKED`, the library does not build `Font`, which renders glyphs. The examples are not built, and the unit tests only test what does not need a font file.

- Select the library you want to use into your project (stb_truetype, libschrift, FreeType) and drop the following files in your project.
  Define the macro of each backend you choose, `FONTRENDERER_USE_STB`, `FONTRENDERER_USE_LIBSCHRIFT`, `FONTRENDERER_USE_FREETYPE` or `FONTRENDERER_USE_BAKED`, for every file you compile: the header of the backend and the C API need it.

  - FontBase.h
  - FontBase.cpp
  - MappedFile.h
  - MappedFile.cpp
  - SkylineBinPack.h
  - Tga.h
  - Tga.cpp
  - UTF8_Utils.h

  ---

  **If you choose libschrift, stb_truetype or FreeType**, which render glyphs, also:

  - Font.h
  - Font.cpp
  - GposKerning.h
  - GposKerning.cpp
  - SkylineBinPack.cpp

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

  **If you choose to use baked fonts**:

  - FontBaked.h
  - FontBaked.cpp

  ---

  **If you also want the C API**:

  - FontC.h
  - FontC.cpp, which only creates fonts with the backends whose macro is defined

## Licenses

FontRenderer is distributed under the Boost Software License 1.0, see [LICENSE](LICENSE).

The licenses of the third party code and fonts in this repository are in [LICENSES](LICENSES): stb_truetype, libschrift, doctest, and the Roboto and DejaVu fonts used by the examples and the tests.

CMake downloads MiniFB for the examples, and FreeType when `FONTRENDERER_USE_FREETYPE` is on and the system has none. They are not part of this repository and have their own licenses. If your program uses the FreeType backend, it includes FreeType, whose license (the FreeType License or the GPLv2, as you choose) asks you to credit FreeType in your documentation.

## Captures

The example shows every backend that is on side by side, the atlas of one of them, and the keys in the status bar. S bakes the font of the shown atlas: it saves `baked.frb` and `baked.tga` next to the example, reads them back with `FontBaked`, and draws a row with it. The baked row keeps the settings it was saved with, so the antialias keys do not change it.

|Capture|Settings|
|---|---|
|![No antialias](screenshots/capture.png)|No antialias|
|![Gaussian antialias](screenshots/captureAAin.png)|Antialias with Gaussian weights|
|![Gaussian antialias, extended](screenshots/captureAA.png)|Antialias with Gaussian weights, glyphs extended one pixel|
|![Mean antialias, extended](screenshots/captureAAm.png)|Antialias with mean weights, glyphs extended one pixel|
|![Clipping and atlas](screenshots/captureClip.png)|Clipping (the darkened area) and the atlas of stb_truetype|
|![Baked font](screenshots/captureBaked.png)|The font of stb_truetype baked with S: its row, drawn with `FontBaked`, and its atlas|
