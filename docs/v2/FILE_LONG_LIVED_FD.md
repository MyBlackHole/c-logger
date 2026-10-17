# 文件后端长期描述符拥有权类型化（#210，D1/T1 有限阶段）

本阶段在 Syslog 长期 fd 类型化已合并的 ABI 2 主线上，将不安装的
`logger_file_t.{fd,dir_fd,lock_fd}` 从裸整数迁至现有
`struct logger_owned_fd`。范围限定活动数据 fd、协作锁 fd 与目录 fd；
不改变公开头文件、ELF ABI、SONAME 或导出清单，不新增堆分配或锁。

## 不变的 I/O 顺序及资源语义

- 初始化显式为无效拥有者（`value=-1`）；fd=0 仍是合法描述符。
- `logger_fork_open/openat` 仍在原协调区间完成取得和位图登记；
  私有 `file_data_fd/file_dir_fd/file_lock_fd` 仅在既有排他访问下借用整数。
- 最终关闭按数据、锁、目录既定次序，在真正 close 前先使各长期字段失效，
  `logger_fd_close_status` 消费位图，保留首错并继续释放后续资源；
  不重试 Linux `close(EINTR)`，不显式调用 `LOCK_UN`。
- reopen 与 rotate 的新候选仍先经过原有同步、目录持久化、验证与崩溃切点；
  准备安装时通过 `logger_fd_move` 移出旧活动拥有者，安装新值后只关闭一次旧拥有者。
- 保持 `detached`、`RENAME_NOREPLACE`、文件/目录 fsync 次序、
  `DIR` fork 资源租约、失败恢复及错误优先级不变。

## 可执行验收及未解决风险

需要在**精确 PR head** 核查静态/共享生产构建、GCC9/Clang10、
ASan/UBSan、TSan 已知 31 项跳过、Linux 5.10/glibc2.31、真实文件后端和
打包消费。重点为 fd0、close(EINTR/EIO) 后宿主复用同一数字、reopen 与
rotate 候选同步失败、detached 恢复、文件锁登记与 fork 子方隔离。
`test_fd_types.py` 的额外正反例只验证编译期类型区分，不能当作动态测试。

C 同型结构复制、`memcpy`、显式裸 int 重新包装和跨函数别名逃逸
仍不被保证；本 PR 不完成 D1 其余单位、D2 AST/跨函数语义检查、
#218/#221 风险接受或 A–F/D1–D6 人工签收。正式 v2.0.0 标签和 Release
仍禁止提前创建。
