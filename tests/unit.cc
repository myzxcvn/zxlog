#include "zxlog/site.h"
#include "zxlog/crc32.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <string>

using zxlog::Type;

static_assert(zxlog::detail::type_of<std::uint16_t>() == Type::U16);
static_assert(zxlog::detail::type_of<float>() == Type::F32);
static_assert(zxlog::detail::type_of<std::string_view>() == Type::String);
static_assert(zxlog::detail::type_of<const char (&)[4]>() == Type::String);
constexpr auto kMeta = zxlog::detail::make_site<std::uint32_t>(
    zxlog::Level::Info, "unit.cc", 42, 1, "speed={}");
static_assert(kMeta.arg_count == 1 && kMeta.types[0] == 6);
static_assert(kMeta.total_bytes == sizeof(kMeta));

int main() {
    const std::uint8_t abc[]{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    if (zxlog::detail::crc32(abc, 9) != 0xcbf43926u) return 2;
    std::uint8_t bytes[32]{};
    std::uint8_t* cursor = bytes;
    if (!zxlog::detail::write_value(cursor, bytes + sizeof(bytes), std::int32_t{-7})) return 3;
    if (!zxlog::detail::write_value(cursor, bytes + sizeof(bytes), std::uint64_t{0x123456789abcdef0ull})) return 3;
    if (!zxlog::detail::write_value(cursor, bytes + sizeof(bytes), std::string_view{"test"})) return 3;
    if (cursor != bytes + 1 + 9 + 1 + 4) return 1;
    const auto *start = bytes;
    std::uint64_t a = 0, b = 0, c = 0;
    if (!zxlog::detail::read_varint(start, cursor, a) || a != 13 ||
        !zxlog::detail::read_varint(start, cursor, b) || b != 0x123456789abcdef0ull ||
        !zxlog::detail::read_varint(start, cursor, c) || c != 4 ||
        std::string(reinterpret_cast<const char*>(start), 4) != "test") return 1;
    // Rejected values must leave the cursor and destination untouched.
    for (std::size_t capacity = 0; capacity < 5; ++capacity) {
        std::array<std::uint8_t, 10> guarded;
        guarded.fill(0xa5);
        auto *p = guarded.data() + 1;
        if (zxlog::detail::write_value(p, p + capacity, std::string_view{"test"}) ||
            p != guarded.data() + 1) return 4;
        for (auto byte : guarded) if (byte != 0xa5) return 5;
        if (zxlog::detail::write_value(p, p + capacity, std::uint64_t{0xffffffffffffffffull})) return 6;
        for (auto byte : guarded) if (byte != 0xa5) return 7;
    }
    for (std::size_t capacity = 5; capacity < 10; ++capacity) {
        std::array<std::uint8_t, 12> guard;
        guard.fill(0xa5);
        auto *p = guard.data() + 1;
        if (zxlog::detail::write_value(p, p + capacity, std::uint64_t{0xffffffffffffffffull}))
            return 8;
        for (auto byte : guard) if (byte != 0xa5) return 9;
    }
    std::cout << "zxlog unit tests passed\n";
}
