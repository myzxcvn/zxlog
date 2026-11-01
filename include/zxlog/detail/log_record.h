#pragma once

#include "zxlog/protocol.h"
#include <array>
#include <cstdint>

namespace zxlog::detail {

struct LogRecord {
    std::array<std::uint8_t, kMaxRecordSize> bytes{};
    std::uint64_t sequence = 0;
    std::uint32_t length = 0;
    Level level = Level::Info;  // backend policy, not part of the wire record
};

}  // namespace zxlog::detail
