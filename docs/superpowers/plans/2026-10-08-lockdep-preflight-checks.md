# Lockdep 获取前与释放前校验实施计划

> **执行者提示：** 按任务逐项实现并保留勾选状态；每项都要先写回归，再运行对应验证。执行本计划时使用 `superpowers:executing-plans`，逐任务检查。

**目标：** 让 debug/test 构建在真实 pthread 加锁可能阻塞前检查递归与禁止锁序，并在真实解锁前验证当前线程的锁记账。

**架构：** 将 lockdep 校验拆成无状态变更的检查和真实 pthread 操作成功后的记账更新。可能阻塞的 lock 在调用 primitive 前检查；非阻塞 trylock 只在成功后检查并登记，失败时原样返回。统一封装项目实际跟踪的 mutex/rwlock 路径；生产构建仍将 lockdep 编译为空操作。

**技术栈：** GNU C11、POSIX pthread、Xmake、现有回归测试框架。

**规格：** GitHub issue [#126](https://github.com/MyBlackHole/c-logger/issues/126)、`docs/v2/LOCKDEP.md`、`docs/v2/LOCKING.md`。

## 全局约束

- 生产目标继续定义 `LOGGER_ENABLE_LOCKDEP=0`；测试支持库和 lockdep 测试翻译单元继续定义 `LOGGER_ENABLE_LOCKDEP=1`。
- 只覆盖项目实际跟踪的锁类别与获取路径，不实现完整 Linux lockdep，不引入动态图或新的通用锁框架。
- pthread 获取失败时不改变 TLS held-lock stack；普通阻塞获取在调用 primitive 前检查，非阻塞 trylock 只在成功后检查并登记，失败时保留原返回语义；成功 unlock 后才撤销记账，unlock 失败沿用 `logger_lockdep_abandon()` 并由现有同步错误路径封闭对象。
- 保持全局生命周期、Audit、取消屏蔽/恢复、条件变量等待和公开 API 的现有行为；测试需验证条件变量等待返回后锁已重新持有且记账仍一致。
- 本次新增或修改的代码注释、Xmake 注释和测试说明使用中文；不把全仓历史注释翻译混入本 PR。
- 不修改正式版本号；2.0 ABI 尚未冻结。

---

## 文件职责

- `src/logger_lockdep.h`：声明内部前置校验接口，保留现有成功获取/释放接口。
- `src/logger_lockdep.c`：在 TLS held-lock stack 上实现无副作用的获取前与释放前检查。
- `src/logger_cleanup.h`：为带锁类的 mutex、rwlock 和 trylock 提供统一内部包装；仅在 pthread 操作成功后改变记账。
- `src/logger_global.c`：将全局 control mutex 与 lifetime rwlock 的真实获取/释放路径接入包装。
- `src/audit.c`：将 Audit control/operation mutex 接入包装，同时保持当前 cancellation 屏蔽及错误封闭流程。
- `tests/regression/test_lockdep.c`：覆盖直接 tracker 和真实 pthread 路径，包括阻塞前诊断、失败路径、线程隔离及条件变量等待返回。
- `docs/v2/LOCKDEP.md`：记录前置检查时机、TLS 状态变化、支持边界和验证方式。
- `xmake.lua`：只在验证发现目标宏定义不一致或需要注册新用例时修改；生产仍为 `0`，回归目标仍为 `1`，修改的注释使用中文。

## 任务 1：拆分无状态前置校验与成功后记账

**文件：**

- 修改：`src/logger_lockdep.h`
- 修改：`src/logger_lockdep.c`
- 修改：`tests/regression/test_lockdep.c`

**接口：**

- 新增 `logger_lockdep_check_acquire(logger_lock_class_t class_id, const void *lock)`：检查参数、递归/重复获取、允许锁序和栈容量，不修改 TLS 状态。
- 新增 `logger_lockdep_check_release(logger_lock_class_t class_id, const void *lock)`：检查当前线程栈顶的锁类别、对象地址和 LIFO 顺序，不修改 TLS 状态。
- 保留 `logger_lockdep_acquire()` 与 `logger_lockdep_release()`：分别复用前置检查，并只在调用者确认真实获取/释放成功后更新 TLS 状态。
- `LOGGER_ENABLE_LOCKDEP=0` 时，两个新增接口也必须是无副作用的 `static inline` 空操作。

- [x] **步骤 1：先写直接 tracker 回归**

在 `test_lockdep.c` 增加一个合法前置检查用例，执行获取前检查后立即断言锁尚未登记，再执行成功获取登记、释放前检查、成功释放及未持有断言。增加两个子进程负向用例，分别让递归获取和禁止锁序在 `logger_lockdep_check_acquire()` 中触发 `SIGABRT`；再增加一个错误释放顺序用例，确认 `logger_lockdep_check_release()` 在状态变更前触发 `SIGABRT`。

- [x] **步骤 2：运行单项测试确认新接口尚未实现**

运行：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m debug -o build-lockdep-debug --build_shared=n --build_private_tests=y --build_regression_tests=y
xmake test -j1 'lockdep_regression/*'
```

预期：编译因新增接口尚未声明/实现而失败；该失败只用于确认测试确实覆盖新接口。

- [x] **步骤 3：实现检查函数并让旧 tracker API 复用它们**

把现有 `logger_lockdep_acquire()` 中的参数、重复持锁、锁序和容量检查提取到 `logger_lockdep_check_acquire()`；把 `logger_lockdep_release()` 的空栈、栈顶类别/地址和 LIFO 检查提取到 `logger_lockdep_check_release()`。检查函数不得写 `state.held[]` 或修改 `state.depth`。成功记账函数在检查通过后执行原有单次压栈/出栈。

- [x] **步骤 4：重跑单项回归**

运行：

```sh
xmake test -j1 'lockdep_regression/*'
```

预期：合法前置检查不改变 TLS 状态；递归、非法锁序和错误释放用例均由 `SIGABRT` 终止；现有 relation-matrix 用例继续通过。

## 任务 2：先写真实 pthread 路径负向回归

**文件：**

- 修改：`tests/regression/test_lockdep.c`

**接口：** 使用任务 3 将提供的内部包装：

- `__cleanup_lockdep_mutex_lock()`、`__cleanup_lockdep_mutex_trylock()`、`__cleanup_lockdep_mutex_unlock()`。
- `__cleanup_lockdep_rwlock_rdlock()`、`__cleanup_lockdep_rwlock_wrlock()`、`__cleanup_lockdep_rwlock_trywrlock()`、`__cleanup_lockdep_rwlock_unlock()`。

- [x] **步骤 1：增加真实 NORMAL mutex 自重入用例**

在子进程中初始化 `PTHREAD_MUTEX_NORMAL`，第一次通过带锁类包装成功获取，再尝试用同一锁对象、同一锁类再次获取。子进程设置短 `alarm()` 看门狗：新实现应收到 `SIGABRT`；旧实现若进入 pthread 阻塞则收到 `SIGALRM`，父进程将其判为失败，避免测试挂死。

- [x] **步骤 2：增加两线程反向锁序用例**

两个线程各自通过真实包装获取不同 instance 锁，在 barrier 同步后都尝试获取对方持有的锁。禁止的第二次锁序必须在 pthread 阻塞前触发 `SIGABRT`；保留看门狗并断言终止信号不是 `SIGALRM`。

- [x] **步骤 3：增加错误 unlock 前置诊断用例**

同一子进程依次获取允许嵌套的 control 与 lifetime 两把真实 mutex，再以非 LIFO 顺序尝试释放外层锁。断言先由 lockdep 收到 `SIGABRT`，证明错误 pthread unlock 尚未执行。

- [x] **步骤 4：先运行回归并确认旧包装未能提前诊断**

运行：

```sh
xmake test -j1 'lockdep_regression/*'
```

预期：任务 1 的直接 tracker 测试通过；自重入/反向锁序用例因收到看门狗信号或新包装缺失而失败，确认测试能区分“tracker 自身会报错”和“真实锁路径在阻塞前会报错”。

## 任务 3：统一接入 mutex、rwlock、trylock 的真实路径

**文件：**

- 修改：`src/logger_cleanup.h`
- 修改：`src/logger_global.c`
- 修改：`src/audit.c`

- [x] **步骤 1：将 instance/Console mutex 包装改为前检、成功后登记**

在普通 lock 包装中，先调用 `logger_lockdep_check_acquire()`，再调用 pthread primitive；仅在返回 `0` 后调用 `logger_lockdep_acquire()`。mutex trylock 先执行非阻塞 pthread primitive；只有返回 `0` 时才做 lockdep 检查并登记，忙错误直接原样返回且 TLS 状态不变。unlock 包装先调用 `logger_lockdep_check_release()`，再执行 pthread unlock；成功后调用 `logger_lockdep_release()`，失败后调用 `logger_lockdep_abandon()` 并原样返回 pthread 错误。

- [x] **步骤 2：增加 tracked rwlock 包装**

为 read lock、write lock、try-write lock 和 unlock 分别增加内部包装。read/write lock 在调用可能阻塞的 primitive 前使用 `LOGGER_LOCK_GLOBAL_LIFETIME` 做检查，成功后登记；try-write 先执行非阻塞 primitive，仅成功时检查并登记，失败时不修改 TLS。unlock 采用与 mutex 一致的前置检查、成功撤销、失败 abandon 规则。此封装只供当前全局生命周期锁图使用；若测试发现同线程合法递归读锁，先明确建模读锁重入语义，不得静默放宽重复获取检查。

- [x] **步骤 3：替换 `logger_global.c` 中所有 tracked primitive 直调**

覆盖 `g_control_mu` 的普通 lock/trylock/unlock，以及 `g_lifetime_lock` 的 rdlock、wrlock、trywrlock、unlock；保留现有 `scope.*_locked`、ticket 回滚、错误返回和取消状态顺序。检查 `logger_global.c` 不再在这些 tracked 路径上直接调用 pthread 获取/释放 primitive。

- [x] **步骤 4：替换 `audit.c` 中所有 tracked primitive 直调**

在 `lock_scope()`、嵌套 operation 获取、`unlock_scope()` 和嵌套 operation 释放中使用包装。获取前置检查必须发生在真实 pthread lock 前；`pthread_setcancelstate()` 的位置与当前实现保持一致；unlock 失败继续记录 Audit 锁错误、封闭 Audit 并维持当前取消禁用策略。

- [x] **步骤 5：重跑真实锁回归**

运行：

```sh
xmake test -j1 'lockdep_regression/*'
```

预期：真实 NORMAL mutex 自重入、反向锁序、错误 unlock 均在 pthread 阻塞/执行前收到 `SIGABRT`；合法锁序通过。

## 任务 4：验证失败路径、TLS 与等待语义

**文件：**

- 修改：`tests/regression/test_lockdep.c`
- 必要时修改：`xmake.lua`

- [x] **步骤 1：验证失败 trylock 不污染 TLS 记账**

线程甲先持有 instance emit 锁；线程乙持有另一个 instance progress 锁。线程甲对 progress 锁执行 trylock，断言返回忙错误且没有因潜在的禁止嵌套触发 lockdep，也没有改变自身 emit 记账或新增 progress 记账。另以 rwlock try-write 争用场景验证相同失败语义。trylock 成功时才做锁序检查并记账；确认 pthread 返回码语义不被包装改写。

- [x] **步骤 2：验证线程间 TLS 隔离**

两个线程分别获取各自 mutex 和锁类；任一线程的断言只能观察本线程已登记的锁。释放后各线程分别断言未持有，避免将全局共享表误当作线程本地记账。

- [x] **步骤 3：验证 `pthread_cond_wait()` 返回后的锁状态**

使用进度锁和条件变量执行一次真实等待/唤醒；等待返回后，断言 pthread 已重新持有 mutex 且 lockdep 仍登记该锁，再经 unlock 包装成功释放并断言未持有。不得在等待期间把 TLS 状态永久弹出，也不得在条件变量返回后重复压入。

- [x] **步骤 4：核对取消与同步错误回归覆盖**

运行：

```sh
xmake test -j1 'global_cancel_regression/*'
xmake test -j1 'lock_failure_regression/*'
xmake test -j1 'audit_concurrency_regression/*'
```

预期：取消仍在所有权资源释放后恢复；注入的 mutex lock/unlock 错误仍按既有错误契约传播；Audit 并发正常路径通过。若某目标未单独注册为 Xmake 测试，则用同一回归构建配置运行 `xmake test -j1` 并确认该目标被执行。

- [x] **步骤 5：复核 Xmake 宏配置**

检查 `xmake.lua`：生产目标 `LOGGER_ENABLE_LOCKDEP=0`；`logger_test_support`、`logger_regression_support` 和 `lockdep_regression` 翻译单元为 `1`。只有发现配置缺口时才修改构建逻辑；修改处注释写中文，并通过一次干净 Xmake 配置确认目标间没有宏不一致。

## 任务 5：更新契约文档并完成集成验证

**文件：**

- 修改：`docs/v2/LOCKDEP.md`
- 按实际需要修改：`xmake.lua`
- 核对：`docs/v2/LOCKING.md`

- [x] **步骤 1：修正文档对跟踪时机的描述**

说明 lockdep 在可能阻塞的真实 lock 前进行无状态检查；trylock 只在成功后检查并登记；只有 pthread 获取成功后才记入 TLS。unlock 前检查线程记账，只有 pthread 解锁成功后才移除。记录失败获取无记账、失败释放沿用 abandon/对象封闭策略、条件变量等待返回后锁已重获的语义。

- [x] **步骤 2：明确 lockdep 的实现边界**

保留固定锁类别关系表，不声称覆盖完整 Linux lockdep、动态锁图或所有 pthread 用法；明确自重入与禁止类别顺序属于项目跟踪锁契约。将本次修改附近新增/修改的注释统一为中文，不翻译无关历史注释。

- [x] **步骤 3：运行 lockdep 与相关并发回归**

运行：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m debug -o build-lockdep-debug --build_shared=n --build_private_tests=y --build_regression_tests=y
xmake test -j1 'lockdep_regression/*'
xmake test -j1
```

预期：lockdep 专项、完整登记测试及相关并发/取消用例通过；测试看门狗没有因 lockdep 漏诊触发。

- [x] **步骤 4：构建生产配置并检查 lockdep 被裁掉**

运行：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o build-lockdep-release --build_shared=n
xmake build
```

预期：生产配置继续用 `LOGGER_ENABLE_LOCKDEP=0` 成功构建；优化后的生产库不包含 `logger_lockdep_check_*`、TLS held-lock state 或 lockdep violation 输出路径。debug/test 构建中的 lockdep 路径仍启用。

- [x] **步骤 5：检查变更并提交**

运行：

```sh
git diff --check
git diff --stat
```

预期：无空白错误；变更仅覆盖 lockdep 获取/释放时机、对应负向测试、契约文档及确有需要的构建配置。提交前记录所运行测试和生产构建结果；不更改版本号。

## 执行记录

- 2026-10-08：lockdep 专项回归最终 12/12 通过，覆盖成功/失败 trylock、成功路径非法锁序、真实锁序、TLS 隔离和条件变量等待。
- Global cancellation 12/12、锁失败 8/8、Audit 并发 2/2、全局错误生命周期 5/5、Audit 生命周期 6/6 通过。
- 完整 debug 回归首轮为 531/587；剩余 56 项均属于 syslog 后端/回退/故障注入及其一个显式取消场景。沙箱禁止 Unix socket bind 后，获准环境中的 syslog 专项 68/68、显式取消场景 1/1 通过。
- release 静态构建成功；符号检查未发现 lockdep 检查函数、TLS 记账或诊断路径进入生产库；全局锁对象代码仍直接调用 pthread 原语。
- `git diff --check` 通过。代码审查没有 Critical/Important 发现；审查指出的成功 trylock 覆盖已补齐并通过上述 12 项回归。

## 后续 issue 顺序

1. 完成本计划并更新 #121 总跟踪中 #125 的状态，附 PR #134/#135 与 TSan 修复证据。
2. 处理 #127：修正文档中的 C11 双 SC 栅栏证明；没有新的运行证据时不改运行时代码。
3. 处理 #128：补生产等价的直接前后 benchmark，再决定性能结论。
4. 处理 #129：明确静默期由外部借用/线程 join 证明，pthread destroy 返回值只作附加诊断。
5. 处理 #130：统一语义门禁、真实负向测试和阶段验收证据；同时在 #121 中追踪尚未独立建 issue 的 R7/A7。
6. 将全仓历史英文备注中文化另列低优先级任务，保持每个行为修复 PR 的范围可审查。
