# c-logger 0.9.1 发布验证

日期：2026-09-25

0.9.1 定位为 **受控生产发布候选**。
它不是通用生产版 v1；目标内核/glibc、真实存储掉电、NFS/SMB、32 位等仍需部署环境验收。

## 发布身份

- 项目版本：`0.9.1`
- ABI 主版本：`0`
- 共享对象：`liblogger.so.0.9.1`
- SONAME：`liblogger.so.0`
- ELF 符号版本：`LOGGER_0.9`
- 公开 C 符号集：与 0.9.0 保持一致
- 密码实现：仅内置 SHA-256
- 外部 `libcrypto/libssl`：无

0.9.1 是补丁版本，没有改变公开结构/函数签名/SONAME/符号版本契约。

## 常规 CI

GitHub Actions 运行：`36089818822`

全部通过：

- 原生发布构建/共享库测试；
- 共享库 ABI 检查；
- 原生 Debug 完整测试集；
- ASan/UBSan；
- TSan。

## 软件包验证

GitHub Actions 运行：`36089818832`

共享库/静态库两个发布软件包任务均通过：

```text
shared:
  full build
  check-package
  production artifact isolation
  ABI inspection
  CPack TGZ + SHA256
  exact package-name gate
  Actions artifact upload

static:
  full build
  check-package
  production artifact isolation
  CPack TGZ + SHA256
  exact package-name gate
  Actions artifact upload
```

`check-package` 实际覆盖：

- DESTDIR 安装；
- 带空格的安装前缀；
- 重定位；
- 已安装 C 消费方；
- 已安装 C++11 消费方；
- SDK/PIC 模块；
- pkg-config 消费方；
- ExactVersion/组件拒绝；
- 冻结旧头文件消费方 + 新库；
- 公开 C/C++ 布局/默认值一致性；
- 生产/测试产物隔离。

## 共享库产物

生产产物检查：

```text
artifact: liblogger.so.0.9.1
passed: true
NEEDED:
  libc.so.6
  ld-linux-x86-64.so.2
crypto: builtin-sha256-only
```

ABI 检查：

```text
passed: true
SONAME: liblogger.so.0
public symbols: @@LOGGER_0.9
```

CPack：

```text
prod-c-logger-0.9.1-Linux-x86_64-shared.tar.gz
prod-c-logger-0.9.1-Linux-x86_64-shared.tar.gz.sha256
```

GitHub Actions 产物：

- 名称：`prod-c-logger-0.9.1-shared`
- 产物 ID：`10844898699`
- 产物 ZIP 摘要：
  `sha256:491746efcfbc9ac8f10d282513b8db737cfd7ceb66153bf34988eec049f4831d`

注意：上面的 Actions 产物摘要是上传 ZIP 的摘要，不替代 TGZ 自带的
`.tar.gz.sha256` 文件。

## 静态库产物

CPack：

```text
prod-c-logger-0.9.1-Linux-x86_64-static.tar.gz
prod-c-logger-0.9.1-Linux-x86_64-static.tar.gz.sha256
```

GitHub Actions 产物：

- 名称：`prod-c-logger-0.9.1-static`
- 产物 ID：`10844953547`
- 产物 ZIP 摘要：
  `sha256:1588b8aebc90c640ad9f2a3e69f268e692e9282d3ecf39543ee96f7094dada25`

## 发布

发布分支合并后，`.github/workflows/release-publish.yml` 必须从最终 `main`
提交**重新**完成共享库/静态库的：

- 构建；
- `check-package`；
- 生产产物检查；
- 共享库 ABI 检查；
- CPack；
- 软件包名称验证。

只有这些步骤再次成功后，工作流才允许：

```text
create GitHub prerelease v0.9.1
target = final main merge commit
attach shared/static TGZ + SHA256
```

这样 PR 验证产物只作为证据，不会直接冒充最终发布包。

## 剩余部署门禁

0.9.1 发布不代表以下事项已经完成：

- 全 Linux 3.x/4.x/5.x、老 glibc、32 位支持矩阵；
- 目标机器/sysroot 上的完整执行验收；
- 非易失 ext4/XFS + 实际存储栈的崩溃/掉电持久性；
- NFS/SMB；
- 远端 Syslog 持久确认/重放；
- 信号安全 / 通用异步取消安全；
- 原始多线程 fork 后复用继承运行时；
- Audit 外部可信签名、可信锚点或 WORM 认证。

这些边界继续以 `docs/PLATFORM_BASELINE.md` 和 `docs/KNOWN_ISSUES.md` 为准。
