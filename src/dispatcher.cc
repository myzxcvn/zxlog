#include "zxlog/dispatcher.h"
#include "zxlog/detail/async_runtime.h"
#include <algorithm>
#include <chrono>
#include <utility>

namespace zxlog {

Dispatcher::Dispatcher() : worker_([this] { run(); }) {}

Dispatcher::~Dispatcher() {
    // Every AsyncRuntime keeps its Dispatcher alive until detached.
    // Therefore destruction requires no registered Logger.
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        stopping_ = true;
    }
    notify();
    if (worker_.joinable()) worker_.join();
}

std::shared_ptr<Dispatcher::Entry> Dispatcher::attach(detail::AsyncRuntime *runtime) {
    auto entry = std::make_shared<Entry>(runtime);
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        registry_.push_back(entry);
    }
    notify();
    return entry;
}

void Dispatcher::detach(const std::shared_ptr<Entry> &entry) noexcept {
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        registry_.erase(std::remove(registry_.begin(), registry_.end(), entry), registry_.end());
    }
    {
        std::unique_lock<std::mutex> lock(entry->mutex);
        entry->detaching = true;
        entry->cv.wait(lock, [&] { return entry->active == 0; });
    }
}

std::size_t Dispatcher::logger_count() const noexcept {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    return registry_.size();
}

void Dispatcher::run() noexcept {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(wake_mutex_);
            if (stopping_) break;
        }
        std::vector<std::shared_ptr<Entry>> snapshot;
        try {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            snapshot = registry_; // keeps each Entry alive after unlocking
        } catch (...) {
            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(5));
            continue;
        }
        bool progress = false;
        for (const auto &entry : snapshot) {
            {
                std::lock_guard<std::mutex> lock(entry->mutex);
                if (entry->detaching) continue;
                ++entry->active;
            }
            // Do NOT hold registry or Entry mutex across user Sink callbacks,
            // disk I/O, fdatasync or event formatting.
            try {
                for (unsigned n = 0; n != 32; ++n) {
                    if (!entry->runtime->process_one()) break;
                    progress = true;
                }
            } catch (...) {
                // AsyncRuntime::process_one normally catches all exceptions.
            }
            {
                std::lock_guard<std::mutex> lock(entry->mutex);
                --entry->active;
                if (entry->detaching) entry->cv.notify_all();
            }
        }
        if (!progress) {
            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(5));
        }
    }
}
}  // namespace zxlog
