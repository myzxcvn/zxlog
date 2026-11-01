#include <atomic>
#include <cerrno>
#include <cstddef>
#include <sys/types.h>

// Link-time syscall injection exists only in the multi-logger test executable.
namespace {
std::atomic<int> fault{0};
std::atomic<bool> partial{false};
}
extern "C" void zxlog_test_fault(int mode) {
    partial.store(false);
    fault.store(mode);
}
extern "C" ssize_t __real_write(int, const void *, std::size_t);
extern "C" int __real_fdatasync(int);
extern "C" int __real_rename(const char *, const char *);
extern "C" ssize_t __wrap_write(int fd, const void *data, std::size_t size) {
    const auto mode = fault.load();
    if (mode == 2 && !partial.exchange(true) && size > 1) return __real_write(fd, data, 1);
    if (mode == 1 || mode == 2) { errno = ENOSPC; return -1; }
    return __real_write(fd, data, size);
}
extern "C" int __wrap_fdatasync(int fd) {
    if (fault.load() == 3) { errno = EIO; return -1; }
    return __real_fdatasync(fd);
}
extern "C" int __wrap_rename(const char *from, const char *to) {
    if (fault.load() == 4) { errno = EACCES; return -1; }
    return __real_rename(from, to);
}
