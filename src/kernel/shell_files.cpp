#include "shell_state.hpp"

namespace annyos::shell {

const char* command_arguments(const char* input, const char* command) {
    uint32_t index = 0;
    while (command[index] != '\0') {
        if (input[index] == '\0' || lowercase(input[index]) != command[index]) return nullptr;
        ++index;
    }
    if (input[index] == '\0') return input + index;
    return input[index] == ' ' ? input + index + 1 : nullptr;
}

bool parse_file_arguments(const char* arguments, char filename[13], const char** content) {
    if (arguments == nullptr || filename == nullptr || content == nullptr) return false;
    while (*arguments == ' ') ++arguments;
    uint32_t length = 0;
    while (arguments[length] != '\0' && arguments[length] != ' ') {
        if (length + 1 >= 13) return false;
        filename[length] = arguments[length];
        ++length;
    }
    if (length == 0) return false;
    filename[length] = '\0';
    arguments += length;
    while (*arguments == ' ') ++arguments;
    *content = arguments;
    return true;
}

bool is_text_file(const uint8_t* data, uint32_t length) {
    for (uint32_t index = 0; index < length; ++index) {
        const uint8_t byte = data[index];
        if (byte != '\n' && byte != '\r' && byte != '\t' &&
            (byte < 32 || byte > 126)) return false;
    }
    return true;
}

void display_file_contents(ConsoleState& console, const uint8_t* data, uint32_t length) {
    if (length == 0) {
        add_history_line(console, "(empty file)");
        return;
    }
    char line[kLineCapacity];
    uint32_t line_length = 0;
    for (uint32_t index = 0; index < length; ++index) {
        const uint8_t byte = data[index];
        if (byte == '\r') continue;
        if (byte == '\n') {
            line[line_length] = '\0';
            add_history_line(console, line);
            line_length = 0;
            continue;
        }
        line[line_length++] = byte == '\t' ? ' ' : static_cast<char>(byte);
        if (line_length + 1 == kLineCapacity) {
            line[line_length] = '\0';
            add_history_line(console, line);
            line_length = 0;
        }
    }
    if (line_length != 0) {
        line[line_length] = '\0';
        add_history_line(console, line);
    }
}

bool read_text_file(ConsoleState& console, const annyos::storage::Fat32Volume& volume,
                    uint32_t directory_cluster, const char* filename,
                    uint32_t* bytes_read) {
    if (filename == nullptr || bytes_read == nullptr) return false;
    *bytes_read = 0;
    annyos::storage::Fat32DirectoryEntry entry{};
    if (!annyos::storage::fat32_find_file(volume, directory_cluster, filename, &entry)) {
        add_history_line(console, "File not found.");
        return false;
    }
    if (entry.size > kFileBufferCapacity) {
        add_history_line(console, "File is larger than the 4 KiB console limit.");
        return false;
    }
    if (!annyos::storage::fat32_read_file(volume, entry, console.editor_buffer,
                                           kFileBufferCapacity, bytes_read) ||
        *bytes_read != entry.size) {
        add_history_line(console, "Could not read file.");
        return false;
    }
    return true;
}

void begin_file_edit(ConsoleState& console, const annyos::storage::Fat32Volume& volume,
                     uint32_t directory_cluster, const char* filename) {
    if (filename == nullptr) {
        add_history_line(console, "Usage: EDIT <FILE>");
        return;
    }
    annyos::storage::Fat32DirectoryEntry entry{};
    console.editor_length = 0;
    console.editor_cursor = 0;
    if (annyos::storage::fat32_find_file(volume, directory_cluster, filename, &entry)) {
        if (entry.size > kFileBufferCapacity) {
            add_history_line(console, "File is larger than the 4 KiB editor limit.");
            return;
        }
        uint32_t bytes_read = 0;
        if (!annyos::storage::fat32_read_file(volume, entry, console.editor_buffer,
                                               kFileBufferCapacity, &bytes_read) ||
            bytes_read != entry.size || !is_text_file(console.editor_buffer, bytes_read)) {
            add_history_line(console, "File cannot be opened as editable text.");
            return;
        }
        uint32_t normalized_length = 0;
        for (uint32_t index = 0; index < bytes_read; ++index) {
            if (console.editor_buffer[index] == '\r') continue;
            console.editor_buffer[normalized_length++] = console.editor_buffer[index];
        }
        console.editor_length = normalized_length;
        console.editor_cursor = normalized_length;
        const char loaded[] = "File loaded.";
        append_text(console.editor_status, 0, sizeof(console.editor_status), loaded);
    } else {
        const char created[] = "New file.";
        append_text(console.editor_status, 0, sizeof(console.editor_status), created);
    }

    uint32_t index = 0;
    while (filename[index] != '\0' && index + 1 < sizeof(console.editor_filename)) {
        console.editor_filename[index] = filename[index];
        ++index;
    }
    console.editor_filename[index] = '\0';
    console.editor_active = true;
}

uint32_t editor_line_start(const ConsoleState& console, uint32_t position) {
    uint32_t start = position > console.editor_length ? console.editor_length : position;
    while (start > 0 && console.editor_buffer[start - 1] != '\n') --start;
    return start;
}

uint32_t editor_line_end(const ConsoleState& console, uint32_t position) {
    uint32_t end = position > console.editor_length ? console.editor_length : position;
    while (end < console.editor_length && console.editor_buffer[end] != '\n') ++end;
    return end;
}

void editor_insert(ConsoleState& console, uint8_t byte) {
    if (console.editor_length >= kFileBufferCapacity) {
        append_text(console.editor_status, 0, sizeof(console.editor_status), "Buffer full (4 KiB).");
        return;
    }
    for (uint32_t index = console.editor_length; index > console.editor_cursor; --index) {
        console.editor_buffer[index] = console.editor_buffer[index - 1];
    }
    console.editor_buffer[console.editor_cursor++] = byte;
    ++console.editor_length;
    console.editor_status[0] = '\0';
}

void editor_erase(ConsoleState& console, uint32_t position) {
    if (position >= console.editor_length) return;
    for (uint32_t index = position; index + 1 < console.editor_length; ++index) {
        console.editor_buffer[index] = console.editor_buffer[index + 1];
    }
    --console.editor_length;
    if (console.editor_cursor > position) --console.editor_cursor;
    console.editor_status[0] = '\0';
}

void editor_move_vertical(ConsoleState& console, bool down) {
    const uint32_t start = editor_line_start(console, console.editor_cursor);
    const uint32_t end = editor_line_end(console, console.editor_cursor);
    const uint32_t column = console.editor_cursor - start;
    uint32_t target_start;
    uint32_t target_end;
    if (down) {
        if (end >= console.editor_length) return;
        target_start = end + 1;
        target_end = editor_line_end(console, target_start);
    } else {
        if (start == 0) return;
        target_end = start - 1;
        target_start = editor_line_start(console, target_end);
    }
    const uint32_t target_length = target_end - target_start;
    console.editor_cursor = target_start + (column < target_length ? column : target_length);
}

void finish_file_edit(ConsoleState& console, annyos::storage::Fat32Volume& volume,
                      const WorkingDirectory& working_directory, bool save,
                      const ThemeColors& colors) {
    if (save) {
        if (working_directory.depth > kWorkingDirectoryDepth) {
            append_text(console.editor_status, 0, sizeof(console.editor_status),
                        "Save failed: directory state invalid.");
            return;
        }
        const uint8_t* data = console.editor_length == 0 ? nullptr : console.editor_buffer;
        if (!annyos::storage::fat32_write_file(
                volume, working_directory.cluster_stack[working_directory.depth],
                console.editor_filename, data, console.editor_length)) {
            append_text(console.editor_status, 0, sizeof(console.editor_status), "Save failed; keep editing.");
            return;
        }
        add_history_line(console, "File saved.", colors.prompt);
    } else {
        add_history_line(console, "Edit cancelled.");
    }
    console.editor_active = false;
    console.editor_status[0] = '\0';
}

void handle_editor_event(ConsoleState& console, annyos::storage::Fat32Volume& volume,
                         const WorkingDirectory& working_directory,
                         const annyos::input::KeyEvent& event, const ThemeColors& colors) {
    if (console.editor_length > kFileBufferCapacity) {
        console.editor_length = kFileBufferCapacity;
    }
    if (console.editor_cursor > console.editor_length) {
        console.editor_cursor = console.editor_length;
    }
    switch (event.action) {
        case annyos::input::KeyAction::character:
            if (event.character >= 32 && event.character <= 126) {
                editor_insert(console, static_cast<uint8_t>(event.character));
            }
            break;
        case annyos::input::KeyAction::enter:
            editor_insert(console, '\n');
            break;
        case annyos::input::KeyAction::backspace:
            if (console.editor_cursor > 0) editor_erase(console, console.editor_cursor - 1);
            break;
        case annyos::input::KeyAction::delete_forward:
            editor_erase(console, console.editor_cursor);
            break;
        case annyos::input::KeyAction::left:
            if (console.editor_cursor > 0) --console.editor_cursor;
            break;
        case annyos::input::KeyAction::right:
            if (console.editor_cursor < console.editor_length) ++console.editor_cursor;
            break;
        case annyos::input::KeyAction::up:
            editor_move_vertical(console, false);
            break;
        case annyos::input::KeyAction::down:
            editor_move_vertical(console, true);
            break;
        case annyos::input::KeyAction::home:
            console.editor_cursor = editor_line_start(console, console.editor_cursor);
            break;
        case annyos::input::KeyAction::end:
            console.editor_cursor = editor_line_end(console, console.editor_cursor);
            break;
        case annyos::input::KeyAction::save:
            finish_file_edit(console, volume, working_directory, true, colors);
            break;
        case annyos::input::KeyAction::escape:
            finish_file_edit(console, volume, working_directory, false, colors);
            break;
    }
}

bool handle_file_command(ConsoleState& console, annyos::storage::Fat32Volume& volume,
                         bool mounted, const WorkingDirectory& working_directory,
                         const ThemeColors& colors) {
    const char* arguments = nullptr;
    enum class Operation : uint8_t { create, read, remove };
    Operation operation;
    if ((arguments = command_arguments(console.input, "create")) != nullptr) {
        operation = Operation::create;
    } else if ((arguments = command_arguments(console.input, "read")) != nullptr ||
               (arguments = command_arguments(console.input, "type")) != nullptr) {
        operation = Operation::read;
    } else if ((arguments = command_arguments(console.input, "delete")) != nullptr ||
               (arguments = command_arguments(console.input, "del")) != nullptr) {
        operation = Operation::remove;
    } else {
        return false;
    }

    if (!mounted) {
        add_history_line(console, "FAT32 volume not found.");
        return true;
    }
    if (working_directory.depth > kWorkingDirectoryDepth) {
        add_history_line(console, "Current directory state is invalid.");
        return true;
    }
    char filename[13];
    const char* content = nullptr;
    if (!parse_file_arguments(arguments, filename, &content)) {
        add_history_line(console, "Usage: CREATE|READ|DELETE <8.3 FILE>");
        return true;
    }
    const uint32_t directory_cluster =
        working_directory.cluster_stack[working_directory.depth];

    switch (operation) {
        case Operation::create: {
            annyos::storage::Fat32DirectoryEntry existing{};
            if (annyos::storage::fat32_find_file(volume, directory_cluster,
                                                  filename, &existing)) {
                add_history_line(console, "File already exists; use EDIT to replace it.");
                return true;
            }
            const uint32_t content_length = bounded_string_length(content, kInputCapacity);
            const uint8_t* data = content_length == 0 ? nullptr :
                reinterpret_cast<const uint8_t*>(content);
            if (content_length >= kInputCapacity ||
                !annyos::storage::fat32_write_file(volume, directory_cluster,
                                                    filename, data, content_length)) {
                add_history_line(console, "Create failed. Use a valid 8.3 name and writable volume.");
            } else {
                add_history_line(console, "File created.", colors.prompt);
            }
            return true;
        }
        case Operation::read: {
            if (*content != '\0') {
                add_history_line(console, "Usage: READ <FILE>");
                return true;
            }
            uint32_t bytes_read = 0;
            if (read_text_file(console, volume, directory_cluster, filename, &bytes_read)) {
                if (!is_text_file(console.editor_buffer, bytes_read)) {
                    add_history_line(console, "File contains binary data; text display is unavailable.");
                } else {
                    add_history_line(console, "FILE CONTENT", colors.title, true);
                    display_file_contents(console, console.editor_buffer, bytes_read);
                }
            }
            return true;
        }
        case Operation::remove:
            if (*content != '\0') {
                add_history_line(console, "Usage: DELETE <FILE>");
            } else if (annyos::storage::fat32_delete_file(
                           volume, directory_cluster, filename)) {
                add_history_line(console, "File deleted.", colors.prompt);
            } else {
                add_history_line(console, "Delete failed; file may be missing, read-only, or a folder.");
            }
            return true;
    }
    return true;
}


} // namespace annyos::shell
