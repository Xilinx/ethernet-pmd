# SPDX-License-Identifier: BSD-3-Clause
# Copyright(C) 2024 AMD-Xilinx

project('xxv-pmd', 'c',
    version: '1.0.0',
    default_options: ['buildtype=release', 'c_std=gnu11'])

# Requires DPDK 25.11 built with -Denable_driver_sdk=true, plus the
# bus/platform 'compatible' string matching patch carried out-of-tree.
dpdk = dependency('libdpdk', required: true)
cc = meson.get_compiler('c')

# Each driver lives under drivers/<class>/<name>/ with its own meson.build
# declaring its shared_library() target, mirroring DPDK's own layout.
subdir('drivers/net/xxv')
