#include "annyos/config_save.hpp"

namespace annyos::config {
namespace {

constexpr char kConfigFileName[] = "ANNYOS.CFG";
constexpr uint32_t kConfigCapacity = 96;

char uppercase(char value) {
    return value >= 'a' && value <= 'z' ? static_cast<char>(value - 'a' + 'A') : value;
}

bool equal_ignore_case(const char* left, const char* right) {
    while (*left != '\0' && *right != '\0') {
        if (uppercase(*left++) != uppercase(*right++)) return false;
    }
    return *left == '\0' && *right == '\0';
}

const char* canonical_theme(const char* theme) {
    if (equal_ignore_case(theme, "BLUE")) return "BLUE";
    if (equal_ignore_case(theme, "GREEN")) return "GREEN";
    if (equal_ignore_case(theme, "AMBER")) return "AMBER";
    if (equal_ignore_case(theme, "MATRIX") || equal_ignore_case(theme, "BLACK")) return "MATRIX";
    return nullptr;
}

bool parse_theme(const char* text, uint32_t size, const char** theme) {
    if (text == nullptr || theme == nullptr || size == 0 || size >= kConfigCapacity) return false;
    uint32_t offset = 0;
    while (offset < size) {
        const uint32_t line_start = offset;
        while (offset < size && text[offset] != '\n' && text[offset] != '\r') ++offset;
        const uint32_t line_length = offset - line_start;
        while (offset < size && (text[offset] == '\n' || text[offset] == '\r')) ++offset;
        if (line_length < 6 || line_length >= 32) continue;

        char key[12]{};
        char value[20]{};
        uint32_t separator = line_start;
        while (separator < line_start + line_length && text[separator] != '=') ++separator;
        if (separator == line_start + line_length) continue;
        const uint32_t key_length = separator - line_start;
        const uint32_t value_length = line_start + line_length - separator - 1;
        if (key_length >= sizeof(key) || value_length >= sizeof(value)) continue;
        for (uint32_t i = 0; i < key_length; ++i) key[i] = text[line_start + i];
        for (uint32_t i = 0; i < value_length; ++i) value[i] = text[separator + 1 + i];
        if (equal_ignore_case(key, "THEME")) {
            *theme = canonical_theme(value);
            return *theme != nullptr;
        }
    }
    return false;
}

void copy_string(char* destination, const char* source) {
    uint32_t index = 0;
    while (source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

} // namespace

void config_save_initialize(ConfigSave* config, const storage::Fat32Volume* volume) {
    if (config == nullptr) return;
    config->volume = volume;
    copy_string(config->active_theme, "BLUE");
    if (volume == nullptr) return;

    storage::Fat32DirectoryEntry entry{};
    if (!storage::fat32_find_file(*volume, volume->root_cluster, kConfigFileName, &entry) ||
        entry.size == 0 || entry.size >= kConfigCapacity) {
        config_save_set_theme(config, "BLUE");
        return;
    }
    char contents[kConfigCapacity]{};
    uint32_t bytes_read = 0;
    if (!storage::fat32_read_file(*volume, entry, reinterpret_cast<uint8_t*>(contents),
                                  sizeof(contents) - 1, &bytes_read)) {
        config_save_set_theme(config, "BLUE");
        return;
    }
    const char* theme = nullptr;
    if (parse_theme(contents, bytes_read, &theme)) copy_string(config->active_theme, theme);
    else config_save_set_theme(config, "BLUE");
}

const char* config_save_active_theme(const ConfigSave* config) {
    return config == nullptr ? "BLUE" : config->active_theme;
}

bool config_save_set_theme(ConfigSave* config, const char* theme) {
    if (config == nullptr || config->volume == nullptr) return false;
    const char* canonical = canonical_theme(theme);
    if (canonical == nullptr) return false;

    char contents[kConfigCapacity] = "VERSION=1\nTHEME=";
    uint32_t length = 16;
    for (uint32_t i = 0; canonical[i] != '\0'; ++i) contents[length++] = canonical[i];
    contents[length++] = '\n';
    if (!storage::fat32_write_file(*config->volume, config->volume->root_cluster,
                                   kConfigFileName, reinterpret_cast<const uint8_t*>(contents), length)) {
        return false;
    }
    copy_string(config->active_theme, canonical);
    return true;
}

} // namespace annyos::config
