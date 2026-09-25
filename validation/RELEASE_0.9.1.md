# c-logger 0.9.1 发布验证

日期：2026-09-25

0.9.1 定位为 **Controlled Production Candidate / 受控生产发布候选**。
它不是通用 Production v1；目标 kernel/glibc、真实存储掉电、NFS/SMB、32-bit 等仍需部署环境验收。

## Release identity

- project version: `0.9.1`
- ABI major: `0`
- shared object: `liblogger.so.0.9.1`
- SONAME: `liblogger.so.0`
- ELF symbol version: `LOGGER_0.9`
- public C symbol set: 与 0.9.0 保持一致
- crypto implementation: builtin SHA-256 only
- external `libcrypto/libssl`: 无

0.9.1 是 patch release，没有改变 public struct/signature/SONAME/symbol-version contract。

## Ordinary CI

GitHub Actions run: `36089818822`

全部通过：

- native Release/shared tests；
- shared ABI inspection；
- native Debug complete suite；
- ASan/UBSan；
- TSan。

## Package validation

GitHub Actions run: `36089818832`

shared/static 两个 Release package job 均通过：

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

- DESTDIR install；
- 带空格 install prefix；
- relocation；
- installed C consumer；
- installed C++11 consumer；
- SDK/PIC MODULE；
- pkg-config consumer；
- ExactVersion/component rejection；
- frozen old-header consumer + new library；
- public C/C++ layout/default 一致性；
- production/test artifact isolation。

## Shared artifact

production artifact inspection：

```text
artifact: liblogger.so.0.9.1
passed: true
NEEDED:
  libc.so.6
  ld-linux-x86-64.so.2
crypto: builtin-sha256-only
```

ABI inspection：

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

GitHub Actions artifact：

- name: `prod-c-logger-0.9.1-shared`
- artifact id: `10844898699`
- artifact ZIP digest:
  `sha256:491746efcfbc9ac8f10d282513b8db737cfd7ceb66153bf34988eec049f4831d`

注意：上面的 Actions artifact digest 是上传 ZIP 的 digest，不替代 TGZ 自带的
`.tar.gz.sha256` 文件。

## Static artifact

CPack：

```text
prod-c-logger-0.9.1-Linux-x86_64-static.tar.gz
prod-c-logger-0.9.1-Linux-x86_64-static.tar.gz.sha256
```

GitHub Actions artifact：

- name: `prod-c-logger-0.9.1-static`
- artifact id: `10844953547`
- artifact ZIP digest:
  `sha256:1588b8aebc90c640ad9f2a3e69f268e692e9282d3ecf39543ee96f7094dada25`

## Release publication

release branch merge 后，`.github/workflows/release-publish.yml` 必须从最终 `main`
commit **重新**完成 shared/static 的：

- build；
- check-package；
- production artifact inspection；
- shared ABI inspection；
- CPack；
- package-name verification。

只有这些步骤再次成功后，workflow 才允许：

```text
create GitHub prerelease v0.9.1
target = final main merge commit
attach shared/static TGZ + SHA256
```

这样 PR validation artifact 只作为证据，不会直接冒充最终发布包。

## Remaining deployment gates

0.9.1 发布不代表以下事项已经完成：

- 全 Linux 3.x/4.x/5.x、老 glibc、32-bit 支持矩阵；
- 目标机器/sysroot 上的完整执行验收；
- 非 volatile ext4/XFS + 实际存储栈的 crash/power-loss durability；
- NFS/SMB；
- 远端 Syslog durable acknowledgement/replay；
- signal-safe / generic async-cancel-safe；
- raw multithreaded fork 后复用 inherited runtime；
- Audit 外部可信签名、可信锚点或 WORM 认证。

这些边界继续以 `docs/PLATFORM_BASELINE.md` 和 `docs/KNOWN_ISSUES.md` 为准。
