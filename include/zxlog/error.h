#pragma once

#include <cstdint>
#include <string>

namespace zxlog {

enum class ErrorCode { None, InvalidRecord, UnknownSite, Format, Open, Write, Sync, Rotate, Internal };

struct ErrorInfo {
    ErrorCode code = ErrorCode::None;
    int system_error = 0;
    std::uint64_t steady_ns = 0;
    std::string operation;
    std::string path;
};

}  // namespace zxlog
