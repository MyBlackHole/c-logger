# c-logger 0.9.2

0.9.2 是 0.9.1 之后的**受控生产发布候选**。本版更新发布门禁并重新构建候选包；
运行时 `src/` 未改变。仍不是完成目标平台及真实掉电验收的通用 Production v1。

## 变化

- shared 和 static 的 Release 发布验证都先运行完整 CTest，再执行安装、ABI/产物检查和 CPack。
- static 的白盒回归改用独立同源测试库，避免 FORTIFY 改写 libc 入口后绕开 `--wrap`
  同步/错误注入钩子。生产 `liblogger.a` 不作为白盒测试库，也不安装测试库。
- ABI 检查器可核对目标 Machine/ELF class；指定 GLIBC 上限但读不到 GLIBC 版本时失败。

## ABI 与接入

- 软件版本：`0.9.2`；ABI major：`0`。
- SONAME：`liblogger.so.0`；动态符号版本：`LOGGER_0.9`。
- 62 个默认 public C 符号及 public 结构/签名保持不变。
- CMake 使用 `find_package(Logger 0.9.2 EXACT CONFIG REQUIRED)`；pkg-config 版本为 `0.9.2`。
- 宿主继续负责实例创建、停止借用者、flush、destroy，再卸载 SDK。

## 尚未完成

最低 kernel/glibc/架构、目标 ext4/XFS 与存储栈的掉电恢复、真实 syslogd、32 位、
NFS/SMB 均没有因本次发布获得新的验收结论。Audit 没有外部可信签名、锚点或 WORM 认证。
详见 `docs/KNOWN_ISSUES.md` 和 `docs/PLATFORM_BASELINE.md`。
