# 文件候选移交的持续反例验收（#210）

## 有限验证范围

专用 `xmake-fd-handoff.lua` 包含根 `xmake.lua`，只追加 `fd-handoff` 分组。
`fd_handoff_verifier` 在该分组中构建并运行三个独立
进程：健康版本直接包含生产 `logger_file.c`；另外两个测试专用版本只在
选定的候选输出或安装阶段，将 move 模拟为同类型复制，并断言该替换实际
发生一次。并非修改生产源码，也不把错误代码写入生产或回归归档。

健康版本要求候选移交后 fd 仍开放、安装后来源失效、空来源 cleanup 不关闭
活动 fd、实际写入/同步/关闭与文件内容正确。错误版本分别以固定协议和
退出码 41、42 报告候选被提前关闭、安装来源未消费；它们只在隔离子进程
内执行。判定器同时核验编译变体身份、准确退出码、完整输出与空 stderr；
普通断言失败、错误二进制、信号、77 跳过、超时、Sanitizer 诊断都必须失败，
不得以任意非零退出冒充反例命中。判定器自身有八项正反测试。

这补上 #261 的局部反例只在会话验证脚本中运行、未进入持续门禁的缺口。
三个驱动均由 Xmake 使用当前构建配置编译，链接同源回归静态归档，且显式
采用与该归档一致的 LOCKDEP=1、FAULT_INJECTION=0。共享配置仍是此静态
白盒边界，不冒充 DSO 内部注入；已有真正生产静态/共享测试继续独立运行。
测试专用源、符号与三个驱动不安装、不进入 XPack；没有新增生产锁、分配
或系统调用。不证明任意 C 同型复制安全或全程序 AST/跨函数所有权分析。

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -F xmake-fd-handoff.lua -m debug --build_shared=n \
  --build_tests=n --build_private_tests=n --build_regression_tests=y
xmake test -F xmake-fd-handoff.lua -g fd-handoff -j1
```

该专用入口不改变普通完整矩阵的注册数。独立 `fd-handoff-validation` CI
在 Ubuntu20.04 的 GCC9/Clang10 下各运行健康及两种错误目标，检查同源生产
归档隔离并上传源码身份、完整构建/测试日志；本地 ASan/UBSan 补充实验与
这个普通构建矩阵分开记载，不能把本专项称为完整 Sanitizer 或发布验收。
