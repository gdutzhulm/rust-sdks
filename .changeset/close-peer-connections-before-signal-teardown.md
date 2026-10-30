---
livekit: patch
---

Close peer transports before awaiting unbounded signalling teardown so cancelled or timed-out room closes do not retain ICE sockets and transport resources.
