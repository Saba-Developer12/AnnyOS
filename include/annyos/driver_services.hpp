#pragma once

#include "annyos/block_device.hpp"
#include "annyos/fat32.hpp"
#include "annyos/ps2_keyboard.hpp"

#include <stdint.h>

namespace annyos::drivers::service {

struct SerialOutputV1 {
    bool (*write_byte)(uint8_t byte);
    bool (*write)(const char* text);
};

struct MonotonicTimerV1 {
    uint64_t (*ticks)();
    bool (*sleep_ms)(uint32_t milliseconds);
};

struct KeyboardInputV1 {
    bool (*poll)(annyos::input::KeyEvent* event);
};

struct AtaDriveSetV1 {
    void* context;
    bool (*get_device)(void* context, uint8_t drive_index,
                       const annyos::storage::BlockDevice** device);
};

struct Fat32DriveSetV1 {
    void* context;
    bool (*get_volume)(void* context, uint8_t drive_index,
                       const annyos::storage::Fat32Volume** volume);
};

} // namespace annyos::drivers::service
