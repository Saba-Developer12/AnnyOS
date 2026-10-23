#pragma once

#include "annyos/fat32.hpp"

namespace annyos::config {

// Small persistent console configuration stored as key/value text in the
// FAT32 root directory (ANNYOS.CFG). This manager currently owns THEME.
struct ConfigSave {
    const storage::Fat32Volume* volume;
    char active_theme[8];
};

// Starts with BLUE and replaces it with a valid persisted THEME value when one
// is present. A missing or malformed file leaves the safe default in place.
void config_save_initialize(ConfigSave* config, const storage::Fat32Volume* volume);
const char* config_save_active_theme(const ConfigSave* config);
bool config_save_set_theme(ConfigSave* config, const char* theme);

} // namespace annyos::config
