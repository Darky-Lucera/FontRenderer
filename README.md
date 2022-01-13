# FontRenderer

FontRenderer is a simple tool for rendering text from a ttf font into a buffer (with clipping).

As an example I have used the Mini Frame Buffer (MiniFB) library to create a window and render some text inside.

# API

The APi is simple:
```cpp
const char text = "Hello World!";
uint32_t   fontSize = 32;
uint32_t   color32 = 0xff0000;

// Load a font
MindShake::FontSTB font("resources/Roboto-Regular.ttf");

// Set the clipping (optional)
font.SetClipping(left, top, right, bottom);

// Enable antialias
font.SetAntialias(true);
font.SetAntialiasWeights(20, 4, 1); // Gausian
font.SetAntialiasWeights(1, 1, 1);  // Mean

// Allow antialias to extend one border pixel per glyph
font.SetAntialiasAllowEx(true);

// In case you need the dimensions of the future rendered text. For instance to horizontal align text...
Rect rect;
font.GetTextBox(text, fontSize, &rect);

// Draw a colored text of height fontSize in your buffer at pos (posX, posY)
font.DrawText(text, fontSize, color32, bufferDest, bufferDestStride, posX, posY);
```

**Note:** As we are rendering only once per glyph per fontSize, user must configure Antialias params at beginning.

# Font Renderer external dependencies

For getting the font glyphs the following libraries are used:
 * [stb_truetype](https://github.com/nothings/stb/blob/master/stb_truetype.h) (.ttf) :ok:
 * [libschrift](https://github.com/tomolt/libschrift) (.ttf, .otf) :ok:
 * [FreeType](https://freetype.org/) (not yet) ❌

# How to use it

You have two options:
- Use CMake and add_subdirectory() where you put fontRenderer as an external dependency.
  Then link against fontRenderer lib as exampleRender does.

- Select the library you want to use into your project (stb_truetype, libschrift,  ~~FreeType~~) and drop the following files in your project:

  - Font.h
  - Font.cpp
  - SkylineBinPack.h
  - SkylineBinPack.cpp
  - UTF8_Utils.h
  ---
  **If you choose to use libschrift**
  - FontSFT.h
  - FontSFT.cpp
  - schrift.h
  - schrift.c
  ---
  **If you choose to use stb_truetype**
  - FontSTB.h
  - FontSTB.cpp
  - stb_truetype.h

# Captures

|Normal|AntiAlias|
|---|---|
|![](screenshots/capture.png)|NO|
|![](screenshots/captureAAin.png)|AA Gausian|
|![](screenshots/captureAA.png)|AA Gausian (allow font extension)|
|![](screenshots/captureAAm.png)|AA Mean (allow font extension)|
