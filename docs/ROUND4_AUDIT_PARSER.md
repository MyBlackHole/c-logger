# Round 4：严格 Audit KV 解析和保守恢复

## 范围

本轮基于 `prod_c_logger_crypto_failclosed_fix.tar.gz`。没有新增公开函数，也没有改变
现有记录的摘要字节域、SHA-256/SM3 算法标识或 checkpoint v2 字段布局。
修复了 quoted 值误解析、宽松 hash 后缀、EOF/长度混淆、checkpoint 宽松读取、
归档发现和 checkpoint 文件位置的猜测。普通 Logger fork 和生产 fault hooks 未修改。

## 共享编码/解析契约

`src/audit_record.c/.h` 是私有 codec。writer 的普通 payload 编码、离线 verifier 和
recovery 都使用它，不再各自使用 `strstr(" hash=")` 找结构字段。

```text
[固定 logger envelope] instance=<32小写hex> seq=<u64> txn=<u64> phase=<ATTEMPT|RESULT>
 event=<quoted> actor=<quoted> source=<quoted> resource=<quoted> operation=<quoted>
 [result=<SUCCESS|FAILURE> error=<int>]
 [detail=<quoted>] prev=<64小写hex> hash=<64小写hex>\n
```

实际是一行。空格和字段顺序固定；RESULT 必须有 result/error，ATTEMPT 不允许有它们。
拒绝未知字段、重复字段、缺失字段、无效枚举、整数溢出、非规范前导零、`error=-0`、
不合法 escape、短/长/非 hex hash，以及 hash 后的任何额外数据（包括空格）。
seq 非零，txn 可为零，error 必须在当前平台 int 的范围内。
event/operation 非空；其余字符串可为空，NULL 按空值编码。

quoted 值支持 `\\`、`\"`、`\n`、`\r`、`\t`。其他 C0 控制字节和 DEL 在新写入时
编码为小写 `\xhh`，因此不再把它们原样放入物理日志行。`\x00` 不是 C 字符串输入可表示的内部字符；拒绝它及不必要/未知的 escape。
高位字节保留，不在这里声称已经验证 UTF-8 或已经脱敏。

普通打印字符串及原先五种 escape 的编码字节保持不变。以前已写入的 raw C0/DEL、
非规范行、旧损坏全零摘要不被静默修成“合法历史”：新严格读者会拒绝，需要保留证据后
人工核查。缺少真实 LF 的完整记录也被拒绝，不自动补换行或删除。

验证器接受当前固定的 Audit envelope 或裸 canonical payload（用于导出校验）。
envelope 只校验语法：时间/时区、INFO、pid/tid、[AUDIT]；**它依然不参与旧 hash 域**。
所以这不是“时间已被密码认证”的功能。若需要认证时间，必须另行设计版本化的审计字段。

## 有界读取，不把缓冲区读满当 EOF

payload 上限 4095 字节，物理记录读取上限 4608 字节（含 LF），checkpoint 读取上限
511 字节（含 LF）。这些限制在私有头中集中定义，writer 对真实 logger message 上限
有静态断言。读取用固定缓冲区，超过限制立即返回 EOVERFLOW，不使用无限增长的 getline。
读取到 NUL、坏完整记录返回 EBADMSG；I/O 错误保留 errno；EINTR 重试。

`AUDIT_LINE_PARTIAL` 只能由真正读到 EOF 得到，不是 `fgets` 返回一个无 LF 缓冲区就推定。
离线验证器一律拒绝 partial。读取和解析失败不会发布半成品 final hash、checkpoint 或 view。

## EOF 残片保留

恢复会先完成整个保留文件集合的解析、密码验证和链定位，之后才考虑改 active 文件。
只有 active 文件真实 EOF 的有限残片可能进入修复；archive 永远不截断。
完整但损坏的行、超长行/超长 EOF、嵌入 NUL、完整结构仅缺 LF 均失败并保留原文件。

可处理的残片顺序：

1. 确认 active inode/大小/mtime/ctime 与扫描时一致，并复核路径仍指向同一文件。
2. 创建同目录唯一 `xxx.audit.log.tail.XXXXXX`，以 0600 保存原始尾部字节。
3. 检查完整写入、证据文件 fsync/close、证据目录 fsync。
4. 再次复核 active，truncate 到最后一个已校验记录边界，检查 active fsync。
5. 只有成功后向调用者发布恢复后的 checkpoint 候选。

`.tail.*` 不是普通可轮换日志，不会被 archive matcher 或 retention 当成日志段。
不要自动删除它；应纳入运维异常检查/保留策略。本轮没有添加公开的“发生过恢复”状态字段
或自动 AUDIT_RECOVERY 事件。

**真正 EOF 不证明一定发生过崩溃**，人工截断或攻击也能产生相同现象。保留证据是为了
不无痕删除；这仍不是认证型防篡改。若 truncate 后的 fsync 失败，函数会返回错误且不
发布 checkpoint，但文件大小可能已经改变，证据文件仍保留。不能声称跨文件操作已回滚。

## Checkpoint v2 的严格读取

必须恰好一行、完整 LF、精确字段和 CRC32，不接受第二行、尾随字段、短 hash/CRC、
负 offset 或数字溢出。offset 不能超过 INT64_MAX。输出参数只在成功后更新；ENOENT 才
视为 checkpoint 缺失，不把空/损坏文件当缺失。读取拒绝末级 symlink 和非普通文件，
避免在 FIFO 上无限等待。没有新增 v1 自动迁移，也不将 CRC32 当安全认证。

## 跨归档：用已验证的链关系，不用文件大小/时间猜测

识别两种精确形状：

```text
xxx.audit.YYYYMMDDTHHMMSS.uuuuuuZ.log  # 当前 file backend 实际输出
xxx.YYYYMMDDTHHMMSS.uuuuuuZ.audit.log  # 兼容此前约定/导出的布局
```

本轮未改变 writer 的命名方式，也未修复通用 rotation 的同时间名覆盖风险。
短相似文件名不会越界读取；不匹配的 .old/.tmp/.tail 文件不当成历史段。

恢复先扫描全部匹配 archive 和 active：每个完整记录按同一 codec 校验，检查文件内部
prev 链；checkpoint 要命中真实已验证记录的 `offset + seq + hash`。
不再使用 `active_size >= offset` 判断“这是原文件”，也不会从错误文件中间开始截断。
offset=0/nonzero hash 的已有段边界 checkpoint 则必须命中已验证 archive 尾部。

随后用各非空段的 first-prev / final-hash 建立唯一链，校验全部保留段。拒绝缺中段、
重复段、分叉、多根、回环，以及 active 出现在非末端。时间回拨导致的文件名排序反转
不影响链顺序；文件名本身不被当作安全身份。空 active/缺 active 的正常轮换窗口也能
从匹配到的历史锚点接续。缺 checkpoint 时只从零锚点的完整保留链重建；只有后半段
历史且无 checkpoint 时拒绝猜测。保留历史存在缺口，即使位于 checkpoint 之前，也会
保守拒绝，而不是默默跳过。

代价明确：**启动不再只是 O(未检查尾部)**，本轮采用安全优先的完整保留集合扫描。
I/O 是 O(保留日志字节数)，段连接检查当前 O(N²)，最多 4096 个 archive，内存有界。
没有声称这是大规模归档的最终性能方案；未来可以引入持久 segment ID、可信锚点/index
优化，但不能重新退回按 size/mtime 推断身份。

## 没有解决的边界

仍依赖受控目录和协作式单 writer 锁；不能抵御有权同时改写日志、checkpoint 和目录的
攻击者。没有外部可信 tip 就不能证明完整尾部未被删除，更不能阻止全链重算。
活动外部写入/外部轮换、锁 inode 替换、网络文件系统锁/持久化语义等不在本轮保证范围。
本轮 process crash 是 `_exit` + fresh exec 恢复，不等于断电/设备故障模拟。

下一步优先普通 Logger fork 契约/超时和生产 fault hooks 隔离；仍不能发布 Production v1。
