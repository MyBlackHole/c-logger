# Xmake Build Parity

## 状态

Xmake 当前是 **并行验证构建**，不是发布权威。

- CMake 继续负责完整测试、install、CMake package、pkg-config、CPack 和 GitHub Release。
- Xmake 第一阶段只负责生产 `logger` shared/static artifact。
- Xmake 产物必须通过与 CMake 发布产物相同的 ABI/production-isolation 检查。
- 当前 Xmake 产物不得直接替代正式 release asset。

CI 固定使用 Xmake 3.1.1；工程最低要求提升为 2.8.5：SONAME version support 需要 2.8.2，
内置 `xmake test` / `add_tests` 从 2.8.5 开始提供。

## 第一阶段覆盖

Xmake 已表达以下生产契约：

- Linux/ELF only；
- GNU C11；
- shared / static；
- static PIC；
- hidden visibility + public `LOGGER_API`；
- `liblogger.so.0.9.3` / SONAME `liblogger.so.0`；
- `LOGGER_0.9` ELF symbol version；
- public symbol allowlist 直接读取现有 `cmake/logger.symbols`，不维护第二份 ABI 清单；
- `--no-undefined` / `--no-undefined-version`；
- production fault injection disabled；
- builtin SHA-256 only；
- optional legacy fork helper。

Xmake 没有采用内置 `mode.release` rule，因为该 rule 默认 strip。当前配置显式保持项目自己的 Release/ABI 语义，而不是让构建框架默认值定义 ABI。

## 使用

Shared Release：

```sh
xmake f -m release --build_shared=y
xmake -j4 logger
```

Static Release：

```sh
xmake f -m release --build_shared=n
xmake -j4 logger
```

生产产物仍使用已有脚本验证：

```sh
python3 scripts/check_production_artifact.py <artifact>
python3 scripts/check_release_abi.py <shared-elf>
```

## 第二阶段：core test parity

已加入 `build_tests` 可选配置，并使用 Xmake 自带的 `add_tests` / `xmake test` 注册和运行
25 个**直接链接 production logger** 的核心测试。该集合覆盖基础 Logger、rotation、Audit、
Console、format/context/permissions、fork guard、lifecycle、concurrency、API/ABI、
single-writer、redaction、reinit 和 multi-instance。

这一阶段故意不把 white-box support library、`--wrap`、fault injection 或 crash hook
混进 production-linked 测试，以继续保持生产/测试实现边界。

### Test-only support parity

Xmake 现在还提供一个默认关闭的 `build_private_tests` 配置，构建独立的
`logger_test_support` 静态测试库。该 target 与 production `logger` 分离，额外编译
`src/logger_fault.c` 并启用测试宏，只用于以下现有测试：

- 5 个 file/state error-path cases；
- 4 个 process crash/recovery cases；
- multi-instance syslog test。

该 job 使用 `xmake test -j1`：现有 4 个 recovery case 复用同一组 `crash.audit.*` 测试文件名，
因此不能在同一工作目录并发执行；这与 production 并发模型无关。

这验证了测试专用实现与 production artifact 可以在 Xmake 下保持分离；更复杂的
regression support、link-time `--wrap` 仍留到下一步。

## 尚未迁移

以下仍由 CMake 独占，未达到 parity 前不得删除 CMake：

1. white-box regression support 与 link-time `--wrap` cases；
2. sanitizer profiles；
3. QEMU power-cut guest target；
4. install tree；
5. `LoggerConfig.cmake` / ExactVersion / components；
6. pkg-config；
7. C/C++/旧 header/SDK MODULE installed consumers；
8. DESTDIR、relocation、prefix migration；
9. CPack TGZ + SHA-256；
10. release-publish。

## 下一门禁

production artifact 与 core test CI 稳定后，再迁 private regression/fault/crash targets。只有 shared/static 测试语义、sanitizer、crash 和 VM power-cut
都能复用相同验证证据后，才进入 install/package parity。最终是否把 Xmake 升为 authoritative
build system，需要在 install/package parity 完成后再决定。
