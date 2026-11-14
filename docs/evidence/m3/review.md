# M3 independent review

Result: **PASS** for M3 and local D1 delivery, 2026-11-15.

Reviewed Windows product HEAD af40108; implementation is 2b722b4 with recovery changes 6ce1a13. WSL clean checkout is 2b722b4. No product files modified and no commits made by this reviewer.

Read implementation-plan.md, m3-recovery.md, m2-media.md and evidence/m3/validation.json; inspected HEVC parameter parsing/cache and decoder recovery, MediaSession generations/lifetime, native Godot upload/surface/session, C# RuntimeOptions/Main and isolated export implementation. Parameter/cache bounds, source-generation reset, random access/RASL handling, recoverable versus terminal failures, accepted generation checks, CPU upload strong references, frame expiry, stop/join and rebind behavior are consistent with the M3 scope. Export validates pinned templates, diagnostics, assemblies and local runtime rather than accepting exit code alone.

Independent execution (not merely reading previous PASS summaries):

- Windows software native recovery: all five checks PASS, including exact 89-frame hash comparison after loss without repeated parameter sets, rejecting old parameter sets on new generation, validated-cache recovery, CRA join/RASL suppression, and 480-frame range/Main10/resolution changes. Evidence native-windows.log.
- WSL CUDA native recovery: same checks PASS. Evidence native-linux.log. Tests run from Linux filesystem with WSL CUDA driver library path.
- Windows CUDA full graphical fault sequence rerun: normal, missing frame/no repeated parameters, malformed stream, 2.5-second silence, source/resolution replacement, full range, Main10 conversion, 1800-group burst and final recovery. Exit 0, errors empty; 20 recovery events, 1 decoder-input error, 1736 queue drops, final 1280x720. Stale picture expired after two seconds. All 12 actual-video stop/start cycles passed with alternating cpu_nv12 and cpu_rgba, and final port rebind succeeded. Evidence windows-recovery-cuda-0.json/log/png. The 100-ms sampler missed the instantaneous overload label; queue_dropped and executed sender output establish actual overload. No graphics/performance jobs ran concurrently.
- Windows D1-windows-v3 official executable: empty cwd, PATH only Windows System32, invalid DOTNET_ROOT and disabled multilevel lookup. Actual C# main UI received video; 657 decoded / 633 submitted, exit 0, errors empty, port rebind succeeded.
- WSL D1-linux-v2 official executable: empty cwd, PATH /usr/bin:/bin, invalid DOTNET_ROOT and disabled multilevel lookup. 604 decoded / 598 submitted, exit 0, errors empty, port rebind succeeded. CUDA decode and llvmpipe rendering are reported honestly.
- Both screenshots visually inspected: actual UI with correct-looking bars, grayscale and aspect ratio. Host traces identify .NET 8.0.34 and CoreCLR under the respective package directories. No editor --script or --path was used for package validation.
- audit.py independently checks Windows package manifest size/hash for every listed file, recorded phases, cycle transfer modes, expiry, package result and runtime logs. PASS.

Original six native and four graphical cases were examined through test source and reported artifacts; independently rerun coverage above is explicitly distinguished from earlier runs. No claim of a fresh OS VM or all-vendor GPU validation is made.

Nonblocking boundaries: Windows CUDA CPU transfer is below sustained 1080p60 and the report says so. No fabricated cross-group PTS latency statistics. M4 zero-readback GPU bridge, M5 actual-format fallback/device matrix, and M6 30-minute/50-cycle GPU stability plus final redistributable license/relink materials are not completed by this PASS. D1 remains a local validation package, not final public redistribution. Main10 conversion is explicitly enabled by the graphics test; normal UI default rejection remains as documented.

Evidence handling: copied run_package.py initially retained its original Linux auto-copy destination, overwriting Windows-side linux-package* in the earlier recovery directory. New independent results are preserved here and on WSL under the matching review directory. The parent was notified and will restore the earlier Windows artifacts from the unchanged WSL recovery directory. The review copy destination was corrected; numerical evidence above refers to this review directory.

No M3-blocking defect found. M3 may be marked complete and work may proceed to M4, keeping the documented future-stage gates intact.
