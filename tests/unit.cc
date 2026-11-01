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
    zxlog::detail::write_value(cursor, std::int32_t{-7});
    zxlog::detail::write_value(cursor, std::uint64_t{0x123456789abcdef0ull});
    zxlog::detail::write_value(cursor, std::string_view{"test"});
    if (cursor != bytes + 4 + 8 + 4 + 4 ||
        zxlog::detail::read_u32(bytes) != 0xfffffff9u ||
        zxlog::detail::read_u64(bytes + 4) != 0x123456789abcdef0ull ||
        zxlog::detail::read_u32(bytes + 12) != 4u) return 1;
    std::cout << "zxlog unit tests passed\n";
}
