//-----------------------------------------------------------------------------
// Copyright (C) 2021 Carlos Aragonés
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt
//-----------------------------------------------------------------------------

#include "FontC.h"
#if defined(FONTRENDERER_USE_FREETYPE)
#include "FontFT.h"
#endif
#if defined(FONTRENDERER_USE_LIBSCHRIFT)
#include "FontSFT.h"
#endif
#if defined(FONTRENDERER_USE_STB)
#include "FontSTB.h"
#endif
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

#if defined(FONTRENDERER_USE_FREETYPE)
static_assert(FR_FONT_FT_HINTING_NONE   == int(MindShake::FontFT::EHinting::None),   "fr_font_ft_hinting must match FontFT::EHinting");
static_assert(FR_FONT_FT_HINTING_LIGHT  == int(MindShake::FontFT::EHinting::Light),  "fr_font_ft_hinting must match FontFT::EHinting");
static_assert(FR_FONT_FT_HINTING_NORMAL == int(MindShake::FontFT::EHinting::Normal), "fr_font_ft_hinting must match FontFT::EHinting");
static_assert(FR_FONT_FT_HINTING_AUTO   == int(MindShake::FontFT::EHinting::Auto),   "fr_font_ft_hinting must match FontFT::EHinting");
#endif

//-------------------------------------
struct fr_font {
    std::unique_ptr<MindShake::Font> value;
#if defined(FONTRENDERER_USE_FREETYPE)
    MindShake::FontFT                *freeType {};     // The same font as value, when it uses FreeType
#endif
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

#if defined(FONTRENDERER_USE_FREETYPE)
    //---------------------------------
    const MindShake::FontFT *
    GetFreeType(const fr_font *font) {
        return (font != nullptr && font->value != nullptr) ? font->freeType : nullptr;
    }

    //---------------------------------
    template <class TFunction>
    fr_status
    InvokeFreeType(fr_font *font, TFunction function) noexcept {
        if(font == nullptr || font->value == nullptr) {
            return FR_STATUS_INVALID_ARGUMENT;
        }
        if(font->freeType == nullptr) {
            return FR_STATUS_INVALID_BACKEND;
        }

        return Invoke(font, [font, &function](MindShake::Font &) { function(*font->freeType); });
    }
#else
    //---------------------------------
    fr_status
    RejectFreeType(const fr_font *font) {
        return (font == nullptr || font->value == nullptr) ? FR_STATUS_INVALID_ARGUMENT : FR_STATUS_INVALID_BACKEND;
    }
#endif

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

    try {
        std::unique_ptr<MindShake::Font> value;
#if defined(FONTRENDERER_USE_FREETYPE)
        MindShake::FontFT                *freeType = nullptr;
#endif
        switch(backend) {
#if defined(FONTRENDERER_USE_STB)
            case FR_FONT_BACKEND_STB:
                value = std::make_unique<MindShake::FontSTB>(font_name);
                break;
#endif
#if defined(FONTRENDERER_USE_LIBSCHRIFT)
            case FR_FONT_BACKEND_SFT:
                value = std::make_unique<MindShake::FontSFT>(font_name);
                break;
#endif
#if defined(FONTRENDERER_USE_FREETYPE)
            case FR_FONT_BACKEND_FT: {
                auto font = std::make_unique<MindShake::FontFT>(font_name);
                freeType  = font.get();
                value     = std::move(font);
                break;
            }
#endif
            default:
                return FR_STATUS_INVALID_BACKEND;
        }

        const fr_status status = ToStatus(value->GetStatus());
        if(status != FR_STATUS_OK) {
            return status;
        }

        auto font = std::make_unique<fr_font>();
        font->value = std::move(value);
#if defined(FONTRENDERER_USE_FREETYPE)
        font->freeType = freeType;
#endif
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

//-------------------------------------
fr_status
fr_font_ft_set_hinting(fr_font *font, fr_font_ft_hinting hinting) {
#if defined(FONTRENDERER_USE_FREETYPE)
    MindShake::FontFT::EHinting value;
    switch(hinting) {
        case FR_FONT_FT_HINTING_NONE:   value = MindShake::FontFT::EHinting::None;   break;
        case FR_FONT_FT_HINTING_LIGHT:  value = MindShake::FontFT::EHinting::Light;  break;
        case FR_FONT_FT_HINTING_NORMAL: value = MindShake::FontFT::EHinting::Normal; break;
        case FR_FONT_FT_HINTING_AUTO:   value = MindShake::FontFT::EHinting::Auto;   break;
        default: return FR_STATUS_INVALID_ARGUMENT;
    }

    return InvokeFreeType(font, [value](MindShake::FontFT &freeType) { freeType.SetHinting(value); });
#else
    (void) hinting;
    return RejectFreeType(font);
#endif
}

//-------------------------------------
fr_font_ft_hinting
fr_font_ft_get_hinting(const fr_font *font) {
#if defined(FONTRENDERER_USE_FREETYPE)
    const MindShake::FontFT *freeType = GetFreeType(font);
    if(freeType == nullptr) {
        return FR_FONT_FT_HINTING_INVALID;
    }

    switch(freeType->GetHinting()) {
        case MindShake::FontFT::EHinting::None:   return FR_FONT_FT_HINTING_NONE;
        case MindShake::FontFT::EHinting::Light:  return FR_FONT_FT_HINTING_LIGHT;
        case MindShake::FontFT::EHinting::Normal: return FR_FONT_FT_HINTING_NORMAL;
        case MindShake::FontFT::EHinting::Auto:   return FR_FONT_FT_HINTING_AUTO;
    }
#else
    (void) font;
#endif
    return FR_FONT_FT_HINTING_INVALID;
}

//-------------------------------------
fr_status
fr_font_ft_set_monochrome(fr_font *font, bool enabled) {
#if defined(FONTRENDERER_USE_FREETYPE)
    return InvokeFreeType(font, [enabled](MindShake::FontFT &freeType) { freeType.SetMonochrome(enabled); });
#else
    (void) enabled;
    return RejectFreeType(font);
#endif
}

//-------------------------------------
bool
fr_font_ft_get_monochrome(const fr_font *font) {
#if defined(FONTRENDERER_USE_FREETYPE)
    const MindShake::FontFT *freeType = GetFreeType(font);
    return freeType != nullptr && freeType->GetMonochrome();
#else
    (void) font;
    return false;
#endif
}

//-------------------------------------
fr_status
fr_font_ft_set_stem_darkening(fr_font *font, bool enabled) {
#if defined(FONTRENDERER_USE_FREETYPE)
    return InvokeFreeType(font, [enabled](MindShake::FontFT &freeType) { freeType.SetStemDarkening(enabled); });
#else
    (void) enabled;
    return RejectFreeType(font);
#endif
}

//-------------------------------------
bool
fr_font_ft_get_stem_darkening(const fr_font *font) {
#if defined(FONTRENDERER_USE_FREETYPE)
    const MindShake::FontFT *freeType = GetFreeType(font);
    return freeType != nullptr && freeType->GetStemDarkening();
#else
    (void) font;
    return false;
#endif
}

} // extern "C"

