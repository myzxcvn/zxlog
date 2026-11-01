#include "path_lease.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

namespace zxlog::detail {
namespace {

bool archive_of(const std::string &path, const std::string &base) {
    return path.size() > base.size() + 1 && path.compare(0, base.size(), base) == 0 &&
           path[base.size()] == '.' &&
           path.find_first_not_of("0123456789", base.size() + 1) == std::string::npos;
}

bool overlaps(const std::string &a, const std::string &b) {
    return a == b || archive_of(a, b) || archive_of(b, a);
}

bool normalize(const std::string &input, std::string &output, std::string *error) {
    namespace fs = std::filesystem;
    const fs::path path(input);
    const auto name = path.filename();
    if (input.empty() || name.empty() || name == "." || name == "..") {
        if (error) {
            *error = "invalid log file path: " + input;
        }

        return false;
    }

    std::error_code ec;
    const auto parent = fs::canonical(path.has_parent_path() ? path.parent_path() : fs::path("."), ec);
    if (ec) {
        if (error) {
            *error = "invalid log directory: " + input + ": " + ec.message();
        }

        return false;
    }

    output = (parent / name).string();
    struct stat st{};
    if (::lstat(output.c_str(), &st) == 0) {
        // Parent directory aliases are normalized. Active symlinks/hard links
        // are rejected, so repair cannot modify an aliased writer's inode.
        if (!S_ISREG(st.st_mode) || st.st_nlink != 1) {
            if (error) {
                *error = "log path must be an unaliased regular file: " + output;
            }

            return false;
        }
    } else if (errno != ENOENT) {
        if (error) {
            *error = "cannot inspect log path: " + output + ": " + std::strerror(errno);
        }

        return false;
    }

    return true;
}

}  // namespace

struct PathLease::Registry {
    std::mutex mutex;
    std::vector<PathLease *> leases;
};

PathLease::PathLease(std::shared_ptr<Registry> registry, std::vector<std::string> paths)
    : registry_(std::move(registry)), paths_(std::move(paths)) {}

std::unique_ptr<PathLease> PathLease::acquire(const std::vector<std::string> &paths, std::string *error) {
    // Leases keep the registry alive across static logger destruction.
    static auto registry = std::make_shared<Registry>();
    std::vector<std::string> normalized;
    for (const auto &path : paths) {
        std::string absolute;
        if (!normalize(path, absolute, error)) {
            return nullptr;
        }

        normalized.push_back(std::move(absolute));
    }

    auto lease = std::unique_ptr<PathLease>(new PathLease(registry, std::move(normalized)));
    std::lock_guard<std::mutex> lock(registry->mutex);
    for (std::size_t i = 0; i < lease->paths_.size(); ++i) {
        const auto &path = lease->paths_[i];
        for (std::size_t j = 0; j < i; ++j) {
            if (overlaps(path, lease->paths_[j])) {
                if (error) {
                    *error = "overlapping log output paths: " + path;
                }

                return nullptr;
            }
        }

        for (const auto *other : registry->leases) {
            for (const auto &reserved : other->paths_) {
                if (overlaps(path, reserved)) {
                    if (error) {
                        *error = "log path already owned by another logger: " + path;
                    }

                    return nullptr;
                }
            }
        }
    }

    registry->leases.push_back(lease.get());
    lease->registered_ = true;
    return lease;
}

PathLease::~PathLease() {
    if (!registered_) {
        return;
    }

    std::lock_guard<std::mutex> lock(registry_->mutex);
    auto &leases = registry_->leases;
    leases.erase(std::remove(leases.begin(), leases.end(), this), leases.end());
}

}  // namespace zxlog::detail
