# vraminfo

在 Linux 上读取 **NVIDIA 显卡的显存颗粒厂商、类型和显存温度**——包括 `nvidia-smi` 读不到的显存温度。

[English](README.md) | **中文**

```
$ sudo ./vraminfo
PCI            DEVICE   MEMORY         MAKER     MEM TEMP      NOTE
02:00.0        0x2204   GDDR6X         Micron    58 C
```

```
$ sudo ./vraminfo --per-module
PCI            DEVICE   MEMORY         MAKER     MEM TEMP      NOTE
20:00.0        0x2b85   GDDR7          Samsung   39 C [38 38 36 36 38 38 36 39]
```

## 功能

| 字段 | 数据来源 | 需要 root | 说明 |
|---|---|---|---|
| 显存厂商（Samsung / Hynix / Micron …） | NVAPI `NvAPI_GPU_GetRamMaker` | 否 | 驱动里记录的数据，任何卡都能读 |
| 显存类型（GDDR5 / GDDR6 / GDDR6X / GDDR7） | NVAPI `NvAPI_GPU_GetRamType` | 否 | |
| 显存温度 | GPU 寄存器（MMIO 直读） | **是** | 仅带传感器的显存类型（GDDR6X、GDDR7） |
| 逐颗粒温度 | 逐模块 DRAM 传感器（GDDR7） | **是** | `--per-module` |

寄存器路径**按 NVAPI 报告的显存类型自动选择**，不依赖型号白名单，所以新出的卡一般无需改代码：

* **GDDR6X**（Ampere / Ada——RTX 3080/3090/3090 Ti、RTX 4070 Ti/4080/4090 等）
  `BAR0 + 0xE2A8`，温度在低 12 位，`摄氏度 = 字段 / 32`；
* **GDDR7**（Blackwell——RTX 5090 等）
  逐模块 DRAM 传感器（DQR）：模块 *p* 位于 `BAR0 + 0x9024C0 + p*0x4000`，有效位在 `+0x10`
  的 `[27:24]`（必须为 `0xF`），温度是位 `[23:16]` 里的 GDDR MR-code（`code 20 = 0 °C`，每单位 +2 °C）；
* **GDDR6 / GDDR5 等**——颗粒本身没有可读传感器，如实报告"不支持"（这是显存颗粒的硬件属性，
  不是软件限制：只有 GDDR6X 和 GDDR7 颗粒带片上温度传感器）。

## 安装

```bash
git clone https://github.com/xzwgit/vraminfo.git
cd vraminfo
make                 # 或：gcc -O2 -Wall -o vraminfo vraminfo.c -ldl
sudo make install    # 安装到 /usr/local/bin/vraminfo
```

前置条件：

* Linux + NVIDIA 显卡 + 较新的驱动（NVAPI 桥接库 `libnvidia-api.so.1` 随驱动提供，R515 以后都有）；
* 编译需要 `gcc` 和 `make`；
* 读取显存温度需要 **root**（`/dev/mem` 以**只读**方式打开）；
* 部分内核还需在内核启动参数加 `iomem=relaxed`
  （`/etc/default/grub` → `GRUB_CMDLINE_LINUX_DEFAULT="... iomem=relaxed"`，然后
  `sudo update-grub && sudo reboot`）；很多发行版默认即可用；
* 需要关闭 Secure Boot，否则无法映射 `/dev/mem`。

## 使用

```bash
sudo vraminfo                 # 表格输出（每张卡的显存热点温度）
sudo vraminfo --per-module    # 逐个显存颗粒的温度
sudo vraminfo --json          # JSON 输出（便于监控采集）
sudo vraminfo --watch         # 每 2 秒刷新
vraminfo                      # 普通用户运行：只读厂商/类型，不读温度
```

JSON 示例：

```json
{
  "gpus": [
    {
      "bdf": "0000:02:00.0",
      "device_id": "0x2204",
      "subsystem": "0x1043:0x87af",
      "memory_maker": "Micron",
      "memory_type": "GDDR6X",
      "memory_maker_id": 10,
      "memory_type_id": 15,
      "memory_temp_c": 52,
      "note": null
    }
  ]
}
```

## 温度是怎么读出来的

`nvidia-smi` / NVML 在 Linux 上多数情况下并不提供显存温度（开源内核模块直接报 `N/A`，
不少消费级 SKU 的字段也缺失）。显存温度其实在 GPU 的寄存器窗口里，用户态就能读到：

1. 从 `/sys/bus/pci/devices/<bdf>/resource` 找到该卡的 BAR0 基址；
2. `open("/dev/mem", O_RDONLY)` 并用 `mmap()` 映射该寄存器所在页（只读）；
3. 读一个 32 位寄存器并按上面的规则解码。

全程**只读**，不会写 GPU 任何寄存器。

## 已验证显卡

| 显卡 | 显存 | 厂商/类型 | 温度来源 |
|---|---|---|---|
| RTX 3090 | Micron GDDR6X | ✔ | `0xE2A8`（与另一个独立实现的同寄存器读数交叉验证，数值一致） |
| RTX 4090（48GB 改版卡） | Micron GDDR6X | ✔ | `0xE2A8`——`nvidia-smi` 显示 `N/A`，但此工具能读出真实温度 |
| RTX 5090 | Samsung GDDR7 | ✔ | 逐模块 DQR 传感器 |
| RTX 3060 | Samsung GDDR6 | ✔ | *无传感器——正确报告为不支持* |
| CMP 50HX | Micron GDDR6 | ✔ | *无传感器（该卡 `0xE2A8` 读出 0）——报告为不支持* |

## 致谢

寄存器偏移与解码规则来自公开的逆向工程成果：
[gddr6 项目](https://github.com/olealgoritme/gddr6) 以及更广泛的改卡/超频社区
（Paulo Gomes、Unwinder、asder00、Martin Malík / HWiNFO）。本仓库是独立实现，
并额外增加了基于 NVAPI 的厂商/类型读取与"按显存类型自动选择寄存器路径"的设计。

## 许可

MIT —— 见 [LICENSE](LICENSE)。
