# FH3 Achievements v1.0.2

- Added FH3MP-compatible tracking for **Welcome to Horizon Online** (five
  completed events) and **Online Adventurer** (25 completed events), including
  existing progress stored by the game.
- Added **Auction Fever** and **5 Star Seller - A+++++** through FH3's native
  Auction House transaction events. Auction counters start with this update and
  are retained in the mod's per-profile records.
- Supported online achievements now show progress in the journal and unlock
  through the usual native trophy notification. Two or more detected together
  still share a notification.
- Removed the mod's offline-only registration flag. The mod reads game saves and
  events locally and forwards the native events unchanged.
- Remaining online achievements stay locked until their requirements and
  triggers are verified. Installing FH3MP does not automatically unlock them.
- Existing records/settings and A/Accept/F9 behavior are preserved. No platform,
  Encore, cloud-save or game-save changes are included.

Validated with the native-event fixture, save/ledger smoke harness, native HUD
fixture and Accept/F9 tests. Actual auction and online-race unlocks still need an
in-game check against the installed FH3MP 0.1.1/Encore services.
