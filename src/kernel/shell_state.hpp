#pragma once

#include "annyos/boot_info.hpp"
#include "annyos/fat32.hpp"
#include "annyos/ps2_keyboard.hpp"

namespace annyos::shell {

struct Color { uint8_t r, g, b; };

inline constexpr Color kDefaultScreenColor{0, 0, 170};
inline constexpr Color kDefaultTextColor{220, 220, 220};
inline constexpr Color kDefaultPromptColor{85, 255, 255};
inline constexpr Color kDefaultInputColor{255, 255, 255};
inline constexpr Color kDefaultHelpTitleColor{255, 220, 120};

inline constexpr int32_t kHelpDescriptionOffset = 128;
inline constexpr int32_t kDirectoryTypeOffset = 120;
inline constexpr int32_t kDirectorySizeOffset = 195;
inline constexpr int32_t kLineHeight = 22;
inline constexpr int32_t kMargin = 28;
inline constexpr uint32_t kHistoryCapacity = 32;
inline constexpr uint32_t kLineCapacity = 100;
inline constexpr uint32_t kInputCapacity = 88;
inline constexpr uint32_t kWorkingDirectoryDepth = 12;
inline constexpr uint32_t kWorkingDirectoryPathCapacity = 80;
inline constexpr uint32_t kFileBufferCapacity = 4096;

struct ConsoleState {
    char history[kHistoryCapacity][kLineCapacity];
    Color history_color[kHistoryCapacity];
    uint8_t history_accent_length[kHistoryCapacity];
    bool history_bold[kHistoryCapacity];
    bool history_columned[kHistoryCapacity];
    bool history_tabular[kHistoryCapacity];
    uint32_t history_count;
    char command_history[kHistoryCapacity][kLineCapacity];
    uint32_t command_history_count;
    uint32_t command_history_cursor;
    char input[kInputCapacity];
    uint32_t input_length;
    bool editor_active;
    char editor_filename[13];
    uint8_t editor_buffer[kFileBufferCapacity];
    uint32_t editor_length;
    uint32_t editor_cursor;
    char editor_status[kLineCapacity];
};

struct WorkingDirectory {
    uint32_t cluster_stack[kWorkingDirectoryDepth + 1];
    uint32_t depth;
    uint8_t drive_index;
    char path[kWorkingDirectoryPathCapacity];
};

struct ThemeColors {
    Color screen;
    Color text;
    Color prompt;
    Color input;
    Color title;
    bool monochrome;
};

bool equal_ignore_case(const char* left, const char* right);
char lowercase(char character);
char uppercase(char character);
bool command_equals(const char* input, const char* command);
bool command_starts_with(const char* input, const char* prefix);
uint32_t append_text(char* destination, uint32_t length, uint32_t capacity, const char* text);
void append_decimal(char* output, uint32_t* length, uint64_t value);
bool parse_decimal(const char* text, uint32_t* value);
uint32_t bounded_string_length(const char* text, uint32_t capacity);
void remember_command(ConsoleState& console);
void recall_command(ConsoleState& console, bool older);
void add_history_line(ConsoleState& console, const char* text,
                      Color color = kDefaultTextColor, bool bold = false,
                      uint8_t accent_length = 0, bool columned = false);
const char* command_arguments(const char* input, const char* command);
void begin_file_edit(ConsoleState& console, const storage::Fat32Volume& volume,
                     uint32_t directory_cluster, const char* filename);
bool is_text_file(const uint8_t* data, uint32_t length);
void display_file_contents(ConsoleState& console, const uint8_t* data, uint32_t length);
void handle_editor_event(ConsoleState& console, storage::Fat32Volume& volume,
                         const WorkingDirectory& working_directory,
                         const input::KeyEvent& event, const ThemeColors& colors);
bool handle_file_command(ConsoleState& console, storage::Fat32Volume& volume,
                         bool mounted, const WorkingDirectory& working_directory,
                         const ThemeColors& colors);

namespace ui {

int32_t text_width(const char* text);
uint8_t glyph_advance(char character);
void fill_rect(const FramebufferInfo& framebuffer, int32_t x, int32_t y,
               int32_t width, int32_t height, Color color);
int32_t draw_text(const FramebufferInfo& framebuffer, int32_t x, int32_t y,
                  const char* text, Color color, bool bold = false);
void draw_editor(const FramebufferInfo& framebuffer, const ConsoleState& console,
                 const ThemeColors& colors);
void redraw_history(const FramebufferInfo& framebuffer, const ConsoleState& console,
                    const WorkingDirectory& working_directory, const ThemeColors& colors);
void redraw_prompt(const FramebufferInfo& framebuffer, const ConsoleState& console,
                   const WorkingDirectory& working_directory, const ThemeColors& colors);

} // namespace ui
} // namespace annyos::shell
