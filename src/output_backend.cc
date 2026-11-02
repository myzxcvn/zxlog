#include "zxlog/detail/output_backend.h"
#include "zxlog/detail/diagnostics.h"
#include "zxlog/detail/log_record.h"
#include "zxlog/options.h"
#include "zxlog/crc32.h"
#include "file_writer.h"
#include "path_lease.h"
#if ZXLOG_BUILD_MODE != 2
#include "text_format.h"
#endif
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <unistd.h>

namespace zxlog::detail {
namespace {
[[maybe_unused]] std::uint64_t ns_wall() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

std::uint64_t ns_steady() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

}  // namespace

OutputBackend::OutputBackend() = default;
OutputBackend::~OutputBackend() = default;

bool OutputBackend::open(const Options &options, std::string *error) {
#if ZXLOG_BUILD_MODE != 2
    auto dictionary = std::make_unique<Dictionary>();
    if (!dictionary->load(options.dictionary_path, options.build_id, error)) {
        return false;
    }
#endif
    std::vector<std::string> paths;
#if ZXLOG_BUILD_MODE != 2
    paths.push_back(options.text_path);
#endif
#if ZXLOG_BUILD_MODE != 1
    paths.push_back(options.binary_path);
#endif
    auto lease = PathLease::acquire(paths, error);
    if (!lease) {
        return false;
    }

    std::size_t path_index = 0;
    FileWriter::Config fc;
    fc.max_file_bytes = options.max_file_bytes;
    fc.max_files = options.max_files;
    fc.batch_bytes = options.io_batch_bytes;
    fc.sync_every_records = options.sync_every_records;
    fc.repair_on_start = options.repair_on_start;
    fc.sync_on_rotate = options.sync_on_rotate;
#if ZXLOG_BUILD_MODE != 1
    const auto wall = ns_wall();
#endif
    const auto steady = ns_steady();
#if ZXLOG_BUILD_MODE != 2
    auto text_file = std::make_unique<FileWriter>();
    fc.path = lease->paths()[path_index++];
    if (!text_file->open(fc, {}, false, error)) {
        return false;
    }
#endif
#if ZXLOG_BUILD_MODE != 1
    std::array<std::uint8_t, kBinaryHeaderSize> header{};
    std::memcpy(header.data(), "ZXLOG03\0", 8);
    write_u16(header.data() + 8, kWireVersion);
    write_u16(header.data() + 10, kBinaryHeaderSize);
    std::memcpy(header.data() + 12, options.build_id.data(), options.build_id.size());
    write_u64(header.data() + 28, wall);
    write_u64(header.data() + 36, steady);
    write_u32(header.data() + 44, static_cast<std::uint32_t>(::getpid()));
    write_u64(header.data() + 48, wall ^ (steady << 1));
    write_u32(header.data() + 56, static_cast<std::uint32_t>(steady >> 32));
    write_u32(header.data() + 60, crc32(header.data(), 60));
    auto binary_file = std::make_unique<FileWriter>();
    fc.path = lease->paths()[path_index++];
    if (!binary_file->open(fc, header, true, error)) {
        return false;
    }
#endif
    // Keep only configuration used after startup; file quotas live in writers.
    policy_ = {options.flush_interval, options.sync_interval,
               options.flush_on_level, options.sync_on_level};
    active_mode_ = options.initial_mode;
#if ZXLOG_BUILD_MODE != 2
    dictionary_path_ = options.dictionary_path;
    text_path_ = options.text_path;
    text_limit_ = static_cast<std::size_t>(std::min<std::uint64_t>(options.max_text_bytes, options.max_file_bytes));
    start_steady_ns_ = steady;
    dictionary_ = std::move(dictionary);
    text_file_ = std::move(text_file);
#endif
#if ZXLOG_BUILD_MODE != 1
    binary_file_ = std::move(binary_file);
#endif
    path_lease_ = std::move(lease);
    return true;
}

void OutputBackend::close() noexcept {
    if (text_file_) text_file_->close();
    if (binary_file_) binary_file_->close();
    path_lease_.reset();
}

void OutputBackend::reset() noexcept {
    text_file_.reset();
    binary_file_.reset();
#if ZXLOG_BUILD_MODE != 2
    dictionary_.reset();
#endif
    path_lease_.reset();
}

bool OutputBackend::has_pending() const noexcept {
    return (text_file_ && text_file_->has_pending()) || (binary_file_ && binary_file_->has_pending());
}

bool OutputBackend::needs_sync() const noexcept {
    return (text_file_ && text_file_->needs_sync()) || (binary_file_ && binary_file_->needs_sync());
}

void OutputBackend::set_mode(OutputMode mode, Diagnostics &diagnostics) {
    // Flush the old output before consuming events in the new mode.
    (void)flush(false, diagnostics);
    active_mode_ = mode;
}

void OutputBackend::write(const LogRecord &slot, Diagnostics &diagnostics) {
    const auto *bytes = slot.bytes.data();
    if (slot.length < kRecordHeaderSize ||
        read_u32(bytes + 16) != record_crc32(bytes, slot.length)) {
        diagnostics.report(ErrorCode::InvalidRecord, "record CRC/length validation");
        return;
    }
#if ZXLOG_BUILD_MODE != 1
    if (active_mode_ == OutputMode::Binary) {
        if (!binary_file_->append(bytes, slot.length)) {
            diagnostics.file_error(binary_file_->last_error());
        }

        return;
    }
#endif
#if ZXLOG_BUILD_MODE != 2
    const auto id = read_u32(bytes + 4);
    const auto *entry = dictionary_->lookup(id);
    if (!entry) {
        diagnostics.report(ErrorCode::UnknownSite, "dictionary lookup", dictionary_path_);
        return;
    }

    const auto stamp = read_u64(bytes + 8);
    const auto delta = stamp >= start_steady_ns_ ? stamp - start_steady_ns_ : 0;
    std::ostringstream out;
    out << "[+" << delta / 1000000000ull << '.' << std::setw(3) << std::setfill('0')
        << (delta / 1000000ull) % 1000ull << "] [" << level_name(entry->level) << "] ";
    auto line = out.str();
    const auto limit = text_limit_;
    if (line.size() + 1 > limit) {
        diagnostics.reject_text();
        return;
    }
    std::string rendered;
    bool too_large = false;
    if (!render_text(*entry, bytes + kRecordHeaderSize, slot.length - kRecordHeaderSize,
                             rendered, limit - line.size() - 1, &too_large)) {
        if (too_large) diagnostics.reject_text();
        else diagnostics.report(ErrorCode::Format, "render text", text_path_);
        return;
    }
    line += rendered;
    line += '\n';
    if (!text_file_->append(reinterpret_cast<const std::uint8_t *>(line.data()), line.size())) {
        diagnostics.file_error(text_file_->last_error());
    }
#endif
}

bool OutputBackend::flush(bool durable, Diagnostics &diagnostics) {
    bool okay = true;
    for (auto *writer : {text_file_.get(), binary_file_.get()}) {
        if (!writer) continue;
        try {
            if (!writer->flush(durable)) {
                diagnostics.file_error(writer->last_error());
                okay = false;
            }
        } catch (...) {
            diagnostics.report(ErrorCode::Internal, "flush exception");
            okay = false;
        }
    }
    return okay;
}

void OutputBackend::add_stats(Stats &stats) const noexcept {
    if (binary_file_) {
        stats.rotations += binary_file_->rotations();
        stats.repaired_bytes += binary_file_->repaired_bytes();
        stats.sync_errors += binary_file_->sync_errors();
    }

    if (text_file_) {
        stats.rotations += text_file_->rotations();
        stats.repaired_bytes += text_file_->repaired_bytes();
        stats.sync_errors += text_file_->sync_errors();
    }

}

}  // namespace zxlog::detail
