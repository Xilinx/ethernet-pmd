# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026, Advanced Micro Devices, Inc.

# rte_platform_register() lives in the bus/platform driver, not a core
# DPDK library, so it isn't part of libdpdk.pc's default (dynamic) Libs.
bus_platform = cc.find_library('rte_bus_platform',
    dirs: [dpdk.get_variable(pkgconfig: 'libdir')])

sources = files(
    'xxv_pmd.c',
    'axidma.c',
)

# Install alongside the in-tree PMDs, in EAL's RTE_EAL_PMD_PATH
# (pmds-<abi>/), and version the .so the same way DPDK versions its own
# drivers: by ABI, not by an independent driver release number.
# libdpdk.pc's Version is the DPDK release (e.g. 25.11.2), not the ABI,
# so derive it from the installed rte_build_config.h instead.
pmd_path = cc.get_define('RTE_EAL_PMD_PATH',
    prefix: '#include <rte_build_config.h>',
    dependencies: dpdk).strip('"')
abi = pmd_path.split('pmds-')[1]

shared_library('rte_net_xilinx', sources,
    dependencies: [dpdk, bus_platform],
    c_args: ['-DALLOW_INTERNAL_API'],
    install: true,
    install_dir: pmd_path,
    version: abi,
    soversion: abi.split('.')[0],
)
