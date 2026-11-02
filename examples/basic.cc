#include "zxlog/zxlog.h"

#include "build_id.h"
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>
#include <string>

struct SensorCode {
    std::uint32_t number;
};
namespace zxlog {

template <>
struct LogAdapter<SensorCode> {
    using Wire = std::uint32_t;
    static constexpr Wire to_wire(const SensorCode &x) noexcept { return x.number; }
};

}  // namespace zxlog

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "usage: zxlog_demo <binary|text|switch|stress|rotate> <dict> <prefix>\n";
        return 2;
    }

    const std::string mode = argv[1];
    const std::string prefix = argv[3];
    zxlog::Options opts;
    opts.dictionary_path = argv[2];
    opts.text_path = prefix + ".log";
    opts.binary_path = prefix + ".blog";
    opts.build_id = zxlog::build::kBuildId;
    opts.max_producers = mode == "churn" ? 2 : 8;
    opts.per_thread_slots = mode == "stress" ? 16 : 128;
    opts.io_batch_bytes = 1024;
    opts.max_file_bytes = mode == "rotate" ? 1800 : (1u << 20);
    opts.max_files = 3;
    opts.level = zxlog::Level::Trace;
    opts.initial_mode = mode == "text" ? zxlog::OutputMode::Text : zxlog::OutputMode::Binary;
    std::string error;
    auto &logger = zxlog::Logger::instance();
    if (!logger.initialize(opts, &error)) {
        std::cerr << "zxlog initialization failed: " << error << '\n';
        return 3;
    }

    [[maybe_unused]] std::uint32_t speed = 1200;
    [[maybe_unused]] float current = 1.52f;
    ZX_LOG(INFO, "Motor speed: {}, current: {:.2f}", speed, current);
    ZX_LOG(DEBUG, "Pose: ({:.2f}, {:.2f})", 1.25, 3.50);
    ZX_LOG(WARN, "Sensor ID: {:04x}", std::uint32_t{0x2a});
    ZX_LOG(ERROR, "Escaped {{}} error code: {}", std::int32_t{-3});
    ZX_LOG(INFO, "Ready");
    ZX_LOG(INFO, "Sensor: {}", std::string_view{"LiDAR"});
    ZX_LOG(INFO, "Enabled: {} char: {}", true, 'R');
    ZX_LOG(INFO, "Aligned: '{:>8}'", "LiDAR");
    ZX_LOG(INFO, "Manual args: {1} {0}", "first", "second");
    ZX_LOG(INFO, "Strong type: {:06X}", SensorCode{0x2a});
    ZX_LOG(INFO, "Dynamic width: [{:>{}}]", "go", 6);
    ZX_LOG(INFO, "Signed exp: {:+.3e}", 3.25);
    ZX_LOG(INFO, "Alt hex: {:#08x}", std::uint32_t{42});

    int side_effect = 0;
    logger.set_level(zxlog::Level::Off);
    ZX_LOG(INFO, "Should not execute: {}", ++side_effect);
    logger.set_level(zxlog::Level::Trace);
    if (side_effect != 0) {
        return 4;
    }

    if (mode == "stress") {
        std::vector<std::thread> producers;
        for (std::uint32_t worker = 0; worker < 4; ++worker) {
            producers.emplace_back([worker] {
                for (std::uint32_t seq = 0; seq < 500; ++seq) {
                    ZX_LOG(DEBUG, "worker={} sequence={}", worker, seq);
                }
            });
        }

        for (auto &p : producers) {
            p.join();
        }
    }

    if (mode == "churn") {
        for (unsigned i = 0; i < 20; ++i) {
            std::thread producer([i] { ZX_LOG(INFO, "churn worker={}", i); });
            producer.join();
            // Confirm prior producer's ring was drained before next registration.
            if (!logger.flush()) {
                return 9;
            }
        }
    }

    if (mode == "rotate") {
        for (int i = 0; i < 300; ++i) {
            ZX_LOG(INFO, "rotation sequence={:05d}", i);
        }
    }

    if (mode == "switch") {
        if (!logger.set_mode(zxlog::OutputMode::Text)) {
            return 5;
        }

        ZX_LOG(INFO, "Phase: {}", "text");
        if (!logger.set_mode(zxlog::OutputMode::Binary)) {
            return 6;
        }

        ZX_LOG(INFO, "Phase: {}", "binary");
    }

    if (!logger.flush(true)) {
        std::cerr << "I/O flush error\n";
        return 7;
    }

    logger.shutdown();
    const auto s = logger.stats();
    std::cout << "accepted=" << s.accepted << " dropped=" << s.dropped << " io_errors=" << s.io_errors
              << " rotations=" << s.rotations << " repaired_bytes=" << s.repaired_bytes
              << " sync_errors=" << s.sync_errors << '\n';
    return ((mode == "stress" || mode == "rotate" || s.dropped == 0) && s.io_errors == 0 &&
            s.sync_errors == 0)
               ? 0
               : 8;
}
