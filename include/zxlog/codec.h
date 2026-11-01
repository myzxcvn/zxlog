#pragma once

#include "zxlog/protocol.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace zxlog {

// Specialize for simple user-defined wrappers (strong typedefs, fixed-point,
// small identifiers). Wire must be a built-in supported zxlog type and to_wire
// must not allocate or perform textual formatting. Offline decoding sees Wire.
template <typename T>
struct LogAdapter {};

}  // namespace zxlog

namespace zxlog::detail {

template <typename T, typename = void>
struct has_log_adapter : std::false_type {};
template <typename T>
struct has_log_adapter<
    T, std::void_t<typename LogAdapter<T>::Wire, decltype(LogAdapter<T>::to_wire(std::declval<const T &>()))>>
    : std::true_type {};

template <typename T>
constexpr Type type_of() noexcept;

template <typename T>
decltype(auto) wire_value(const T &value) noexcept {
    using V = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (has_log_adapter<V>::value) {
        using Wire = typename LogAdapter<V>::Wire;
        using Result = decltype(LogAdapter<V>::to_wire(value));
        static_assert(std::is_same_v<std::decay_t<Result>, Wire>,
                      "zxlog: LogAdapter return type must match Wire");
        static_assert(!has_log_adapter<Wire>::value && type_of<Wire>() != Type::Invalid,
                      "zxlog: LogAdapter Wire must be a supported built-in type");
        static_assert(noexcept(LogAdapter<V>::to_wire(value)),
                      "zxlog: LogAdapter::to_wire must be noexcept");
        return LogAdapter<V>::to_wire(value);
    } else {
        return (value);
    }
}

template <typename T>
constexpr Type type_of() noexcept {
    using V = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (has_log_adapter<V>::value) {
        return type_of<typename LogAdapter<V>::Wire>();
    } else if constexpr (std::is_enum_v<V>) {
        return type_of<std::underlying_type_t<V>>();
    } else if constexpr (std::is_same_v<V, bool>) {
        return Type::Boolean;
    } else if constexpr (std::is_same_v<V, char>) {
        return Type::Character;
    } else if constexpr (std::is_same_v<V, std::string> || std::is_same_v<V, std::string_view> ||
                         std::is_same_v<V, char *> || std::is_same_v<V, const char *> ||
                         (std::is_array_v<V> &&
                          std::is_same_v<std::remove_cv_t<std::remove_extent_t<V>>, char>)) {
        return Type::String;
    } else if constexpr (std::is_same_v<V, float>) {
        return Type::F32;
    } else if constexpr (std::is_same_v<V, double>) {
        return Type::F64;
    } else if constexpr (std::is_integral_v<V> && std::is_signed_v<V>) {
        if constexpr (sizeof(V) == 1) {
            return Type::I8;
        }

        if constexpr (sizeof(V) == 2) {
            return Type::I16;
        }

        if constexpr (sizeof(V) == 4) {
            return Type::I32;
        }

        if constexpr (sizeof(V) == 8) {
            return Type::I64;
        }
    } else if constexpr (std::is_integral_v<V> && std::is_unsigned_v<V>) {
        if constexpr (sizeof(V) == 1) {
            return Type::U8;
        }

        if constexpr (sizeof(V) == 2) {
            return Type::U16;
        }

        if constexpr (sizeof(V) == 4) {
            return Type::U32;
        }

        if constexpr (sizeof(V) == 8) {
            return Type::U64;
        }
    }

    return Type::Invalid;
}

inline std::string_view string_view_of(const std::string &s) noexcept { return s; }
inline std::string_view string_view_of(std::string_view s) noexcept { return s; }
inline std::string_view string_view_of(const char *s) noexcept {
    return s ? std::string_view{s} : std::string_view{"(null)"};
}

inline std::string_view string_view_of(char *s) noexcept {
    return string_view_of(static_cast<const char *>(s));
}

template <typename T>
std::size_t value_size(const T &value) noexcept {
    constexpr auto code = type_of<T>();
    static_assert(code != Type::Invalid, "zxlog: unsupported log argument type");
    using V = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (has_log_adapter<V>::value) {
        return value_size(wire_value(value));
    } else if constexpr (code == Type::String) {
        const auto length = string_view_of(value).size();
        // Bound before adding the 4-byte length field (also avoids overflow).
        return length > kMaxRecordSize ? kMaxRecordSize + 1 : length + 4;
    } else if constexpr (code == Type::Boolean || code == Type::Character || code == Type::I8 ||
                         code == Type::U8) {
        return 1;
    } else if constexpr (code == Type::I16 || code == Type::U16) {
        return 2;
    } else if constexpr (code == Type::I32 || code == Type::U32 || code == Type::F32) {
        return 4;
    } else {
        return 8;
    }
}

inline void write_u16(std::uint8_t *dst, std::uint16_t v) noexcept {
    for (unsigned i = 0; i != 2; ++i) {
        dst[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

inline void write_u32(std::uint8_t *dst, std::uint32_t v) noexcept {
    for (unsigned i = 0; i != 4; ++i) {
        dst[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

inline void write_u64(std::uint8_t *dst, std::uint64_t v) noexcept {
    for (unsigned i = 0; i != 8; ++i) {
        dst[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

inline std::uint16_t read_u16(const std::uint8_t *p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (std::uint16_t(p[1]) << 8));
}

inline std::uint32_t read_u32(const std::uint8_t *p) noexcept {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) {
        v = (v << 8) | p[i];
    }

    return v;
}

inline std::uint64_t read_u64(const std::uint8_t *p) noexcept {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }

    return v;
}

template <typename T>
bool write_value(std::uint8_t *&p, const std::uint8_t *end, const T &value) noexcept {
    constexpr Type code = type_of<T>();
    static_assert(code != Type::Invalid, "zxlog: unsupported log argument type");
    using Custom = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (has_log_adapter<Custom>::value) {
        // Convert exactly once; the temporary stays alive through this call.
        return write_value(p, end, wire_value(value));
    } else if constexpr (code == Type::String) {
        const auto s = string_view_of(value);
        const auto remaining = static_cast<std::size_t>(end - p);
        if (remaining < 4 || s.size() > remaining - 4 ||
            s.size() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        write_u32(p, static_cast<std::uint32_t>(s.size()));
        p += 4;
        if (!s.empty()) {
            std::memcpy(p, s.data(), s.size());
        }

        p += s.size();
    } else if (static_cast<std::size_t>(end - p) < value_size(value)) {
        return false;
    } else if constexpr (code == Type::F32) {
        std::uint32_t raw;
        static_assert(sizeof(raw) == sizeof(value));
        std::memcpy(&raw, &value, sizeof(raw));
        write_u32(p, raw);
        p += 4;
    } else if constexpr (code == Type::F64) {
        std::uint64_t raw;
        static_assert(sizeof(raw) == sizeof(value));
        std::memcpy(&raw, &value, sizeof(raw));
        write_u64(p, raw);
        p += 8;
    } else {
        using V = std::remove_cv_t<std::remove_reference_t<T>>;
        if constexpr (std::is_enum_v<V>) {
            using Underlying = std::underlying_type_t<V>;
            return write_value(p, end, static_cast<Underlying>(value));
        } else if constexpr (code == Type::Boolean || code == Type::Character || code == Type::I8 ||
                             code == Type::U8) {
            *p++ = static_cast<std::uint8_t>(value);
        } else {
            using Unsigned = std::make_unsigned_t<V>;
            const auto raw = static_cast<std::uint64_t>(static_cast<Unsigned>(value));
            if constexpr (sizeof(V) == 2) {
                write_u16(p, static_cast<std::uint16_t>(raw));
                p += 2;
            }

            if constexpr (sizeof(V) == 4) {
                write_u32(p, static_cast<std::uint32_t>(raw));
                p += 4;
            }

            if constexpr (sizeof(V) == 8) {
                write_u64(p, raw);
                p += 8;
            }
        }
    }

    return true;
}

}  // namespace zxlog::detail
