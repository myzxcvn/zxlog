#include "zxlog/logger.h"
#include "zxlog/zxlog.h"
#include "build_id.h"

#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <thread>

struct ChangingText { mutable unsigned calls = 0; };
namespace zxlog {
template <> struct LogAdapter<ChangingText> {
    using Wire = std::string_view;
    static Wire to_wire(const ChangingText &value) noexcept {
        static const std::array<char, 2000> large{};
        return ++value.calls == 1 ? Wire{"once"} : Wire{large.data(), large.size()};
    }
};
}

extern "C" void zxlog_test_fault(int mode);

namespace fs = std::filesystem;

namespace {
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Fixture {
    std::string dictionary;
    fs::path root;
    zxlog::Options options(const std::string &name) const {
        zxlog::Options options;
        options.build_id = zxlog::build::kBuildId;
        options.dictionary_path = dictionary;
        options.binary_path = (root / (name + ".blog")).string();
        options.text_path = (root / (name + ".log")).string();
        options.per_thread_slots = 64;
        options.max_files = 3;
        options.sync_on_rotate = false;
#if ZXLOG_BUILD_MODE == 1
        options.initial_mode = zxlog::OutputMode::Text;
#endif
        return options;
    }
    std::unique_ptr<zxlog::Logger> create(const std::string &name) const { return create(options(name)); }
    std::unique_ptr<zxlog::Logger> create(const zxlog::Options &options) const {
        std::string error;
        auto logger = zxlog::Logger::create(options, &error);
        require(logger != nullptr, error);
        return logger;
    }
};

void flush(zxlog::Logger &logger) { require(logger.flush(true), "flush failed"); }

void report(const std::string &name, const zxlog::Logger &logger) {
    const auto stats = logger.stats();
    require(stats.dropped == stats.queue_full + stats.producer_limit + stats.tls_limit + stats.record_too_large,
            "drop categories do not account for total");
    require(stats.io_errors == 0 && stats.sync_errors == 0, "logger I/O error");
    std::cout << name << ' ' << stats.accepted << ' ' << stats.dropped << ' ' << stats.rotations << '\n';
}

void common_site(zxlog::Logger &logger, int value) { ZX_LOG(logger, INFO, "custom {}", value); }

void isolation(const Fixture &f) {
    auto &global = zxlog::Logger::instance();
    std::string error;
    require(global.initialize(f.options("global"), &error), error);
    auto a = f.create("a");
    auto b = f.create("b");
    std::shared_ptr<zxlog::Logger> shared = f.create("shared");
    for (int i = 0; i < 10; ++i) {
        ZX_LOG(INFO, "global {}", i);
        common_site(*a, i);
        common_site(*b, 100 + i);
    }
    ZX_LOG(TRACE, "level {}", 0);
    ZX_LOG(DEBUG, "level {}", 1);
    ZX_LOG(INFO, "level {}", 2);
    ZX_LOG(WARN, "level {}", 3);
    ZX_LOG(ERROR, "level {}", 4);
    ZX_LOG(CRITICAL, "level {}", 5);
    ZX_LOG(a, TRACE, "custom level {}", 0);
    ZX_LOG(a, DEBUG, "custom level {}", 1);
    ZX_LOG(a, INFO, "custom level {}", 2);
    ZX_LOG(a, WARN, "custom level {}", 3);
    ZX_LOG(a, ERROR, "custom level {}", 4);
    ZX_LOG(a, CRITICAL, "custom level {}", 5);
    ChangingText changing;
    ZX_LOG(a, INFO, "adapter {}", changing);
    require(changing.calls == 1, "adapter converted more than once");
    auto *pointer = a.get();
    ZX_LOG(pointer, INFO, "pointer");
    ZX_LOG((*pointer), INFO, "reference");
    ZX_LOG(shared, INFO, "shared");
    ZX_LOG((f.create("temporary")), INFO, "temporary");
    int lookups = 0;
    auto resolve = [&]() -> zxlog::Logger & {
        ++lookups;
        return *a;
    };
    if (true)
        ZX_LOG((resolve()), INFO, "resolved");
    else
        throw std::runtime_error("macro swallowed else");
    require(lookups == 1, "logger expression evaluated more than once");

    int side_effect = 0;
    zxlog::Logger *null_pointer = nullptr;
    std::unique_ptr<zxlog::Logger> null_unique;
    std::shared_ptr<zxlog::Logger> null_shared;
    ZX_LOG(null_pointer, ERROR, "filtered {}", ++side_effect);
    ZX_LOG(null_unique, ERROR, "filtered {}", ++side_effect);
    ZX_LOG(null_shared, ERROR, "filtered {}", ++side_effect);
    a->set_level(zxlog::Level::Error);
    ZX_LOG(a, WARN, "filtered {}", ++side_effect);
    global.set_level(zxlog::Level::Off);
    ZX_LOG(INFO, "filtered {}", ++side_effect);
    ZX_LOG(b, INFO, "custom while global disabled");
    ZX_LOG(a, ERROR, "accepted {}", ++side_effect);
    require(side_effect == 1, "filtered arguments evaluated");
    global.set_level(zxlog::Level::Trace);
    a->set_level(zxlog::Level::Trace);
    flush(global);
    flush(*a);
    flush(*b);

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 25; ++i) {
                ZX_LOG(INFO, "global thread {} {}", t, i);
                ZX_LOG(a, INFO, "a thread {} {}", t, i);
                ZX_LOG(b, INFO, "b thread {} {}", t, i);
            }
        });
    }
    for (auto &thread : threads)
        thread.join();
#if ZXLOG_BUILD_MODE == 3
    require(b->set_mode(zxlog::OutputMode::Text), "mode switch failed");
    ZX_LOG(b, INFO, "switched text");
    ZX_LOG(INFO, "global stays binary");
    ZX_LOG(a, INFO, "other stays binary");
    require(b->set_mode(zxlog::OutputMode::Binary), "mode switch failed");
    ZX_LOG(b, INFO, "switched binary");
#endif
    flush(global);
    flush(*a);
    flush(*b);
    flush(*shared);
    a->shutdown();
    ZX_LOG(a, INFO, "filtered {}", ++side_effect);
    require(side_effect == 1, "closed logger evaluated arguments");
    ZX_LOG(b, INFO, "after other closed");
    flush(*b);
    for (auto *logger : {&global, a.get(), b.get(), shared.get()}) {
        require(logger->stats().dropped == 0, "unexpected dropped messages");
    }
    report("global", global);
    report("a", *a);
    report("b", *b);
    report("shared", *shared);
    global.shutdown();
}

void diagnostics(const Fixture &f) {
    for (int mode = 1; mode <= 4; ++mode) {
        auto options = f.options("fault" + std::to_string(mode));
        options.flush_interval = std::chrono::milliseconds(0);
        options.error_interval = std::chrono::hours(1);
        options.max_file_bytes = 1100;
        std::atomic<unsigned> callbacks{0};
        std::atomic<bool> callback_ok{false};
        zxlog::Logger *raw = nullptr;
        options.error_handler = [&](const zxlog::ErrorInfo &error) {
            ++callbacks;
            const auto copy = raw->last_error();
            int evaluated = 0;
            ZX_LOG(raw, ERROR, "recursive callback {}", ++evaluated);
            const bool refused = !raw->flush() && !raw->set_mode(zxlog::OutputMode::Text);
            raw->shutdown(); // Reentrant lifecycle operations must not join the worker itself.
            std::string create_error;
            const bool no_creation = !zxlog::Logger::create(options, &create_error);
            callback_ok.store(copy.code == error.code && evaluated == 0 && refused && no_creation);
            throw std::runtime_error("handler exception is isolated");
        };
        auto logger = f.create(options);
        raw = logger.get();
        zxlog_test_fault(mode);
        const int count = mode == 4 ? 8 : 1;
        for (int i = 0; i < count; ++i)
            ZX_LOG(logger, INFO, "fault payload {}", std::string(300, 'x'));
        require(!logger->flush(true), "fault did not reach flush result");
        require(logger->output_failed(), "failed output not observable");
        auto error = logger->last_error();
        const auto expected = mode == 3 ? zxlog::ErrorCode::Sync :
                              mode == 4 ? zxlog::ErrorCode::Rotate : zxlog::ErrorCode::Write;
        require(error.code == expected && error.system_error == (mode == 3 ? EIO : mode == 4 ? EACCES : ENOSPC)
                && !error.path.empty() && !error.operation.empty() && error.steady_ns != 0,
                "missing fault context");
        require(callbacks.load() == 1 && callback_ok.load(), "handler rate limit/reentrancy failed");
        ZX_LOG(logger, INFO, "after failed writer");
        require(!logger->flush(), "failed writer became healthy without recovery");
        require(logger->last_error().operation == error.operation && callbacks.load() == 1,
                "original failure overwritten");
        const auto stats = logger->stats();
        require((mode <= 2 && stats.write_errors > 0) || (mode == 3 && stats.sync_errors > 0) ||
                (mode == 4 && stats.rotate_errors > 0), "fault category missing");
        zxlog_test_fault(0);
        logger->shutdown();
        options.error_handler = {};
        auto recovered = f.create(options);
        ZX_LOG(recovered, INFO, "recovered");
        flush(*recovered);
        require(!recovered->output_failed(), "fresh instance retained error state");
    }
#if ZXLOG_BUILD_MODE != 2
    auto options = f.options("format_error");
    options.initial_mode = zxlog::OutputMode::Text;
    auto format = f.create(options);
    ZX_LOG(format, INFO, "{:{}}", "bad width", -1);
    ZX_LOG(format, INFO, "after bad format");
    require(!format->flush(), "format error not reported");
    require(format->stats().format_errors == 1 && !format->output_failed() &&
            format->last_error().code == zxlog::ErrorCode::Format, "format error misclassified");
#endif
    auto periodic_options = f.options("periodic_fault");
    periodic_options.flush_interval = std::chrono::milliseconds(0);
    periodic_options.sync_interval = std::chrono::milliseconds(10);
    auto periodic_failure = f.create(periodic_options);
    zxlog_test_fault(3);
    ZX_LOG(periodic_failure, INFO, "periodic sync failure");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!periodic_failure->output_failed() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(periodic_failure->output_failed() && !periodic_failure->flush(),
            "periodic sync did not reach fdatasync");
    zxlog_test_fault(0);
    periodic_failure->shutdown();
    auto options_producer = f.options("producer_limit");
    options_producer.max_producers = 1;
    auto producer = f.create(options_producer);
    ZX_LOG(producer, INFO, "owns producer slot");
    std::thread extra([&] { ZX_LOG(producer, INFO, "no producer slot"); });
    extra.join();
    flush(*producer);
    require(producer->stats().producer_limit == 1 && producer->stats().dropped == 1,
            "producer exhaustion misclassified");
}

void periodic(const Fixture &f) {
    const auto wait_for_output = [&](const zxlog::Options &options) {
        const auto path = ZXLOG_BUILD_MODE == 1 ? options.text_path : options.binary_path;
        const auto initial = ZXLOG_BUILD_MODE == 1 ? 0u : zxlog::kBinaryHeaderSize;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (fs::file_size(path) <= initial && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        require(fs::file_size(path) > initial, "automatic flush did not write output");
    };
    auto options = f.options("timer");
    auto timer = f.create(options);
    ZX_LOG(timer, INFO, "timer event");
    wait_for_output(options); // No explicit flush, shutdown or batch threshold.

    options = f.options("manual");
    options.flush_interval = std::chrono::milliseconds(0);
    auto manual = f.create(options);
    ZX_LOG(manual, INFO, "manual event");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    require(fs::file_size(ZXLOG_BUILD_MODE == 1 ? options.text_path : options.binary_path) ==
            (ZXLOG_BUILD_MODE == 1 ? 0u : zxlog::kBinaryHeaderSize), "disabled timer flushed");
    flush(*manual);

    options = f.options("severity");
    options.flush_interval = std::chrono::milliseconds(0);
    options.flush_on_level = zxlog::Level::Error;
    auto severity = f.create(options);
    ZX_LOG(severity, INFO, "buffered before error");
    ZX_LOG(severity, ERROR, "severity event");
    wait_for_output(options);

    options = f.options("sync_timer");
    options.flush_interval = std::chrono::milliseconds(0);
    options.sync_interval = std::chrono::milliseconds(20);
    auto synced = f.create(options);
    ZX_LOG(synced, INFO, "sync timer event");
    wait_for_output(options);
    flush(*synced);

    options = f.options("sync_level");
    options.flush_interval = std::chrono::milliseconds(0);
    options.sync_on_level = zxlog::Level::Error;
    auto sync_level = f.create(options);
    ZX_LOG(sync_level, ERROR, "sync level event");
    wait_for_output(options);
    flush(*sync_level);
    for (auto *logger : {timer.get(), manual.get(), severity.get(), synced.get(), sync_level.get()})
        require(logger->stats().io_errors == 0, "automatic flush failed");
    options.flush_interval = std::chrono::milliseconds(-1);
    std::string error;
    require(!zxlog::Logger::create(options, &error), "negative interval accepted");
}

void text_limits(const Fixture &f) {
#if ZXLOG_BUILD_MODE != 2
    auto options = f.options("text_limits");
    options.initial_mode = zxlog::OutputMode::Text;
    options.max_file_bytes = 1100;
    auto logger = f.create(options);
    ZX_LOG(logger, INFO, "before oversized text");
    ZX_LOG(logger, INFO, "{:2000}", 1);
    ZX_LOG(logger, INFO, "{:{}}", 1, 1000000);
    ZX_LOG(logger, INFO, "after oversized text");
    flush(*logger);
    require(logger->stats().accepted == 4 && logger->stats().text_too_large == 2 &&
            logger->stats().io_errors == 0, "oversize text poisoned output");
    options = f.options("text_budget");
    options.initial_mode = zxlog::OutputMode::Text;
    options.max_text_bytes = 48;
    auto budget = f.create(options);
    ZX_LOG(budget, INFO, "{:60}", 1);
    ZX_LOG(budget, INFO, "budget survives");
    flush(*budget);
    require(budget->stats().text_too_large == 1 && budget->stats().io_errors == 0,
            "text budget ignored");
#else
    (void)f;
#endif
}

void encoding(const Fixture &f) {
    auto logger = f.create("encoding");
    ChangingText changing;
    const auto emit = [&] { ZX_LOG(logger, INFO, "adapter {}", changing); };
    emit();
    emit(); // The second conversion is oversize and must not publish a record.
    ZX_LOG(logger, INFO, "partial {} {}", 42, std::string(2000, 'x'));
    ZX_LOG(logger, INFO, "after rejected records");
    flush(*logger);
    require(changing.calls == 2, "adapter conversion count changed");
    require(logger->stats().accepted == 2 && logger->stats().dropped == 2,
            "partial/oversize record published");
    report("encoding", *logger);
}

void churn(const Fixture &f) {
    std::vector<std::unique_ptr<zxlog::Logger>> closed;
    for (unsigned i = 0; i < zxlog::Logger::kMaxLoggersPerThread * 3; ++i) {
        auto logger = f.create("churn");
        ZX_LOG(logger, INFO, "generation {}", i);
        flush(*logger);
        require(logger->stats().accepted == 1 && logger->stats().dropped == 0, "stale TLS binding");
        logger->shutdown();
        if (i % 2)
            closed.push_back(std::move(logger));
    }
}

void capacity(const Fixture &f) {
    std::vector<std::unique_ptr<zxlog::Logger>> loggers;
    for (unsigned i = 0; i <= zxlog::Logger::kMaxLoggersPerThread; ++i) {
        auto options = f.options("capacity" + std::to_string(i));
        options.max_producers = 1;
        options.per_thread_slots = 2;
        loggers.push_back(f.create(options));
        auto &logger = loggers.back();
        ZX_LOG(logger, INFO, "capacity {}", i);
    }
    require(loggers.back()->stats().accepted == 0 && loggers.back()->stats().dropped == 1,
            "TLS bound ignored");
    loggers.front()->shutdown();
    auto &last = loggers.back();
    ZX_LOG(last, INFO, "recovered slot");
    for (unsigned i = 1; i < loggers.size(); ++i)
        flush(*loggers[i]);
    require(last->stats().accepted == 1, "closed logger still owns TLS capacity");
    for (unsigned i = 0; i < loggers.size(); ++i)
        report("capacity" + std::to_string(i), *loggers[i]);
}

void thread_churn(const Fixture &f) {
    auto options = f.options("a");
    options.max_producers = 1;
    auto a = f.create(options);
    options = f.options("b");
    options.max_producers = 1;
    auto b = f.create(options);
    for (int i = 0; i < 40; ++i) {
        std::thread producer([&, i] {
            common_site(*a, i);
            common_site(*b, 100 + i);
        });
        producer.join();
        flush(*a);
        flush(*b);
    }
    require(a->stats().accepted == 40 && b->stats().accepted == 40, "thread queues not recycled");
    require(a->stats().dropped == 0 && b->stats().dropped == 0, "thread churn dropped messages");
    report("a", *a);
    report("b", *b);
}

void concurrent_close(const Fixture &f) {
    auto a = f.create("closing");
    auto b = f.create("survivor");
    std::atomic<bool> started{false}, stop{false};
    std::thread producer([&] {
        int i = 0;
        do {
            ZX_LOG(a, INFO, "closing {}", i++);
            started.store(true, std::memory_order_release);
        } while (!stop.load(std::memory_order_acquire));
    });
    std::thread survivor([&] {
        for (int i = 0; i < 200; ++i) {
            ZX_LOG(b, INFO, "survivor {}", i);
            if (i % 16 == 15)
                flush(*b);
        }
    });
    std::atomic<unsigned> controls_started{0};
    std::vector<std::thread> controllers;
    for (unsigned i = 0; i < 4; ++i) {
        controllers.emplace_back([&] {
            flush(*a);
            controls_started.fetch_add(1, std::memory_order_release);
            while (a->flush()) {
#if ZXLOG_BUILD_MODE == 3
                (void)a->set_mode(zxlog::OutputMode::Text);
                (void)a->set_mode(zxlog::OutputMode::Binary);
#endif
                require(a->stats().io_errors == 0 && a->last_error().code == zxlog::ErrorCode::None,
                        "concurrent controls reported an error");
            }
        });
    }
    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();
    while (controls_started.load(std::memory_order_acquire) != controllers.size())
        std::this_thread::yield();
    a->shutdown();
    stop.store(true, std::memory_order_release);
    producer.join();
    survivor.join();
    for (auto &controller : controllers) controller.join();
    require(!a->flush() && !a->set_mode(zxlog::OutputMode::Text), "closed logger accepted a control");
    flush(*b);
    require(b->stats().accepted == 200 && b->stats().dropped == 0, "closing another logger disrupted writer");
    report("closing", *a);
    report("survivor", *b);
}

std::string bytes(const std::string &path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string &active_path(zxlog::Options &options) {
#if ZXLOG_BUILD_MODE == 1
    return options.text_path;
#else
    return options.binary_path;
#endif
}

void conflicts(const Fixture &f) {
    auto options = f.options("live");
    auto &global = zxlog::Logger::instance();
    std::string error;
    auto failed = f.options("initial_failure");
#if ZXLOG_BUILD_MODE != 1
    {
        std::ofstream stream(failed.binary_path);
        stream << "not a log";
    }
#else
    failed.dictionary_path = (f.root / "missing.zdict").string();
#endif
    require(!global.initialize(failed, &error), "invalid global initialization accepted");
    int side_effect = 0;
    ZX_LOG(INFO, "filtered {}", ++side_effect);
    require(side_effect == 0, "failed initialization published logger");
    // Exercise rollback after output resources exist but before the worker starts.
    struct ThrowingHandler {
        bool *fail;
        explicit ThrowingHandler(bool *value) : fail(value) {}
        ThrowingHandler(const ThrowingHandler &other) : fail(other.fail) {
            if (*fail) throw std::runtime_error("injected handler copy failure");
        }
        void operator()(const zxlog::ErrorInfo &) const {}
    };
    bool fail_copy = false;
    auto failed_copy = f.options("copy_failure");
    failed_copy.error_handler = ThrowingHandler(&fail_copy);
    fail_copy = true;
    require(!global.initialize(failed_copy, &error) && error == "injected handler copy failure",
            "handler copy failure was not rolled back");
    require(!global.enabled(zxlog::Level::Info), "exception published logger");
    fail_copy = false;
    auto after_copy_failure = f.create(failed_copy); // Identical paths must be released.
    ZX_LOG(after_copy_failure, INFO, "after copy failure");
    flush(*after_copy_failure);
    report("copy_failure", *after_copy_failure);
    require(global.initialize(options, &error), error);
    ZX_LOG(INFO, "live");
    flush(global);
    const auto before_binary = bytes(options.binary_path);
    const auto before_text = bytes(options.text_path);
    const auto rejected = [&](const zxlog::Options &candidate) {
        require(!zxlog::Logger::create(candidate, &error) && !error.empty(), "conflict accepted");
        require(bytes(options.binary_path) == before_binary && bytes(options.text_path) == before_text,
                "conflict modified active output");
        require(!fs::exists(active_path(options) + ".1"), "conflict rotated live output");
    };
    rejected(options);
    auto wrong_identity = f.options("wrong_identity");
    wrong_identity.build_id[0] ^= 1;
    rejected(wrong_identity);
    auto alias = f.options("./live");
    rejected(alias);
    fs::create_directory_symlink(f.root, f.root / "alias");
    rejected(f.options("alias/live"));
    auto archive = f.options("archive");
    active_path(archive) = active_path(options) + ".1";
    rejected(archive);
#if ZXLOG_BUILD_MODE == 3
    auto text_alias = f.options("text_alias");
    text_alias.text_path = options.text_path;
    rejected(text_alias);
    auto internal = f.options("internal");
    internal.binary_path = internal.text_path;
    rejected(internal);
    require(!fs::exists(internal.text_path), "internal conflict created a file");
#endif
    auto linked = f.options("linked");
    fs::create_symlink(active_path(options), active_path(linked));
    rejected(linked);
    fs::remove(active_path(linked));
    fs::create_hard_link(active_path(options), active_path(linked));
    rejected(linked);
    fs::remove(active_path(linked));

    auto bad = f.options("retry");
#if ZXLOG_BUILD_MODE != 1
    {
        std::ofstream stream(bad.binary_path);
        stream << "not a log";
    }
    require(!zxlog::Logger::create(bad, &error), "corrupt binary accepted");
    fs::remove(bad.binary_path);
#else
    bad.dictionary_path = (f.root / "missing.zdict").string();
    require(!zxlog::Logger::create(bad, &error), "missing dictionary accepted");
    bad.dictionary_path = f.dictionary;
#endif
    auto retry = f.create(bad);
    ZX_LOG(retry, INFO, "retry succeeds");
    flush(*retry);
    report("retry", *retry);
    global.shutdown();
    require(!global.initialize(options, &error), "closed instance restarted");
    auto replacement = f.create(options);
    ZX_LOG(replacement, INFO, "replacement");
    flush(*replacement);
    require(bytes(active_path(options) + ".1") == (ZXLOG_BUILD_MODE == 1 ? before_text : before_binary),
            "replacement lost old log");
}



class CaptureSink final : public zxlog::Sink {
public:
    explicit CaptureSink(zxlog::OutputMode mode) : mode_(mode) {}
    zxlog::OutputMode mode() const noexcept override { return mode_; }
    bool append(const std::uint8_t *bytes, std::size_t size) noexcept override {
        try {
            records.emplace_back(reinterpret_cast<const char *>(bytes), size);
            return true;
        } catch (...) { return false; }
    }
    bool flush(bool) noexcept override { ++flushes; return true; }
    void close() noexcept override { closed = true; }
    std::vector<std::string> records;
    unsigned flushes = 0;
    bool closed = false;
private:
    zxlog::OutputMode mode_;
};

void shared_dispatcher(const Fixture &f) {
    const auto trace = [](const char *message) {
        if (std::getenv("ZXLOG_TEST_TRACE")) std::cout << "TRACE: " << message << std::endl;
    };
    trace("start");
    auto dispatcher = std::make_shared<zxlog::Dispatcher>();
    auto aopts = f.options("dispatch_a");
    auto bopts = f.options("dispatch_b");
    aopts.dispatcher = dispatcher;
    bopts.dispatcher = dispatcher;
    auto a = f.create(aopts);
    auto b = f.create(bopts);
    require(dispatcher->logger_count() == 2, "shared dispatcher registration failed");
    trace("registered");
    for (int i = 0; i != 100; ++i) {
        ZX_LOG(a, INFO, "shared a {}", i);
        ZX_LOG(b, INFO, "shared b {}", i);
        if (i % 16 == 15) { flush(*a); flush(*b); trace("batch flushed"); }
    }
    flush(*a);
    flush(*b);
    trace("all flushed");
    a->shutdown();
    trace("a shutdown");
    require(dispatcher->logger_count() == 1, "unregistered Logger remains");
    ZX_LOG(b, INFO, "shared after close");
    flush(*b);
    trace("b flushed again");
    require(a->stats().dropped == 0 && b->stats().dropped == 0, "shared dispatcher dropped events");
    report("dispatch_a", *a);
    report("dispatch_b", *b);
    b->shutdown();
    trace("b shutdown");
    require(dispatcher->logger_count() == 0, "shared dispatcher leaked registration");
}

void custom_sink(const Fixture &f) {
    auto opts = f.options("with_sink");
    auto capture = std::make_shared<CaptureSink>(opts.initial_mode);
    opts.extra_sinks.push_back(capture);
    auto logger = f.create(opts);
    ZX_LOG(logger, INFO, "sink {}", 19);
    flush(*logger);
    require(capture->records.size() == 1 && capture->flushes > 0, "custom Sink failed to receive event");
    if (opts.initial_mode == zxlog::OutputMode::Text) {
        require(capture->records[0].find("sink 19") != std::string::npos, "text Sink got wrong payload");
    } else {
        const auto *p = reinterpret_cast<const std::uint8_t*>(capture->records[0].data());
        require(capture->records[0].size() >= zxlog::kRecordHeaderSize &&
                zxlog::detail::read_u32(p) == capture->records[0].size(),
                "binary Sink did not receive a complete event");
    }
    report("with_sink", *logger);
    logger->shutdown();
    require(capture->closed, "custom Sink not closed after draining");
}

void resource_budget(const Fixture &f) {
    auto opts = f.options("small_queue");
    opts.max_producers = 2;
    opts.per_thread_slots = 8;
    opts.max_record_bytes = 128;
    opts.max_queue_bytes = 2304; // 16 * (128 + sizeof(slot metadata)), 16*16
    auto logger = f.create(opts);
    const auto before = logger->stats();
    require(before.queue_payload_bytes == 2048 && before.queue_memory_bytes <= opts.max_queue_bytes,
            "producer queue storage not bounded");
    ZX_LOG(logger, INFO, "small {}", 7);
    ZX_LOG(logger, INFO, "large {}", std::string(500, 'x'));
    flush(*logger);
    require(logger->stats().record_too_large == 1, "oversized record not rejected");
    report("small_queue", *logger);
    logger->shutdown();
    auto rejected = f.options("rejected_budget");
    rejected.max_producers = 2;
    rejected.per_thread_slots = 8;
    rejected.max_record_bytes = 128;
    rejected.max_queue_bytes = 2048; // payload alone; metadata must count
    std::string error;
    require(!zxlog::Logger::create(rejected, &error) &&
            error == "SPSC queue memory budget exceeded", "insufficient budget accepted");
    require(!fs::exists(rejected.binary_path) && !fs::exists(rejected.text_path),
            "rejected memory budget created output");
}

void rotation(const Fixture &f) {
    auto options = f.options("rotate");
    options.max_file_bytes = 1800;
    auto rotating = f.create(options);
    auto other = f.create("other");
    for (int i = 0; i < 200; ++i) {
        ZX_LOG(rotating, INFO, "rotation payload abcdefghijklmnopqrstuvwxyz0123456789 {}", i);
        if (i % 10 == 9) {
            flush(*rotating);
            ZX_LOG(other, INFO, "other batch {}", i);
        }
    }
    flush(*rotating);
    flush(*other);
    require(rotating->stats().rotations > 0 && other->stats().rotations == 0, "rotation not independent");
    require(rotating->stats().dropped == 0 && other->stats().dropped == 0, "rotation lost queued messages");
    report("rotate", *rotating);
    report("other", *other);
}
}  // namespace

int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    try {
        Fixture fixture{argv[2], fs::absolute(argv[3])};
        const std::string scenario = argv[1];
        if (scenario == "isolation")
            isolation(fixture);
        else if (scenario == "diagnostics")
            diagnostics(fixture);
        else if (scenario == "periodic")
            periodic(fixture);
        else if (scenario == "text_limits")
            text_limits(fixture);
        else if (scenario == "encoding")
            encoding(fixture);
        else if (scenario == "churn")
            churn(fixture);
        else if (scenario == "capacity")
            capacity(fixture);
        else if (scenario == "thread_churn")
            thread_churn(fixture);
        else if (scenario == "close")
            concurrent_close(fixture);
        else if (scenario == "conflicts")
            conflicts(fixture);
        else if (scenario == "rotation")
            rotation(fixture);
        else if (scenario == "resource_budget")
            resource_budget(fixture);
        else if (scenario == "custom_sink")
            custom_sink(fixture);
        else if (scenario == "shared_dispatcher")
            shared_dispatcher(fixture);
        else
            return 2;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
