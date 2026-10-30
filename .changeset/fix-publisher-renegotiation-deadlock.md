---
livekit: patch
---

Backport the official publisher transport renegotiation deadlock fix.

Release the transport mutex guard before starting a deferred renegotiation, preventing a permanent lock when publish or unpublish operations overlap an in-flight offer/answer exchange.
