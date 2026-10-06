# Production v1 公共 ABI / UAPI 规则

本项目的公开 C ABI 按 Linux UAPI 的演进纪律维护，而不是按“普通用户态库可以随意修改结构体”
的方式维护。pre-v1 候选接口不构成兼容承诺；从 Production v1 发布开始，下列规则成为硬约束。

## 1. 已发布 ABI 不破坏

- 已发布 public symbol、函数类型、结构体已有字段偏移、枚举/宏数值不得原地重解释。
- 破坏性变化进入新的 ABI major / symbol version。
- 不为 0.x 候选接口保留兼容适配层；v1 之前允许清理，v1 之后禁止破坏。

## 2. 可扩展结构体

对带 `struct_size/version` 的 public config：

1. 先只读取固定 ABI header：`struct_size`、`version`。
2. 不支持的 version 返回 `EPROTONOSUPPORT`。
3. `struct_size < 当前已知结构大小` 返回 `EINVAL`。
4. `struct_size == 当前已知结构大小` 正常复制。
5. `struct_size > 当前已知结构大小` 时检查未知 tail：
   - 全 0：接受；
   - 任意非 0：返回 `E2BIG`。

这等价于 Linux `copy_struct_from_user()` 的关键演进语义：未来调用者可以把未知字段保持为零
与旧实现协作，但旧实现绝不能静默忽略一个实际请求的新功能。

## 3. 扩展规则

- 新字段只能追加在结构体尾部，不能插入已有字段之间。
- 新增字段的零值必须表示“未请求新语义”或安全默认行为。
- 如果无法满足“零值可由旧实现安全忽略”，必须提升 `version`。
- 新 flag 使用独立 bit；未知置位 bit 必须拒绝，不能静默清除。
- 不复用已发布枚举值、flag bit、reserved 值。

## 4. 配置快照与指针

结构体本身在 init/create 期间复制到库内快照。结构体中的字符串指针属于调用期借用：
调用者必须保证它们在调用返回前有效且不被并发修改。除非 API 明确说明，运行时不长期持有
配置结构体或其字符串指针。

## 5. 适用对象

Production v1 的顶层配置入口统一遵守本规则：

- `logger_config_t`
- `audit_config_t`
- `console_config_t`

`logger_syslog_config_t` 当前仅作为 `logger_config_t` 的嵌套组成部分，不是独立版本入口；
其现有字段不得原地扩展。未来 Syslog 新配置应追加到父结构尾部，或在需要独立演进时引入新的
带 size/version 的版本化结构，而不是改变已发布嵌套布局。

## 6. 错误码

- `EINVAL`：当前版本下结构过短或字段值非法。
- `EPROTONOSUPPORT`：配置 ABI version 不支持。
- `E2BIG`：结构包含当前实现未知且非零的未来字段/语义。

这些错误码属于 Production v1 公共合同。
