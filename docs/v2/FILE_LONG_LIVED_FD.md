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

## 跨函数候选移交的类型闭合

长期字段类型化后，`open_candidate(..., int *out, ...)` 与
`install_fd(..., int fd, ...)` 曾在中间移交处退回裸整数。此处只收紧这两个
静态函数的合同：前者向空的 `logger_owned_fd` 输出槽成功移交，失败不改变
该槽；后者消费候选拥有者并使源失效。首次打开、重开、轮转三个调用点从
取得候选之前就持有初始化为 `-1` 的词法拥有者，沿整个函数调用链保持类型。

`open_candidate` 仍在原来的 `openat`/校验位置取得并登记资源；只有校验
成功才 `logger_fd_move` 给调用者。`install_fd` 仍在全部准备成功后安装，
目的旧拥有者必须先由调用者移出。失败的 `fsync`/目录同步继续优先于候选
清理错误；作用域退出时无效拥有者只做空操作，不重复关闭刚装入的活动 fd。
没有增加锁、分配、系统调用或公开符号，也没有引入通用所有权框架。

`tests/scripts/test_fd_types.py` 的四组新增编译正反例直接包含生产
`src/logger_file.c`，检查真实 static 函数而非复制一份声明：拒绝 raw/borrowed
输出槽进入 `open_candidate`，拒绝 raw/borrowed 参数进入 `install_fd`，
每组仅修正类型的对照必须成功。该测试只做语法检查，不执行含占位参数的
代码；实际 I/O 与失效后不重复关闭仍由已有生产/回归路径验证。

这不是已发现的生产双重关闭缺陷修复，也不等于完整 D2 AST/跨函数逃逸
分析。空输出槽、有效源、目的拥有者已移出仍由私有调用约定和回归测试
约束；C 同型复制及显式强制转换不在此有限编译约束的保证范围内。

使用原有 Xmake `fd-ownership` 分组和完整回归，不另建构建系统：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release --build_shared=n --build_tests=y \
  --build_private_tests=y --build_regression_tests=y
xmake test -g fd-ownership -j1
xmake test -j1
```

`fd_type_contract` 仍选用 Xmake 实际 C 编译器；脚本现为 21 组编译正反例
加一项显式逃逸边界，共 22 项。共享/静态包、最低平台及 Sanitizer
必须以本候选准确提交的独立 CI 为准，不将 #260 旧结果记作本轮新执行。
