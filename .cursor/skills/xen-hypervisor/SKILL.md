---
name: xen-hypervisor
description: "修改 Xen hypervisor、工具栈或 stubdom 时使用。用户提到 Xen、dom0、hypercall、xen/ 或 tools/libxl 时也要使用。"
---

# 改这棵 Xen

先读仓库根目录的 `CODING_STYLE` 和 `CONTRIBUTING`，再按同目录已有代码写。

- hypervisor 在 `xen/`。工具栈在 `tools/`。不要在 hypervisor 里调用工具栈的库。
- 客人可见的结构和 hypercall 以 `xen/include/public/` 为准。加字段只能追加，并同时考虑 32 位和 64 位客人。
- 锁跟同子系统现有写法。不要在中断或关中断路径里睡眠。
- 构建入口是根目录的 `configure` 和 `make`。沿用现有变量，不要另起一套构建。
- 提交说明写清改的是 hypervisor、工具栈，还是两边，以及客人可见行为有没有变。
