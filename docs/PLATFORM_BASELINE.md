# 目标平台构建基线

## C 语言与编译器方言

项目的语言基线是 **C11**，但不是 strict ISO C11。Xmake 的 production target 显式使用：

```text
-std=gnu11 -Wall -Wextra -Wpedantic -Werror
```

内部 Linux-style 资源管理依赖 GCC/Clang 兼容 GNU C 扩展，包括
`__attribute__((cleanup))`、`typeof`/statement expression 和唯一标识宏。
因此生产源码按 **C11 + GNU C extensions**（通常即 `-std=gnu11`）维护。
这些扩展只存在于私有实现，不进入已安装 public ABI/header 契约。

当前 CI 验证的是 Linux/ELF 上的 GNU-compatible toolchain；更换编译器或 sysroot 时，
除了 C11 支持，还必须验证这些 cleanup 扩展、原子语义和 pthread 行为。不得仅通过改写
语言标准 flag 来宣称 strict-C11 兼容。

## 不把工具链元数据当平台验收

一个在新 glibc 上构建的 `.so` 会产生其实际使用的符号版本依赖。设置 SOVERSION、
ELF symbol version script 或改文件名，都不能把这些调用变成旧 glibc 可用的实现。

主线 `glibc-runtime` 现在在 x86_64 Ubuntu 20.04 / 22.04 / 24.04 容器中分别构建并运行
shared/static production artifact，执行 production-linked core tests、installed C/C++ consumer
和 pkg-config consumer；shared 产物还用实际容器 glibc 作为 `--max-glibc` ceiling。当前已验证
glibc 2.31、2.35、2.39 三个 userland。0.9.4 的 Ubuntu 20.04 shared build 实际最高
GLIBC symbol requirement 为 2.25，但这只是该次构建的观测值，不自动把 2.25 宣称成长期
支持下限。容器共享 GitHub runner kernel，因此这些结果**不建立最低 Linux kernel**；
32-bit 也仍未验收。

当前主线另有两类 crash/durability 证据：Release shared/static 的进程终止恢复矩阵，
以及 Ubuntu 24.04 runner 上 QEMU guest 使用 raw ext4/XFS 虚拟盘的 10 点强制断电矩阵。
后者在写入阶段观察到指定 serial marker 后由 host SIGKILL QEMU，再使用同一 raw disk
启动恢复并验证 Audit chain；XFS guest 会把与所选 guest kernel 匹配的 XFS module/dependency
打入最小 initramfs。串口证据显式记录 guest kernel release 和 mounted filesystem，避免把
宿主环境误当成 guest 验收结果。该矩阵比 OverlayFS 进程退出测试更接近掉电语义，但仍不是
实际服务器、存储控制器/磁盘 volatile cache 或最低支持 kernel 的平台验收。

## 检查产物的可重复命令

```sh
python3 scripts/check_release_abi.py build/liblogger.so > abi.json
# 必须由发布负责人填写真实的部署基线；越界返回非零，不修改二进制。
python3 scripts/check_release_abi.py build/liblogger.so --max-glibc 2.25
# 目标架构和位数应来自已决定的支持矩阵。
python3 scripts/check_release_abi.py build/liblogger.so --machine 'Advanced Micro Devices X86-64' --elf-class ELF64
readelf -h -d -V build/liblogger.so
```

检查脚本记录导出集、默认符号版本、SONAME、动态依赖、CPU/ELF 类别和 GLIBC requirements。
指定 GLIBC 上限却读不到 GLIBC 版本时检查失败；可显式核对目标架构和 ELF 位数。
它也拒绝 libcrypto/libssl、sanitizer 运行时、私有 GLIBC、机器私有 RPATH/RUNPATH/TEXTREL。
它不执行目标 ELF，所以可用来检查交叉产物；通过并不代表该产物已在目标环境运行过。
静态 `.a` 没有最终 GLIBC 动态需求；对最终 SDK/应用 ELF 做依赖检查，不能从 archive
编造一个 GLIBC 最低版本。公开名称检查和 glibc ceiling 是不同门禁。

## 构建与验证顺序

1. 明确 architecture、最低 kernel、libc 版本、文件系统、挂载与 procfs 策略。
2. 在实际最低支持环境，或使用与它匹配的交叉工具链/sysroot 构建。
3. 记录编译器、sysroot 来源/校验、构建选项、产物 checksum、动态符号需求。
4. 在实际目标执行完整功能/错误/生命周期测试，检测运行期特性。
5. 在非 volatile 的 ext4/XFS 和真实存储栈验证崩溃/掉电语义，进行持续运行和资源检查。

项目不再提供伪造“最低平台”的 CMake toolchain 模板。部署团队必须提供真实且可追溯的
compiler/toolchain/sysroot，并在 Xmake 配置中显式选择；不要把 `/` 或当前 runner 根目录伪装成
旧系统 sysroot 来制造“交叉兼容成功”。工具链路径、版本、sysroot 来源和 checksum 都应进入
发布证据。

已知需平台验证/兼容设计的功能：`getrandom` wrapper/系统调用，`RENAME_NOREPLACE` 的内核和
文件系统支持，Audit 的 procfd 路径，32 位 off_t/原子操作，以及 timezone 和本地 syslogd。
没有能力时沿用既有明确错误，不降级为覆盖式 rename、不切换算法或隐藏 I/O 失败。

## 参考接口契约

- Xmake target version/SONAME: https://xmake.io/api/description/project-target.html
- Xmake 安装/打包接口: https://xmake.io/api/description/xpack-interfaces.html
- GNU ld symbol version: https://sourceware.org/binutils/docs/ld/VERSION.html
- getrandom 版本边界: https://man7.org/linux/man-pages/man2/getrandom.2.html

参考文档不构成对本产物在全部相关平台上已经验收的声明。
