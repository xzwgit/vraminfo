# vraminfo

Read **NVIDIA GPU memory (VRAM) vendor, type and temperature** on Linux — including VRAM
temperatures that `nvidia-smi` does not report.

[中文说明](#中文说明)

```
$ sudo ./vraminfo
PCI            DEVICE   MEMORY         MAKER     MEM TEMP      NOTE
02:00.0        0x2204   GDDR6X         Micron    52 C
```

```
$ sudo ./vraminfo --per-module
PCI            DEVICE   MEMORY         MAKER     MEM TEMP      NOTE
20:00.0        0x2b85   GDDR7          Samsung   39 C [38 38 36 36 38 38 36 39]
```

## What it reports

| Field | Source | Root needed | Notes |
|---|---|---|---|
| Memory maker (Samsung / Hynix / Micron / …) | NVAPI `NvAPI_GPU_GetRamMaker` | no | read from the driver, works on every card |
| Memory type (GDDR5 / GDDR6 / GDDR6X / GDDR7) | NVAPI `NvAPI_GPU_GetRamType` | no | |
| Memory temperature | GPU registers over MMIO | **yes** | only on DRAM types that have a sensor (GDDR6X, GDDR7) |
| Per-module temperatures | per-module DRAM sensors (GDDR7) | **yes** | `--per-module` |

The register path is selected from the **memory type** reported by NVAPI, not from a model
whitelist, so new cards generally work without code changes:

* **GDDR6X** (Ampere / Ada — RTX 3080/3090/3090 Ti, RTX 4070 Ti/4080/4090, …)
  `BAR0 + 0xE2A8`, temperature in bits `[11:0]`, `Celsius = field / 32`.
* **GDDR7** (Blackwell — RTX 5090, …)
  per-module DRAM sensors (`DQR`): module *p* at `BAR0 + 0x9024C0 + p*0x4000`, validity
  nibble in bits `[27:24]` of `+0x10` (must be `0xF`), temperature as a GDDR MR-code in
  bits `[23:16]` (`code 20 = 0 °C`, `+2 °C` per unit above).
* **GDDR6 / GDDR5 / …** — no readable sensor; reported as unsupported (this is a hardware
  property of the DRAM, not a software limitation: GDDR6X and GDDR7 chips carry an on-die
  thermal sensor, plain GDDR6 does not).

## Requirements

* Linux, an NVIDIA GPU, a recent driver (the NVAPI bridge `libnvidia-api.so.1` ships with
  the driver; it is present from ~R515 onwards).
* `root` for the temperature part (`/dev/mem` is opened **read-only**).
* On kernels with strict `/dev/mem` policy, add `iomem=relaxed` to the kernel command line
  (`/etc/default/grub` → `GRUB_CMDLINE_LINUX_DEFAULT="... iomem=relaxed"`, then
  `update-grub && reboot`). Many distributions work without it.
* Secure Boot must be disabled for `/dev/mem` access to the GPU aperture.

## Build

```bash
make            # or: gcc -O2 -Wall -o vraminfo vraminfo.c -ldl
sudo make install
```

## Usage

```bash
sudo ./vraminfo                 # table, hotspot temperature per card
sudo ./vraminfo --per-module    # list every DRAM module's temperature
sudo ./vraminfo --json          # machine-readable output
sudo ./vraminfo --watch         # refresh every 2 seconds
./vraminfo                      # as a normal user: vendor/type only
```

JSON example:

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

## How the temperature is read

`nvidia-smi`/NVML do not expose memory temperature on Linux for most setups (the open
kernel modules report `N/A`, and the field is absent for many consumer SKUs). The memory
temperature lives in the GPU's register aperture, which can be read from userspace:

1. find the GPU's BAR0 base in `/sys/bus/pci/devices/<bdf>/resource`;
2. `open("/dev/mem", O_RDONLY)` and `mmap()` the page containing the register (read-only);
3. read one 32-bit register and decode it as described above.

Nothing is written to the GPU. The tool only ever maps pages `PROT_READ`.

## Verified on

| GPU | Memory | Reported | Temperature source |
|---|---|---|---|
| RTX 3090 | Micron GDDR6X | ✔ | `0xE2A8` (cross-checked against HiveOS `nvtool` — same value) |
| RTX 4090 (48 GB mod) | Micron GDDR6X | ✔ | `0xE2A8` |
| RTX 5090 | Samsung GDDR7 | ✔ | per-module DQR |
| RTX 3060 | Samsung GDDR6 | ✔ | *no sensor — correctly reported unsupported* |
| CMP 50HX | Micron GDDR6 | ✔ | *no sensor — correctly reported unsupported* |

## Register documentation credits

The register offsets and decoders were established by the public reverse-engineering work of
the [gddr6 project](https://github.com/olealgoritme/gddr6) and the wider modding community
(Paulo Gomes, Unwinder, asder00, Martin Malík / HWiNFO). This is an independent
implementation that adds NVAPI-based maker/type reporting and type-driven register
selection.

## License

MIT — see [LICENSE](LICENSE).

---

## 中文说明

在 Linux 上读取 **NVIDIA 显卡的显存颗粒厂商、类型和显存温度**——包括 `nvidia-smi` 读不到的显存温度。

* **厂商 / 类型**：走驱动自带的 NVAPI 桥接库（`libnvidia-api.so.1`），**不需要 root**，任何卡都能读；
* **显存温度**：直接用 MMIO 读 GPU 寄存器（`/dev/mem` 只读打开，**需要 root**）。
  寄存器路径按 NVAPI 报告的**显存类型**自动选择（不依赖型号白名单）：
  * GDDR6X（Ampere / Ada）：`BAR0+0xE2A8`，取低 12 位除以 32；
  * GDDR7（Blackwell）：逐模块 DRAM 传感器（`0x9024C0 + p*0x4000`，有效位 nibble=0xF，MR-code 解码）；
  * GDDR6 / GDDR5：颗粒本身没有可读传感器，如实报告"不支持"（这是显存颗粒的硬件属性，
    不是软件限制——只有 GDDR6X 和 GDDR7 颗粒带片上温度传感器）。

```bash
make && sudo ./vraminfo            # 编译并运行
sudo ./vraminfo --per-module       # 逐个颗粒的温度
sudo ./vraminfo --json             # JSON 输出（便于监控采集）
```

部分内核需要在内核启动参数加 `iomem=relaxed`（`/etc/default/grub` 里加，然后 `update-grub && reboot`），
并关闭 Secure Boot。很多发行版默认即可用。

**注意**：显存温度读取需要 root，且只做只读映射，不会写 GPU 任何寄存器。
