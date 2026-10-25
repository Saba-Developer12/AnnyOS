#include "shell_state.hpp"
#include "annyos/font_data.hpp"

namespace annyos::shell::ui {

uint32_t append_text(char* destination, uint32_t length, uint32_t capacity, const char* text) {
    if (destination == nullptr || capacity == 0 || text == nullptr) return 0;
    if (length >= capacity) length = capacity - 1;
    while (length + 1 < capacity && *text != '\0') destination[length++] = *text++;
    destination[length] = '\0';
    return length;
}

void append_decimal(char* output, uint32_t* length, uint64_t value) {
    char digits[20];
    uint32_t count = 0;
    do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; }
    while (value != 0 && count < sizeof(digits));
    while (count > 0) output[(*length)++] = digits[--count];
    output[*length] = '\0';
}

uint32_t editor_line_end(const ConsoleState& console, uint32_t start) {
    while (start < console.editor_length && console.editor_buffer[start] != '\n') ++start;
    return start;
}

bool is_default_text_color(Color color) {
    return color.r == kDefaultTextColor.r && color.g == kDefaultTextColor.g &&
           color.b == kDefaultTextColor.b;
}

uint32_t pack_color(Color color, const annyos::FramebufferInfo& fb) {
    if (fb.format == annyos::PixelFormat::rgb_reserved_8) {
        return static_cast<uint32_t>(color.r) |
               (static_cast<uint32_t>(color.g) << 8) |
               (static_cast<uint32_t>(color.b) << 16);
    }
    if (fb.format == annyos::PixelFormat::bgr_reserved_8) {
        return static_cast<uint32_t>(color.b) |
               (static_cast<uint32_t>(color.g) << 8) |
               (static_cast<uint32_t>(color.r) << 16);
    }
    const auto pack_channel = [](uint8_t channel, uint32_t mask) {
        if (mask == 0) return uint32_t{0};
        const uint32_t shift = static_cast<uint32_t>(__builtin_ctz(mask));
        const uint64_t maximum = mask >> shift;
        const uint32_t scaled = static_cast<uint32_t>(
            (static_cast<uint64_t>(channel) * maximum + 127u) / 255u);
        return (scaled << shift) & mask;
    };
    return pack_channel(color.r, fb.mask.red) |
           pack_channel(color.g, fb.mask.green) |
           pack_channel(color.b, fb.mask.blue) |
           fb.mask.reserved;
}

void fill_rect_packed(const annyos::FramebufferInfo& fb, int32_t x, int32_t y,
                      int32_t width, int32_t height, uint32_t packed_color) {
    if (fb.address == nullptr || width <= 0 || height <= 0 ||
        fb.pixels_per_scanline < fb.width || fb.size_bytes < sizeof(uint32_t)) return;
    const int64_t x0 = x < 0 ? 0 : x;
    const int64_t y0 = y < 0 ? 0 : y;
    const int64_t requested_x1 = static_cast<int64_t>(x) + width;
    const int64_t requested_y1 = static_cast<int64_t>(y) + height;
    const int64_t x1 = requested_x1 > fb.width ? fb.width : requested_x1;
    const int64_t y1 = requested_y1 > fb.height ? fb.height : requested_y1;
    const uint64_t available_pixels = fb.size_bytes / sizeof(uint32_t);
    for (int64_t py = y0; py < y1; ++py) {
        const uint64_t row_offset = static_cast<uint64_t>(py) * fb.pixels_per_scanline;
        if (row_offset >= available_pixels) break;
        const uint64_t row_pixel_capacity = available_pixels - row_offset;
        const uint64_t bounded_x1 = static_cast<uint64_t>(x1) < row_pixel_capacity
            ? static_cast<uint64_t>(x1) : row_pixel_capacity;
        volatile uint32_t* row = fb.address + row_offset;
        for (int64_t px = x0; px < x1 && static_cast<uint64_t>(px) < bounded_x1; ++px) {
            row[px] = packed_color;
        }
    }
}

void fill_rect(const annyos::FramebufferInfo& fb, int32_t x, int32_t y,
               int32_t width, int32_t height, Color color) {
    fill_rect_packed(fb, x, y, width, height, pack_color(color, fb));
}

const annyos::ui::Glyph& get_glyph(char character, bool bold = false) {
    if (character < 32 || character > 126) character = '?';
    const unsigned index = static_cast<unsigned>(character - 32);
    return bold ? annyos::ui::kSegoeUiBoldGlyphs[index] : annyos::ui::kSegoeUiGlyphs[index];
}

int32_t draw_text(const annyos::FramebufferInfo& fb, int32_t x, int32_t y,
                  const char* text, Color color, bool bold) {
    if (text == nullptr) return x;
    const uint32_t packed = pack_color(color, fb);
    int32_t pen_x = x;
    while (*text != '\0') {
        const annyos::ui::Glyph& glyph = get_glyph(*text++, bold);
        const int32_t glyph_x = pen_x + glyph.left;
        const int32_t glyph_y = y + annyos::ui::kSegoeUiBaseline - glyph.top;
        for (uint32_t row = 0; row < glyph.height; ++row) {
            for (uint32_t column = 0; column < glyph.width; ++column) {
                if ((glyph.rows[row] & (1u << (glyph.width - 1 - column))) != 0) {
                    fill_rect_packed(fb, glyph_x + static_cast<int32_t>(column),
                                     glyph_y + static_cast<int32_t>(row), 1, 1, packed);
                }
            }
        }
        pen_x += glyph.advance;
    }
    return pen_x;
}


int32_t text_width(const char* text) {
    int32_t width = 0;
    if (text != nullptr) while (*text != '\0') width += get_glyph(*text++, false).advance;
    return width;
}

uint8_t glyph_advance(char character) { return get_glyph(character, false).advance; }

int32_t visible_rows(const annyos::FramebufferInfo& fb) {
    const int32_t rows = (static_cast<int32_t>(fb.height) - 2 * kMargin) / kLineHeight;
    return rows > 1 ? rows : 1;
}

void draw_editor(const annyos::FramebufferInfo& fb, const ConsoleState& console, const ThemeColors& colors) {
    const int32_t width = static_cast<int32_t>(fb.width);
    const int32_t height = static_cast<int32_t>(fb.height);
    const Color header_color{0, 0, 96};
    fill_rect(fb, 0, 0, width, height, colors.screen);
    fill_rect(fb, 0, 0, width, 48, header_color);
    fill_rect(fb, 0, height - 52, width, 52, header_color);
    draw_text(fb, kMargin, 12, "ANNYOS TEXT EDITOR", colors.title, true);
    const int32_t filename_x = draw_text(fb, kMargin + 225, 12, console.editor_filename,
                                         colors.input, true);
    char size_text[32] = "  ";
    uint32_t size_length = 2;
    append_decimal(size_text, &size_length, console.editor_length);
    append_text(size_text, size_length, sizeof(size_text), " / 4096 bytes");
    draw_text(fb, filename_x + 12, 12, size_text, colors.text);

    const int32_t body_top = 60;
    const int32_t footer_top = height - 52;
    const int32_t body_height = footer_top - body_top;
    const int32_t rows = body_height / kLineHeight > 0 ? body_height / kLineHeight : 1;
    uint32_t cursor_line = 0;
    for (uint32_t index = 0; index < console.editor_cursor; ++index) {
        if (console.editor_buffer[index] == '\n') ++cursor_line;
    }
    const uint32_t first_line = cursor_line >= static_cast<uint32_t>(rows)
        ? cursor_line - static_cast<uint32_t>(rows) + 1 : 0;
    uint32_t line_number = 0;
    uint32_t line_start = 0;
    int32_t row = 0;
    while (line_start <= console.editor_length && row < rows) {
        const uint32_t line_end = editor_line_end(console, line_start);
        if (line_number >= first_line) {
            const int32_t y = body_top + row * kLineHeight;
            char number[12] = "";
            uint32_t number_length = 0;
            append_decimal(number, &number_length, line_number + 1);
            draw_text(fb, kMargin, y, number, colors.prompt);

            uint32_t visible_start = line_start;
            if (line_number == cursor_line) {
                const uint32_t cursor_column = console.editor_cursor - line_start;
                const uint32_t horizontal_offset = cursor_column >= kInputCapacity - 4
                    ? cursor_column - (kInputCapacity - 5) : 0;
                visible_start += horizontal_offset;
            }
            char visible[kInputCapacity] = "";
            char cursor_prefix[kInputCapacity] = "";
            uint32_t visible_length = 0;
            uint32_t prefix_length = 0;
            for (uint32_t index = visible_start;
                 index < line_end && visible_length + 1 < sizeof(visible); ++index) {
                const char character = console.editor_buffer[index] == '\t'
                    ? ' ' : static_cast<char>(console.editor_buffer[index]);
                visible[visible_length++] = character;
                if (line_number == cursor_line && index < console.editor_cursor) {
                    cursor_prefix[prefix_length++] = character;
                }
            }
            visible[visible_length] = '\0';
            cursor_prefix[prefix_length] = '\0';
            const int32_t text_x = kMargin + 38;
            draw_text(fb, text_x, y, visible, colors.text);
            if (line_number == cursor_line) {
                const int32_t cursor_x = text_x + text_width(cursor_prefix);
                fill_rect(fb, cursor_x, y + 18, 8, 2, colors.input);
            }
            ++row;
        }
        if (line_end == console.editor_length) break;
        line_start = line_end + 1;
        ++line_number;
    }

    draw_text(fb, kMargin, footer_top + 5,
              "F2 Save     ESC Cancel     Arrows Move     HOME/END Line",
              colors.prompt, true);
    draw_text(fb, kMargin, footer_top + 27,
              "ENTER New line     BACKSPACE/Delete Edit text",
              colors.text);
    if (console.editor_status[0] != '\0') {
        const int32_t status_x = text_width("F2 Save     ESC Cancel     Arrows Move     HOME/END Line") +
                                 2 * kMargin;
        draw_text(fb, status_x, footer_top + 5, console.editor_status, colors.title);
    }
}

void draw_prompt(const annyos::FramebufferInfo& fb, const ConsoleState& console,
                 const WorkingDirectory& working_directory, int32_t row, const ThemeColors& colors) {
    const int32_t y = kMargin + row * kLineHeight;
    fill_rect(fb, 0, y, static_cast<int32_t>(fb.width), kLineHeight, colors.screen);
    const int32_t path_x = draw_text(fb, kMargin, y, working_directory.path, colors.prompt);
    const int32_t input_x = draw_text(fb, path_x, y, "> ", colors.prompt);
    const int32_t cursor_x = draw_text(fb, input_x, y, console.input, colors.input);
    fill_rect(fb, cursor_x, y + 18, 8, 2, colors.input);
}

void redraw_history(const annyos::FramebufferInfo& fb, const ConsoleState& console,
                    const WorkingDirectory& working_directory, const ThemeColors& colors) {
    if (console.editor_active) {
        draw_editor(fb, console, colors);
        return;
    }
    const int32_t rows = visible_rows(fb);
    fill_rect(fb, 0, 0, static_cast<int32_t>(fb.width), static_cast<int32_t>(fb.height), colors.screen);
    const int32_t history_rows = rows - 1;
    const uint32_t visible_history = console.history_count < static_cast<uint32_t>(history_rows)
        ? console.history_count : static_cast<uint32_t>(history_rows);
    const uint32_t first_line = console.history_count - visible_history;
    uint32_t visible_index = 0;
    for (uint32_t line = first_line; line < console.history_count; ++line, ++visible_index) {
        const int32_t y = kMargin + static_cast<int32_t>(visible_index) * kLineHeight;
        const uint8_t accent_length = console.history_accent_length[line];
        if (console.history_tabular[line]) {
            char fields[3][24]{};
            uint32_t field = 0;
            uint32_t field_length = 0;
            for (const char* cursor = console.history[line]; *cursor != '\0'; ++cursor) {
                if (*cursor == '\t') {
                    if (field < 2) ++field;
                    field_length = 0;
                } else if (field < 3 && field_length + 1 < sizeof(fields[field])) {
                    fields[field][field_length++] = *cursor;
                }
            }
            draw_text(fb, kMargin, y, fields[0], colors.text, console.history_bold[line]);
            draw_text(fb, kMargin + kDirectoryTypeOffset, y, fields[1], colors.prompt,
                      console.history_bold[line]);
            draw_text(fb, kMargin + kDirectorySizeOffset, y, fields[2], colors.text,
                      console.history_bold[line]);
        } else if (accent_length > 0) {
            char accent[kLineCapacity];
            uint32_t index = 0;
            while (index < accent_length && index + 1 < sizeof(accent)) {
                accent[index] = console.history[line][index];
                ++index;
            }
            accent[index] = '\0';
            const int32_t body_x = draw_text(fb, kMargin, y, accent, colors.prompt, true);
            if (console.history_columned[line]) {
                draw_text(fb, kMargin + kHelpDescriptionOffset, y,
                          console.history[line] + 16, colors.text);
            } else {
                draw_text(fb, body_x, y, console.history[line] + accent_length, colors.text);
            }
        } else {
            const Color color = colors.monochrome ? colors.text :
                (is_default_text_color(console.history_color[line])
                    ? colors.text : console.history_color[line]);
            draw_text(fb, kMargin, y, console.history[line], color,
                      console.history_bold[line]);
        }
    }
    draw_prompt(fb, console, working_directory, static_cast<int32_t>(visible_history), colors);
}

void redraw_prompt(const annyos::FramebufferInfo& fb, const ConsoleState& console,
                   const WorkingDirectory& working_directory, const ThemeColors& colors) {
    if (console.editor_active) {
        draw_editor(fb, console, colors);
        return;
    }
    const int32_t history_rows = visible_rows(fb) - 1;
    const uint32_t visible_history = console.history_count < static_cast<uint32_t>(history_rows)
        ? console.history_count : static_cast<uint32_t>(history_rows);
    draw_prompt(fb, console, working_directory, static_cast<int32_t>(visible_history), colors);
}

} // namespace annyos::shell::ui
