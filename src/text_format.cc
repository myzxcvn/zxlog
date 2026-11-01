#define FMT_HEADER_ONLY
#include "text_format.h"
#include "zxlog/codec.h"
#include <fmt/args.h>
#include <fmt/format.h>
#include <cstring>
#include <fstream>
#include <vector>
#include <utility>

namespace zxlog::detail {
namespace {

struct Arg {
    Type type = Type::Invalid;
    std::int64_t signed_value = 0;
    std::uint64_t unsigned_value = 0;
    double floating = 0.0;
    std::string text;
};

std::uint64_t uread(const std::uint8_t *p, std::size_t count) noexcept {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < count; ++i) {
        v |= std::uint64_t(p[i]) << (8 * i);
    }

    return v;
}

std::int64_t sread(const std::uint8_t *p, std::size_t count) noexcept {
    const auto bits = uread(p, count);
    if (count == 8) {
        std::int64_t s;
        std::memcpy(&s, &bits, sizeof(s));
        return s;
    }

    const auto sign = std::uint64_t(1) << (8 * count - 1);
    if ((bits & sign) == 0) {
        return static_cast<std::int64_t>(bits);
    }

    return static_cast<std::int64_t>(bits) - static_cast<std::int64_t>(std::uint64_t(1) << (8 * count));
}

bool decode_one(Type type, const std::uint8_t *&p, const std::uint8_t *end, Arg &arg) {
    const auto remaining = static_cast<std::size_t>(end - p);
    arg.type = type;
    if (type == Type::String) {
        if (remaining < 4) {
            return false;
        }

        const auto n = read_u32(p);
        p += 4;
        if (n > static_cast<std::size_t>(end - p)) {
            return false;
        }

        arg.text.assign(reinterpret_cast<const char *>(p), n);
        p += n;
        return true;
    }

    std::size_t count = 0;
    if (type >= Type::I8 && type <= Type::U8) {
        count = 1;
    } else if (type == Type::Boolean || type == Type::Character) {
        count = 1;
    } else if (type == Type::I16 || type == Type::U16) {
        count = 2;
    } else if (type == Type::I32 || type == Type::U32 || type == Type::F32) {
        count = 4;
    } else if (type == Type::I64 || type == Type::U64 || type == Type::F64) {
        count = 8;
    } else {
        return false;
    }

    if (remaining < count) {
        return false;
    }

    arg.unsigned_value = uread(p, count);
    arg.signed_value = sread(p, count);
    if (type == Type::F32) {
        const auto u = static_cast<std::uint32_t>(arg.unsigned_value);
        float f;
        std::memcpy(&f, &u, sizeof(f));
        arg.floating = f;
    } else if (type == Type::F64) {
        const auto u = arg.unsigned_value;
        std::memcpy(&arg.floating, &u, sizeof(arg.floating));
    }

    p += count;
    return true;
}

}  // namespace

bool Dictionary::load(const std::string &path, const std::array<std::uint8_t, 16> &build_id,
                      std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "dictionary not found: " + path;
        }

        return false;
    }

    in.seekg(0, std::ios::end);
    const auto file_size = in.tellg();
    if (file_size < 36 || file_size > 64 * 1024 * 1024) {
        if (error) {
            *error = "dictionary size invalid";
        }

        return false;
    }

    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
    if (!in.read(reinterpret_cast<char *>(bytes.data()), bytes.size())) {
        if (error) {
            *error = "failed to read dictionary";
        }

        return false;
    }

    const std::uint8_t *p = bytes.data();
    const std::uint8_t *end = p + bytes.size();
    if (std::memcmp(p, "ZXDICT1\0", 8) != 0 || read_u16(p + 8) != 1 || read_u16(p + 10) != 36 ||
        std::memcmp(p + 12, build_id.data(), 16) != 0) {
        if (error) {
            *error = "dictionary version or build ID mismatch";
        }

        return false;
    }

    const auto count = read_u32(p + 28);
    if (count > 1000000) {
        if (error) {
            *error = "too many dictionary records";
        }

        return false;
    }

    p += 36;
    std::unordered_map<std::uint32_t, DictionaryEntry> parsed;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (static_cast<std::size_t>(end - p) < 16) {
            if (error) {
                *error = "truncated dictionary entry";
            }

            return false;
        }

        DictionaryEntry entry;
        entry.id = read_u32(p);
        entry.line = read_u32(p + 4);
        const auto raw_level = p[8];
        const std::uint8_t argc = p[9];
        const std::size_t file_len = read_u16(p + 10);
        const std::size_t fmt_len = read_u16(p + 12);
        p += 16;
        if (raw_level > static_cast<std::uint8_t>(Level::Critical) ||
            static_cast<std::size_t>(end - p) < argc + file_len + fmt_len) {
            if (error) {
                *error = "invalid dictionary entry";
            }

            return false;
        }

        entry.level = static_cast<Level>(raw_level);
        for (unsigned j = 0; j < argc; ++j) {
            entry.types.push_back(static_cast<Type>(*p++));
        }

        entry.file.assign(reinterpret_cast<const char *>(p), file_len);
        p += file_len;
        entry.format.assign(reinterpret_cast<const char *>(p), fmt_len);
        p += fmt_len;
        if (!parsed.emplace(entry.id, std::move(entry)).second) {
            if (error) {
                *error = "duplicate dictionary event ID";
            }

            return false;
        }
    }

    if (p != end) {
        if (error) {
            *error = "unexpected dictionary trailing bytes";
        }

        return false;
    }

    entries_ = std::move(parsed);
    return true;
}

const DictionaryEntry *Dictionary::lookup(std::uint32_t id) const noexcept {
    auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second;
}

bool render_text(const DictionaryEntry &entry, const std::uint8_t *data, std::size_t size, std::string &out) {
    std::vector<Arg> args;
    args.reserve(entry.types.size());
    const std::uint8_t *p = data;
    const std::uint8_t *end = p + size;
    for (const Type type : entry.types) {
        Arg arg;
        if (!decode_one(type, p, end, arg)) {
            return false;
        }

        args.emplace_back(std::move(arg));
    }

    if (p != end) {
        return false;
    }

    fmt::dynamic_format_arg_store<fmt::format_context> store;
    for (const auto &arg : args) {
        switch (arg.type) {
        case Type::I8:
        case Type::I16:
        case Type::I32:
        case Type::I64:
            store.push_back(arg.signed_value);
            break;
        case Type::U8:
        case Type::U16:
        case Type::U32:
        case Type::U64:
            store.push_back(arg.unsigned_value);
            break;
        case Type::Boolean:
            store.push_back(arg.unsigned_value != 0);
            break;
        case Type::Character:
            store.push_back(static_cast<char>(arg.unsigned_value));
            break;
        case Type::F32:
            store.push_back(static_cast<float>(arg.floating));
            break;
        case Type::F64:
            store.push_back(arg.floating);
            break;
        case Type::String:
            store.push_back(arg.text);
            break;
        default:
            return false;
        }
    }

    try {
        out = fmt::vformat(entry.format, store);
        return true;
    } catch (const fmt::format_error &) {
        return false;
    }
}

const char *level_name(Level level) noexcept {
    switch (level) {
    case Level::Trace:
        return "TRACE";
    case Level::Debug:
        return "DEBUG";
    case Level::Info:
        return "INFO";
    case Level::Warn:
        return "WARN";
    case Level::Error:
        return "ERROR";
    case Level::Critical:
        return "CRITICAL";
    default:
        return "UNKNOWN";
    }
}

}  // namespace zxlog::detail
