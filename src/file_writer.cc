#include "file_writer.h"

#include "zxlog/codec.h"
#include "zxlog/crc32.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace zxlog::detail {
namespace {

bool exists(const std::string &path) { return ::access(path.c_str(), F_OK) == 0; }
std::string archived(const std::string &name, unsigned index) { return name + "." + std::to_string(index); }
std::string parent_dir(const std::string &path) {
    auto p = path.find_last_of('/');
    if (p == std::string::npos) {
        return ".";
    }

    return p == 0 ? "/" : path.substr(0, p);
}

bool read_at(int fd, std::uint8_t *p, std::size_t n, off_t offset) {
    while (n != 0) {
        const ssize_t count = ::pread(fd, p, n, offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }

        if (count <= 0) {
            return false;
        }

        p += count;
        n -= static_cast<std::size_t>(count);
        offset += count;
    }

    return true;
}

}  // namespace

FileWriter::~FileWriter() { close(); }

void FileWriter::fail(const char *reason, int err) {
    failed_ = true;
    error_ = reason;
    if (err != 0) {
        error_ += ": ";
        error_ += std::strerror(err);
    }
}

bool FileWriter::open(const Config &config, const std::array<std::uint8_t, kBinaryHeaderSize> &header,
                      bool binary, std::string *error) {
    if (initialized_) {
        if (error) {
            *error = "file writer already opened";
        }

        return false;
    }

    cfg_ = config;
    binary_ = binary;
    header_ = header;
    if (cfg_.path.empty() || cfg_.max_files < 1 || cfg_.max_files > 256 || cfg_.batch_bytes == 0 ||
        cfg_.batch_bytes > (1u << 20) ||
        cfg_.max_file_bytes < (binary_ ? kBinaryHeaderSize + kMaxRecordSize : kMaxRecordSize) ||
        cfg_.max_file_bytes > (std::uint64_t(1) << 40)) {
        if (error) {
            *error = "invalid file writer quota/batch configuration";
        }

        return false;
    }

    pending_.reserve(cfg_.batch_bytes + kMaxRecordSize);
    if (!prune_excess_archives()) {
        if (error) {
            *error = error_;
        }

        return false;
    }

    if (exists(cfg_.path)) {
        if (cfg_.repair_on_start && !repair_existing()) {
            if (error) {
                *error = error_;
            }

            return false;
        }

        // A torn header has no recoverable events; don't archive an empty file.
        struct stat current{};
        if (::stat(cfg_.path.c_str(), &current) == 0 && current.st_size == 0) {
            if (::unlink(cfg_.path.c_str()) != 0 || !create_file()) {
                if (error) {
                    *error = error_.empty() ? "remove empty previous file failed" : error_;
                }

                return false;
            }
        } else if (!rotate()) {
            // Do not append records from a previous process/clock epoch.
            if (error) {
                *error = error_;
            }

            return false;
        }
    } else if (!create_file()) {
        if (error) {
            *error = error_;
        }

        return false;
    }

    initialized_ = true;
    return true;
}

bool FileWriter::prune_excess_archives() {
    const std::string dir = parent_dir(cfg_.path);
    const auto slash = cfg_.path.find_last_of('/');
    const std::string base = cfg_.path.substr(slash == std::string::npos ? 0 : slash + 1) + ".";
    DIR *handle = ::opendir(dir.c_str());
    if (!handle) {
        fail("open log directory failed", errno);
        return false;
    }

    for (dirent *entry = ::readdir(handle); entry; entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (name.compare(0, base.size(), base) != 0) {
            continue;
        }

        const std::string suffix = name.substr(base.size());
        if (suffix.empty() || suffix.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }

        if (suffix.size() > 9) {
            continue;  // don't interpret huge numeric suffixes
        }

        const unsigned long index = std::strtoul(suffix.c_str(), nullptr, 10);
        if (index < cfg_.max_files) {
            continue;
        }

        if (::unlink((dir + "/" + name).c_str()) != 0 && errno != ENOENT) {
            const int saved = errno;
            ::closedir(handle);
            fail("prune old log archive failed", saved);
            return false;
        }
    }

    ::closedir(handle);
    return true;
}

bool FileWriter::create_file() {
    fd_ = ::open(cfg_.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        fail("create log file failed", errno);
        return false;
    }

    size_ = 0;
    if (binary_ && !write_all(header_.data(), header_.size())) {
        return false;
    }

    // Persist the new file name, not just the data inode. A durable flush
    // then also covers a freshly created log after a cold start.
    return !cfg_.sync_on_rotate || sync_directory();
}

bool FileWriter::repair_existing() {
    int fd = ::open(cfg_.path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fail("open existing log for repair", errno);
        return false;
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 0) {
        fail("stat existing log failed", errno);
        ::close(fd);
        return false;
    }

    std::uint64_t total = static_cast<std::uint64_t>(st.st_size);
    std::uint64_t valid = 0;
    if (binary_) {
        std::array<std::uint8_t, 8> prefix{};
        if (total != 0 && (total < 8 || !read_at(fd, prefix.data(), 8, 0) ||
                           (std::memcmp(prefix.data(), "ZXLOG01\0", 8) != 0 &&
                            std::memcmp(prefix.data(), "ZXLOG02\0", 8) != 0))) {
            fail("unrecognized existing binary log; refusing to overwrite");
            ::close(fd);
            return false;
        }

        // A partially written v2 header cannot contain a complete record.
        if (total > 0 && total < kBinaryHeaderSize && std::memcmp(prefix.data(), "ZXLOG02\0", 8) == 0) {
            valid = 0;
        } else if (total >= 48) {
            std::array<std::uint8_t, kBinaryHeaderSize> header{};
            const bool legacy = total >= 48 && read_at(fd, header.data(), 48, 0) &&
                                std::memcmp(header.data(), "ZXLOG01\0", 8) == 0 &&
                                read_u16(header.data() + 8) == 1 && read_u16(header.data() + 10) == 48;
            const bool modern = !legacy && total >= kBinaryHeaderSize &&
                                read_at(fd, header.data(), header.size(), 0) &&
                                std::memcmp(header.data(), "ZXLOG02\0", 8) == 0 &&
                                read_u16(header.data() + 8) == kWireVersion &&
                                read_u16(header.data() + 10) == kBinaryHeaderSize &&
                                read_u32(header.data() + 60) == crc32(header.data(), 60);
            if (!legacy && !modern) {
                fail("existing binary header invalid; refusing to overwrite");
                ::close(fd);
                return false;
            }

            const std::uint64_t header_size = legacy ? 48 : kBinaryHeaderSize;
            const std::size_t min_rec = legacy ? 16 : kRecordHeaderSize;
            const std::size_t max_rec = legacy ? 512 : kMaxRecordSize;
            valid = header_size;
            std::array<std::uint8_t, kMaxRecordSize> rec{};
            while (valid + min_rec <= total) {
                if (!read_at(fd, rec.data(), min_rec, static_cast<off_t>(valid))) {
                    break;
                }

                const std::uint32_t length = read_u32(rec.data());
                if (length < min_rec || length > max_rec || valid + length > total) {
                    break;
                }

                if (!read_at(fd, rec.data(), length, static_cast<off_t>(valid))) {
                    break;
                }

                if (modern && read_u32(rec.data() + 16) != record_crc32(rec.data(), length)) {
                    break;
                }

                valid += length;
            }
        }
    } else {
        // Text: truncate a torn trailing line (if any) before archiving.
        std::array<std::uint8_t, 4096> buf{};
        std::uint64_t cursor = total;
        while (cursor > 0) {
            const auto block = static_cast<std::size_t>(std::min<std::uint64_t>(cursor, buf.size()));
            cursor -= block;
            if (!read_at(fd, buf.data(), block, static_cast<off_t>(cursor))) {
                fail("read existing text log failed", errno);
                ::close(fd);
                return false;
            }

            bool found = false;
            for (std::size_t i = block; i != 0; --i) {
                if (buf[i - 1] == '\n') {
                    valid = cursor + i;
                    found = true;
                    break;
                }
            }

            if (found) {
                break;
            }
        }
    }

    if (valid < total) {
        if (::ftruncate(fd, static_cast<off_t>(valid)) != 0) {
            fail("repair truncate failed", errno);
            ::close(fd);
            return false;
        }

        repaired_bytes_ += total - valid;
        if (cfg_.sync_on_rotate && ::fdatasync(fd) != 0) {
            ++sync_errors_;
            fail("repair fdatasync failed", errno);
            ::close(fd);
            return false;
        }
    }

    ::close(fd);
    return true;
}

bool FileWriter::sync_file() {
    if (fd_ < 0) {
        return true;
    }

    if (::fdatasync(fd_) == 0) {
        unsynced_records_ = 0;
        return true;
    }

    ++sync_errors_;
    fail("fdatasync failed", errno);
    return false;
}

bool FileWriter::sync_directory() {
    int directory = ::open(parent_dir(cfg_.path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        fail("open log directory for fsync", errno);
        return false;
    }

    const int result = ::fsync(directory);
    const int saved = errno;
    ::close(directory);
    if (result != 0) {
        ++sync_errors_;
        fail("directory fsync failed", saved);
        return false;
    }

    return true;
}

bool FileWriter::rotate() {
    if (fd_ >= 0) {
        if (!flush_pending()) {
            return false;
        }

        if (cfg_.sync_on_rotate && !sync_file()) {
            return false;
        }

        ::close(fd_);
        fd_ = -1;
    }

    if (cfg_.max_files == 1) {
        if (::unlink(cfg_.path.c_str()) != 0 && errno != ENOENT) {
            fail("remove previous active log failed", errno);
            return false;
        }
    } else {
        (void)::unlink(archived(cfg_.path, cfg_.max_files - 1).c_str());
        for (unsigned i = cfg_.max_files - 1; i > 1; --i) {
            const auto from = archived(cfg_.path, i - 1);
            if (exists(from) && ::rename(from.c_str(), archived(cfg_.path, i).c_str()) != 0) {
                fail("rotate archive failed", errno);
                return false;
            }
        }
        if (exists(cfg_.path) && ::rename(cfg_.path.c_str(), archived(cfg_.path, 1).c_str()) != 0) {
            fail("rotate active file failed", errno);
            return false;
        }
    }

    if (!create_file()) {
        return false;
    }

    ++rotations_;
    return true;
}

bool FileWriter::write_all(const std::uint8_t *p, std::size_t n) {
    while (n != 0) {
        const ssize_t written = ::write(fd_, p, n);
        if (written < 0 && errno == EINTR) {
            continue;
        }

        if (written <= 0) {
            fail("write log file failed", written < 0 ? errno : EIO);
            return false;
        }

        p += written;
        n -= static_cast<std::size_t>(written);
        size_ += static_cast<std::uint64_t>(written);
    }

    return true;
}

bool FileWriter::flush_pending() {
    if (failed_) {
        return false;
    }

    if (!pending_.empty()) {
        if (!write_all(pending_.data(), pending_.size())) {
            return false;
        }

        pending_.clear();
    }

    return true;
}

bool FileWriter::append(const std::uint8_t *bytes, std::size_t size) {
    if (failed_ || fd_ < 0 || (binary_ && size > kMaxRecordSize) ||
        cfg_.max_file_bytes < (binary_ ? kBinaryHeaderSize : 0) + size) {
        fail("log event exceeds configured file limit");
        return false;
    }

    if (size_ + pending_.size() + size > cfg_.max_file_bytes) {
        if (!rotate()) {
            return false;
        }
    }

    pending_.insert(pending_.end(), bytes, bytes + size);
    ++unsynced_records_;
    if (pending_.size() >= cfg_.batch_bytes && !flush_pending()) {
        return false;
    }

    if (cfg_.sync_every_records > 0 && unsynced_records_ >= cfg_.sync_every_records) {
        return flush(true);
    }

    return true;
}

bool FileWriter::flush(bool durable) {
    if (!flush_pending()) {
        return false;
    }

    return !durable || sync_file();
}

void FileWriter::close() noexcept {
    if (fd_ >= 0) {
        (void)flush_pending();
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace zxlog::detail
