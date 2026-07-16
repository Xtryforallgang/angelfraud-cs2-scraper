---
name: angelfraud-rate-limit
description: "Rate limiting configuration: 1 request per 5 seconds on AngelFraud API"
metadata:
  type: reference
---

# AngelFraud API Rate Limiting

Applied in `ErScripts/AngelfraudAPI.cpp`, `WorkerLoop()`:

- **MAX_CONCURRENT = 1** — only one thread at a time
- **Per-request cooldown: 5000ms** — 5-second pause after each `FetchPrice()` call
- **Profile refresh: every ~30s** — separate GET to `/profile`, not part of the price-check loop
- **Cache TTL: 300s (5 min)** — players with cached prices are skipped

This ensures a maximum of **1 request per 5 seconds** to `angelfraud.steamcommunity.asia`.

All other API calls (ToggleFavorite, RemoveFavorite, SaveNote) are user-initiated via the overlay and not rate-limited by the worker.
