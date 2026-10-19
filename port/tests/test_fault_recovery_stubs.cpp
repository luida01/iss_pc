// SPDX-License-Identifier: GPL-2.0-or-later
// The loader's fault-recovery hook (src/runtime_memory.c) is not linked in the standalone
// renderer tests: there is no guest memory there, so nothing can fault. The real
// tlg_setjmp/tlg_longjmp come from src/compat_win.c, which the test links.
#include "../gpu/shim/tlg_toggles.h"

extern "C" __thread sigjmp_buf* runtime_fault_recover = nullptr;
