# Automatic transfer selection during stop

The `2e340dc` Windows release passed its CUDA lifecycle run, but D3D11VA cycle 51 failed after successfully presenting both 1920×1080 and 1280×720 video. Its `auto` transfer had already selected CPU NV12. The final decoded frame arrived after `request_stop()`: the decoder's capability notification called `presentation_failed()`, whose automatic-recovery condition excluded stopping sessions, so it incorrectly changed a normal stop to `Faulted`.

Non-CUDA capability selection now shares a locked fallback helper with live GPU recovery. The helper ignores notifications after stop and keeps repeated selection idempotent. Actual upload failures and GPU retirement failures retain their fault paths. No test hook is present in shipped sources.

An external regression pauses the real decoder callback immediately before session handling, after either the first decoded frame or an already established fallback, then issues a stop. It uses real HEVC input and loopback UDP with a software decoder. Both schedules fail on `2e340dc` and pass with the correction; real CPU/GPU error controls still produce `Faulted`. Evidence is in `processing/2026-11-17-rm-stop-race/windows-race-v1` outside the product repository. Probe SHA256: `de5857b8e0b6d9fe9b9bc911e35e81fa921dd55a65e39d3683c7e73d9e0c0df9`.

These are deterministic core checks on Windows. New standalone packages, hardware lifecycle checks and formal measurements remain separate acceptance requirements. Previous candidate performance and functional results are not attributed to this correction.
