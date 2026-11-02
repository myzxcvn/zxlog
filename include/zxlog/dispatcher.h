#pragma once
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace zxlog {
namespace detail { class AsyncRuntime; }

// Optional one-worker executor shared between independent loggers.
// Callbacks and file I/O are serialized on this worker. A slow Sink can
// delay other loggers, but it never holds the registration mutex during I/O.
class Dispatcher {
public:
    Dispatcher();
    ~Dispatcher();
    Dispatcher(const Dispatcher &) = delete;
    Dispatcher &operator=(const Dispatcher &) = delete;
    std::size_t logger_count() const noexcept;
private:
    friend class detail::AsyncRuntime;
    struct Entry {
        explicit Entry(detail::AsyncRuntime *owner) : runtime(owner) {}
        detail::AsyncRuntime *runtime;
        std::mutex mutex;
        std::condition_variable cv;
        bool detaching = false;
        unsigned active = 0; // guarded by mutex
    };
    std::shared_ptr<Entry> attach(detail::AsyncRuntime *runtime);
    void detach(const std::shared_ptr<Entry> &entry) noexcept;
    void notify() noexcept { wake_cv_.notify_one(); }
    void run() noexcept;
    mutable std::mutex registry_mutex_;
    std::vector<std::shared_ptr<Entry>> registry_;
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    bool stopping_ = false; // written only by destructor after registrations gone
    std::thread worker_;
};
}  // namespace zxlog
