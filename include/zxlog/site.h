#pragma once

#include "zxlog/codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// A self-contained, relocation-free ELF section record. The Python extractor
// reads exactly this v1 wire layout, not the compiler's pointer representation.
#if defined(__GNUC__) || defined(__clang__)
#  if defined(__has_attribute)
#    if __has_attribute(retain)
#      define ZXLOG_RETAIN_ATTR __attribute__((retain))
#    else
#      define ZXLOG_RETAIN_ATTR
#    endif
#  else
#    define ZXLOG_RETAIN_ATTR
#  endif
#  define ZXLOG_SITE_ATTR __attribute__((used, section("zxlog_meta"), aligned(1))) ZXLOG_RETAIN_ATTR
#else
#  error "zxlog v0.1 metadata extraction requires GCC/Clang on ELF"
#endif

namespace zxlog::detail {

constexpr std::uint32_t kSiteMagic = 0x4d4c585au; // "ZXLM" little-endian
constexpr std::uint32_t kHashInit = 2166136261u;
constexpr std::uint32_t hash_byte(std::uint32_t h, std::uint8_t b) noexcept {
    return (h ^ b) * 16777619u;
}

template <std::size_t N>
constexpr std::uint32_t hash_chars(std::uint32_t h, const char (&s)[N]) noexcept {
    for (std::size_t i = 0; i + 1 < N; ++i)
        h = hash_byte(h, static_cast<std::uint8_t>(s[i]));
    return h;
}

#pragma pack(push, 1)
template <std::size_t F, std::size_t S, std::size_t A>
struct SiteBlob {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t total_bytes;
    std::uint32_t id;
    std::uint32_t line;
    std::uint16_t file_bytes;
    std::uint16_t format_bytes;
    std::uint8_t arg_count;
    std::uint8_t level;
    std::uint16_t reserved;
    std::uint8_t types[A ? A : 1];
    char file[S];
    char format[F];
};
#pragma pack(pop)

template <typename... Args, std::size_t S, std::size_t F>
constexpr auto make_site(Level level,
                         const char (&file)[S],
                         std::uint32_t line,
                         std::uint32_t ordinal,
                         const char (&format)[F]) noexcept {
    constexpr std::size_t A = sizeof...(Args);
    static_assert(A <= 255, "zxlog: too many arguments");
    static_assert(sizeof(SiteBlob<F, S, A>) <= 65535,
                  "zxlog: metadata record too large");
    static_assert(((type_of<Args>() != Type::Invalid) && ...),
                  "zxlog: unsupported argument type");
    SiteBlob<F, S, A> site{};
    site.magic = kSiteMagic;
    site.version = 1;
    site.total_bytes = static_cast<std::uint16_t>(sizeof(site));
    site.line = line;
    site.file_bytes = static_cast<std::uint16_t>(S);
    site.format_bytes = static_cast<std::uint16_t>(F);
    site.arg_count = static_cast<std::uint8_t>(A);
    site.level = static_cast<std::uint8_t>(level);
    const std::array<Type, A> codes{{type_of<Args>()...}};
    for (std::size_t i = 0; i < A; ++i) site.types[i] = static_cast<std::uint8_t>(codes[i]);
    for (std::size_t i = 0; i < S; ++i) site.file[i] = file[i];
    for (std::size_t i = 0; i < F; ++i) site.format[i] = format[i];
    auto h = hash_chars(hash_chars(kHashInit, file), format);
    for (unsigned n = 0; n != 4; ++n) h = hash_byte(h, static_cast<std::uint8_t>(line >> (8 * n)));
    for (unsigned n = 0; n != 4; ++n) h = hash_byte(h, static_cast<std::uint8_t>(ordinal >> (8 * n)));
    h = hash_byte(h, static_cast<std::uint8_t>(level));
    for (std::size_t i = 0; i < A; ++i) h = hash_byte(h, site.types[i]);
    site.id = h;
    return site;
}

} // namespace zxlog::detail
