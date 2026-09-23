# 固定摘要参考数据与历史样本

## SHA-256 向量

`../regression/crypto_boundary_vectors.h` 保留 148 个确定性二进制输入的 SHA-256 预期值。
其来源是更早轮次的 Python hashlib/OpenSSL 独立计算，不是当前被测算法。当前实现和
测试不会调用任何外部密码库。此次只删除 SM3 列，SHA-256 长度和预期值逐项保持一致。
标准空输入、abc、多块、million-a 向量仍在 test_crypto_contract.c。

不要用生产实现输出重新生成自己的预期值。未来更改需独立来源和核对。

## 合法 SHA-256 历史

`legacy_crypto/sha256/app.audit.{log,state}` 是前轮从未修改的 external backend
生成的冻结数据；出处、生成日志和 checksum 见历史 `validation/no_openssl/`。
当前测试验证、恢复并接续，确认旧字节前缀不变。不需要运行旧工具。

## 已删除算法的负向样本

`legacy_crypto/sm3/app.audit.{log,state}` 保留相同历史来源的原始数据，只用于验证
新版本拒绝原算法编号 2 和相关历史、且不截断/重算/改写日志与 checkpoint。
fixture 字节未变，不提供 SM3 计算、验证或迁移能力。公开配置常量已删除。
