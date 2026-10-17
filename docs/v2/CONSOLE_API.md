# Console 统一状态接口（#218）

Console 的十二个接口统一为 `0/-errno`，所有正常返回保留入口 errno；旧 void
配置和无参布尔查询不保留兼容层。当前仍为未发布 ABI 2，旧开发头文件/二进制
不得与新库混用。函数名数量未变不代表函数类型或错误语义兼容。

## 所有权和进程边界

Console 是宿主的终端展示工具，使用当前进程域的配置和同步对象，不创建、
借用或销毁 Logger 默认实例。默认 Logger 槽的借用责任仍由 DEFAULT_BORROW
定义；两者共用进程准入，不合并为一套文件输出生命周期。

调用前宿主显式 `logger_process_init()`。未初始化为 ENODEV，初始化中为
EAGAIN，继承但未重新初始化为 ECHILD，同线程重入为 EDEADLK。准入先于
读取配置和输出指针；失败不启动进程域、执行格式化或接触旧锁。

## 配置

```c
int console_init(const console_config_t *);
int console_set_verbosity(console_verbosity_t);
int console_set_color(console_color_mode_t);
```

NULL 配置为 EINVAL；恢复默认值须显式传入 CONSOLE_DEFAULT_CONFIG()。
错误版本、短结构、非零未知尾部分别按配置合同拒绝。无效枚举或配置保持
原快照，成功 init 原子提交全部字段，setter 只更新自己的字段。

配置已经提交、随后取消策略恢复失败时，返回恢复错误但不回滚配置；配置
本身的主错误优先。没有新增全局锁、分配、工作线程或隐式 Logger 所有者。

## TTY 查询

```c
int console_stdout_is_tty(int *out);
int console_stderr_is_tty(int *out);
```

成功返回0并写入0/1；ENOTTY转换为成功的“非终端”结果。EBADF等保留为负错误码，
无法取得错误信息时为EIO。调用者提供独占、存活、可写的输出槽；NULL为EINVAL。
包括取消策略恢复在内的任何失败均不修改*out，不把“查询失败”混成false。
范围是项目的Linux/glibc支持基线；TTY不表示有人在终端交互。

依据：[Linux isatty](https://man7.org/linux/man-pages/man3/isatty.3.html)。

## 输出

console_print/info/warn/error/verbose/debug/debug_source 保留原路由、标签、颜色、
换行、过滤、有界来源格式和 FILE 缓冲刷新。成功返回0（含按级别过滤），失败
返回负错误码。fmt为NULL始终EINVAL，即使该消息会被过滤。入口errno也用于%m，
不能把错误返回迁移变成消息文本变化。已发生的部分输出不回滚、不重试整个记录。

局部SIGPIPE屏蔽、待处理信号与宿主处理器保持规则不变；闭管道EPIPE，较早的
格式/stdio错误优先于后续刷新/解锁/取消恢复错误。锁释放证明失败仍毒化Console，
不会因错误模型迁移而继续使用未知锁状态。取消保持延后到va_end和全部收尾之后，
不新增取消点，不把本接口宣传成信号安全或有时限的输出。

## 可执行验证

`xmake test -g console-api -j1`：四个公开场景分别直接链接生产静态/共享库，
包括冷准入、配置不被失败改变、七种输出精确字节、真实伪终端/非终端/关闭fd、
fork后显式重初始化。十个故障场景分别链接生产静态和白盒归档；setup/restore
逐一覆盖十二个入口，其余验证主错优先、TTY输出槽以及锁失败/毒化。

restore在成功重试前通过真实输出验证三个配置变更已经提交，并逐次核对七类
输出的精确字节，拒绝记录缺失或重复。检查器直接读取文件描述符，不替Console
刷新FILE。写入故障同时拦截glibc的vfprintf和加固入口__vfprintf_chk，保留生产
加固选项；未命中实际入口不能算作故障注入通过。

静态新增24个注册项，共享新增14个。原有SIGPIPE、FILE回调、取消、配置并发、
重入和fork夹具直接迁移断言，不删除信号状态/字节/次数验证。配置扩展测试
现在检查本次console_init返回值，不能沿用上一条logger_create的旧错误。

七组新增类型正负例拒绝void配置和无参TTY签名；安装后的C/C++11消费者实际
调用新配置、查询和错误路径。静态包装不等于对生产DSO内部函数实施拦截。

结果以实际提交和运行记录为准；不替代其他公开接口的独立验收、TSan的
既有fork能力边界、性能或最终发布验收。
