# 目标平台构建基线

> 当前支持权威见 [v2/SUPPORT_MATRIX.md](v2/SUPPORT_MATRIX.md)。下文退役测试描述为历史证据；
> 当前普通 Logger 的八点进程崩溃合同见 [LOGGER_CRASH_CONTRACT.md](LOGGER_CRASH_CONTRACT.md)。

## C 语言与编译器方言

项目的语言基线是 **C11**，但不是严格 ISO C11。Xmake 的生产目标显式使用：

```text
-std=gnu11 -Wall -Wextra -Wpedantic -Werror
```

内部 Linux 风格资源管理依赖 GCC/Clang 兼容 GNU C 扩展，包括
`__attribute__((cleanup))`、`typeof`/语句表达式和唯一标识宏。
因此生产源码按 **C11 + GNU C 扩展**（通常即 `-std=gnu11`）维护。
这些扩展只存在于私有实现，不进入已安装公开 ABI/头文件契约。

当前 CI 验证的是 Linux/ELF 上的 GNU 兼容工具链；更换编译器或 sysroot 时，
除了 C11 支持，还必须验证这些清理扩展、原子语义和 pthread 行为。不得仅通过改写
语言标准标记来宣称严格 C11 兼容。

## 不把工具链元数据当平台验收

一个在新 glibc 上构建的 `.so` 会产生其实际使用的符号版本依赖。设置 SOVERSION、
ELF 符号版本脚本或改文件名，都不能把这些调用变成旧 glibc 可用的实现。

主线 `glibc-runtime` 现在在 x86_64 Ubuntu 20.04 / 22.04 / 24.04 容器中分别构建并运行
共享库/静态库生产产物，执行生产库链接核心测试、安装后的 C/C++ 消费方
和 pkg-config消费方；共享库产物还用实际容器 glibc 作为 `--max-glibc` 上限。当前已验证
glibc 2.31、2.35、2.39 三个用户态环境。0.9.4 的 Ubuntu 20.04 共享库构建实际最高
GLIBC 符号需求为 2.25，但这只是该次构建的观测值，不自动把 2.25 宣称成长期
支持下限。容器共享 GitHub 运行器内核，因此这些结果**不建立最低 Linux 内核**；
32 位也仍未验收。

当前主线维护 Release 共享库/静态库配置下的普通 Logger 八点进程终止恢复矩阵。
历史 QEMU/ext4/XFS 断电报告描述对应旧提交，当前已移除该测试的专用程序、脚本、
构建目标与 CI，不再维护该测试类别。进程测试不等于物理存储认证。
最低内核 VM 属于平台兼容性验证，其脚本、CI 与运行时测试继续保留。

## 检查产物的可重复命令

```sh
python3 scripts/check_release_abi.py build/liblogger.so > abi.json
# 必须由发布负责人填写真实的部署基线；越界返回非零，不修改二进制。
python3 scripts/check_release_abi.py build/liblogger.so --max-glibc 2.25
# 目标架构和位数应来自已决定的支持矩阵。
python3 scripts/check_release_abi.py build/liblogger.so --machine 'Advanced Micro Devices X86-64' --elf-class ELF64
readelf -h -d -V build/liblogger.so
```

检查脚本记录导出集、默认符号版本、SONAME、动态依赖、CPU/ELF 类别和 GLIBC 需求。
指定 GLIBC 上限却读不到 GLIBC 版本时检查失败；可显式核对目标架构和 ELF 位数。
它也拒绝 libcrypto/libssl、Sanitizer 运行时、私有 GLIBC、机器私有 RPATH/RUNPATH/TEXTREL。
它不执行目标 ELF，所以可用来检查交叉产物；通过并不代表该产物已在目标环境运行过。
静态 `.a` 没有最终 GLIBC 动态需求；对最终 SDK/应用 ELF 做依赖检查，不能从归档
编造一个 GLIBC 最低版本。公开名称检查和 glibc 上限是不同门禁。

## 构建与验证顺序

1. 明确架构、最低内核、libc 版本、文件系统与挂载策略。
2. 在实际最低支持环境，或使用与它匹配的交叉工具链/sysroot 构建。
3. 记录编译器、sysroot 来源/校验、构建选项、产物校验和、动态符号需求。
4. 在实际目标执行完整功能/错误/生命周期测试，检测运行期特性。
5. 进行目标环境持续运行和资源检查；仅当部署另外要求物理掉电保证时，才在其非 volatile
   ext4/XFS 和真实存储栈上单独验收。这不是普通 Logger 基本 API 的发布门槛。

项目不再提供伪造“最低平台”的 CMake 工具链模板。部署团队必须提供真实且可追溯的
编译器/工具链/sysroot，并在 Xmake 配置中显式选择；不要把 `/` 或当前运行器根目录伪装成
旧系统 sysroot 来制造“交叉兼容成功”。工具链路径、版本、sysroot 来源和校验和都应进入
发布证据。

当前最低上游内核基线为 **Linux 5.10 / x86_64**，
与最低用户态 Ubuntu 20.04 / glibc 2.31 组合做真实 QEMU 运行验证。选择 5.10 的理由不是
“某个系统调用 最早在哪个版本出现”，而是它仍属于上游长期维护线，并且完整覆盖
当前运行时已实际依赖的 getrandom、renameat2/RENAME_NOREPLACE
以及 ext4/XFS 本地文件系统能力。更老的 3.x/4.x 不进入默认支持合同；若未来存在明确客户
需求，按具体发行版/向旧版本移植作为独立兼容层级验证，而不是扩大通用支持基线。

最低内核门禁只验证内核相关运行时能力：一个权威共享生产候选产物
在 Linux 5.10 + glibc 2.31 客户机中执行 getrandom 与 ext4/XFS 文件轮转
冒烟测试。共享/静态打包、完整消费方、process-crash 和 sanitizer 已有独立门禁，
不在 minimum-kernel 工作流中重复执行。

已知需平台验证/兼容设计的功能：`getrandom` 包装器/系统调用，`RENAME_NOREPLACE` 的内核和
文件系统支持，32 位 off_t/原子操作，以及时区和本地 syslogd。
没有能力时沿用既有明确错误，不降级为覆盖式重命名、不切换算法或隐藏 I/O 失败。

## 参考接口契约

- Xmake 目标版本/SONAME: https://xmake.io/api/description/project-target.html
- Xmake 安装/打包接口: https://xmake.io/api/description/xpack-interfaces.html
- GNU ld 符号版本: https://sourceware.org/binutils/docs/ld/VERSION.html
- getrandom 版本边界: https://man7.org/linux/man-pages/man2/getrandom.2.html

参考文档不构成对本产物在全部相关平台上已经验收的声明。
