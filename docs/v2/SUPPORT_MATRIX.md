# c-logger 2.0 支持基线

本文件冻结 2.0 初始工程基线。扩大支持范围必须有独立设计、CI 和运行证据，不能仅因为“理论上能编译”就宣称支持。

## 初始 Production 目标

| 项目 | 2.0 初始基线 |
|---|---|
| 操作系统 | Linux only |
| CPU | x86_64 |
| ELF | ELF64 |
| 最低 Linux 内核 | 5.10 |
| 最低 glibc | 2.31 |
| C 方言 | GNU C11 |
| GCC | >= 9 |
| Clang | >= 10 |
| Xmake | >= 2.8.5；CI 固定 3.1.1 |
| 链接形式 | shared + static |
| C++ consumer | public headers 保持 C++11 consumer 可用 |
| musl | 不在初始支持范围 |
| AArch64 | 不在初始支持范围 |
| 32-bit | 不支持 |

## 证据要求

当前 v1 已有：

- Ubuntu 20.04/glibc 2.31 runtime gate；
- Linux 5.10 guest gate；
- x86_64 ELF/SONAME/GLIBC ceiling 检查；
- shared/static installed consumer 验证。

2.0 必须保留等价或更强证据。

GCC 9 是 Ubuntu 20.04 的工程基线；Clang 10 是 2.0 的最低目标版本。`.github/workflows/v2-engineering-baseline.yml` 在 Ubuntu 20.04 用户态分别使用 GCC 与 Clang 构建并运行完整静态测试集，同时记录实际编译器版本。该 gate 成功后，GCC/Clang baseline 才视为已有可重复 CI 证据。

## 扩展规则

新增 AArch64、musl 或更旧用户态不是普通修复，必须同时回答：

- C11 atomic/memory-order 语义是否仍成立；
- ABI/layout 是否变化；
- syscall/libc 行为差异；
- sanitizer/toolchain 是否可用；
- packaging 与 installed consumer 是否验证；
- crash/persistence gate 是否需要新平台运行。

支持矩阵变化必须在 2.0 ABI freeze 前完成，或作为后续明确版本能力发布。


## 编译期守卫

2.0 初始编译门禁：

- 新增 VLA 通过 `-Wvla` 暴露，并在正常 `-Werror` target 中成为错误；
- production library 及其同源 test/regression support archive 对超过 24 KiB 的单函数 stack frame 发出诊断；历史 test executable 不在这一轮被强制重写；这是用户态初始 guardrail，不等于 Linux kernel stack 大小；
- 24 KiB 阈值当前 Clang 10 已测得 `src/audit.c:create_runtime()` 约 19,464B；在 #105 Audit 重构和 2.0 stack inventory 后应收紧，但扩大阈值必须有 review 依据。
