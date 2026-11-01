# zxlog v0.4.0

A C++17 structured event logger for Linux/ELF with a uniform `LOG_INFO("{}", arg)` front-end, build-time LogSite dictionaries, bounded per-thread SPSC rings, and selectable TEXT, BINARY or RUNTIME backends. Binary records use a CRC32-protected v2 wire protocol. Supports batched file output, quotas/rotation, `fdatasync`, startup tail repair, and offline recovery using Python or a matching C++ {fmt}-based decoder. The text backend uses MIT-licensed {fmt} 12.1.0 headers bundled in `third_party/fmt`.

See **[README.zh-CN.md](README.zh-CN.md)** for complete architecture, usage, GN/Ninja build instructions, recovery tools, deployment constraints, and limitations.

```bash
gn gen out/Binary --args='zxlog_mode="BINARY"'
ninja -C out/Binary check

gn gen out/Text --args='zxlog_mode="TEXT"'
ninja -C out/Text check

gn gen out/Runtime --args='zxlog_mode="RUNTIME"'
ninja -C out/Runtime check
```

**Verification note (2026-11-01):** With GCC 12.3.0, GN 2318 and Ninja 1.10.1, `gn gen`, `gn check`, unit tests and C++ decoder builds pass in all three modes. The full `check` target fails during dictionary extraction because the linked ELF lacks the `zxlog_meta` section; integration tests therefore have not run successfully on this toolchain. Source requires Linux GCC/Clang, POSIX, ELF, objcopy, Python 3, GN and Ninja for the full GN workflow.

Important limitations: single-process log path, no hard realtime guarantees, asynchronous data not yet flushed can be lost on process exit/power failure; Python offline formatter covers common scalar {fmt} syntax rather than arbitrary C++ custom formatters. See the Chinese README for additional details.
