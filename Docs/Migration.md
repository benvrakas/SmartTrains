# Migration tool

Converts an existing train network (vanilla timetables, DynamicTimeTable groups) to SmartTrains roles and managed
trains, with a preview, a CSV report and undo. `/st migrate` opens it as a window; the chat steps below do the same
without it.

## Evidence per cargo platform

| Source | Used for |
|---|---|
| Load/unload setting | Load → Supply, Unload → Demand. |
| Connected belts and pipes (`FSTPlatformAccess::CountConnections`) | Nothing connected → left Off. Load with no input, or unload with no output → conflict. |
| Platform inventory | Items. |
| Load filters (load-mode platforms) and unload filters (unload-mode platforms) of every timetable stop at the station | Items, if they fit the platform's cargo type. |

| Connection trace (`STTrace`) | Items, from the machines behind the belts and pipes, even when the factory is empty or idle. |

### Connection trace

A load platform is traced upstream to what produces, an unload platform downstream to what consumes. The walk goes
through belts, lifts, splitters, mergers, pipes, junctions and pumps and stops at the first machine with a recipe
(manufacturer, miner, pump). It reads the configured recipe and the clock speed, so the rate is a starting target.

- Smart splitters are walked through but their filters are not mapped to outputs, so items reached through one are
  marked shared (medium at best).
- Limits: 40 hops and 80 nodes; hitting one marks the result incomplete (medium).
- Confidence: **high** when the platform's inventory or timetable filters agree with the trace, or a clean single
  path ends at machines with a recipe; **medium** through splits, merges, shared outputs or a limit; **unclear**
  when no machine is reached, a recipe is missing, or the pipes show several fluids.
- A disagreement between the trace and the platform's own history is a conflict, and the station is not applied by
  `all`.
- The preview chat lines mark non-high platforms, e.g. `P2=Supply[Wire](medium)`. The CSV has the columns
  Confidence and Connection trace, with the path, e.g. `Belt x3 > SplitterSmart > ConstructorMk1`.

Not yet used:

- what is on the attached belts (belt contents);
- rates measured beforehand. Rates are only measured for platforms that have a role, so after migrating, observe
  only mode gives them before trains are handed over.

## Which trains and stations migrate

- A train is **pull-based** when at least one of its stops is a member of a DynamicTimeTable group. Only those are
  ticked.
- Other trains are **dedicated** (e.g. a raw quartz shuttle between two fixed stations). Apply marks them *keep
  vanilla* so later runs and the manage toggle leave them alone.
- A station used only by dedicated trains is flagged and gets no roles. Otherwise managed trains could take its
  items away from its shuttle.
- A station used by both kinds is flagged; apply it by number to give it roles anyway.
- A station no train uses is flagged.
- Without DynamicTimeTable no train is ticked and no station is flagged for that reason. Pick trains with `/st
  migrate apply trains <numbers>`.

## DynamicTimeTable

Read through `DynamicTimeTableSubsystem::GetStationGroups` by reflection (see Architecture.md). Its data is never
changed. Groups whose members were all migrated are listed after apply, for the player to delete in DynamicTimeTable.

## Undo

Apply stores each touched platform's previous settings and load/unload mode, and the trains it managed. Undo puts the
platforms back and unmanages those trains, which restores their saved timetables and autopilot. Keep-vanilla marks
stay.

## Testing flow

1. `/st testcopy`, then load the copy from the main menu.
2. `/st migrate preview`, and read the chat lines and the CSV in `%LOCALAPPDATA%\FactoryGame\Saved\Logs`.
3. Turn on Observe only in the Mods menu, then `/st migrate apply all`. Trains keep running their old timetables
   while the log shows what SmartTrains would do (`WOULD JOB`).
4. Turn Observe only off to let it drive.
