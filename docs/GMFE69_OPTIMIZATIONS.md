# GMFE69 permanent optimizations

The validated Medal of Honor: Frontline optimizations are part of the build and
survive deletion of `port-build/`.

## Enabled

- native DolRecomp cache-control and generic SPR helpers (no generic fallback)
- unique-image native burst with canonical-chunk cache and direct image dispatch
- GameCube `/12` timebase specialization
- O3 module build by default; ThinLTO remains off
- GMFE69 idle-loop PC `0x80115F64`
- hardware FMA target on x86-64 GCC/Clang
- CPWritePointer single-writer fast path
- advisory BlockingLoop relaxed clear
- hot FP availability fast path in five GMFE69 chunks
- particle LFD reconstruction: 11 sites
- particle stack MEM1 fast path: 7 sites
- particle RNG seed MEM1 fast path: 11 sites
- single MEM1 bounds check and unlikely reservation branch
- enhancement config hot path and `moh_timing_wait(ctx)` API alignment when present

Generated-code optimizations live in `tools/gmfe69_postgen.py` and are applied
automatically by `tools/build_all_exec_module.py`.

## Intentionally excluded

These experiments regressed performance or were not proven correct:

- FIFO video-thread batch8
- StaticRecomp producer FIFO batching
- CP status local cache / interrupt / distance experiments
- ThinLTO
- particle dead-stack-store removal
- speculative low-memory / ARAM remapping

`CPReadWriteDistance` remains an atomic RMW. The late-game low-address audio-read
issue is tracked separately; diagnostic tracing is not a production patch.
