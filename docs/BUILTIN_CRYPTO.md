# 内置 SHA-256：当前契约

## 范围

本版仅提供 src/audit_integrity.c 的内置 SHA-256。SM3 的压缩函数、IV、旋转函数、
注册项、标准/二进制正向向量和运行参数已删除；不存在隐藏可选后端。未引入 OpenSSL、
libcrypto/libssl、外部密码 provider 或其他替代密码依赖。只读历史 fixture 不构成运行依赖。

SHA-256 的轮函数、常量、输入检查、padding 和输出字节语义保留；单算法版本直接调用
SHA-256 压缩函数，不再保留共用 compressor 回调。保留私有 checked digest adapter，
让 Audit 写入、恢复、验证统一使用 `int hash(...)` 的 0/-errno、错误时输出不变契约。
它只有一个实现，拒绝任何不支持的 ID，没有后端装载或回退。CRYPTO_FAILED 状态保留。

## 配置

```c
audit_config_t cfg = AUDIT_DEFAULT_CONFIG(); /* 默认 SHA-256 */
cfg.log_dir = "/var/log/myapp";
cfg.name = "backup";
/* cfg.integrity = AUDIT_INTEGRITY_SHA256; 默认如此，无需再写。 */
```

`AUDIT_INTEGRITY_NONE` 继续作为显式关闭完整性的既有开关，默认不开启；它不是另一个
摘要算法。hash/verifier 不能用 NONE，也绝不能因错误自动关闭完整性。
`audit_crypto_backend()` 仍返回 `builtin`。没有新增公共 API、改动公有结构布局。
删除 `AUDIT_INTEGRITY_SM3` 常量是源码 API 收紧：引用它的应用必须修改配置。

## 构建

```sh
cmake -S . -B build -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target check -j4
cmake -S . -B build-prod -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF
cmake --build build-prod -j4
python3 scripts/check_production_artifact.py build-prod/liblogger.so
```

不需要密码后端或摘要算法的 CMake 开关。旧 LOGGER_CRYPTO_BACKEND=builtin 参数仍按
迁移规则提示弃用并删除缓存；旧 openssl 或其他值明确拒绝。宿主自己的密码依赖不受影响。

## 已有数据与拒绝语义

- SHA-256 = 1 和 NONE = 0 编号不变，原编号 2 永久保留，不复用。
- 旧 SHA-256 日志和 checkpoint v2 无需重写；双方版本可验证和接续。
- API 显式传原算法编号 2：初始化/with-verifier 拒绝 EPROTONOSUPPORT，不计算其他算法。
- checkpoint v2 alg=2：load 按原严格 parser 约定返回 EBADMSG、输出不变；audit_init
  停止，已有日志/state 字节不变。失败的 init 可能创建正常协调锁文件，但不发布新实例。
- 没有 checkpoint 的 SM3 记录也不作为 SHA-256 继续写；默认 SHA-256 全链验证拒绝。
- 即使把旧 state 的算法 ID 改成 1 并重算 CRC，真实记录摘要仍会在恢复时验证失败。
- 不自动识别纯日志行的算法，不自动重算历史或在一条链中换算法。SM3 历史请只读保留，
  用旧版本离线验证，为新 SHA-256 历史选择独立文件名/目录，避免破坏历史证据。

`tests/fixtures/legacy_crypto/sm3` 是删除前冻结的只读负向样本，仅证明拒绝不会改写历史，
不是本版支持或实现 SM3。历史文档与 validation 仍如实保留曾经的 SM3/OpenSSL 结果；
不得把历史通过数量当作当前清单。当前验证见 validation/SHA256_ONLY_RESULTS.md。

## 验证与保证边界

148 个 SHA-256 二进制长度预期值从独立生成的旧 fixture 原样保留，不从被测实现再生成；
空输入、abc、多块、百万字符、输入输出别名、非法长度、并发和错误注入均继续验证。
仅删除 SM3 专属参数化分支/用例（含该算法的故障场景）；其他文件、Syslog、Console、生命周期、队列、审计失败、
严格解析/恢复测试保留。新增原 ID/历史的 fail-closed 负向测试和产物扫描。

这是摘要算法范围收敛，不是密码模块认证或 Production v1 发布。无密钥 hash chain
不能阻止整体重算、截尾或回滚。旧可疑数据不自动修复；平台、真实掉电、安装/ABI
发布和性能仍按 KNOWN_ISSUES.md 的未验收范围处理。
