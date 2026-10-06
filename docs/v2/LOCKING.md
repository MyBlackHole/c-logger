# c-logger 2.0 Locking

状态：**由 #103 设计，当前未冻结具体锁图**。

每把锁最终必须记录：

- protects what；
- allowed execution context；
- lock order；
- nesting；
- callback/reentry restrictions；
- 是否参与 lifetime proof。

开发期应提供 lockdep-style assertion，但锁断言不能替代生命周期证明。
