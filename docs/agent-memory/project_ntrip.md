---
name: project_ntrip
description: NTRIP push architecture, the reconnect-storm bug, and diagnostics
metadata:
  node_type: memory
  type: project
---

NTRIP push client design and the hard-won root cause of "rejected"/drop errors. See [[project_structure]].

## Data flow
- `base_station.cpp` accumulates RTCM into a 1024 B batch, flushed every 200 ms or when full (`kRtcmBatchUs`), then `push()`es to each `NtripPushClient`.
- Each client (`main/ntrip_push.cpp`) has a 12-deep FreeRTOS queue + worker task. Worker drains queue and `send()`s to the caster.
- Clients: RTK2go (v1 SOURCE), Onocoy (v2 POST), RTKdata (v1). Hosts in `app_config.hpp`.

## Root cause of push errors (fixed 2026-06-12)
**The bug:** on queue-full, `push()` did `++dropped_batches_; reconnect_ = true;` — forcing a full TCP reconnect to the caster. The queue fills easily because the worker's `send_all()` can block up to ~5 s on a stalled TCP window while the producer keeps enqueuing every 200 ms (12 slots fill in ~2.4 s). So a brief network hiccup escalated into a disconnect, and **frequent reconnects are exactly what RTK2go/Onocoy rate-limit and ban** → "rejected" errors and churn.

**The fix:** on queue-full, drop the OLDEST batch and enqueue the newest (keep freshest corrections), increment `dropped_batches_`, and **do NOT** set `reconnect_`. Genuine socket wedge is still recovered via `send_all()` returning false → `close_socket()` → reconnect in `run()`.

**Why:** RTCM corrections are superseded every second, so dropping a stale batch is harmless; tearing down the caster connection is not.

## Diagnostics added (NtripStatus)
- `reconnects` (live-connection drops), `last_error` (sticky reject reason, not overwritten by "connecting"), `connected_sec` (current connection uptime), `last_send_age_sec` + `ever_sent` (data freshness). Surfaced on `/status` JSON, the status page service rows, and the new `/logs` page.

## How to apply
- If push churn returns: check `/logs` and the status page reconnect counter + last_error. High reconnects with low dropped = network/caster issue; high dropped = producer outpacing sender (consider larger queue depth `kQueueDepth`).
- Never reintroduce a reconnect-on-drop. Dropping batches is the correct backpressure for RTCM.
