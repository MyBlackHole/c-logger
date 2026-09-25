# 0.9.3 受控生产发布候选

这是一份**完整功能、仅内置 SHA-256**的受控生产发布候选，不是已经完成全部目标平台与掉电验收的 Production v1。
相对 0.9.0，本版保持 public ABI、SONAME 0 和 `LOGGER_0.9` 符号版本不变，主要收敛 async queue 内存/热点、self-paced wakeup、按需 metadata、ownership/locking 文档与对应回归。版本号不代表安全认证。

## 版本与 ABI

- 软件版本 `0.9.3`；候选 ABI major `0`；Linux SONAME `liblogger.so.0`。0.9.3 未新增/删除 public C symbol，继续使用 `LOGGER_0.9` 版本节点。
- 实体文件 `liblogger.so.0.9.3`，构建/安装系统生成 `liblogger.so.0`、`liblogger.so` 相对软链接。
- 动态导出严格限于 `cmake/logger.symbols` 中的 **62** 个既有公共 C 函数，版本节点 `LOGGER_0.9`。
  可选旧 fork helper 增加 `logger_fork_reinit`，共 **63** 个。没有新导出版本查询函数；
  `logger_version.h` 是编译时标识，`LoggerConfig.cmake` 也声明候选身份。
- 宿主不得用 packing、短枚举等选项改变公开布局；结构版本字段不修复编译器 ABI 差异。
- 公共函数用 `LOGGER_API` 标识，库使用 hidden visibility；ELF version script 使用 `local: *`。
  同一对象中的内部名称仍可能出现在调试/普通符号表；不承诺通过 strip 防止逆向分析。
- `find_package(Logger 0.9.3 EXACT CONFIG REQUIRED)`：pre-1.0 使用 ExactVersion，
  不许把“设置 SOVERSION”误写成“所有未来版本都二进制兼容”。v1 必须另行冻结支持矩阵和 ABI。
- 原有结构/枚举布局未改。C++11 默认配置宏新增 header-only 分支，不修改 C 分支或既有动态符号。
- 历史库 SONAME 为无版本 `liblogger.so`。本机旧二进制替换试验仅证明该实际组合；
  不能将这个候选普遍作为任意历史库的热替换品。依赖私有符号的旧调用方不受支持。
- 不同 ABI/独立静态副本之间不得交换 `logger_t *`。宿主及 SDK 要共享实例，应使用同一库实现，
  或把日志变成宿主回调。库名称相同不是对象可混用的证明。

## 构建、安装

最低构建工具 CMake 3.16、C11 编译器、Linux/ELF 与支持版本脚本的 GNU-compatible linker。
运行旧系统要使用对应工具链/系统根；构建工具最低版本不是运行平台验收结论。

```sh
cmake -S . -B build-shared -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF \
  -DCMAKE_INSTALL_PREFIX=/opt/logger/0.9.3
cmake --build build-shared -j4
cmake --install build-shared
```

静态产物使用独立构建目录：

```sh
cmake -S . -B build-static -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
  -DCMAKE_INSTALL_PREFIX=/opt/logger/0.9.3-static
cmake --build build-static -j4
cmake --install build-static
```

每个安装前缀描述一种选定的链接类型。不要先后往同一个前缀混装 static/shared 或兼容/默认
产物；覆盖 Targets/pc 文件不代表它们会自动变成多组件并存包。替换安装用新的干净前缀。
安装目录必须相对 prefix，不接受绝对 LIBDIR/INCLUDEDIR 或 `..`，保证包重定位。
支持 `DESTDIR` 和 `cmake --install --prefix`，默认私有头、测试库、fault injector、示例业务
SDK 二进制和 validation 不进入安装树。

```text
<prefix>/include/logger/{logger.h,audit.h,console.h,logger_export.h,logger_version.h}
<prefix>/lib/liblogger.so.0.9.3（或 liblogger.a）
<prefix>/lib/cmake/Logger/{LoggerConfig.cmake,LoggerConfigVersion.cmake,LoggerTargets*.cmake}
<prefix>/lib/pkgconfig/logger.pc
<prefix>/share/doc/prod_c_logger/{API.md,SECURITY.md,TESTING.md,docs/,examples/}
```

实际 lib/doc 目录受 GNUInstallDirs 影响，可以使用 `lib64` 或 multiarch 路径。
旧 helper 仅 `LOGGER_ENABLE_LEGACY_FORK_HELPER=ON` 编译并安装其头；CMake target 和 pc
同时传播相应宏，不在默认包中安装一个声明不存在符号的兼容头。

## 宿主接入

```cmake
find_package(Logger 0.9.3 EXACT CONFIG REQUIRED) # 可指定 COMPONENTS shared/static/legacy_fork
add_executable(host main.c)
target_link_libraries(host PRIVATE Logger::logger)
```

```c
#include <logger.h>
#include <audit.h>
#include <console.h>
```

无 CMake 时通过 pkg-config 获取路径/线程依赖。使用静态包时加 `--static`。
含空格路径使用支持 pkg-config shell quoting 的构建系统/参数解析；不要在 shell 中对输出
做未经处理的普通词拆分。没有依赖外部密码库。

只含无空格路径的常规例子：

```sh
PKG_CONFIG_PATH=/opt/logger/0.9.3/lib/pkgconfig \
  pkg-config --cflags --libs logger
# 运行时应配置 loader 路径或宿主自身 RUNPATH；库不自带机器私有 RUNPATH。
```

`examples/installed_consumer/` 是不需要源码树头文件的独立 C/C++11 工程，执行文件日志、多实例、
Audit、Console，并加载一个完整销毁实例后才卸载的 SDK MODULE。静态 Logger 使用 PIC，
可以被 SDK 链入；其符号默认不泄漏到 SDK 的动态符号表。SDK 自己决定其公开接口。
普通全局 LOG_* 与系统 syslog 同名宏的历史命名边界没有在本轮改变。

## 验收和分发包

```sh
cmake -S . -B build-check -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-check --target check -j4
cmake --build build-check --target check-package -j4
cpack --config build-check/CPackConfig.cmake -G TGZ
python3 scripts/check_release_abi.py build-check/liblogger.so
python3 scripts/check_production_artifact.py build-check/liblogger.so
```

`check-package` 会构建生产库再测试真正 install、DESTDIR、含空格前缀迁移、独立 CMake/
C++11/pkg-config consumer、PIC MODULE、旧头 consumer、公开布局/default 一致性、版本/组件
拒绝、ELF exports/SONAME 和运行期 dlsym/dlvsym。需要 native C/C++ 编译器、Python 3.8+、
pkg-config、nm、readelf。白盒测试不再要求公开私有符号；真实 DSO 测试继续直接运行 DSO。

CPack TGZ 为本工具链生成的 native **候选二进制**，不捆绑 glibc，不代表任意 Linux 可运行。
CPack 同时生成 SHA-256 校验文件。上游没有提供项目 LICENSE 文本，本轮不擅自增加许可证；
发行者仍应明确第三方交付授权和版本支持策略。

Sanitizer 构建默认 `LOGGER_ENABLE_INSTALL=OFF`；显式要求安装插桩产物会配置失败。
这些构建不生成包，不能误发给宿主。交叉编译不会尝试在 build host 执行 native 安装测试；
必须在目标环境另行运行，而不是当成测试通过。

## 兼容平台与未完成门禁

见 PLATFORM_BASELINE.md。当前这里只冻结一个可检查的候选快照，不新增老平台兼容代码，
不宣称系统掉电、任意网络文件系统、任意 fork 或非法生命周期安全。全部功能保留，
不以“缩减为 File-only”替代实际支持矩阵和持久化验收。
