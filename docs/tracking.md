# Achievement tracking

Version 1.0.2 tracks local copies of the original 135 Xbox One achievements.
It keeps the original native journal, HUD notifications and two-or-more grouping.

## World and route matching

The supported game's world identifiers are 552 (Australia), 553 (Blizzard
Mountain) and 554 (Hot Wheels Islands). Online DLC race counters require both
that world and an identifier from that world's route table. Route identifiers
are not classified by a numeric range. Offline event identifiers are matched
against the original route, track and career-race mappings in `event_data.inc`.
For example, Mountain Rush Descent is route 2022, High Speed Highway is route
515 and Hot Wheels Goliath is route 528. An Island route paired with world 553
is rejected. Timed goals also require the correct car and a strictly shorter
completion time than the limit; Rivals does not count as a race completion.

Native ExitStatus 2 is required. Undefined, aborted, failed, draw and incomplete
results do not count. Completed race notifications share a RoundId de-duplication
window, so generic and specialized notifications cannot increment one race twice.

## Native stunt evidence

`pr_data.inc` contains 83 Australia, 23 Mountain and 29 Island PR entity names and
gold thresholds, extracted from the supported EntityModel definitions.
`pr_reader.cpp` observes the existing native PR reward callback without changing
its arguments or result. Its four original callers are Danger Sign, Drift Zone,
Speed Trap and Speed Zone completion paths. The reader validates the native
MedalData and Entity types and uses the entity's stable name, not its memory
address, as persistent evidence.

The current car is resolved through the game's local-player services. Car ID,
current class and mass are read with bounded ReadProcessMemory calls; known
method addresses validate the layout. Mass is converted from the physics unit
by the game's own factor of 100. Unknown layouts or unreadable pointers are
ignored. This layout was inspected in a parked, live FH3MP 0.1.1 session with a
BMW M4: world 552, car 2154, current S1 class 4, mass 1,537 kg.

Different Raptor stunts, Hoonigan stunts, the Blazer's three named Danger Signs,
Airstrip classes, Art Director's three activities, the two bodykit manufacturers,
and seven Hoonigan championship cars are stored as unique evidence. Old personal
bests alone cannot recover the car used in a past attempt.

## Online availability

Encore FH3MP 0.1.1 is detected by its `bcrypt.dll`, `fh3mp-shim.ini` and
`fh3mp-cloud.exe` installation files together, without reading credentials. A
`fh3-mods/FH3MP.dll` installation is also recognized. Installation is logged
separately from online status. This avoids the platform 1.4.2 installation
detector, which requires `FH2MP.dll`. No shared loader changes or new
achievement-unlock gates are introduced. The journal retains numeric progress
and earned dates, without an extra connection notice. Native online completion
events remain required for online-mode achievements; a multiplayer lobby is
not required merely to use services such as the Auction House.

Xbox's achievement service can queue eligible offline updates and post them
after reconnection, even when the title is no longer running
([Microsoft API documentation](https://learn.microsoft.com/en-us/dotnet/api/microsoft.xbox.services.achievements.achievementservice?view=xboxlive-dotnet-2017.11.20171204.01)).
This mod's local journal and HUD do not synchronize with that service; local
offline unlocks continue to appear without waiting for a network connection.

A local rule is enabled for every online achievement. This does not implement
missing FH3MP modes, restore Microsoft's retired Forza Hub app, or submit Xbox
account achievements. A rule waits for its original completion event when its
service is unavailable. Successful auction events are separate from listings;
Co-op and Playground flags are validated independently. Infected accepts the
game's catalog/gameplay mode representations and also requires its Playground
flag and the Hoonigan Bel Air.

The final Forzathon goals use the combined race/skill alternatives documented by
[Forza Support](https://support.forza.net/hc/en-us/articles/360019230034-FH3-Forzathon-Into-the-Horizon-Challenges).
Completed career-race result counts and persisted Ultimate/Snowman counters
provide conservative backfill. Races entered and ordinary wins are not used as
substitutes for completed race counts. Live evidence supplies the remaining
skill alternatives as they occur.

## Persistence and verification

Multiple `User_*` careers can coexist under `player-1`. The active local player
is resolved from FH3 1.0.125.0's player services (`4d6fc00`), with bounded memory
reads and method-layout checks. Identity getter `3c56d0` delegates to `22a6270`
on `this+8`, reading the XUID at player `+900`. This was inspected read-only in
the loaded offline career. Save selection matches that XUID, including hex case,
and never picks a career based on save timestamps, largest revision number,
the installed FH3MP shim or a stale launcher identity file.

Both normal reads and older-revision fallback stay inside the selected profile.
Unknown player identities or missing active saves do not borrow another
career's records. A profile switch loads its own `User_*.txt` record, retaining
the other careers' earned dates and evidence. Event batches spanning an identity
change are discarded because they cannot safely be attributed to either user.
The fixtures cover two populated careers, an unsaved third player and returning
to the original career without losing its live evidence.

The level 20 and 150 goals take the greater of the saved level and the level
proved by `Main/XP`. The supported game's `PlayerXPLevels.xml` uses 20,000 XP per
level through both goals, so the thresholds are 380,000 and 2,980,000 XP. An
empty career remains at zero progress, and XP arithmetic is bounded. The loaded
offline save had 395,385 XP and the HUD displayed level 20 while its cached
`Main/Level` was still 5; this mismatch is covered by a regression fixture.

FH3ACH4 adds `S achievementId stableKey` evidence rows to the per-profile local
record. Versions 2 and 3 migrate without changing earned timestamps. Writes are
atomic, bounds checked, and independent of game saves. F9 stops both native
readers, drains the pending batch, and preserves counters and unique evidence.
Older DLLs do not understand the new record; keep a backup before downgrading.

Automated fixtures cover all 135 initialized targets, zero starting progress,
thresholds, wrong maps/routes/cars, duplicate race events and unique stunt/car
sets, malformed native data, native-call forwarding, profile records, migration,
HUD queues and repeated resident/full reloads. The running FH3MP session verified
the native player/stunt layouts, successful DLL reload, hook installation and
journal loading. It did not earn all 135 achievements in live gameplay. Online
race completion, auctions and DLC stunts still need ordinary gameplay checks on
the installed server version. A parked-car inspection cannot prove those end-to-end
unlocks.
