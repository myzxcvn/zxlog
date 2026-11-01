#include "zxlog/zxlog.h"
#include "build_id.h"
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>
#include <string>

struct SensorCode { std::uint32_t number; };
namespace zxlog {
template<> struct LogAdapter<SensorCode> {
    using Wire = std::uint32_t;
    static constexpr Wire to_wire(const SensorCode& x) noexcept { return x.number; }
};
}
int main(int argc, char** argv) {
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
    auto& logger = zxlog::Logger::instance();
    if (!logger.initialize(opts, &error)) {
        std::cerr << "zxlog initialization failed: " << error << '\n';
        return 3;
    }

    std::uint32_t speed = 1200;
    float current = 1.52f;
    LOG_INFO("Motor speed: {}, current: {:.2f}", speed, current);
    LOG_DEBUG("Pose: ({:.2f}, {:.2f})", 1.25, 3.50);
    LOG_WARN("Sensor ID: {:04x}", std::uint32_t{0x2a});
    LOG_ERROR("Escaped {{}} error code: {}", std::int32_t{-3});
    LOG_INFO("Ready");
    LOG_INFO("Sensor: {}", std::string_view{"LiDAR"});
    LOG_INFO("Enabled: {} char: {}", true, 'R');
    LOG_INFO("Aligned: '{:>8}'", "LiDAR");
    LOG_INFO("Manual args: {1} {0}", "first", "second");
    LOG_INFO("Strong type: {:06X}", SensorCode{0x2a});
    LOG_INFO("Dynamic width: [{:>{}}]", "go", 6);
    LOG_INFO("Signed exp: {:+.3e}", 3.25);
    LOG_INFO("Alt hex: {:#08x}", std::uint32_t{42});

    int side_effect = 0;
    logger.set_level(zxlog::Level::Off);
    LOG_INFO("Should not execute: {}", ++side_effect);
    logger.set_level(zxlog::Level::Trace);
    if (side_effect != 0) return 4;
    if (mode == "stress") {
        std::vector<std::thread> producers;
        for (std::uint32_t worker = 0; worker < 4; ++worker) {
            producers.emplace_back([worker] {
                for (std::uint32_t seq = 0; seq < 500; ++seq)
                    LOG_DEBUG("worker={} sequence={}", worker, seq);
            });
        }
        for (auto& p : producers) p.join();
    }
    if (mode == "churn") {
        for (unsigned i = 0; i < 20; ++i) {
            std::thread producer([i] { LOG_INFO("churn worker={}", i); });
            producer.join();
            // Confirm prior producer's ring was drained before next registration.
            if (!logger.flush()) return 9;
        }
    }
    if (mode == "rotate") {
        for (int i = 0; i < 300; ++i)
            LOG_INFO("rotation sequence={:05d}", i);
    }
    if (mode == "switch") {
        if (!logger.set_mode(zxlog::OutputMode::Text)) return 5;
        LOG_INFO("Phase: {}", "text");
        if (!logger.set_mode(zxlog::OutputMode::Binary)) return 6;
        LOG_INFO("Phase: {}", "binary");
    }
    if (!logger.flush(true)) { std::cerr << "I/O flush error\n"; return 7; }
    logger.shutdown();
    const auto s = logger.stats();
    std::cout << "accepted=" << s.accepted << " dropped=" << s.dropped
              << " io_errors=" << s.io_errors << " rotations=" << s.rotations
              << " repaired_bytes=" << s.repaired_bytes << " sync_errors=" << s.sync_errors << '\n';
    return ((mode == "stress" || mode == "rotate" || s.dropped == 0) &&
            s.io_errors == 0 && s.sync_errors == 0) ? 0 : 8;
}
