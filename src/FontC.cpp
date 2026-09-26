//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "FontC.h"
#include "FontSFT.h"
#include "FontSTB.h"
//-------------------------------------
#include <memory>
#include <new>

// The C API repeats these values so that C can use them as constants. The build fails if they drift apart.
//-------------------------------------
static_assert(FR_FONT_MAX_TEXTURE_SIZE     == MindShake::Font::kMaxTextureSize,     "FR_FONT_MAX_TEXTURE_SIZE must match Font::kMaxTextureSize");
static_assert(FR_FONT_MAX_ANTIALIAS_WEIGHT == MindShake::Font::kMaxAntialiasWeight, "FR_FONT_MAX_ANTIALIAS_WEIGHT must match Font::kMaxAntialiasWeight");

static_assert(FR_STATUS_OK                == int(MindShake::Font::EStatus::Ok),             "fr_status must match Font::EStatus");
static_assert(FR_STATUS_NOT_LOADED        == int(MindShake::Font::EStatus::NotLoaded),      "fr_status must match Font::EStatus");
static_assert(FR_STATUS_CANNOT_OPEN_FILE  == int(MindShake::Font::EStatus::CannotOpenFile), "fr_status must match Font::EStatus");
static_assert(FR_STATUS_CANNOT_READ_FILE  == int(MindShake::Font::EStatus::CannotReadFile), "fr_status must match Font::EStatus");
static_assert(FR_STATUS_INVALID_FONT      == int(MindShake::Font::EStatus::InvalidFont),    "fr_status must match Font::EStatus");
static_assert(FR_STATUS_OUT_OF_MEMORY     == int(MindShake::Font::EStatus::OutOfMemory),    "fr_status must match Font::EStatus");

static_assert(FR_FONT_TEXTURE_GROWTH_HEIGHT == int(MindShake::Font::ETextureGrowth::Height), "fr_font_texture_growth must match Font::ETextureGrowth");
static_assert(FR_FONT_TEXTURE_GROWTH_WIDTH  == int(MindShake::Font::ETextureGrowth::Width),  "fr_font_texture_growth must match Font::ETextureGrowth");
static_assert(FR_FONT_TEXTURE_GROWTH_BOTH   == int(MindShake::Font::ETextureGrowth::Both),   "fr_font_texture_growth must match Font::ETextureGrowth");

static_assert(FR_FONT_SIZE_MODE_LINE_HEIGHT == int(MindShake::Font::ESizeMode::LineHeight), "fr_font_size_mode must match Font::ESizeMode");
static_assert(FR_FONT_SIZE_MODE_EM_SIZE     == int(MindShake::Font::ESizeMode::EmSize),     "fr_font_size_mode must match Font::ESizeMode");

static_assert(FR_FONT_PACKING_LEVEL_BOTTOM_LEFT   == int(MindShake::Font::ELevelChoiceHeuristic::LevelBottomLeft),  "fr_font_packing_heuristic must match Font::ELevelChoiceHeuristic");
static_assert(FR_FONT_PACKING_LEVEL_MIN_WASTE_FIT == int(MindShake::Font::ELevelChoiceHeuristic::LevelMinWasteFit), "fr_font_packing_heuristic must match Font::ELevelChoiceHeuristic");

//-------------------------------------
struct fr_font {
    std::unique_ptr<MindShake::Font> value;
};

namespace {

    //---------------------------------
    fr_status
    ToStatus(MindShake::Font::EStatus status) {
        using EStatus = MindShake::Font::EStatus;

        switch(status) {
            case EStatus::Ok:             return FR_STATUS_OK;
            case EStatus::NotLoaded:      return FR_STATUS_NOT_LOADED;
            case EStatus::CannotOpenFile: return FR_STATUS_CANNOT_OPEN_FILE;
            case EStatus::CannotReadFile: return FR_STATUS_CANNOT_READ_FILE;
            case EStatus::InvalidFont:    return FR_STATUS_INVALID_FONT;
            case EStatus::OutOfMemory:    return FR_STATUS_OUT_OF_MEMORY;
        }

        return FR_STATUS_INTERNAL_ERROR;
    }

    //---------------------------------
    template <class TFunction>
    fr_status
    Invoke(fr_font *font, TFunction function) noexcept {
        if(font == nullptr || font->value == nullptr) {
            return FR_STATUS_INVALID_ARGUMENT;
        }

        try {
            function(*font->value);
            return FR_STATUS_OK;
        }
        catch(const std::bad_alloc &) {
            return FR_STATUS_OUT_OF_MEMORY;
        }
        catch(...) {
            return FR_STATUS_INTERNAL_ERROR;
        }
    }

} // namespace

extern "C" {

//-------------------------------------
fr_status
fr_font_create(const char *font_name, fr_font_backend backend, fr_font **out_font) {
    if(out_font == nullptr) {
        return FR_STATUS_INVALID_ARGUMENT;
    }
    *out_font = nullptr;

    if(font_name == nullptr) {
        return FR_STATUS_INVALID_ARGUMENT;
    }

    if(backend != FR_FONT_BACKEND_STB && backend != FR_FONT_BACKEND_SFT) {
        return FR_STATUS_INVALID_BACKEND;
    }

    try {
        std::unique_ptr<MindShake::Font> value;
        if(backend == FR_FONT_BACKEND_STB) {
            value = std::make_unique<MindShake::FontSTB>(font_name);
        }
        else {
            value = std::make_unique<MindShake::FontSFT>(font_name);
        }

        const fr_status status = ToStatus(value->GetStatus());
        if(status != FR_STATUS_OK) {
            return status;
        }

        auto font = std::make_unique<fr_font>();
        font->value = std::move(value);
        *out_font = font.release();
        return FR_STATUS_OK;
    }
    catch(const std::bad_alloc &) {
        return FR_STATUS_OUT_OF_MEMORY;
    }
    catch(...) {
        return FR_STATUS_INTERNAL_ERROR;
    }
}

//-------------------------------------
void
fr_font_destroy(fr_font *font) {
    delete font;
}

//-------------------------------------
fr_status
fr_font_reset(fr_font *font) {
    return Invoke(font, [](MindShake::Font &value) { value.Reset(); });
}

//-------------------------------------
const char *
fr_font_get_name(const fr_font *font) {
    return (font != nullptr && font->value != nullptr) ? font->value->GetFontName().c_str() : nullptr;
}

//-------------------------------------
const uint8_t *
fr_font_get_texture(const fr_font *font) {
    return (font != nullptr && font->value != nullptr) ? font->value->GetTexture() : nullptr;
}

//-------------------------------------
uint32_t
fr_font_get_texture_width(const fr_font *font) {
    return (font != nullptr && font->value != nullptr) ? font->value->GetTextureWidth() : 0;
}

//-------------------------------------
uint32_t
fr_font_get_texture_height(const fr_font *font) {
    return (font != nullptr && font->value != nullptr) ? font->value->GetTextureHeight() : 0;
}

//-------------------------------------
uint32_t
fr_font_get_used_texture_width(const fr_font *font) {
    return (font != nullptr && font->value != nullptr) ? font->value->GetUsedTextureWidth() : 0;
}

//-------------------------------------
uint32_t
fr_font_get_used_texture_height(const fr_font *font) {
    return (font != nullptr && font->value != nullptr) ? font->value->GetUsedTextureHeight() : 0;
}

//-------------------------------------
fr_status
fr_font_set_texture_growth(fr_font *font, fr_font_texture_growth growth) {
    MindShake::Font::ETextureGrowth value;
    switch(growth) {
        case FR_FONT_TEXTURE_GROWTH_HEIGHT: value = MindShake::Font::ETextureGrowth::Height; break;
        case FR_FONT_TEXTURE_GROWTH_WIDTH:  value = MindShake::Font::ETextureGrowth::Width;  break;
        case FR_FONT_TEXTURE_GROWTH_BOTH:   value = MindShake::Font::ETextureGrowth::Both;   break;
        default: return FR_STATUS_INVALID_ARGUMENT;
    }

    return Invoke(font, [value](MindShake::Font &font_value) { font_value.SetTextureGrowth(value); });
}

//-------------------------------------
fr_font_texture_growth
fr_font_get_texture_growth(const fr_font *font) {
    if(font == nullptr || font->value == nullptr) {
        return FR_FONT_TEXTURE_GROWTH_INVALID;
    }

    switch(font->value->GetTextureGrowth()) {
        case MindShake::Font::ETextureGrowth::Height: return FR_FONT_TEXTURE_GROWTH_HEIGHT;
        case MindShake::Font::ETextureGrowth::Width:  return FR_FONT_TEXTURE_GROWTH_WIDTH;
        case MindShake::Font::ETextureGrowth::Both:   return FR_FONT_TEXTURE_GROWTH_BOTH;
    }

    return FR_FONT_TEXTURE_GROWTH_INVALID;
}

//-------------------------------------
fr_status
fr_font_set_size_mode(fr_font *font, fr_font_size_mode mode) {
    MindShake::Font::ESizeMode value;
    switch(mode) {
        case FR_FONT_SIZE_MODE_LINE_HEIGHT: value = MindShake::Font::ESizeMode::LineHeight; break;
        case FR_FONT_SIZE_MODE_EM_SIZE:      value = MindShake::Font::ESizeMode::EmSize;     break;
        default: return FR_STATUS_INVALID_ARGUMENT;
    }

    return Invoke(font, [value](MindShake::Font &font_value) { font_value.SetSizeMode(value); });
}

//-------------------------------------
fr_font_size_mode
fr_font_get_size_mode(const fr_font *font) {
    if(font == nullptr || font->value == nullptr) {
        return FR_FONT_SIZE_MODE_INVALID;
    }

    switch(font->value->GetSizeMode()) {
        case MindShake::Font::ESizeMode::LineHeight: return FR_FONT_SIZE_MODE_LINE_HEIGHT;
        case MindShake::Font::ESizeMode::EmSize:     return FR_FONT_SIZE_MODE_EM_SIZE;
    }

    return FR_FONT_SIZE_MODE_INVALID;
}

//-------------------------------------
fr_status
fr_font_set_glyph_padding(fr_font *font, uint32_t padding) {
    if(font == nullptr || font->value == nullptr) {
        return FR_STATUS_INVALID_ARGUMENT;
    }

    try {
        return font->value->SetGlyphPadding(padding) ? FR_STATUS_OK : FR_STATUS_INVALID_ARGUMENT;
    }
    catch(const std::bad_alloc &) {
        return FR_STATUS_OUT_OF_MEMORY;
    }
    catch(...) {
        return FR_STATUS_INTERNAL_ERROR;
    }
}

//-------------------------------------
uint32_t
fr_font_get_glyph_padding(const fr_font *font) {
    return (font != nullptr && font->value != nullptr) ? font->value->GetGlyphPadding() : 0;
}

//-------------------------------------
fr_status
fr_font_set_packing_heuristic(fr_font *font, fr_font_packing_heuristic heuristic) {
    MindShake::Font::ELevelChoiceHeuristic value;
    switch(heuristic) {
        case FR_FONT_PACKING_LEVEL_BOTTOM_LEFT:
            value = MindShake::Font::ELevelChoiceHeuristic::LevelBottomLeft;
            break;
        case FR_FONT_PACKING_LEVEL_MIN_WASTE_FIT:
            value = MindShake::Font::ELevelChoiceHeuristic::LevelMinWasteFit;
            break;
        default:
            return FR_STATUS_INVALID_ARGUMENT;
    }

    return Invoke(font, [value](MindShake::Font &font_value) { font_value.SetPackingHeuristic(value); });
}

//-------------------------------------
fr_font_packing_heuristic
fr_font_get_packing_heuristic(const fr_font *font) {
    if(font == nullptr || font->value == nullptr) {
        return FR_FONT_PACKING_INVALID;
    }

    switch(font->value->GetPackingHeuristic()) {
        case MindShake::Font::ELevelChoiceHeuristic::LevelBottomLeft:
            return FR_FONT_PACKING_LEVEL_BOTTOM_LEFT;
        case MindShake::Font::ELevelChoiceHeuristic::LevelMinWasteFit:
            return FR_FONT_PACKING_LEVEL_MIN_WASTE_FIT;
    }

    return FR_FONT_PACKING_INVALID;
}

//-------------------------------------
fr_status
fr_font_draw_text(fr_font *font, const char *utf8, uint8_t text_height, uint32_t color,
                  uint32_t *dst, uint32_t dst_stride, int32_t pos_x, int32_t pos_y) {
    if(utf8 == nullptr || dst == nullptr || dst_stride == 0) {
        return FR_STATUS_INVALID_ARGUMENT;
    }

    return Invoke(font, [&](MindShake::Font &value) {
        value.DrawText(utf8, text_height, color, dst, dst_stride, pos_x, pos_y);
    });
}

//-------------------------------------
fr_status
fr_font_get_text_box(fr_font *font, const char *utf8, uint8_t text_height, fr_rect *rect) {
    if(rect == nullptr) {
        return FR_STATUS_INVALID_ARGUMENT;
    }

    *rect = fr_rect {};
    if(utf8 == nullptr) {
        return FR_STATUS_INVALID_ARGUMENT;
    }

    MindShake::Font::Rect value {};
    const fr_status status = Invoke(font, [&](MindShake::Font &font_value) {
        font_value.GetTextBox(utf8, text_height, &value);
    });
    if(status == FR_STATUS_OK) {
        rect->x      = value.x;
        rect->y      = value.y;
        rect->width  = value.width;
        rect->height = value.height;
    }
    return status;
}

//-------------------------------------
fr_status
fr_font_set_clipping(fr_font *font, int32_t left, int32_t top, int32_t right, int32_t bottom) {
    return Invoke(font, [=](MindShake::Font &value) { value.SetClipping(left, top, right, bottom); });
}

//-------------------------------------
fr_status
fr_font_set_antialias(fr_font *font, bool enabled) {
    return Invoke(font, [enabled](MindShake::Font &value) { value.SetAntialias(enabled); });
}

//-------------------------------------
bool
fr_font_get_antialias(const fr_font *font) {
    return font != nullptr && font->value != nullptr && font->value->GetAntialias();
}

//-------------------------------------
fr_status
fr_font_set_antialias_allow_ex(fr_font *font, bool enabled) {
    return Invoke(font, [enabled](MindShake::Font &value) { value.SetAntialiasAllowEx(enabled); });
}

//-------------------------------------
bool
fr_font_get_antialias_allow_ex(const fr_font *font) {
    return font != nullptr && font->value != nullptr && font->value->GetAntialiasAllowEx();
}

//-------------------------------------
fr_status
fr_font_set_antialias_weights(fr_font *font, int32_t center, int32_t border, int32_t corner) {
    if(font == nullptr || font->value == nullptr) {
        return FR_STATUS_INVALID_ARGUMENT;
    }

    try {
        return font->value->SetAntialiasWeights(center, border, corner) ? FR_STATUS_OK : FR_STATUS_INVALID_ARGUMENT;
    }
    catch(const std::bad_alloc &) {
        return FR_STATUS_OUT_OF_MEMORY;
    }
    catch(...) {
        return FR_STATUS_INTERNAL_ERROR;
    }
}

//-------------------------------------
int32_t
fr_font_get_antialias_center(const fr_font *font) {
    return (font != nullptr && font->value != nullptr) ? font->value->GetAntialiasCenter() : 0;
}

//-------------------------------------
int32_t
fr_font_get_antialias_border(const fr_font *font) {
    return (font != nullptr && font->value != nullptr) ? font->value->GetAntialiasBorder() : 0;
}

//-------------------------------------
int32_t
fr_font_get_antialias_corner(const fr_font *font) {
    return (font != nullptr && font->value != nullptr) ? font->value->GetAntialiasCorner() : 0;
}

} // extern "C"

