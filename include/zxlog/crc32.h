#pragma once
#include "zxlog/protocol.h"
#include "zxlog/codec.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace zxlog::detail {
// CRC32 IEEE (poly 0xedb88320), table created at compile time in C++17.
constexpr std::uint32_t crc32_entry(std::uint32_t v) noexcept {
    for (unsigned bit = 0; bit != 8; ++bit)
        v = (v >> 1) ^ (0xedb88320u & (0u - (v & 1u)));
    return v;
}
constexpr auto make_crc32_table() noexcept {
    std::array<std::uint32_t, 256> result{};
    for (std::uint32_t i = 0; i != result.size(); ++i)
        result[i] = crc32_entry(i);
    return result;
}
inline constexpr auto kCrc32Table = make_crc32_table();
inline std::uint32_t crc32_update(std::uint32_t state,
                                  const std::uint8_t* data,
                                  std::size_t size) noexcept {
    for (std::size_t n = 0; n < size; ++n)
        state = (state >> 8) ^ kCrc32Table[(state ^ data[n]) & 0xffu];
    return state;
}
inline std::uint32_t crc32(const std::uint8_t* bytes, std::size_t size) noexcept {
    return ~crc32_update(0xffffffffu, bytes, size);
}
inline std::uint32_t record_crc32(const std::uint8_t* rec, std::size_t size) noexcept {
    if (size < kRecordHeaderSize) return 0;
    const auto state = crc32_update(0xffffffffu, rec, 16);
    return ~crc32_update(state, rec + kRecordHeaderSize, size - kRecordHeaderSize);
}
} // namespace zxlog::detail
