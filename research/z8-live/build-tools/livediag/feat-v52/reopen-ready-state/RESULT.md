# SET_PARAMS readiness revision

Candidate source: `work/src-fix37/codec/live_w5enc.c`, SHA-256 `5ccd5458dd9ce995e4a9d59fb6d5ad03011e4d9f54ea387dcb302046247f33f2`. This is based on immutable E and does not include the independently developed persistent HEADERS reply buffer. Root merges that change separately.

The E reopen observer saw SET_PARAMS return 0, followed by HEADERS returning -1 before any second hardware HEADER command. The native worker can still be initializing when the service acknowledges. The new code submits SET_PARAMS once, keeps nonzero service returns as failures, and waits at most one second for native state 2 before HEADERS.

The wait allows state 0 (request not yet taken) and state 1 (initializing). It verifies the original VDI arena at `a02d3d950`, VDI base, client 4 mapping, exclusive service slot, absence of other clients, release request and previous close failure. In state 1/2 it captures the actual native instance, checks active/type, and refuses pointer replacement or state regression. Other states and timeout fail without clearing native events, changing result slots, resubmitting initialization or freeing the pool. `native_instance` remains at offset 1057016 and the allocation ABI is unchanged.

The pre-request state-0 condition comes directly from the worker's INIT handler at `40c3b17c`. The instance pointer is published by native open at `40c45380`, native sequence setup returns successfully, and only then does `40c3d524` publish state 1. Native init completion publishes state 2 at `40c3d8b4`; HEADERS tests that exact state at `40c3b668`.

The first candidate incorrectly required shared result `a02d32788 == 0`. Its isolated tests passed, but F full QEMU rejected the first open. Existing E full-run checkpoints proved that a successfully encoding instance could have `2788 == -1`. That result is not a persistent readiness flag. The old `reopen-ready` source, its tests, F binary and failed run remain separate evidence; this revision supersedes that condition.

`test_native_ready_state.py` executes unmodified native instructions to reproduce:

1. `40c3d898`: result 0, then state 2, then event `0x20`.
2. `40c3b17c`: another INIT request in state 2 is rejected, overwrites the shared result with -1 and posts `0x20`, while leaving state 2 unchanged.
3. `40c3b668`: HEADERS still accepts state 2, submits VPU command `0x33` and transitions to state 3.

The same test executes the complete native per-stream reset and confirms it preserves both result slots and event wrappers. Original RTOS flag matching with mode `0x21` consumes just the matching bits. A normally consumed completion does not remain set. A late completion after timeout could remain; the complete-run observation does not identify that as the sole producer.

Validation: `ready-arm-tests.json` contains 92 cases using the actual compiled C in both HEVC and AVC builds, including full `open_stream` call order, stale shared results, state 0→1→2, owner loss, instance substitution, nonzero SET_PARAMS returns, and bounded timeout. There are no writes to native globals by the new guard. `native-ready-state.json` contains the native counterexample above. Time, API calls and scheduling are explicit boundaries in these tests; full G integration and hardware validation are separate.

Failed open retains the existing pinned-allocation policy. It does not solve the broader partial-open recovery policy or claim safe disposal of an incompletely initialized native instance.
