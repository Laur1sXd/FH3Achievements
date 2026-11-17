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
- Added local tracking paths for the remaining online and DLC goals. DLC race
  progress validates both map and route; car-specific PR goals use the native
  result, stunt identifier, car, class and mass. Distinct stunts, cars and
  classes persist across reloads.
- Recognizes Encore's FH3MP shim installation without relying on the platform's
  FH2MP detector. The journal keeps its normal progress and unlocked status.
  Installing FH3MP does not unlock goals or add missing server modes/services.
- Fixed zero achievements when offline and Encore saves coexist. The tracker
  selects the active player reported by FH3, reads only that career's save
  revisions, and restores its separate achievement record when switching
  profiles. Missing active saves do not inherit another career's progress.
- Fixed level achievement backfill when the cached save level lags behind
  earned XP. Level 20 and 150 progress now also uses the original XP curve.
- Existing records/settings and A/Accept/F9 behavior are preserved. No platform,
  Encore, cloud-save or game-save changes are included.

Validated with the native-event fixture, save/ledger smoke harness, native HUD
fixture and Accept/F9 tests. Actual auction and online-race unlocks still need an
in-game check against the installed FH3MP 0.1.1/Encore services.
