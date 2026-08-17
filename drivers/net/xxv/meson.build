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

shared_library('rte_net_xilinx', sources,
    dependencies: [dpdk, bus_platform],
    c_args: ['-DALLOW_INTERNAL_API'],
    install: true,
    install_dir: get_option('libdir') / 'dpdk' / 'drivers',
    version: meson.project_version(),
    soversion: meson.project_version().split('.')[0],
)
