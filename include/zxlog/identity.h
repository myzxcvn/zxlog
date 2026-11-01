#pragma once

#include <array>
#include <cstdint>

namespace zxlog {
namespace detail {
extern const volatile std::uint8_t kLinkedBuildId[16]
    __attribute__((section("zxlog_identity"), aligned(1)));
}

// Filled in the linked ELF by extract_dictionary.py, before metadata stripping.
// Volatile reads prevent LTO/constant folding from embedding the zero placeholder.
inline std::array<std::uint8_t, 16> linked_build_id() noexcept {
    std::array<std::uint8_t, 16> result{};
    for (unsigned i = 0; i < result.size(); ++i) result[i] = detail::kLinkedBuildId[i];
    return result;
}
}  // namespace zxlog
