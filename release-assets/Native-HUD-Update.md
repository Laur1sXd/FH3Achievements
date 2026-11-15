# Native achievement HUD update

New unlocks now use FH3's own driving HUD notification, including its
`Achievement` trophy icon and existing notification template.

- One unlock: **ACHIEVEMENT UNLOCKED** / **Achievement name - 10G**.
- Two or more in one detection pass: **ACHIEVEMENTS UNLOCKED** / the count and
  combined Gamerscore, preserving the mod's previous grouping rule.
- Independent batches queue in order and wait for the native notification
  component to become available in free roam while the game is in focus.
- The first-run save-import summary and **Achievement pop-ups** setting remain
  supported. Changing profiles discards notices belonging to the previous career.
- Off stops notification updates and clears live notices. Small native
  constructor/destructor watches remain during resident Off/On switching to keep
  HUD component pointers valid; full shutdown disables all notification hooks.
- The custom Windows overlay has been removed. This update uses the game's HUD;
  it does not submit Xbox account achievements or create Xbox system toasts.

Build with `Build.ps1 -SmokeTest`, then run `native-notices-test.exe` for the
native ABI fixture and `runtime-test.exe <new scratch directory>` for Accept/F9.
The save smoke harness also checks backfill, ledger migration, live counters,
save integrity and resident/full shutdown.

The local install/source ZIPs remain named **1.0.0**. The already-published
GitHub v1.0.0 release assets are separate and have not been replaced by this update.
