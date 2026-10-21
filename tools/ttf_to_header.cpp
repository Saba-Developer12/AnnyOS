#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdio>

namespace {
constexpr unsigned kFirstCharacter = 32;
constexpr unsigned kCharacterCount = 95;
constexpr unsigned kMaximumRows = 18;
constexpr unsigned kMaximumWidth = 16;

struct Glyph {
    int left;
    int top;
    unsigned width;
    unsigned height;
    unsigned advance;
    unsigned short rows[kMaximumRows];
};

bool load_glyphs(FT_Library library, const char* font_path, Glyph* glyphs) {
    FT_Face face = nullptr;
    if (FT_New_Face(library, font_path, 0, &face) != 0 || FT_Set_Pixel_Sizes(face, 0, 15) != 0) {
        std::fprintf(stderr, "Cannot load TrueType font: %s\n", font_path);
        if (face != nullptr) FT_Done_Face(face);
        return false;
    }

    bool valid = true;
    for (unsigned index = 0; index < kCharacterCount; ++index) {
        const unsigned codepoint = kFirstCharacter + index;
        if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
            std::fprintf(stderr, "Font %s is missing ASCII character %u\n", font_path, codepoint);
            valid = false;
            break;
        }
        const FT_GlyphSlot slot = face->glyph;
        const FT_Bitmap& bitmap = slot->bitmap;
        Glyph& glyph = glyphs[index];
        glyph.left = slot->bitmap_left;
        glyph.top = slot->bitmap_top;
        glyph.width = bitmap.width;
        glyph.height = bitmap.rows;
        glyph.advance = static_cast<unsigned>((slot->advance.x + 32) >> 6);
        if (glyph.width > kMaximumWidth || glyph.height > kMaximumRows || bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
            std::fprintf(stderr, "Unsupported glyph bitmap in %s for ASCII character %u\n", font_path, codepoint);
            valid = false;
            break;
        }
        for (unsigned y = 0; y < glyph.height; ++y) {
            const unsigned char* row = bitmap.pitch >= 0
                ? bitmap.buffer + y * static_cast<unsigned>(bitmap.pitch)
                : bitmap.buffer + (glyph.height - 1 - y) * static_cast<unsigned>(-bitmap.pitch);
            unsigned short bits = 0;
            for (unsigned x = 0; x < glyph.width; ++x) {
                if (row[x] >= 96) bits |= static_cast<unsigned short>(1u << (glyph.width - 1 - x));
            }
            glyph.rows[y] = bits;
        }
    }
    FT_Done_Face(face);
    return valid;
}

void write_glyph_table(FILE* output, const char* symbol, const Glyph* glyphs) {
    std::fprintf(output, "inline constexpr Glyph %s[95] = {\n", symbol);
    for (unsigned index = 0; index < kCharacterCount; ++index) {
        const Glyph& glyph = glyphs[index];
        std::fprintf(output, "    {%d, %d, %u, %u, %u, {", glyph.left, glyph.top,
                     glyph.width, glyph.height, glyph.advance);
        for (unsigned y = 0; y < kMaximumRows; ++y) {
            std::fprintf(output, "%s0x%04x", y == 0 ? "" : ", ", glyph.rows[y]);
        }
        std::fprintf(output, "}},\n");
    }
    std::fprintf(output, "};\n\n");
}

bool write_header(const char* regular_path, const char* bold_path, const char* italic_path,
                  const char* bold_italic_path, const char* output_path) {
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) {
        std::fprintf(stderr, "Cannot initialize FreeType\n");
        return false;
    }

    Glyph regular[kCharacterCount]{};
    Glyph bold[kCharacterCount]{};
    Glyph italic[kCharacterCount]{};
    Glyph bold_italic[kCharacterCount]{};
    const bool valid = load_glyphs(library, regular_path, regular) &&
                       load_glyphs(library, bold_path, bold) &&
                       load_glyphs(library, italic_path, italic) &&
                       load_glyphs(library, bold_italic_path, bold_italic);
    FILE* output = valid ? std::fopen(output_path, "w") : nullptr;
    if (output == nullptr) {
        if (valid) std::perror(output_path);
        FT_Done_FreeType(library);
        return false;
    }
    std::fprintf(output, "#pragma once\n#include <stdint.h>\n\nnamespace annyos::ui {\n"
                         "struct Glyph { int8_t left, top; uint8_t width, height, advance; uint16_t rows[18]; };\n\n");
    write_glyph_table(output, "kSegoeUiGlyphs", regular);
    write_glyph_table(output, "kSegoeUiBoldGlyphs", bold);
    write_glyph_table(output, "kSegoeUiItalicGlyphs", italic);
    write_glyph_table(output, "kSegoeUiBoldItalicGlyphs", bold_italic);
    std::fprintf(output, "inline constexpr int kSegoeUiBaseline = 15;\n}\n");
    const bool write_ok = std::fclose(output) == 0;
    FT_Done_FreeType(library);
    return write_ok;
}
}

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "Usage: ttf_to_header regular.ttf bold.ttf italic.ttf bold-italic.ttf output.hpp\n");
        return 2;
    }
    return write_header(argv[1], argv[2], argv[3], argv[4], argv[5]) ? 0 : 1;
}
