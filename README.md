# ethernet-pmd

Standalone, out-of-tree DPDK platform-bus PMD for the AMD-Xilinx XXV
Ethernet + AXI DMA IP.

The repository follows the same driver hierarchy used by DPDK
(`drivers/<class>/<name>/`), allowing additional out-of-tree drivers
to be added in the future without restructuring the project.

## Repo structure

```
ethernet-pmd/
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
   (`drivers/bus/platform/bus_platform_driver.h` and `platform.c`),
   carried via `dpdk_25.11.%.bbappend` since it's not in upstream DPDK 25.11.

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

Output: `build/drivers/net/xxv/librte_net_xilinx.so` (symlink to the
versioned `librte_net_xilinx.so.<dpdk_abi>`, e.g. `.so.26.0` for DPDK
25.11). The version/soversion and install path are derived from the
target DPDK's `RTE_EAL_PMD_PATH` (`rte_build_config.h`), matching
in-tree PMD conventions, so `ninja install` places it under
`<libdir>/dpdk/pmds-<abi>/` alongside DPDK's own drivers.

This is a DPDK PMD shared library, not a standalone executable. It must
be loaded by a DPDK application such as `testpmd`.

The cross file must declare a `pkgconfig` binary under `[binaries]`
(meson requires this per-machine for cross builds; the plain system
`pkg-config` works fine, since it's just a text-file parser).

## Notes on the build

- Compiles with `-DALLOW_INTERNAL_API` to use `__rte_internal` DPDK
  calls (`rte_eth_dev_allocate`, `rte_platform_register`, etc.).
- `librte_bus_platform` is linked explicitly via `cc.find_library()`,
  since it's not part of `pkg-config --libs libdpdk`'s default output.

## Runtime

Load with:
```bash
dpdk-testpmd -d build/drivers/net/xxv/librte_net_xilinx.so -- -i
```
`-d` is required whenever the DPDK application is statically linked
against DPDK (the common case, including a default-built `testpmd`):
EAL only auto-scans its installed `pmds-<abi>/` directory when DPDK
itself is a shared build.

The device's device-tree node must have
`compatible = "xlnx,xxv-ethernet-1.0"` for probing to match.

The AXI DMA must be attached to a cache-coherent port (HPC), with
`dma-coherent` set on the device-tree node. `axidma_flush_bd_cache()`
only issues `rte_wmb()`/`rte_io_wmb()` ordering barriers, not an actual
cache flush — it relies on this coherency for BD ring visibility to the
DMA engine.

## License

BSD-3-Clause

Copyright (C) 2026, Advanced Micro Devices, Inc.
