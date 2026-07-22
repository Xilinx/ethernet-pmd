# SPDX-License-Identifier: BSD-3-Clause
# Copyright(C) 2024 AMD-Xilinx

if not is_linux
    build = false
    reason = 'only supported on Linux'
    subdir_done()
endif

sources = files('xxv_pmd.c', 'axidma.c')
deps += ['bus_platform']