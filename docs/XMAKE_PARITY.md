# Xmake Build Parity

## 状态

Xmake 当前是 **并行验证构建**，不是发布权威。

- CMake 继续负责完整测试、install、CMake package、pkg-config、CPack 和 GitHub Release。
- Xmake 第一阶段只负责生产 `logger` shared/static artifact。
- Xmake 产物必须通过与 CMake 发布产物相同的 ABI/production-isolation 检查。
- 当前 Xmake 产物不得直接替代正式 release asset。

CI 固定使用 Xmake 3.1.1；工程最低要求为 2.8.2，因为 SONAME version support 从该版本开始。

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

## 尚未迁移

以下仍由 CMake 独占，未达到 parity 前不得删除 CMake：

1. 完整 CTest / regression / fault / crash 测试注册；
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

第一阶段 CI 稳定后再迁测试目标。只有 shared/static 测试语义、sanitizer、crash 和 VM power-cut
都能复用相同验证证据后，才进入 install/package parity。最终是否把 Xmake 升为 authoritative
build system，需要在 install/package parity 完成后再决定。
