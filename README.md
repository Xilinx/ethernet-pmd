# xxv-pmd

Standalone, out-of-tree DPDK platform-bus PMD for the AMD-Xilinx XXV
Ethernet + AXI DMA IP.

The repository follows the same driver hierarchy used by DPDK
(`drivers/<class>/<name>/`), allowing additional out-of-tree drivers
to be added in the future without restructuring the project.

## Repo structure

```
xxv-pmd/
├── README.md
├── meson.build                  # project setup, dpdk dependency, subdir() per driver
└── drivers/
    └── net/
        └── xxv/
            ├── meson.build      # this driver's target (bus_platform lookup + shared_library())
            ├── xxv_pmd.c        # ethdev-facing PMD logic (probe/remove, dev_ops, RX/TX bursts)
            ├── xxv_ethdev.h
            ├── axidma.c         # AXI DMA register-level access
            └── axidma.h
```

## Requirements

This driver only builds against a DPDK install that has:

1. Driver-SDK headers installed (`meson setup ... -Denable_driver_sdk=true`).
2. The `compatible`-field patch for `struct rte_platform_driver`
   (`drivers/bus/platform/bus_platform_driver.h` and `platform.c`,
   upstream commit `3de08fff97`). This is **not** in stock DPDK 25.11 —
   it must be carried on top (e.g. as a Yocto recipe patch). Without it,
   the build fails at compile time with:
   `error: field designator 'compatible' does not refer to any field
   in 'struct rte_platform_driver'` — this fails loudly, not silently.

Target DPDK version: **25.11** (LTS).

## Build

```bash
export PKG_CONFIG_PATH=/path/to/dpdk-install/lib/pkgconfig

# Native build
meson setup build

# Cross compile for ARM64
meson setup build --cross-file <aarch64_cross.ini>

ninja -C build
```

Output: `build/drivers/net/xxv/librte_net_xilinx.so`

This is a DPDK PMD shared library, not a standalone executable. It must
be loaded by a DPDK application such as `testpmd`.

The cross file must declare a `pkgconfig` binary under `[binaries]`
(meson requires this per-machine for cross builds; the plain system
`pkg-config` works fine, since it's just a text-file parser).

## Notes on the build

- `meson.build` compiles with `-DALLOW_INTERNAL_API`. Several DPDK
  calls this driver makes (`rte_eth_dev_allocate`,
  `rte_eth_dma_zone_reserve`/`_free`, `rte_eth_dev_probing_finish`,
  `rte_platform_register`, etc.) are marked `__rte_internal`, which
  DPDK compile-blocks outside its own build tree unless this macro is
  defined. This is an intentional DPDK guard being deliberately
  overridden, not a workaround for a bug.
- `librte_bus_platform` (which provides `rte_platform_register`) is
  linked explicitly via `cc.find_library()`, since drivers aren't part
  of `pkg-config --libs libdpdk`'s default output — only core DPDK
  libraries are.
- None of DPDK's own source tree (`drivers/...`, `lib/...`) is
  referenced anywhere in this build — only pkg-config output and
  installed driver-SDK headers.

## Runtime

Load with:
```bash
dpdk-testpmd -d build/drivers/net/xxv/librte_net_xilinx.so -- -i
```
The device's device-tree node must have
`compatible = "xlnx,xxv-ethernet-1.0"` for probing to match.
