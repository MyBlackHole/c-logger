# c-logger 2.0 Architecture

状态：**未冻结**。

本文件只建立 2.0 architecture authority；具体对象模型由 #104 设计并冻结。

设计必须遵循：

```text
requirements
 -> object model
 -> ownership graph
 -> lifetime
 -> concurrency
 -> choose primitive
 -> implementation
```

在 #104 完成前，不预设 Logger 必须 refcounted，不预设 Global 必须 RCU，不预设 Queue 必须 lock-free。
