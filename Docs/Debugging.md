# Debugging SmartTrains

**Detailed log** (Mods menu) shows the Verbose lines below without any console command; turn it on before
reproducing a problem. For even more (every move and fit check), use the console: `log LogSmartTrains VeryVerbose`.

The log is `%LOCALAPPDATA%\FactoryGame\Saved\Logs\FactoryGame.log`; every SmartTrains line starts with
`LogSmartTrains:`. More detail: add `LogSmartTrains Verbose` (or `VeryVerbose`, which includes every rejected fit) under
`[Core.Log]` in `%LOCALAPPDATA%\FactoryGame\Saved\Config\Windows\Engine.ini`, or type `log LogSmartTrains Verbose` in
the console.

## Lines to look for

| Line | Meaning |
|---|---|
| `Subsystem ready` | Loaded; counts and whether Freight Car Control / DynamicTimeTable were found. |
| `PLATFORM ... -> ...` | A platform's role or items changed, and by whom. |
| `MANAGE` / `UNMANAGE` | A train was taken over or given back (saved stop count, autopilot). |
| `JOB n <train>: ...` | A job was handed out, with its score parts. |
| `WRITE <train> job n: [...]` | The timetable written, and whether through Freight Car Control. |
| `JOB n ...: docked at ... for leg i/n, +x s against plan` | Arrival, and how late against the plan. |
| `JOB n ...: left ... after x s, loaded/unloaded ...` | What was actually moved. |
| `JOB n ... DONE` | Delivered amount against plan, total time. |
| `TRIP A -> B took x s (estimate was y s ...)` | Travel time learning; big differences mean the speed estimate is off. |
| `PARK` | A train's autopilot was switched off where it stands. |
| `WOULD JOB` | Observe-only mode: the job it would have given. |
| `REPLAN <train> job n: [before] -> [after]` | The rest of a trip after a stop: trimmed, extended, or planned again. Stops are `Station (+loaded -unloaded)`. |
| `... carries cargo no trip wants now` | A free train's cargo has no delivery; see "Unwanted cargo" in Architecture.md. |
| `IDLE <train>: no trip: ...` | Why a free managed train got nothing, logged when the reason changes. Also in its locomotive menu ("Why no trip") and `/st jobs`. |
| `DISPATCH: n free trains ... x ms` (Verbose) | One line per dispatch round: free trains, assignments, trips, planning time. |
| `PLANNER <train>: n moves` (Verbose) | How many moves a free train had to choose from. |
| `Dispatch: planning time (n ms) ran out` (Warning, at most every 5 min) | Free trains were left for later rounds; raise "Planning time per round". |
| `Settings: ...` / `Settings changed: Key=value (was ...)` | All Mods menu settings at start, then every change. |
| `TRACE <platform> upstream/downstream: ...` (Verbose) | The migration tool's belt and pipe trace of a platform. |
| `EXTERNAL TIMETABLE CHANGE` | Someone else changed a managed train's timetable; the job was written back. |
| `DOCK MAP MISMATCH` | A train docked with different cars beside the platforms than expected. The new map is kept. |
| `NO FIT` (VeryVerbose) | Why a train can't do a supplier → consumer pair. |
| `MIGRATION` | Preview, apply and undo summaries; the CSV path. |
| `Test copy` / `Backup` | Save copies and the session name being put back. |

## Menus

- `UI hook ... added` at start-up, and `4 of 4 UI hooks registered`. A missing vanilla widget or panel name is logged
  as an error: the game's UI changed.
- `USTPlatformPanel shown in ...` / `Platform panel geometry: ...` when a platform menu opens. If the panel is drawn
  outside the window or cut off, the geometry line shows where it landed.
- `Platform menu: role on ... (edit N)`, `Locomotive menu: ...`, `Station menu: ...`, `Opening the migration window` / `Migration window opened|closed`, `Migration window: step ...`
  on the client. The server logs the change itself (`PLATFORM`, `MANAGE`, `STATION`, ...) with the player's name.
- `Remote call object not registered on the local player`: SmartTrains isn't on the server. Its menus stay empty.
- `Clipboard hooks installed` at start-up. `Game clipboard: copy from ...` / `paste onto ...` on the player's side
  when the game's copy/paste-settings is used on a platform, and `CLIPBOARD <player> copied|pasted ...` on the
  server. No `Game clipboard` line after the game's copy key on a platform means the game doesn't route that key
  through `AFGPlayerState::CopyFactoryClipboard` for platforms; the panel's Copy / Paste still work.
- **SML slot setup:** `UWidgetBlueprintHookSlot_Generic::SetupPanelSlot` dereferences a null `HorizontalBoxSlot` when
  the parent is a vertical box (SML 3.12). So hooks into vertical boxes pass no slot setup, and the panels pad
  themselves (`USTPanel::GetOuterPadding`).

## Known gaps (0.4.0)

- Sushi supply platforms (one platform with several items) give a car every filter item they hold; the planner
  allows for it, but the split between items in that car is only estimated until the re-plan after the stop.
- A station that both supplies and takes the same item isn't supported.
- A train that is parked counts as standing at the station where it last docked.

- `SELFDRIVING <train>: <error> for N s`: a managed train's self-driving error (for example `SDLE_NoPath`,
  `SDLE_StationUnreachable`) once it has lasted 5 s, and `SDLE_NoError` when a logged one clears. A train that stands
  without moving usually shows one. Shorter errors aren't logged: a train re-routing as it leaves a station shows
  `SDLE_NoPath` for 1-4 s.
- `REPEAT <train>: not repeating its last trip: <why>`: a train with no new trip didn't run its last trip again, for
  example none of its stops would move anything, another train runs the same stations, or it would queue. It waits
  where it is and tries again every 30 s; the line is logged when the reason changes.
- `PLAYER <train>: ...`: a player got into or out of a train's driver seat. With autopilot on the train keeps getting
  trips; a player driving by hand (autopilot off) holds it, and it gets no new trip until autopilot is on.
- `NOPROGRESS ... Platforms there: P1 unload, Demand [...], 12 of 48 slots used: ...`: a stop moved none of a planned
  item. The platform list shows why, e.g. a demand platform full of other items, or set to the wrong mode. For a
  train running its last trip again it is only in the detailed log.
- `REQUEST <item> into P2 of <station> (<why it asks>, e.g. "below its target in 12 min at its use, with what is on the
  way"): <train> empties its car on the way and brings it` (or `empties its car first`): a platform asked for a train
  (Request a train) and this one serves it. `... ; no free train can serve it (N free; <train>: <why>)`, `... ; <train>
  can bring it sooner than emptying a train`, `... ; a train is emptied for it in about N min`: why it has no trip
  yet, logged when that changes. `<train>: its request cars still hold other cargo before the pickup; the rest is
  planned again`, `... delivered N by <train>`, `... <train> no longer serves it (<why>)`: the rest of a request.