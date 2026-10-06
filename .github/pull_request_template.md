## 变更目的

<!-- 一个 PR 只解决一个逻辑问题。 -->

## 2.0 main review contract

如果本 PR 目标分支是 `main`，请逐项确认；1.x 维护 PR 可以标记 N/A。

- [ ] 已明确 ownership / lifetime 是否变化。
- [ ] 已明确执行位置（INIT/PUBLIC_API/WORKER/CALLBACK/TEARDOWN）。
- [ ] 已明确 MAY_BLOCK/MUST_NOT_BLOCK、MAY_ALLOC/NO_ALLOC、HOT_PATH 等约束。
- [ ] 新增 allocation 有界且 OOM/fallback 语义明确。
- [ ] stack footprint 受控，没有新增 VLA/大局部缓冲。
- [ ] string/buffer truncation 可观察，没有 silent truncation。
- [ ] timeout/retry 使用 monotonic deadline，而非 wall clock。
- [ ] 并发代码满足 ISO C11 data-race rules。
- [ ] failure path 有测试；cleanup error 不覆盖 primary error。
- [ ] 行为变更没有混入无关 rename/formatting/cleanup。

## 验证

<!-- 写明执行的测试、Sanitizer、fault injection 或 benchmark。 -->
