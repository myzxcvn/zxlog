#pragma once

#include "zxlog/protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace zxlog::detail {

struct DictionaryEntry {
    std::uint32_t id = 0;
    std::uint32_t line = 0;
    Level level = Level::Trace;
    std::string file;
    std::string format;
    std::vector<Type> types;
};

class Dictionary {
public:
    bool load(const std::string& path, const std::array<std::uint8_t, 16>& build_id,
              std::string* error);
    const DictionaryEntry* lookup(std::uint32_t id) const noexcept;
private:
    std::unordered_map<std::uint32_t, DictionaryEntry> entries_;
};

// Returns false on truncated/malformed payload or unsupported format.
bool render_text(const DictionaryEntry& entry, const std::uint8_t* data,
                 std::size_t size, std::string& out);
const char* level_name(Level level) noexcept;

} // namespace zxlog::detail
