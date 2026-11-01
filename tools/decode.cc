#include "zxlog/crc32.h"
#include "zxlog/codec.h"
#include "text_format.h"
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size <= 0 || size > 1024 * 1024 * 1024)
        throw std::runtime_error("invalid/oversized binary log");
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(data.data()), size))
        throw std::runtime_error("cannot read " + path);
    return data;
}
struct Header {
    bool modern = false;
    std::size_t size = 0;
    std::array<std::uint8_t, 16> build_id{};
};
Header read_header(const std::vector<std::uint8_t>& data) {
    Header h;
    if (data.size() >= 64 && std::memcmp(data.data(), "ZXLOG02\0", 8) == 0 &&
        zxlog::detail::read_u16(data.data() + 8) == 2 &&
        zxlog::detail::read_u16(data.data() + 10) == 64) {
        if (zxlog::detail::crc32(data.data(), 60) !=
            zxlog::detail::read_u32(data.data() + 60))
            throw std::runtime_error("file header CRC32 mismatch");
        h.modern = true;
        h.size = 64;
    } else if (data.size() >= 48 &&
               std::memcmp(data.data(), "ZXLOG01\0", 8) == 0 &&
               zxlog::detail::read_u16(data.data() + 8) == 1 &&
               zxlog::detail::read_u16(data.data() + 10) == 48) {
        h.size = 48;
    } else {
        throw std::runtime_error("invalid binary log header");
    }
    std::memcpy(h.build_id.data(), data.data() + 12, h.build_id.size());
    return h;
}

bool render_at(const std::vector<std::uint8_t>& data, std::size_t offset,
               bool modern, const zxlog::detail::Dictionary& dict,
               std::uint64_t& stamp, std::string& line, std::size_t& length) {
    const std::size_t minimum = modern ? 20 : 16;
    const std::size_t maximum = modern ? zxlog::kMaxRecordSize : 512;
    if (data.size() - offset < minimum) return false;
    const auto* rec = data.data() + offset;
    length = zxlog::detail::read_u32(rec);
    if (length < minimum || length > maximum || length > data.size() - offset)
        return false;
    if (modern && zxlog::detail::read_u32(rec + 16) !=
        zxlog::detail::record_crc32(rec, length)) return false;
    const auto* entry = dict.lookup(zxlog::detail::read_u32(rec + 4));
    if (!entry) return false;
    std::string message;
    if (!zxlog::detail::render_text(*entry, rec + minimum,
                                    length - minimum, message)) return false;
    stamp = zxlog::detail::read_u64(rec + 8);
    line = std::string{"["} + zxlog::detail::level_name(entry->level) + "] " + message;
    return true;
}

void restore(const std::string& path, const std::string& dictionary, bool recover) {
    const auto data = read_file(path);
    const Header h = read_header(data);
    zxlog::detail::Dictionary dict;
    std::string error;
    if (!dict.load(dictionary, h.build_id, &error))
        throw std::runtime_error(error);
    std::size_t pos = h.size;
    std::uint64_t first_stamp = 0;
    bool first = true;
    while (pos < data.size()) {
        std::uint64_t stamp = 0;
        std::size_t length = 0;
        std::string message;
        if (!render_at(data, pos, h.modern, dict, stamp, message, length)) {
            if (!recover) throw std::runtime_error("bad event at offset " + std::to_string(pos));
            const auto start = pos;
            do { ++pos; }
            while (pos < data.size() &&
                   !render_at(data, pos, h.modern, dict, stamp, message, length));
            std::cerr << "zxlog recovery: skipped " << pos - start << " bytes in " << path << '\n';
            if (pos >= data.size()) return;
        }
        if (first) { first_stamp = stamp; first = false; }
        const double seconds = stamp >= first_stamp ?
            static_cast<double>(stamp - first_stamp) / 1e9 : 0.0;
        std::cout.setf(std::ios::fixed);
        std::cout.precision(3);
        std::cout << "[" << seconds << "s] " << message << '\n';
        pos += length;
    }
}
} // namespace

int main(int argc, char* argv[]) {
    std::string dictionary;
    std::vector<std::string> paths;
    bool recover = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dict" && i + 1 < argc) dictionary = argv[++i];
        else if (arg == "--input" && i + 1 < argc) paths.emplace_back(argv[++i]);
        else if (arg == "--recover") recover = true;
        else {
            std::cerr << "usage: zxlog_decode --dict file.zdict --input file.blog "
                         "[--input file.blog.1] [--recover]\n";
            return 2;
        }
    }
    if (dictionary.empty() || paths.empty()) {
        std::cerr << "requires --dict and at least one --input\n";
        return 2;
    }
    try {
        for (const auto& path : paths) restore(path, dictionary, recover);
    } catch (const std::exception& ex) {
        std::cerr << "zxlog decode error: " << ex.what() << '\n';
        return 1;
    }
}
