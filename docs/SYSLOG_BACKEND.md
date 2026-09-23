# Syslog 后端契约：非阻塞、有界重连、显式失败

本轮基于 File Backend Fix 修改。面向宿主拥有的显式 Logger，也由默认全局
Logger 复用；没有接管宿主线程/进程、没有新 worker、没有新增磁盘 spool。
本文件是当前 Syslog 契约，优先于历史版本中“阻塞 syslog”的说明。

## 支持范围

Linux AF_UNIX / SOCK_DGRAM 本地端点。默认 `/dev/log`。线上报文保持本地
syslogd 接收格式：`<PRI>ident[pid]: ` 后接既有 Logger 文本，去除最后的 LF，
不附带 NUL；不是新实现的 RFC5424 网络协议。每个实例有独立 fd、目标、tag、
facility、计数和重连冷却。没有 TCP、UDP、TLS 或 SOCK_STREAM 隐式切换。
同一个端点上的多个实例相互独立，但共享接收者本身的容量。

宿主仍负责显式实例生命周期：销毁前停止/join 所有使用者；继承实例仍受已有
ECHILD 防护。后台 worker 输出和同步/溢出回退使用同一后端并被 emit_mu 串行化。
本轮没有使普通 API 成为异步线程取消/信号处理接口。

## 创建配置

```c
logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
cfg.outputs = LOGGER_OUT_FILE | LOGGER_OUT_SYSLOG;
cfg.file_path = "./app.log";
cfg.ident = "backup";
cfg.syslog.path = "/dev/log"; /* NULL 也是这个默认值 */
cfg.syslog.facility = LOGGER_SYSLOG_FACILITY_LOCAL0;
cfg.syslog.reconnect_interval_ms = 1000;
cfg.syslog.startup = LOGGER_SYSLOG_START_REQUIRED;
logger_t *log = logger_create(&cfg);
```

| 字段 | 规则 |
|---|---|
| path | NULL 默认 /dev/log；空串拒绝；pathname 类型，不支持 abstract namespace；创建时复制，相对路径转为当时 cwd 的绝对字符串 |
| 路径长度 | 完整路径含 NUL 必须放入 Linux sockaddr_un.sun_path（本机 108 字节）；不截断；cwd 太长无法转换也返回 ENAMETOOLONG |
| facility | 标准 facility 编号乘 8，范围 0..23×8，低 3 位必须为 0；默认 USER=8；提供 namespaced USER/LOCAL0..7 常量，无需包含有 LOG_* 名称冲突的 syslog.h |
| interval | 单调时钟毫秒，0 使用 1000，显式支持 1..60000；不是睡眠时间或 I/O 超时 |
| startup | REQUIRED 默认；DEFERRED 必须显式选择 |
| ident | NULL/空串用 app；最多 127 字节；可打印 ASCII 非空白 token，拒绝冒号、方括号和控制字节；不再静默截断 |

相对路径是在创建时快照，不会因宿主 chdir 改变。不同于文件后端，它没有绑定
目录 fd：若路径所指目录/符号链接被改动，下次重连按同一个绝对路径重新解析。
目录必须由宿主管理；不防恶意端点替换。不能保证初始化后 chroot 或 mount
namespace 变化后原地址仍可访问。生产不读取 LOGGER_SYSLOG_PATH；旧测试库
仅在没有显式 path 时保留该环境覆盖。

REQUIRED 连接失败即创建失败。DEFERRED 只允许 connect 返回 ENOENT、
ECONNREFUSED、ECONNRESET、ENOTCONN、EAGAIN/EWOULDBLOCK 或 EINTR 时创建
离线实例。socket 创建失败、权限错误、端点类型错误或配置错误仍导致创建失败。
DEFERRED 返回成功只表示离线状态被明确接受，不表示端点健康：初始化故障保存在
syslog.last_error，也进入 Logger 的 sticky first_error。即便尚无记录，status
flush 也会报告该故障。创建失败的候选 fd 正常关闭；错误清理不覆盖主要 errno。

## 写入与背压

- socket 原子设置 SOCK_NONBLOCK | SOCK_CLOEXEC；send 额外指定 MSG_DONTWAIT |
  MSG_NOSIGNAL，不改变宿主 SIGPIPE handler。
- 每条有效记录至多执行 4 次 send；只有 EINTR 可以立即重试。4 次均中断则返回
  EINTR。不会因 EAGAIN、ENOBUFS、ENOMEM 等压力错误立即重试，不等待接收队列。
- 背压记录记为输出失败，而不是入队失败。返回/计数真实可见；不会静默切 backend。
- 不增加无限内存队列/第二个重试队列，不在后台重新发送失败记录。宿主需无损持久
  接收时应使用相应受确认的持久化系统，而非将本地 Syslog send 当作审计回执。
- 超过 packet 容量（8192）直接 EMSGSIZE，不截断。单条不拆多个 datagram。
  返回零/短 send 视为 EIO，不续发后缀，不自动重放这个不确定结果。
- 同一条记录独立尝试所有所选后端。文件失败不会阻止 Syslog 尝试；Syslog 失败
  不重写已成功的文件/stderr。总体 emitted_records 仍要求所有输出都返回成功。

“有界”是**有限 syscall 次数且不等待 socket 队列空间**，不是硬实时截止时间。
线程调度、emit_mu 竞争、其他后端的文件/stdio I/O、socket pathname 查找等不具有
本轮新增的绝对时限。文件 write/fsync 和整个 flush/destroy 不能因此被宣称不阻塞。

## 连接失效与恢复

```text
connected
   | send: refused/reset/notconn/pipe/noent/dest-required/unexpected-short(EIO)
   v
close old socket, remember cause and monotonic cooldown
   |
future record, interval elapsed
   v
one new socket + one connect
   | failure                         | success
   v                                 v
close candidate, cooldown again      send THIS future record (bounded EINTR)
```

当前失败记录不重试；只有以后的日志调用触发连接检查。冷却期间返回保存的连接
错误并增加 cooldown_records，不创建 socket，不 connect/send。每个重新连接周期
只调用一次 socket 和一次 connect（socket 失败不调用 connect）；connect 的
EINTR/EAGAIN 也丢弃候选，不能假定连接状态可复用。socket 创建错误不会无限重试。

失败的 send 才触发旧连接关闭。不定期轮询端点 inode；如果旧 daemon socket
仍然存活，即使路径被重新绑定，也可能继续发向旧端点。无流量时不主动重连。
flush/destroy 不重连，不重放历史；关闭只清理已有资源。

冷却依据 CLOCK_MONOTONIC；取时错误明确报错。若断连时无法取时，下一次成功取时
先建立新的冷却窗口；异常倒退同样重建窗口，避免快速无限重连。没有 sleep/poll
循环或专用计时线程。成功恢复只改变当前 Syslog 状态，不抹掉 Logger 历史错误。

## 可观测性

新增 `int logger_get_syslog_metrics(logger_t *, logger_syslog_metrics_t *)`。
0 成功；-1 + errno 失败。无 Syslog 为 ENOTSUP，NULL 参数为 EINVAL，child 为
ECHILD；失败不修改输出。正常调用在 emit_mu 下取一致快照，不发 I/O、不重连。
metrics 与 flush 一样不能和 raw pointer 销毁并发。

- submitted_records = sent_records + failed_records（快照时）；未计入过滤/入队丢弃。
- sent_records 只表示 send 返回整个 datagram 长度，不证明 daemon 收到或持久化。
- backpressure_records、cooldown_records 是 failed_records 的子集，可能重叠，
  不应相加当作总失败数。
- connected 仅表示本地当前持有连接 fd，不是接收方活跃状态的探测。
- last_error 是最近一次初始化/输出结果；成功发送后为 0。first_error 仍是全实例
  sticky 历史错误，不能靠重连/flush/reopen 清除。
- connect_attempts 包括初始 socket+connect 周期；reconnect_successes 不计初次成功。
- send_attempts 是 syscall 数，包含 EINTR 重试；interrupted_calls 同时计入创建/
  连接阶段的 EINTR。close_failures/last_close_error 保留断连及候选清理错误。
- 正常 destroy 返回时对象已释放，不能为读取最终指标而再次解引用；销毁前取快照。

## Config ABI 变化

`logger_config_t` 尾部追加 `logger_syslog_config_t syslog`，保留所有旧字段的
偏移和 LOGGER_CONFIG_VERSION=1。不是简单要求旧客户端传新的 sizeof。

- 原版 version-1 **完整前缀**（本机 120 字节；当前完整配置为 144 字节，具体以平台布局为准）由新库读取，
  Syslog 使用默认设置；既有 logger_metrics_t/logger_io_metrics_t 不扩写。
- 新 default 宏填充完整尺寸和 syslog 默认值。新库只复制已知前缀与完整扩展。
- 介于旧完整尺寸与新完整尺寸之间的部分尾部拒绝，未来更大尾部忽略未知部分。
- size=0/version=0 仅为重编译源码的旧前缀兼容；不读取尾部。新配置必须使用默认
  宏或正确设置 size/version。既不支持没有这两个字段的旧二进制，也无法验证一个
  C 指针是否真的拥有调用者声称的可读大小。
- 新代码链接旧库会丢失新 Syslog 配置能力，本轮不承诺反向功能兼容。正式 SONAME/
  符号版本和其他平台 ABI 冻结仍待后续完成。

测试包含真实 guard-page 截断对象、冻结原头文件编译的独立旧 consumer。静态
consumer 通过 test-only connect wrapper 测原默认 Syslog；shared consumer 真正
链接新 DSO，测试原配置布局和异步文件日志（不是假称 wrap 可以拦截 DSO）。

## 验证边界

本地可控 datagram socket、实际接收队列填满、接收端关闭重建、同实例并发、
多后端故障、同步回退及有限错误注入由自动测试覆盖。DSO 版本另运行同组集成。
本轮未在真实 rsyslog/syslog-ng/systemd-journald 发行版矩阵、Linux3.x/32bit 上验收，
也未引入可靠网络 Syslog 协议。旧 OpenSSL TSan 报告仅适用于历史后端；该依赖现已移除，不等于其 race 根因已定位。
