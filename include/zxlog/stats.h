#pragma once

#include <cstdint>

namespace zxlog {
struct Stats {
    std::uint64_t accepted = 0;
    std::uint64_t dropped = 0;
    std::uint64_t io_errors = 0;
    std::uint64_t rotations = 0;
    std::uint64_t repaired_bytes = 0;
    std::uint64_t sync_errors = 0;
    std::size_t queue_memory_bytes = 0;  // reserved slot payload plus slot metadata
    std::size_t queue_payload_bytes = 0;
    std::uint64_t text_too_large = 0;  // accepted events rejected by text rendering
    std::uint64_t queue_full = 0, producer_limit = 0, tls_limit = 0, record_too_large = 0;
    std::uint64_t invalid_records = 0, unknown_sites = 0, format_errors = 0;
    std::uint64_t write_errors = 0, open_errors = 0, rotate_errors = 0, internal_errors = 0;
};

}  // namespace zxlog
