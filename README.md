# vraminfo

Read **NVIDIA GPU memory (VRAM) vendor, type and temperature** on Linux — including VRAM
temperatures that `nvidia-smi` does not report.

**English** | [中文文档](README.zh-CN.md)

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

## Installation

**Requirements**

* Linux, an NVIDIA GPU, a recent driver — the NVAPI bridge `libnvidia-api.so.1` ships with
  the driver (present from ~R515 onwards).
* `gcc` (or any C compiler) and GNU make.
* `root` for the temperature part (`/dev/mem` is opened **read-only**).
* On kernels with a strict `/dev/mem` policy, add `iomem=relaxed` to the kernel command
  line (`/etc/default/grub` → `GRUB_CMDLINE_LINUX_DEFAULT="... iomem=relaxed"`, then
  `sudo update-grub && sudo reboot`). Many distributions work without it.
* Secure Boot must be disabled, otherwise `/dev/mem` cannot be mapped.

**Build and install**

```bash
git clone https://github.com/xzwgit/vraminfo.git
cd vraminfo
make                 # or: gcc -O2 -Wall -o vraminfo vraminfo.c -ldl
sudo make install    # installs /usr/local/bin/vraminfo
```

## Usage

```bash
sudo vraminfo                 # table, hotspot temperature per card
sudo vraminfo --per-module    # list every DRAM module's temperature
sudo vraminfo --json          # machine-readable output
sudo vraminfo --watch         # refresh every 2 seconds
vraminfo                      # as a normal user: vendor/type only, no temperature
```

JSON example:

```json
{
  "gpus": [
    {
      "bdf": "0000:02:00.0",
      "device_id": "0x2204",
      "subsystem": "0x1043:0x87af",
      "board_vendor": "ASUSTeK Computer Inc.",
      "board_vendor_id": "0x1043",
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

## What it reports

| Field | Source | Root needed | Notes |
|---|---|---|---|
| Board vendor (ASUSTeK / GALAX / MSI / …) | PCI subsystem ID, resolved with the system `pci.ids` | no | every card, driver not involved; shown as `N/A` when the subsystem ID does not name a board partner (e.g. NVIDIA's generic reference ID `0x10de`) |
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
| RTX 3090 | Micron GDDR6X | ✔ | `0xE2A8` (cross-checked against another independent reading of the same register — values agree) |
| RTX 4090 (48 GB mod) | Micron GDDR6X | ✔ | `0xE2A8` — readable even though `nvidia-smi` shows `N/A` |
| RTX 5090 | Samsung GDDR7 | ✔ | per-module DQR sensors |
| RTX 3060 | Samsung GDDR6 | ✔ | *no sensor — correctly reported unsupported* |
| CMP 50HX | Micron GDDR6 | ✔ | *no sensor (`0xE2A8` reads 0 on this card) — reported unsupported* |

## Credits

The register offsets and decoders were established by the public reverse-engineering work of
the [gddr6 project](https://github.com/olealgoritme/gddr6) and the wider modding community
(Paulo Gomes, Unwinder, asder00, Martin Malík / HWiNFO). This is an independent
implementation that adds NVAPI-based maker/type reporting and type-driven register
selection.

## License

MIT — see [LICENSE](LICENSE).
