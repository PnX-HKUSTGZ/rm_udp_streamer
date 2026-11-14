# M0 independent review — PASS

Reviewed 2026-11-15; product commit ce64937. Scope: M0 gates in docs/implementation-plan.md, not M1–M6. No product implementation was modified.

## Independent work performed

Read the implementation gates, capability report, dependency lock/build entry, C++ decoder and GL/CUDA probe, C# consumer lifecycle and cached Godot renderer source. Re-ran Windows run_static_decode.py, Windows run_windows_probe.ps1, WSL run_static_decode.py and WSL run_linux_probe.py against real graphics sessions. Waited for the Windows and WSL process sessions to finish. Windows/WSL decode runners exited 0; all five engine cases reported actual successful execution and exit 0. Independently parsed the resulting JSON rather than accepting runner exit status alone. Rechecked all seven recorded probe/sample SHA256 values and all six Windows/Linux FFmpeg archive SHA256 values against the product lock/evidence.

The saved check_evidence.py and checks.json contain additional independent assertions. Re-runs refreshed the original task logs. The product working tree remained clean.

## Gate findings

- Both OS targets actually run Godot 4.7.2 Mono, .NET runtime 8.0.34 and native FFmpeg n8.1.3 extension code, including a joined native worker thread. Windows Vulkan chooses RTX 5070 and Arc 140T in the separate cases; WSL Vulkan/GL reports llvmpipe. These are real graphical runs, not headless substitutes.
- Windows software, CUDA, Intel D3D11VA and NVIDIA D3D11VA each decode 120 frames matching the software reference; WSL software/CUDA also match all 120. Source forces the requested hardware format, rejects a frame without hw_frames_ctx and counts hardware frames before diagnostic download. Logs show cuda or d3d11 and hardware_frames=120. This supports actual hardware HEVC Main decoding, not merely device enumeration.
- Windows GL creates/imports RGBA8 and performs CUDA register/map/device-memory memset/device-to-array copy/unmap/synchronize. All 12 cycles have success return codes and gpu_pattern_shift=1. Six checked images match the CUDA-shifted pattern. The GL-only initial pattern would not match those expected pixels with the recorded shift. This is valid evidence for the tested primitive, not proof of HEVC surface conversion or a continuous video bridge.
- Producer GL fence completion precedes CUDA access; CUDA synchronization and unmap precede GL consumption. Consumer removal precedes retirement on the same rendering context; the native consumer fence must return ALREADY_SIGNALED or CONDITION_SATISFIED before freeing RID/native texture. I independently checked every producer/consumer fence and every GL error code. Six cycles omit screenshot readback and stop after the first draw; they still execute a real completion fence. The cached Godot texture release implementation excludes native-handle-owned GL storage, supporting separate RID/native texture destruction. No global RenderingDevice.sync is used. These conclusions apply to this single-consumer probe and must not be generalized to future multi-frame queues.
- WSL GL returns CUDA device query error 304 for llvmpipe, while native GL image import itself passes. Reporting the current WSL CUDA/GL/Vulkan pairing unsupported, with CPU fallback and native Ubuntu separate, is reasonable. Other candidates remain unverified with explicit reasons; AMD and VAAPI are not silently claimed supported.
- Source commits, versions, compilation choices, archive fingerprints and toolchain versions are recorded. The Windows/Linux FFmpeg archives available here match all locked hashes. The export template has a pinned version, official URL, size and SHA512. An unfinished template cache is NOT an M0 blocker: M0 requires dependency locking and runtime/extension feasibility, while independent export is explicitly an M3/M6 gate. Download completion and checksum verification remain necessary before export claims.

## Non-blocking improvements for later verification

1. processing/2026-11-15-m0-environment/run_linux_probe.py:24–26 logs child exit codes but does not fail on nonzero status, missing/stale JSON or an error field. Future automation should remove the old report before each run, require a new report, parse required checks and propagate child failure. This review checked fresh contents and actual child statuses independently; no false success occurred in this run.
2. processing/2026-11-15-m0-environment/probe/Probe.cs:83 and :121 allow the GL import probe to pass with CUDA unavailable, using the original GL pattern. This is appropriate for the WSL GL import experiment but the broad gpu_import_passed name must never be treated as CUDA support. Add an explicit expected-CUDA mode/assertion for Windows bridge regression tests. This review separately required all CUDA return values and pattern shift 1 on all Windows cycles.
3. rm_udp_streamer/build/build_ffmpeg.py:34–37 checks HEAD but not a dirty source checkout. For strict release reproduction, reject tracked source modifications or record their patch hash; also enforce archive/source verification in the release build. The supplied locked archives match today and this does not invalidate M0 feasibility.

## Decision

PASS. All M0 gates have supporting source-level and independently reproduced runtime evidence. No blocking discrepancy found. Proceed to M1, retaining the explicit M4 requirements for real HEVC GPU surfaces, YUV conversion, repeated updates and in-flight ownership, and M3/M6 requirements for independent exports, licenses/relink material and stability. Do not market M0 as a finished zero-readback video receiver or all-vendor support.

## Supplemental review — three follow-up improvements accepted

2026-11-15: Inspected the updated Linux runner, Windows runner, C# GL probe and FFmpeg build verification only; no full matrix rerun was needed. The Linux runner deletes the previous JSON, rejects child failure/missing output, parses errors and checks lifecycle/fences. The Windows runner deletes previous JSON and explicitly requires CUDA for its GL case. C# now reports GL import and CUDA copies separately and fails a required-CUDA cycle unless the native operation completed and shifted the GPU pattern.

Inspected the newly produced reports: Windows gl_import_passed=true, cuda_copy_passed=true, cuda_copy_cycles=12; WSL gl_import_passed=true, cuda_copy_passed=false, cuda_copy_cycles=0; neither has an error. These fields correctly preserve the capability distinction. The FFmpeg builder now rejects modified/untracked files after verifying pinned HEAD; --verify-only returns before any build. Independently ran --verify-only against the real WSL cache: exit 0, both pinned source checkouts clean.

All three prior non-blocking recommendations are addressed. M0 decision remains PASS. This supplement does not claim additional M1–M6 functionality or replace the parent task's final evidence/hash refresh after changed probe sources.
