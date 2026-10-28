#pragma once

#include <stdint.h>

namespace annyos::curl {

constexpr uint32_t kMaximumResponseBytes = 8192;
constexpr uint32_t kMaximumDiagnosticBytes = 2048;

struct HostResponse {
    uint32_t body_length;
    uint32_t diagnostic_length;
    uint8_t curl_exit_code;
    bool truncated;
    uint8_t body[kMaximumResponseBytes];
    uint8_t diagnostic[kMaximumDiagnosticBytes];
};

// VM development bridge: sends an HTTP(S) curl argument line over COM2 to the
// host helper, which executes the upstream curl CLI checked out in the project root.
// Arguments are parsed and allowlisted by the host helper; shell quotes are
// preserved so option values containing spaces arrive as one argument.
bool fetch_from_host(const char* arguments, HostResponse* response);

} // namespace annyos::curl
