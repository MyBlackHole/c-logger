# 宿主拥有 Logger，业务库只借用

> 当前创建/销毁以[新生命周期合同](v2/LIFECYCLE_API.md)为准：直接使用拥有者槽与负错误码，不保留旧接口。

## 职责

| 组件 | 管理的资源 | 不应该做的事 |
|---|---|---|
| 宿主 | Logger 所有者、日志策略、进程模型、业务线程、回调上下文 | 不必理解内部队列 |
| 业务 SDK | 自身业务实例/业务资源、同步调用日志出口 | 不自动创建默认 Logger，不释放 log_user，不替宿主 fork |
| Logger | 每实例工作线程/队列/后端、完成水位、I/O 错误、正常释放 | 不扫描全宿主来决定业务运行方式 |

独立实例不等于每个模块一条线程：多个模块可共享同一实例。确需文件/级别/性能隔离时由宿主创建多个。
本轮不新增通用回调框架到 liblogger，也不让 Log 引擎持有外部业务回调。
示例 SDK 以自己的业务 ABI 暴露回调，宿主编译一个很轻量的适配器。

## 可编译模板

`examples/host_owned/example_sdk.h/.c` 定义 `example_sdk_create/run/destroy`。
SDK 的配置只是 `name + callback + log_user`，name 被复制，后两者借用。
回调=NULL 时不打印标准输出/标准错误，不创建文件、不启动线程、不依赖 liblogger。

`logger_adapter.h` 编译进宿主，log_user 指向宿主自己的 logger_t。
它映射 level，并用 `%.*s` 把消息当数据而不是格式字符串转发。
第三方已有日志系统时，改用自己的回调，不需要这个适配器或 Logger 二进制依赖。

```c
logger_config_t c = LOGGER_DEFAULT_CONFIG();
c.outputs = LOGGER_OUT_FILE;
c.file_path = "./app.log";
c.async_mode = 0; /* 也可以由宿主明确选择 1 */
logger_t *log = NULL;
int rc = logger_create(&c, &log);
if (rc)
        return rc;
/* 以下为示例 SDK 的真实类型，生产 SDK 使用自己的配置项。 */
struct example_sdk_options opts = {"backup", example_to_logger, log};
/* SDK 只使用 opts.log(opts.log_user, &record)，不创建/销毁 log。 */
```

完整错误处理见 `host.c`，由 dlopen/dlsym 加载 SDK，并让两个业务实例共享一个 Logger。

## 回调并发与所有权

- 回调在 SDK 调用方线程同步执行；可能由多个调用方并发触发。
- 调用时不持有 SDK 业务锁；SDK 的配置在创建后不再并发修改。
- 记录/消息/源信息只在回调期间有效。自定义异步接收器须复制所需数据。
- 回调不得销毁正在执行的 SDK、释放其借入 Logger，也不能让异常/longjmp 跨 C ABI 逃逸。
- `void callback` 是诊断日志，不是可靠交付或 Audit 提交回执。业务错误通过返回值传播。
- 宿主回调的代码和 log_user 必须活到最后一次回调返回。

## 卸载

正常应用顺序：

```text
停止新 SDK 调用 → join 外部调用者 → 销毁 SDK（并停止 SDK 自己的后台任务）
→ Logger 刷新/状态 → SDK dlclose
→ 不再有其他使用者时 Logger 销毁/状态 → 最后才能卸载 Logger
```

多个 SDK 共享 Logger 时，销毁一个 SDK 不能关闭共享 Logger。
一个独立实例的创建失败/关闭也不能改变另一个实例或宿主的全局 Logger。

本轮异步队列已拥有源信息快照，因此在业务回调全部返回、SDK 可合法卸载后，
已排入 Logger 的记录不再引用 SDK 的 .rodata 或 SDK 实例 name。
回归测试专门暂停工作线程格式化、销毁 SDK 并 dlclose 后再恢复输出。
这不允许卸载 Logger 自身、宿主回调所在 DSO 或仍有业务任务执行的 SDK。
标准 dlclose 返回也不保证依赖当场完全解除映射；测试验证生命周期协议，不声称强制卸载整个依赖树。

## Fork 只公布边界，不接管宿主

宿主安排进程创建和库初始化时机。本轮**没有**放宽原始 fork 继承运行时的 ECHILD 防护。
独立 B Logger 不是任意多线程 fork 后重新 malloc/启动线程的通用安全保证。
首选在宿主工作线程启动后首次初始化，或 exec 后初始化。
同步单线程 fork 前初始化可以作为将来的窄契约设计，但当前版本不因移除 /proc 扫描就自动支持。

`logger_fork_reinit()`、兼容头文件与构建选项已移除。生产产物没有 /proc 线程扫描、
干净状态令牌或 fork 操作；PID/atfork 的防御性拒绝仍保留。
内部 guard 不重绑子进程身份、不修复继承锁，继承运行时的子进程需要 exec。

## ABI/构建

- Xmake `--build_shared=y` 生成 liblogger.so，`n` 生成 liblogger.a；默认静态库。
- 所有直接借用方必须使用创建该 logger_t 的同一个实现，不跨静态副本、版本或 dlmopen 命名空间传句柄。
- 仅使用回调的 SDK 不链接 logger，更适合独立发布、不同宿主日志系统和语言边界。
- 当前共享库发布已固定 SONAME、隐藏可见性、显式 ABI 允许列表与安装/XPack 契约。
- 有 --wrap的白盒回归在共享库配置下使用同源生产静态测试库。
  普通集成测试、宿主_owned_example 和 liblogger_dlclose 真正调用 liblogger.so；报告明确区分。

## 依据（不是用第三方文档代替本项目测试）

- GLib 要求日志全局写入器由应用设置，库不应安装：
  https://docs.gtk.org/glib/func.log_set_writer_func.html
- libcurl 的调试回调显式传递 size 和用户数据：
  https://curl.se/libcurl/c/CURLOPT_DEBUGFUNCTION.html
- dlopen/dlclose 资源与卸载语义：
  https://man7.org/linux/man-pages/man3/dlopen.3.html
