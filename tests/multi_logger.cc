#include "zxlog/logger.h"
#include "zxlog/zxlog.h"
#include "build_id.h"

#include <atomic>
#include <filesystem>
#include <fstream>
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
    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();
    a->shutdown();
    stop.store(true, std::memory_order_release);
    producer.join();
    survivor.join();
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
        else
            return 2;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
