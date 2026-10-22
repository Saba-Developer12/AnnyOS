#include "annyos/usb_hid.hpp"

namespace annyos::input::usb {

namespace {
constexpr uint16_t kPciAddress = 0x0cf8;
constexpr uint16_t kPciData = 0x0cfc;
constexpr uint32_t kPciEnable = 0x80000000u;
constexpr uint8_t kPciBaseClassSerialBus = 0x0c;
constexpr uint8_t kPciSubClassUsb = 0x03;
constexpr uint8_t kPciProgrammingInterfaceXhci = 0x30;
constexpr uint32_t kMaxPciScanReads = 1u << 20;
uint8_t previous_keys[6];
bool caps_lock;

void out32(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

uint32_t in32(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint32_t pci_read(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    const uint32_t address = kPciEnable | (static_cast<uint32_t>(bus) << 16) |
        (static_cast<uint32_t>(device) << 11) | (static_cast<uint32_t>(function) << 8) |
        (offset & 0xfcu);
    out32(kPciAddress, address);
    return in32(kPciData);
}

uint8_t byte_at(uint32_t value, uint8_t index) {
    return static_cast<uint8_t>(value >> (index * 8));
}

bool key_present(const uint8_t* keys, uint8_t usage) {
    for (uint32_t index = 0; index < 6; ++index) {
        if (keys[index] == usage) return true;
    }
    return false;
}

char usage_to_ascii(uint8_t usage, bool shift, bool caps) {
    if (usage >= 0x04 && usage <= 0x1d) {
        const bool upper = shift != caps;
        return static_cast<char>((upper ? 'A' : 'a') + (usage - 0x04));
    }
    if (usage >= 0x1e && usage <= 0x27) {
        static constexpr char normal[] = "1234567890";
        static constexpr char shifted[] = "!@#$%^&*()";
        return shift ? shifted[usage - 0x1e] : normal[usage - 0x1e];
    }
    switch (usage) {
        case 0x2c: return ' ';
        case 0x2d: return shift ? '_' : '-';
        case 0x2e: return shift ? '+' : '=';
        case 0x2f: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        default: return 0;
    }
}
}

bool probe_xhci(ControllerInfo* info) {
    if (info == nullptr) return false;
    *info = {};
    uint32_t reads = 0;

    for (uint32_t bus = 0; bus < 256 && reads < kMaxPciScanReads; ++bus) {
        for (uint32_t device = 0; device < 32 && reads < kMaxPciScanReads; ++device) {
            const uint32_t first_function_id = pci_read(static_cast<uint8_t>(bus),
                static_cast<uint8_t>(device), 0, 0x00);
            ++reads;
            if (static_cast<uint16_t>(first_function_id) == 0xffff) continue;
            const uint32_t header_type = pci_read(static_cast<uint8_t>(bus),
                static_cast<uint8_t>(device), 0, 0x0c);
            ++reads;
            const uint8_t function_count = (byte_at(header_type, 2) & 0x80) != 0 ? 8 : 1;

            for (uint32_t function = 0; function < function_count && reads < kMaxPciScanReads;
                 ++function) {
                const uint32_t id = pci_read(static_cast<uint8_t>(bus),
                    static_cast<uint8_t>(device), static_cast<uint8_t>(function), 0x00);
                ++reads;
                if (static_cast<uint16_t>(id) == 0xffff) continue;
                const uint32_t class_code = pci_read(static_cast<uint8_t>(bus),
                    static_cast<uint8_t>(device), static_cast<uint8_t>(function), 0x08);
                ++reads;
                if (byte_at(class_code, 3) != kPciBaseClassSerialBus ||
                    byte_at(class_code, 2) != kPciSubClassUsb ||
                    byte_at(class_code, 1) != kPciProgrammingInterfaceXhci) continue;
                if (info->pci_xhci_matches != 0xffff) ++info->pci_xhci_matches;

                const uint32_t bar_low = pci_read(static_cast<uint8_t>(bus),
                    static_cast<uint8_t>(device), static_cast<uint8_t>(function), 0x10);
                ++reads;
                const uint32_t bar_high = pci_read(static_cast<uint8_t>(bus),
                    static_cast<uint8_t>(device), static_cast<uint8_t>(function), 0x14);
                ++reads;
                const uint64_t mmio = (static_cast<uint64_t>(bar_high) << 32) | (bar_low & ~0x0fu);
                info->mmio_base = mmio;
                if ((bar_low & 1u) != 0 || ((bar_low >> 1) & 3u) != 2u) continue;
                // Accept canonical x86-64 MMIO addresses. QEMU and newer
                // firmware commonly place PCI apertures above 4 GiB.
                if (mmio == 0 || mmio > 0x00007fffffffffffull) continue;

                // MMIO is deliberately not dereferenced here. Current UEFI
                // page tables do not guarantee that PCI apertures are mapped.
                // The kernel needs a page-table manager before register access.

                info->mmio_base = mmio;
                info->bus = static_cast<uint8_t>(bus);
                info->device = static_cast<uint8_t>(device);
                info->function = static_cast<uint8_t>(function);
                return true;
            }
        }
    }
    return false;
}

void reset_boot_keyboard_decoder() {
    for (uint32_t index = 0; index < 6; ++index) previous_keys[index] = 0;
    caps_lock = false;
}

bool decode_boot_keyboard_report(const uint8_t* report, uint32_t report_size,
                                 KeyEvent* event) {
    if (report == nullptr || event == nullptr || report_size < 8) return false;
    const uint8_t modifiers = report[0];
    const uint8_t* keys = report + 2;
    bool emitted = false;

    if (key_present(keys, 0x39) && !key_present(previous_keys, 0x39)) caps_lock = !caps_lock;
    const bool shift = (modifiers & 0x22u) != 0;

    // Boot reports are unordered arrays. Compare the current report to the
    // previous one and emit at most one newly pressed key per poll.
    for (uint32_t index = 0; index < 6 && !emitted; ++index) {
        const uint8_t usage = keys[index];
        if (usage == 0 || usage < 0x04 || usage == 0x01 || usage == 0x02 || usage == 0x03 ||
            key_present(previous_keys, usage)) continue;
        const char character = usage_to_ascii(usage, shift, caps_lock);
        if (character != 0) {
            event->action = KeyAction::character;
            event->character = character;
            emitted = true;
        } else if (usage == 0x28) {
            event->action = KeyAction::enter;
            event->character = '\n';
            emitted = true;
        } else if (usage == 0x2a) {
            event->action = KeyAction::backspace;
            event->character = '\b';
            emitted = true;
        } else if (usage == 0x52) {
            event->action = KeyAction::up;
            event->character = 0;
            emitted = true;
        } else if (usage == 0x51) {
            event->action = KeyAction::down;
            event->character = 0;
            emitted = true;
        }
    }

    for (uint32_t index = 0; index < 6; ++index) previous_keys[index] = keys[index];
    return emitted;
}

} // namespace annyos::input::usb
