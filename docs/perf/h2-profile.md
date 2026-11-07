# H2 `nave` CPU profile

## Method

The supplied fixture checksums were verified with `shasum -a 256 -c SHA256SUMS`. The profile host was Ubuntu 24.04 x86_64 / GCC 13.3.0 running under QEMU on an Apple Silicon Mac, configured with four guest CPUs. It is Linux x86_64, but its CPU is emulated and its timings are not representative of the physical Haswell VPS.

`perf` could not find a matching tool for the VM kernel (`6.8.0-117`), so gprof was used. The profile executable was built with `CFLAGS="-O2 -pg" LDFLAGS="-static -pg"`; one cold acceptance run executed per profile. The H1 baseline worktree was at `ad9d5b8`; the H2 profile includes the inactive-debugger and idle-FDC optimizations. Percentages are gprof self time for each separate instrumented run, not production wall-clock shares.

The supplied VPS report is the physical Linux baseline: `-O2` took 5.1–5.6 s for 2,740 frames; `-O3 -march=native` took 4.5–4.8 s. Those numbers are supplied by the tester and are not measurements made here.

## Top ten functions

| Rank | H1 baseline | Self % | H2 final | Self % |
|---:|---|---:|---|---:|
| 1 | `cpc_step` | 32.54% | `cpc_step` | 32.21% |
| 2 | `cpc_video_tick` | 22.97% | `cpc_video_tick` | 26.86% |
| 3 | `psg_tick` | 10.36% | `psg_tick` | 12.61% |
| 4 | `z80_step` | 7.46% | `z80_step` | 9.15% |
| 5 | `fdc_tick` | 7.17% | `psg_mix` | 3.97% |
| 6 | `psg_mix` | 4.49% | `cpc_mem_fetch` | 3.80% |
| 7 | `tape_tick` | 3.48% | `fdc_tick` | 3.54% |
| 8 | `cpc_mem_fetch` | 3.48% | `cpc_run_frame` | 1.73% |
| 9 | `dbg_check_code` | 2.46% | `cpc_mem_read` | 1.12% |
| 10 | `cpc_mem_read` | 1.01% | `rot_shift` | 0.78% |

The active FDC tick count fell from 27,128,104 calls per cold run to 9,354,253. The gprof self-time estimate for `fdc_tick` fell from 0.99 s to 0.41 s across the separately instrumented runs. In the fixture, the tape is not loaded, so null tape ticks are skipped. With symbols loaded but no active breakpoints, `dbg_check_code` is no longer entered for every instruction.

The remaining profile is dominated by per-instruction CPU stepping and per-T-state video/PSG work. That points to the next substantial optimization area, but changing timing granularity there needs a new canonical-state and native/WASM conformance harness to protect behavior.

## Five-run benchmark

`Tests/headless/bench_nave.py` runs one cold acceptance sequence five times, including load, BASIC boot, keyboard-matrix movement, capture and restore replay. The benchmark was run on the same QEMU VM, first from a clean H1 worktree and then from H2, using the default `-O2` build. These are **emulated-CPU timings**, not VPS target results.

| Revision | Run factors | Median | Min | Max |
|---|---|---:|---:|---:|
| H1 (`ad9d5b8`) | 1.73x, 1.73x, 1.79x, 1.70x, 1.80x | 1.73x | 1.70x | 1.80x |
| H2 | 1.73x, 1.90x, 1.78x, 1.95x, 1.92x | 1.90x | 1.73x | 1.95x |

All ten runs returned H1-format hash `a8750fbe248c354e`. The median is approximately 9.8% higher under this emulator. This does not establish that the physical Linux VPS improved by the same amount, and it does not meet the 50x target.
