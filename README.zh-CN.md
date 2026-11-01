# zxlog v0.4.0 — C++17 结构化日志框架

**设计目标**：业务使用 `ZX_LOG(INFO, "...{}", arg)` 或 `ZX_LOG(logger, INFO, "...{}", arg)`，所有生产线程只序列化类型化参数，每个 logger 的独立后台线程根据构建/运行模式输出文本或紧凑二进制。静态格式字符串、LogID、参数类型、源码位置在 **链接后从 ELF 提取为字典**；没有开机 LogSite 注册。

> 平台：Linux/ELF + GCC/Clang，C++17，POSIX。2026-11-01 使用 GCC 12.3.0、GN 2318、Ninja 1.10.1 验证：三种模式的 `gn gen`、`gn check` 和完整 `check` 通过，包含 19 个日志点的字典提取、集成测试及部署镜像元数据剥离。需要 `python3`、`objcopy`、`gn` 和 `ninja` 执行正式 GN 流程。

## 0.4 功能及设计边界

| 能力 | 状态 / 实现 |
| --- | --- |
| 统一日志宏，级别过滤时参数不求值 | ✅ `ZX_LOG(level, ...)`，级别为 `TRACE / DEBUG / INFO / WARN / ERROR / CRITICAL` |
| 自定义 logger | ✅ `Logger::create()`，`ZX_LOG(logger, level, ...)`；省略 logger 使用全局实例 |
| TEXT / BINARY / RUNTIME 三种构建模式 | ✅ 全部走同一条二进制事件序列化路径 |
| 编译期格式校验 | ✅ C++17 `{fmt}` `FMT_STRING`（内置 MIT-licensed fmt 12.1.0 头文件） |
| 类型化参数 / 自定义类型 | ✅ 整数、浮点、布尔、字符、枚举、字符串，`LogAdapter<T>` 单标量适配 |
| 建立完整字典 | ✅ ELF `zxlog_meta` 节 + 构建期提取 + EventID 碰撞检查 |
| 生产端无需格式化/动态分配 | ✅ 稳态日志热路径只操作预分配 SPSC 环形缓冲区；首次线程注册使用智能指针 |
| 统一队列顺序与模式切换屏障 | ✅ 实例内单调序号 + 按序消费，`flush`/`set_mode` 为控制事件 |
| 批量文件写入与轮转 | ✅ 后台批量写、按大小轮转、保留上限、清理超限的旧归档 |
| 二进制记录 CRC32 和文件头 CRC32 | ✅ 协议 v2，小端编码、独立校验 |
| 异常退出后恢复 | ✅ 下次启动检查有效记录前缀、截断损坏尾部、归档旧启动日志 |
| 立即持久化请求 | ✅ `flush(true)` → `fdatasync()`；旋转时可同步目录元数据 |
| C++ 离线解码工具 | ✅ `zxlog_decode` 复用设备端 `{fmt}` 解码逻辑，文本恢复语义一致 |
| Python 离线解码工具 | ✅ 协议 v1/v2，严格校验、`--recover` 尽力扫描损坏记录 |

**不做的事情**：不保证异常退出前尚处于内存队列中的日志一定存在；不提供跨进程合并、裸机 MCU/RTOS、完整自定义对象/formatter 的跨语言持久化 Schema、信号处理器内日志、文件系统级事务或电源故障硬件保障。Python 解码器支持 `{fmt}` 与 Python 重叠的常见标量格式语法，不保证所有 `{fmt}` 格式细节完全一致。

## 架构

下图是单个 logger 的流转路径。全局 logger 和自定义 logger 各自持有队列池、控制队列、后台线程、输出文件与统计；同一线程对不同 logger 的 SPSC 注册按实例 ID 隔离。同一可执行文件中的 logger 使用该 ELF 对应的构建字典。

```text
业务线程 N              构建期
ZX_LOG(INFO, ...)       *.cc → *.o → final ELF
   │                                          │
   ├── 级别过滤                              └── zxlog_meta → zxlog.zdict / JSON
   ├── EventID + 类型化参数编码                        │
   └── 每线程有界 SPSC 缓冲区                           │
           │                                           │
           └──────────────┐                            │
                          ▼                            │
                   单后台 Dispatcher                    │
                   实例内 sequence 排序                 │
                         │                             │
                  ┌──────┴───────────┐                 │
                  ▼                  ▼                 │
            TextFormatter        BinaryWriter          │
            查字典 + {fmt}        CRC + 批量写入         │
                  │                  │                 │
                *.log             *.blog ◄──────────────┘ 离线工具
```

**生产端**始终是 `EventID + timestamp + args` 的同一二进制结构；模式只改变后台输出。二进制构建可使用 `objcopy` 删除运行镜像的 `zxlog_meta`，并且不部署字典。文本构建和动态切换构建需要设备能读取匹配 `.zdict`。

## GN + Ninja 构建

```bash
gn gen out/Binary  --args='zxlog_mode="BINARY"'
ninja -C out/Binary zxlog_all
ninja -C out/Binary check

gn gen out/Text    --args='zxlog_mode="TEXT"'
ninja -C out/Text check

gn gen out/Runtime --args='zxlog_mode="RUNTIME"'
ninja -C out/Runtime check
```

正式构建工具链由 `build/toolchain/BUILD.gn` 定义；入口为 `.gn`、`build/BUILDCONFIG.gn`、`BUILD.gn`。`zxlog_mode` 取 `TEXT`（1）、`BINARY`（2）或 `RUNTIME`（3），默认 `BINARY`。`zxlog_optimize` 默认 `true`。

GN 目标：`zxlog` 静态库、`generate_build_id`、`zxlog_demo_raw`、`zxlog_dictionary`、`prepare_runtime_image`、`zxlog_unit`、`zxlog_decode`、`zxlog_all`、`check`。`check` 同时依赖 `check_multi`，后者为 `zxlog_multi_raw` 单独提取 `multi.zdict`、剥离部署镜像并验证多实例场景。

**集成到其它程序时注意**：字典提取必须对最终可执行 ELF（而不是 `libzxlog.a`）执行；用于生成 BuildID 的输入列表需要覆盖本应用所有 LogSite 相关源码、编译器/格式定义与构建选项。必要时使用 `-fmacro-prefix-map` 统一 `__FILE__` 路径。字典应与可执行文件一起归档；不能用仅源文件清单的哈希替代对最终 ELF 的构建追踪。

GN 的 `zxlog` 目标会通过公共配置将 `build/zxlog_meta.ld` 传递给依赖它的可执行文件。使用其它构建系统时，也需以 `-fdata-sections` 编译，并在最终链接时添加 `-Wl,-T,/path/to/zxlog/build/zxlog_meta.ld`。GCC 12 会忽略模板内静态变量的 `section` 属性（[GCC PR c++/88061](https://gcc.gnu.org/pipermail/gcc-bugs/2023-December/846120.html)），将日志元数据放入 `.rodata.*`；链接脚本在默认 `.rodata` 匹配之前，将这些节与正常的 `zxlog_meta` 输入节归并，并用 `KEEP` 保留。先提取字典，再从部署镜像剥离 `zxlog_meta`。

## 快速使用

统一使用 `ZX_LOG(level, "格式字符串", args...)` 或 `ZX_LOG(logger, level, "格式字符串", args...)`，`level` 为大写标记
`TRACE`、`DEBUG`、`INFO`、`WARN`、`ERROR` 或 `CRITICAL`；无参数日志可写成
`ZX_LOG(INFO, "Ready")`。级别过滤发生在参数求值之前。
原有 `LOG_*`、`ZXLOG_*` 分级日志宏及 `ZXLOG_NO_SHORT_MACROS` 开关已移除。
`Options::level` 和 `set_level()` 仍使用 `zxlog::Level` 枚举。

```cpp
#include "zxlog/zxlog.h"
#include "build_id.h" // GN generate_build_id 生成

zxlog::Options opts;
opts.initial_mode = zxlog::OutputMode::Binary; // TEXT 构建改为 Text
opts.build_id = zxlog::build::kBuildId;
opts.dictionary_path = "zxlog.zdict";  // TEXT/RUNTIME 必须部署
opts.binary_path = "logs/robot.blog";  // 路径父目录必须存在
opts.text_path = "logs/robot.log";
opts.level = zxlog::Level::Debug;
opts.max_producers = 8;
opts.per_thread_slots = 32;
opts.max_file_bytes = 1 << 20;
opts.max_files = 4;
opts.io_batch_bytes = 16384;
opts.sync_every_records = 0; // 0：避免每条日志调用 fdatasync

std::string error;
auto& logger = zxlog::Logger::instance();
if (!logger.initialize(opts, &error)) {
    // 通过其它可靠通道输出初始化错误
}

ZX_LOG(INFO, "Motor speed: {}, current: {:.2f}", 1200, 1.52f);
ZX_LOG(WARN, "Sensor ID: {:04x}", 0x2a);
ZX_LOG(DEBUG, "Aligned: [{:>8}]", "LiDAR");

// 仅 RUNTIME 构建可用，控制事件与业务日志共享该 logger 的序号。
logger.set_mode(zxlog::OutputMode::Text);
ZX_LOG(INFO, "Now in text mode: {}", 1);

logger.flush(true); // 后台屏障 + fdatasync，失败返回 false
logger.shutdown();
```

### 创建自定义 logger

以下片段放在全局 logger 初始化成功后、关闭前。创建失败返回空 `std::unique_ptr<Logger>`，原因写入 `error`。

```cpp
zxlog::Options motor_opts = opts; // 共用同一 ELF 的 BuildID 与字典
motor_opts.binary_path = "logs/motor.blog";
motor_opts.text_path = "logs/motor.log";
motor_opts.max_file_bytes = 10u << 20;
motor_opts.max_files = 5;
auto motor = zxlog::Logger::create(motor_opts, &error);
if (!motor) {
    // 通过其它可靠通道处理 error
} else {
    ZX_LOG(INFO, "system ready");           // 全局实例
    ZX_LOG(motor, INFO, "speed={}", 1200);  // motor 实例
    auto& motor_ref = *motor;
    ZX_LOG(motor_ref, WARN, "temperature={}", 80);
    ZX_LOG((motor.get()), DEBUG, "Ready");  // 复杂表达式加括号
    if (!motor->flush(true)) {
        // 处理写入或持久化失败
    }
    motor->shutdown();
}
```

- 显式 logger 支持 `Logger&`、`Logger*`、`std::unique_ptr<Logger>`、`std::shared_ptr<Logger>`。命名对象/句柄可直接传入；解引用、函数调用、带逗号等复杂表达式应整体加括号，例如 `ZX_LOG((*ptr), INFO, "Ready")`。六个级别标记是宏分派保留标记，不要用作 logger 名称或定义为其它宏。
- logger 表达式只求值一次；临时智能指针存活到该次宏调用结束。空句柄、未初始化或已关闭的 logger 丢弃调用，不求值格式化参数，也不回退全局实例。调用方仍须处理 `create()` 的失败。
- `set_level()`、`set_mode()`、`flush()`、`shutdown()` 和 `stats()` 作用于目标实例。实例内有序，实例之间不建立总顺序。TEXT/BINARY 构建分别固定模式，只有 RUNTIME 构建允许实例独立切换。
- 一个实例只支持一次成功初始化；失败后可重试。工厂完成资源准备后才返回实例。`shutdown()` 停止接收、排空并关闭输出，析构会调用它；销毁 logger 前必须停止并等待所有可能访问它的调用者，智能指针变量本身也不能被无同步地并发修改。`shutdown()` 不等价于 `flush(true)`。
- 输出路径父目录必须存在。初始化先统一预留该实例的全部输出及数字后缀归档命名空间，再打开文件；重复路径、路径与另一实例归档重叠均拒绝。父目录的相对路径和符号链接会规范化；已有活动文件必须是非符号链接、非硬链接别名的普通文件。RUNTIME 会打开两套输出，因此两条路径都必须独立。
- 路径预留只在本进程内协调 zxlog 实例，关闭后释放。其它进程或外部文件操作不能同时修改这些活动文件及归档；初始化中途失败会关闭已打开资源并释放预留，但不承诺回滚已完成的文件创建、修复或归档。

### 自定义强类型参数（单个内置 Wire 类型）

`to_wire()` 必须声明 `noexcept`，返回类型去掉引用及 cv 修饰后必须等于 `Wire`；`Wire` 必须是受支持的内置类型。每个实际编码的参数只转换一次，容量不足时整条事件丢弃，不发布部分记录。

```cpp
struct SensorCode { std::uint32_t value; };
namespace zxlog {
template<> struct LogAdapter<SensorCode> {
    using Wire = std::uint32_t;
    static constexpr Wire to_wire(const SensorCode& x) noexcept { return x.value; }
};
}
ZX_LOG(INFO, "Sensor={:04X}", SensorCode{42});
```

**约束**：转换必须无异常、低开销；设备端和 Python 解码器看到的是 `Wire` 的类型与值。复杂结构体如 Pose/PointCloud 需要后续独立的稳定 Schema，不能直接把 C++ 对象布局写入日志。字符串必须在日志调用期间复制内容，不能只留下 `char*` 指针。

### 运行示例与离线恢复

```bash
mkdir -p out/Binary/logs
out/Binary/zxlog_demo binary out/Binary/zxlog.zdict out/Binary/logs/demo
out/Binary/zxlog_decode --dict out/Binary/zxlog.zdict \
  --input out/Binary/logs/demo.blog

# Python 工具适合没有编译器的离线分析环境
python3 tools/decode.py --dict out/Binary/zxlog.zdict \
  --input out/Binary/logs/demo.blog

# 遇到损坏/截断记录：严格模式默认报错；恢复模式丢弃不能校验的字节
python3 tools/decode.py --recover --dict out/Binary/zxlog.zdict \
  --input out/Binary/logs/demo.blog

# 对多个分片按时间顺序多次传入 --input
python3 tools/decode.py --dict out/Binary/zxlog.zdict \
  --input out/Binary/logs/demo.blog.2 \
  --input out/Binary/logs/demo.blog.1 \
  --input out/Binary/logs/demo.blog
```

不同固件版本的归档可能对应不同 BuildID；必须按文件头选取对应字典，不能随意使用最新版字典解码旧文件。两套离线工具均保留 v0.1 协议的读取兼容性；v0.4 运行库启动时可将旧 v1 活动文件归档为 `.1`，随后创建新的 v2 文件。

## 二进制协议 v2

**文件头 64 字节**：magic `ZXLOG02\0` (8) / wire-version (2) / header-size (2) / BuildID (16) / wall-ns (8) / steady-ns (8) / pid (4) / boot-discriminator (12) / CRC32 (4)。

**记录头 20 字节**：length-u32 / EventID-u32 / steady-ns-u64 / CRC32-u32 / typed payload。CRC32 IEEE 对记录 `[0:16] + [20:length]` 计算，跳过 CRC 字段本身。最大单条记录 `1024` 字节。数值均使用小端序编码。字典文件仍使用 `ZXDICT1` schema v1（元数据格式未变化）。

### 可靠性与资源约束

- `max_file_bytes` 为每个活动文件及归档文件的容量上限；`max_files` 包含活动文件（默认 4）。轮转保留 `.1`, `.2` 等后缀，旧文件超额时清理。
- 每个 logger 为各生产线程分配独立的预分配 SPSC ring；记录发布不需要共享互斥锁。实例内 `sequence` 保证日志与控制事件的顺序。线程退出后，其各个 ring 在消费完毕时可以重新注册给其它线程。
- 每个 logger 默认 `8` 个生产者 ring，每 ring `32` 槽，每槽略高于 `1024` 字节；**每实例队列本身约占 0.26 MiB**，另有线程栈、写缓冲和文本字典内存。可缩减 `max_producers` 和 `per_thread_slots`；注册满/队列满/记录超长会丢弃，并计入该实例的 `Stats::dropped`。
- 每线程最多同时绑定 `Logger::kMaxLoggersPerThread`（32）个活跃 logger，达到上限时向新实例写入会计入其 `dropped`。TLS 以不复用的实例 ID 区分生命周期，用弱引用管理注册；关闭实例后队列内存释放，其 TLS 槽位可复用，不必等生产线程退出。稳定调用路径不增加堆分配。
- `stats()` 将生产端丢弃细分为 `queue_full / producer_limit / tls_limit / record_too_large`，它们的合计等于 `dropped`（并发采样可能短暂不一致）；后端另统计 `text_too_large`、格式、未知日志点、记录校验及文件操作失败。`last_error()` 返回线程安全的错误快照（类别、操作、路径、errno、单调时间），`output_failed()` 标记文件输出故障。`io_errors` 保留历史累计语义，发生错误后显式 `flush()` 会返回 false。
- `Options::error_handler` 在后台线程调用，默认按实例每秒最多通知一次；计数和 `last_error()` 不受限频影响。回调必须及时返回、不能销毁 logger；回调中的日志不求值，flush/set_mode 返回 false，shutdown 不执行，create/initialize 拒绝创建或初始化；异常被隔离。失败 writer 保留首次文件错误，不自动重放部分写入：先停止调用者、shutdown，再创建新实例，启动恢复会保留有效记录。
- 默认 `flush_interval=100ms` 定时刷出低流量日志，设为 0 可禁用；`flush_on_level` 可按级别刷新。`sync_interval` 和 `sync_on_level` 独立控制 `fdatasync`，默认禁用；时间间隔允许 0～24 小时。定时策略受线程调度和 I/O 延迟影响，不是硬实时保证；`flush(true)` 仍等待前序事件写入并同步。
- 文本行（含前缀和换行）受 `max_text_bytes`（默认 64 KiB，最大 1 MiB）及 `max_file_bytes` 的较小值限制；超限整条丢弃并计入 `text_too_large`，writer 继续工作。文本渲染的输出缓冲受限，离线 C++ 解码器默认也限制单条正文为 64 KiB。
- `Stats::accepted` 表示成功入队，**不代表已经落盘**。`flush(false)` 等待以前的事件被后台写入，`flush(true)` 进一步执行 `fdatasync`。仍然不能保证存储硬件在掉电时完成内部刷新。
- 异常退出后再次启动：校验有效记录前缀、截断尾部损坏内容、归档旧活动日志并创建新文件；中间 CRC 错误的后续数据在设备恢复时保守丢弃。离线 `--recover` 可以尝试跨损坏点搜索下一条有效记录。日志队列中尚未写入的记录无法凭空恢复。
- 记录时钟为单调纳秒；文件头记录 wall/steady 关联。**不同重启文件不要仅按记录的 steady-ns 合并排序**。
- `sync_on_rotate=true` 会尝试同步文件与目录元数据。文件系统不支持目录 `fsync` 时可能返回 I/O 错误。进程故障后的可恢复性不等于原子事务或完备的掉电安全保障。
- 支持单进程内多个独立 logger，每个活动日志路径由一个实例独占，不支持跨进程共享输出。每个后台线程最大空闲唤醒周期约 5ms，不保证硬实时。
- `{fmt}` 编译期检查作用于标量 Wire 类型，设备端文本使用真正的 `{fmt}`；Python 恢复工具只保证常见标量 `format` 语法的交集。并非任意 `fmt::formatter<T>` 都自动可以序列化。

## 测试

`ninja -C out/<Mode> check` 自动执行：

- C++17 单元测试与编译期错误检查（不合法格式必须编译失败）
- 三种构建模式，BuildID/字典校验
- 十九个日志调用点、扩展 fmt 格式、自定义强类型
- 四线程并发生产、队列满丢弃统计、线程退出后队列回收
- CRC 字节翻转、截断损坏、严格解码失败和尽力恢复
- 旧 v1 日志迁移、损坏旧文件拒绝覆盖、启动截断恢复
- 文件轮转、归档数量/大小配额、旧归档清理
- 生产镜像已去除 `zxlog_meta` 节
- 两种宏形式及六级别、对象/裸指针/智能指针、临时句柄、空句柄及参数惰性求值
- 多实例交替写入、四线程隔离、单实例关闭与其它实例继续写入
- 反复创建关闭、TLS 32 实例容量与回收、生产线程退出回收
- 重复路径、归档命名冲突、父目录别名、活动文件链接别名、创建失败后的资源释放
- 多实例独立轮换、RUNTIME 独立模式切换，以及两套离线解码器一致性

2026-11-01 本地验证使用 GCC 12.3.0、GN 2318、Ninja 1.10.1。`BINARY/TEXT/RUNTIME` 的 `gn gen`、`gn check` 和完整 `ninja -C out/<Mode> check` 均通过；`BINARY` 的 `zxlog_optimize=false` 构建也通过。上述集成场景按各模式适用分支执行，部署镜像额外检查正常日志和被级别过滤日志的格式字符串均已移除。上线前还应在实际目标工具链、文件系统、编译优化选项（特别是 LTO）上做构建、性能和断电测试。

多实例改造另使用 Clang 21.1.5 `-O1 -g` 验证：RUNTIME 的 AddressSanitizer/UndefinedBehaviorSanitizer 和 BINARY 的 ThreadSanitizer 均通过完整多实例场景，包含元数据提取、部署镜像剥离和两套解码器核对，未报告检测器错误。

历史手工验证（多实例改造前）：GCC 12.3.0 的 `-O2 -flto` BINARY 示例通过完整集成测试；Clang 21.1.5 的 `-O1 -g -fsanitize=address,undefined` 单元测试与 BINARY 示例也通过，未报告 sanitizer 错误。两者均覆盖字典提取和部署镜像剥离，C++ 离线解码器复用常规 GN 构建产物。

## 目录

```text
include/zxlog/             对外日志 API、codec、CRC、ELF 元数据和协议
src/logger.cc             多实例生命周期、SPSC 注册、实例内排序、后台线程
src/path_lease.{h,cc}     进程内输出路径及归档命名空间占用检查
src/file_writer.{h,cc}    批量写入、CRC 文件恢复、轮转、fdatasync
src/text_format.{h,cc}    构建字典加载、{fmt} 文本恢复
tools/build_id.py         构建 ID
tools/extract_dictionary.py  ELF 字典提取与冲突校验
tools/strip_metadata.py   剥离运行镜像里的构建期元数据
tools/decode.py           Python v1/v2 离线解码和恢复
tools/decode.cc           C++ {fmt} 一致性离线解码器
third_party/fmt/          MIT 许可的 fmt 12.1.0 头文件子集
build/, BUILD.gn, .gn     GN 工程
tests/                   单元及集成测试
examples/basic.cc        三种模式、轮转、并发、线程重用示例
```
