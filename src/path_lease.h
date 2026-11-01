#pragma once

#include <memory>
#include <string>
#include <vector>

namespace zxlog::detail {

// Reserves active paths and their numeric archive families within this process.
// Acquire all outputs before opening any of them; release only after closing.
class PathLease {
public:
    static std::unique_ptr<PathLease> acquire(const std::vector<std::string> &paths, std::string *error);

    ~PathLease();
    PathLease(const PathLease &) = delete;
    PathLease &operator=(const PathLease &) = delete;
    const std::vector<std::string> &paths() const noexcept { return paths_; }

private:
    struct Registry;
    PathLease(std::shared_ptr<Registry> registry, std::vector<std::string> paths);
    std::shared_ptr<Registry> registry_;
    std::vector<std::string> paths_;
    bool registered_ = false;
};

}  // namespace zxlog::detail
