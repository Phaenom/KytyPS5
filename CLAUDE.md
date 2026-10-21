# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

KytyPS5 is a PS5 translation layer in C++20 (fork of Kyty). Guest x86-64 game code runs natively on
the host; system libraries are reimplemented (HLE); the RDNA 2 GPU is emulated on Vulkan 1.3 by
interpreting PM4 command buffers and recompiling shaders to SPIR-V.

## Build

Clang only: `clang`/`clang++` on Linux and macOS, `clang-cl` on Windows (MSVC `cl.exe` is rejected at
configure time). `src/` builds with `-fno-exceptions -fno-rtti`, so avoid throwing APIs (`std::stoull`,
`std::filesystem` overloads without `std::error_code`, ...). Only `scalar_provenance_tests`,
`resource_tracking_tests` and `virtual_memory_allocation_tests` are built with `-fexceptions`.

```bash
git submodule update --init --recursive
cmake -S . -B _Build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++        # add -DCMAKE_PREFIX_PATH=<Qt6> if needed
cmake --build _Build/linux --target launcher --parallel        # launcher depends on kyty_emulator
cmake --install _Build/linux --prefix _Build/linux/install     # runnable tree, Qt bundled
```

- `-DKYTY_BUILD_LAUNCHER=OFF` builds without Qt (targets `kyty_emulator`, `kyty_tests`).
- `-DKYTY_ENABLE_CLANG_TIDY=ON` runs clang-tidy (`src/.clang-tidy`) during the build.
- Fedora Atomic hosts (Bazzite, Silverblue): build inside a Distrobox, e.g.
  `distrobox enter kyty-build -- bash -c 'cd ~/Workspace/KytyPS5 && cmake --build _Build/linux --target launcher --parallel'`.
  The installed binaries also run on the host.
- Release links with `-Wl,--strip-all` but compiles with `-g -fno-omit-frame-pointer`. For symbolized
  profiles, relink without `--strip-all` (`ninja -d keeprsp kyty_emulator` keeps the response file).

## Tests

Each test is its own executable (`tests/*Tests.cpp`, one `main()`, no per-case filter), declared
`EXCLUDE_FROM_ALL` in the top-level `CMakeLists.txt`. The ctest name drops the `_tests` suffix.

```bash
cmake --build _Build/linux --target kyty_tests --parallel                  # all test executables
ctest --test-dir _Build/linux --output-on-failure -j8                      # everything
cmake --build _Build/linux --target resource_tracking_tests && \
  ctest --test-dir _Build/linux --output-on-failure -R '^resource_tracking$'   # one test
```

CI (`.github/workflows/build.yml`) runs only a CPU-only subset:
`-R '^(audio_out2_port|pad_haptics|controller_settings|host_input|ime_dialog|lru_cache|memory_tracker|page_manager|resource_tracking|scalar_provenance|resource_materialization|shader_vertex_metadata|archive_file|avplayer_file|virtual_memory_allocation)$'`.
GPU tests need a Vulkan device. On AMD RADV, 8 fail before any change: the 6 texture/buffer cache
tests that hard-code `VK_FORMAT_D24_UNORM_S8_UINT`, `shader_recompiler_compute` (IndirectBufferStore)
and `gpu_tiler` (family=4 bpe=1). Compare against that baseline, not against zero.

New test cases go into the existing `tests/*Tests.cpp` for the area: add a function and call it
from that file's `main()` (e.g. `Run("name", TestFunction)` in `ResourceTrackingTests.cpp`).

## Formatting

clang-format (`src/.clang-format`: tabs for indentation, 100 columns) runs through pre-commit on
staged `src/**/*.{cpp,h,inc}`: `python -m pre_commit install --install-hooks`.

## Running

`kyty_emulator --game <dir|eboot.bin|.zar> [options]`; `--help` lists everything. The launcher builds
the same command line (its last run is written to `_Build/linux/install/kyty_run.sh`). Useful flags:
- `--perf-overlay true`: in-game panel, F2 toggles and Shift+F2 shows details, backed by
  `Common::DebugCounters` and `common/hostStats`.
- `--profile`: Tracy zones (`KYTY_PROFILER_FUNCTION`).
- `--shader-log-direction File --printf-direction File --printf-output-file <path>`: decoded
  RDNA 2 and IR dumps per shader. `--graphics-debug-dump true` writes `_Shaders/`.
- `--vulkan-validation`, `--shader-validation`, `--gpu <index>`.

## Architecture

**CPU side.** `src/loader` loads the ELF/SELF and modules, and `runtimeLinker` binds imports to HLE
implementations in `src/libs` (system libraries) and `src/kernel` (memory, pthreads, file system,
event queues). `guestInstructionPatcher` and `x64InstructionEmulator` patch or emulate guest
instructions the host CPU lacks. Host signals go through `Common::HostException` to
`Loader::KytyExceptionHandler`. Guest memory faults on GPU-tracked pages are forwarded to
`RenderContext::HandleFault`.

**GPU frame path.** Every stage runs on one thread, the GPU command thread:
1. Guest submissions reach `GuestGpu` (`graphics/guest_gpu/graphicsRun.cpp`). Its thread
   (`GuestGpu::ThreadRun`) owns all GPU state. Other threads post work with `SendCommand` or
   `SendCommandSync`.
2. `CommandProcessor` (`guest_gpu/command_processor`) decodes PM4 packets and register state into
   draws and dispatches.
3. `RenderExecutor` (`host_gpu/renderer/render.h`, `renderDraw.cpp`, `renderCompute.cpp`,
   `pipeline/descriptors.cpp`) runs each draw:
   - `PipelineCache::GetGraphicsPrograms` picks the programs.
   - `PrepareBindings` and `FindBuffers` resolve resources.
   - `CommitBindings` writes the descriptors.
   - Commands are recorded through `CommandScheduler`.
4. Shaders are recompiled lazily and cached in `_PipelineCache/`:
   - `shader/recompiler/frontend` decodes RDNA 2.
   - `ir` builds SSA IR.
   - `ir/passes` runs `ResourceTracking`, `ResourceMaterialization`, `SrtWalker` and the other passes.
   - `backend/spirv` emits SPIR-V.
5. Resource specialization runs on every draw and dispatch, not once per shader:
   - `IR::MaterializeResources` uses `SrtWalker` to evaluate the shader's resource-table (SRT) reads
     from live guest memory (an interpreter, with an Xbyak JIT for simple plans).
   - The result fills a `ResourceSnapshot`, which picks the descriptors and the specialized program.
   - The `specialization_reads` it records must not overlap what the draw writes; `CommitBindings`
     checks this.

**Memory coherence.** `BufferCache` and `TextureCache` (`host_gpu/renderer/cache`) mirror guest
memory on the GPU. `MemoryTracker`, `PageManager` and `RegionManager` (`host_gpu/`) track page state:
- CPU writes to tracked pages fault and invalidate the cached copies.
- CPU reads of GPU-modified pages fault into `BufferCache::ReadMemory`, which downloads the data.
  It blocks until the GPU has finished the work that wrote it.
- All CPU-side waits for the GPU go through `MasterSemaphore::Wait` (a timeline semaphore). The
  overlay's "CPU waiting on GPU" counts them.

**Presentation.** `graphics/presentation` holds the window, swapchain, video out, OpenXR and the
ImGui overlays (`systemOverlay`, `performanceOverlay`).

Reference for shader work: AMD's RDNA 2 ISA guide (document 70648). Keep changes valid under both
RDNA 2 semantics and Vulkan/SPIR-V validation.

## Contribution rules (from README)

- Upstream (`KytyPS5/KytyPS5`) closes PRs that are general refactors or bundle unrelated changes.
  Each PR names the affected game, the exact problem, and evidence of the fix.
- Shared code must not regress Windows or Linux.
- AI policy: disclose the scope of AI involvement and the human review and testing. PR
  descriptions, code comments and issue comments must come from the human contributor. Draft them
  for the user; do not post them autonomously.
- Commit subjects use an area prefix: `shader: ...`, `renderer: ...`, `tests: ...`, `launcher: ...`.

## Current work: performance (`perf-testing` branch on `fork` = github.com/Phaenom/KytyPS5)

Goal: make 60–120 FPS possible. Test game is PPSA01342, which runs about 6–8 FPS in-game and 30–60
in menus, on a Ryzen 9 7950X with an RX 7900 XTX (RADV).

- Not GPU-bound: the GPU is about 40% busy. The game's "BPE JobWorkerTh" threads at 100% are an idle
  polling loop, not work.
- The bottleneck is the GPU command thread: per-draw resource specialization plus GPU readbacks.
- Done:
  - Inlined IR `Value`/`Inst` accessors.
  - `CommitBindings` gathers written ranges once.
  - Together these took the thread from 108 to 76 ms per frame, 6.4 to 8.3 FPS.
  - Linux GPU stats in the overlay.
  - `IR::ResourceMemo` reuses interpreted `MaterializeResources` results whose user data
    registers and guest reads are unchanged: 69% of interpreted calls reused, 109 to 103 ms.
- Next: about 300 blocking GPU waits per frame, all `BufferCache::DownloadBufferMemory`:
  - about 55% are `GuestGpu::SendCommandSync` read requests from other threads;
  - about 41% are faults while `MaterializeResources`/`SrtWalker` read GPU-written memory;
  - about 2% are indirect-draw arguments.
- Frame time is the GPU thread's CPU time plus its blocking waits, run one after the other
  (about 109 + 58 ms). Fewer waits alone do not help: the rest still wait for all queued GPU work.
  Reading GPU-protected SRT pages without a readback (local branch `wip/srt-protected-reads`)
  cut waits from 341 to 262 per frame but cost 9 ms more CPU, 6.0 to 5.8 FPS. The CPU time per
  draw (SRT evaluation, texture-cache lookups) has to come down first.
- A frame makes about 14,000 `ProgramCache::Get` calls. 63% of them refresh through the SRT JIT
  and bypass the memo. `CommandProcessor::DrawIndirectMulti` runs the full draw path once per
  sub-draw (shader lookup, bindings, pipeline, render targets) and takes about half of the
  thread's CPU time. Sharing that work across the sub-draws is the next large lever.
- Measuring:
  - FPS: count `Libs::VideoOut::FlipQueue::Reserve` calls with
    `perf stat -e mem:<addr>:x -p <pid>`.
  - Who waits: `perf record -e mem:<waitSemaphores call in MasterSemaphore::Wait>:x --call-graph fp`.
  - Addresses come from an unstripped relink and change with every build.
