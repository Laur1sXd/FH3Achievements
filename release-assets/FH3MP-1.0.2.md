# Achievements v1.0.2

- **Offline and FH3MP support:** works with standalone Azimuth and FH3MP through
  Encore. Recognizes Encore's FH3MP shim installation without depending on the
  Mod Platform's older FH2MP detector.
- **Multiple-save compatibility:** automatically selects FH3's active player
  when offline and Encore careers coexist. Each profile keeps its own unlocks,
  dates, counters and stunt evidence. Switching careers restores the matching
  journal; missing or unreadable saves cannot display another player's progress.
- **Existing-save backfill fixes:** restores supported achievements from the
  selected career. Level 20 and 150 goals also use earned XP when the cached
  level is outdated. Fresh careers start with zero unlocked achievements.
- **Expanded tracking:** all 135 Xbox One achievement entries have local
  tracking paths when the native event and PR readers are active, covering the
  base game, Blizzard Mountain, Hot Wheels, Hoonigan and the final Forzathon goals.
- **Online achievement tracking:** records genuine completed events for Online
  Adventure, Online Freeroam and Co-op, successful Auction House claims/sales,
  community activities and supported Playground conditions. Going online alone
  never awards an achievement.
- **Correct DLC race matching:** Blizzard Mountain and Hot Wheels race counters
  require the correct world and a matching route. Quits, incomplete results and
  Rivals are excluded. Duplicate notifications for one race count only once.
- **Car-specific stunt tracking:** native PR results validate the stunt,
  three-star threshold, map, current car, class and mass. Different stunts, car
  classes and Hoonigan cars are retained as distinct evidence across restarts.
- **Progress preservation:** upgrades older local records to FH3ACH4 while
  retaining earned timestamps. Counters and distinct evidence survive F9 reloads.
- **In-game name:** renamed to **Achievements**. Keeps the original Progress
  tile, FH2-style journal and normal achievement status without an extra offline
  connection notice.
- **Existing controls and notifications retained:** FH3's native trophy HUD,
  two-or-more unlock grouping, first-import summary, pop-up setting and
  A-to-accept/F9 Load controls remain available.

These are local mod achievements; they do not unlock achievements on an Xbox
account. Online requirements still depend on FH3MP providing the relevant mode
or service. Tracking support does not restore unavailable services such as
Forza Hub. Historical progress is recovered only where the save proves it;
live-only conditions need to be completed with the tracker running.

Validated with real offline and Encore save reads, profile selection and XP
threshold fixtures, and DLL smoke checks covering save integrity, multiple
careers, missing/corrupt saves, evidence isolation, record migration and F9
lifecycle. Actual online-race and auction unlocks still require gameplay checks
against the installed FH3MP services.

No DINPUT8.dll, Encore, cloud-save or game-save changes are included.
