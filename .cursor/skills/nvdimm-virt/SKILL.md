---
name: nvdimm-virt
description: "修改 Xen、QEMU 或内核里的 vNVDIMM、libnvdimm、NFIT 或持久内存直通时使用。用户提到 nvdimm、pmem、SPA、DPA、NFIT 或 xen_nvdimm 时也要使用。"
---

# vNVDIMM

这三棵树是一套：内核 4.8-rc7 的 libnvdimm（分支 `libnvdimm-for-next`）、QEMU 2.11.50，以及配套的 Xen。改一边时指出另外两边要不要一起改。

- 内核持久内存在 `drivers/nvdimm/`，ACPI NFIT 在 `drivers/acpi/`。区域、命名空间和 label 的生命周期跟同目录已有代码走。
- QEMU 的 vNVDIMM 在 `hw/mem/`。客人可见的 NFIT 和 _DSM 以这棵 QEMU 里已有的实现为准，不要按更新的 ACPI 规范另写一套。
- Xen 只通过已有的 hypercall 和 `xen/include/public/` 把区域交给客人。写地址转换时说明 SPA、DPA 和客人物理地址分别是哪一个。
- 持久内存的写路径不要按普通 RAM 假设。改映射或 flush 时，指出是哪条指令或哪个 hypercall 把数据送到持久介质。
- 不要发明这三棵树里不存在的 ioctl 或 ACPI 方法。缺了就先指出缺在哪一层。
