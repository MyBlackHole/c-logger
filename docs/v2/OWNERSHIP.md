# c-logger 2.0 Ownership & Lifetime

状态：**由 #102 设计，当前未冻结对象级模型**。

统一分类：

```text
AUTO
OWNED
BORROWED
MOVED
SHARED
REFCOUNTED
```

每个非平凡资源必须回答：

1. 谁创建；
2. 当前谁拥有；
3. 何时 move；
4. 最终谁 release；
5. 哪些执行者可以借用；
6. final free 的 quiescence proof。

在 ownership graph 完成前，不得因为已经实现 refcount primitive 就把对象改成 REFCOUNTED。
