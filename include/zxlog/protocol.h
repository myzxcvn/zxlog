#pragma once

#include <cstddef>
#include <cstdint>

namespace zxlog {

enum class Level : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5,
    Off = 6
};

enum class OutputMode : std::uint8_t {
    Text = 1,
    Binary = 2,
};

enum class Type : std::uint8_t {
    Invalid = 0,
    I8 = 1,
    U8 = 2,
    I16 = 3,
    U16 = 4,
    I32 = 5,
    U32 = 6,
    I64 = 7,
    U64 = 8,
    F32 = 9,
    F64 = 10,
    Boolean = 11,
    Character = 12,
    String = 13
};

// Wire protocol v2. All integers little-endian; no struct memcpy for headers.
// Record: u32 length, u32 id, u64 steady_ns, u32 CRC32, encoded args.
// CRC32 IEEE covers bytes [0,16) and [20,length), excluding its own field.
constexpr std::size_t kRecordHeaderSize = 20;
constexpr std::size_t kBinaryHeaderSize = 64;
constexpr std::size_t kMaxRecordSize = 1024;
constexpr std::uint16_t kWireVersion = 2;

}  // namespace zxlog
