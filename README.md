# zxlog v0.4.0

A C++17 structured event logger for Linux/ELF with a uniform `ZX_LOG(INFO, "{}", arg)` front-end, build-time LogSite dictionaries, bounded per-thread SPSC rings, and selectable TEXT, BINARY or RUNTIME backends. Binary records use a CRC32-protected v2 wire protocol. Supports batched file output, quotas/rotation, `fdatasync`, startup tail repair, and offline recovery using Python or a matching C++ {fmt}-based decoder. The text backend uses MIT-licensed {fmt} 12.1.0 headers bundled in `third_party/fmt`.

See **[README.zh-CN.md](README.zh-CN.md)** for complete architecture, usage, GN/Ninja build instructions, recovery tools, deployment constraints, and limitations.

Use `ZX_LOG(level, "format", args...)` with the uppercase token `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, or `CRITICAL`. Calls without arguments, such as `ZX_LOG(INFO, "Ready")`, are supported. Filtered calls do not evaluate their arguments. The former `LOG_*` and `ZXLOG_*` per-level macros and `ZXLOG_NO_SHORT_MACROS` switch have been removed; runtime level configuration still uses `zxlog::Level`.

`ZX_LOG(logger, level, "format", args...)` selects a custom logger; omitting it selects `Logger::instance()`. Create an initialized custom instance with `Logger::create(options, &error)`, which returns a `std::unique_ptr<Logger>` or null on failure. Set distinct output paths for each instance, including both paths in RUNTIME builds. Objects/references, raw pointers, unique pointers and shared pointers are accepted. Parenthesize complex expressions: `ZX_LOG((*ptr), INFO, "Ready")`. The logger expression is evaluated once; null or closed loggers discard lazily and never fall back to the global logger.

Pending output is flushed every 100 ms by default (`flush_interval`, zero disables). `flush_on_level` adds severity-based flushing. Independent `sync_interval` and `sync_on_level` opt into `fdatasync`; an automatic write flush alone does not guarantee durable storage.

Each instance owns its queues, worker, output files, rotation and statistics. Order and flush barriers apply within one instance. The same linked application's loggers use the same build dictionary. Each thread has a bounded cache of 32 live logger bindings; exhaustion increments the target's dropped count, and closing an instance releases its queues and cache capacity. Stop all callers before destroying an instance; `shutdown()` may overlap logging but is not a durable flush. Successful initialization is once per instance; failures may be retried.

Output paths and numeric archive families are reserved within the process before any output opens. Parent-directory aliases are normalized; overlapping families and existing active symlinks/hard-link aliases are rejected. Closing releases the reservation. External processes/filesystem operations must not modify these files concurrently. Failed creation releases resources but does not roll back completed filesystem changes. See the Chinese README for complete usage and ownership contracts.

```bash
gn gen out/Binary --args='zxlog_mode="BINARY"'
ninja -C out/Binary check

gn gen out/Text --args='zxlog_mode="TEXT"'
ninja -C out/Text check

gn gen out/Runtime --args='zxlog_mode="RUNTIME"'
ninja -C out/Runtime check
```

**Verification note (2026-11-01):** With GCC 12.3.0, GN 2318 and Ninja 1.10.1, `gn gen`, `gn check` and the full `check` target pass in all three modes, including the original 19-site demo and the separately extracted multi-logger test executable. Tests cover isolation, lifecycle, TLS limits/reclamation, path conflicts, independent rotation/mode switching, decoding and metadata stripping. The BINARY build also passes with `zxlog_optimize=false`. Source requires Linux GCC/Clang, POSIX, ELF, objcopy, Python 3, GN and Ninja for the full GN workflow.

Clang 21.1.5 `-O1 -g` unit and multi-logger checks pass with AddressSanitizer/UndefinedBehaviorSanitizer and ThreadSanitizer in RUNTIME mode, including I/O fault injection, final identity assignment and metadata stripping, with no sanitizer diagnostics. GCC 12.3.0 `-O2 -flto` RUNTIME multi-logger checks also pass.

The build now finalizes each linked ELF with its own runtime identity. `gen/build_id.txt` is a source fingerprint, not the final image ID; `Options::build_id` defaults to `linked_build_id()`. Raw linker outputs intentionally cannot initialize loggers. Archive the deployed executable, its `.zdict` and `.manifest.json` together. External builds must compile `src/identity.cc`, pass `--patched-elf` to extraction, then pass `--dict`, `--metadata-json` and `--manifest` to stripping; see the Chinese README for the full commands. Binary log and dictionary wire formats remain compatible.

GN propagates `build/zxlog_meta.ld` to executables using `zxlog`. External build systems must also compile with `-fdata-sections` and link with `-Wl,-T,/path/to/zxlog/build/zxlog_meta.ld`. The script retains metadata and collects template-local blobs that GCC 12 places in `.rodata.*` despite their `section` attribute ([GCC PR c++/88061](https://gcc.gnu.org/pipermail/gcc-bugs/2023-December/846120.html)). Extract the dictionary from the linked ELF before stripping `zxlog_meta` from the deployment image.

Important limitations: single-process log path, no hard realtime guarantees, asynchronous data not yet flushed can be lost on process exit/power failure; Python offline formatter covers common scalar {fmt} syntax rather than arbitrary C++ custom formatters. See the Chinese README for additional details.
