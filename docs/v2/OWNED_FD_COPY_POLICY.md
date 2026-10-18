# fd 拥有者的有限 AST 复制检查（#106 S5 / #210 D2）

本检查拒绝“裸结构赋值或直接字节复制绕过 move/take”，复用已经完成的
长期 fd 类型化和跨函数移交合同。没有修改生产源码、公开 ABI、
故障注入或发布签收，也没有因此完成全部 D1/D2。

## 检查对象与规则

`owned_fd_value_copy` 检查真实 Clang 预处理 AST 中 `logger_owned_fd` 的按值读取，以及包含它的
结构/联合体/内嵌数组的整体按值读取。覆盖初始化、赋值、宏展开、typedef、
返回值、直接和间接调用的按值实参；赋值结果再次使用的链式复制也拒绝。
只持有指针的借用容器不递归取得所指对象的所有权。复合字面量初始化和
`logger_fd_move` 正常构造不误判；`sizeof` 与 `_Generic` 未求值部分不当作复制。

`owned_fd_memory_copy` 检查直接调用的 `memcpy`、`memmove`、
`__builtin_memcpy`、`__builtin_memmove`、`__builtin___memcpy_chk` 和
`__builtin___memmove_chk`。被调表达式必须直接解析到对应的 `FunctionDecl`；
括号及直接函数取地址/解引用不改变身份，不追踪函数指针变量的实际目标。
前两个实参中，任意一侧能直接恢复为指向拥有者对象或其内嵌容器/数组的
指针就拒绝；允许在同一实参内回看括号、数组退化和隐式/显式指针转换。
只解一层指针，不回溯字段的所属对象、取地址对象或变量初值。因此
`file->path`、`syslog->ident` 的字节复制，以及指针变量/借用容器的复制不会
因为它们关联拥有者而被拒绝。类型仍明确的指针运算结果也适用此规则。

字节复制是语法禁令：不证明长度、源目标重叠或随后的来源清空，零长度、
部分复制和自复制也保守拒绝。命中不代表已经证明 fd 被复制或发生双重关闭。
调用本身的副作用与返回值是否使用无关，普通语句和 `(void)` 调用都检查；
`sizeof` 和 `_Generic` 未求值部分继续跳过。

规则解析 Clang 的 `RecordDecl`、`RecordType`、字段和类型别名，而不是
搜索源代码中的固定变量名。诊断包含翻译单元、函数、类型与原始源码位置
（含宏展开位置）；字节复制另记录 `callee` 和命中实参的索引、指针类型及位置。
头文件中被解析的函数体同样受检。未知 JSON、缺编译器、
任何编译诊断、非零退出、超时、空任务集或整批未观察到拥有者类型均失败关闭。

`xmake-owned-copy.lua` 包含根 `xmake.lua`，从真实 `logger`、
`logger_regression_support`、`logger_test_support` 目标逐源取得编译参数。
因此覆盖生产、锁检测、故障注入三个真实宏配置，不手写第二套构建源列表。
当前各配置为16/16/17个翻译单元；新增生产源会由Xmake目标自动进入检查。
专项 CI 在 Ubuntu20.04/24.04 与共享/静态配置运行，记录实际编译器及
完整参数、源码/tree、每个 AST 摘要、诊断、判定器测试和生产产物隔离结果。
真实源码回归先接受未修改的 `src/logger_file.c`，再分别将 `open_candidate`
和 `install_fd` 的唯一 move 语句替换为按值复制、`memcpy` 或 `memmove`。
变体必须先通过真实 Clang 编译，再由对应所有权规则定位到移交函数；
编译错误不能代替规则命中。

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -F xmake-owned-copy.lua -m debug --toolchain=clang \
  --build_shared=n --build_tests=n --build_private_tests=y --build_regression_tests=y
xmake test -F xmake-owned-copy.lua -g owned-copy -j1 -v
```

## 明确的限制

这是有限复制语法规则，不是 C 线性类型、跨翻译单元数据流或生命周期证明。
两端都已存入 `void *` 变量的复制、自定义包装函数、函数指针间接调用、
转成 `char *` 再偏移后的来源回溯、指针经整数转换逃逸、逐个整数成员复制、借用失效后使用、
把来源资源重新 `logger_fd_own`、向非空目标 move、函数指针的真实被调语义，
以及本轮三种目标之外的预处理配置均没有获得普遍证明。测试明确保留这些
未覆盖边界，禁止把正例解释成允许生产使用它们。故意复制的动态变体属于
独立测试驱动，不加入本检查的生产/支持库源列表，也不添加按函数名豁免。

Clang AST JSON 不是本项目可控制的稳定 ABI；真实编译正反例在每次专项运行
先执行。检测器升级必须重新验收，不允许因为输入形状变化就静默清空结果。
TSan31项原生覆盖、宿主fork以及A–F/D1–D6的风险接受仍保持独立。
