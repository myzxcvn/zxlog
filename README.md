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

**Verification note (2026-11-01):** With GCC 12.3.0, GN 2318 and Ninja 1.10.1, `gn gen`, `gn check` and the full `check` target pass in all three modes, including extraction of 19 LogSites and metadata stripping. The BINARY build also passes with `zxlog_optimize=false`. Source requires Linux GCC/Clang, POSIX, ELF, objcopy, Python 3, GN and Ninja for the full GN workflow.

GN propagates `build/zxlog_meta.ld` to executables using `zxlog`. External build systems must also compile with `-fdata-sections` and link with `-Wl,-T,/path/to/zxlog/build/zxlog_meta.ld`. The script retains metadata and collects template-local blobs that GCC 12 places in `.rodata.*` despite their `section` attribute ([GCC PR c++/88061](https://gcc.gnu.org/pipermail/gcc-bugs/2023-December/846120.html)). Extract the dictionary from the linked ELF before stripping `zxlog_meta` from the deployment image.

Important limitations: single-process log path, no hard realtime guarantees, asynchronous data not yet flushed can be lost on process exit/power failure; Python offline formatter covers common scalar {fmt} syntax rather than arbitrary C++ custom formatters. See the Chinese README for additional details.
