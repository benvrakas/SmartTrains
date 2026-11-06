# Migration tool

Converts an existing train network (vanilla timetables, DynamicTimeTable groups) to SmartTrains roles and managed
trains, with a preview, a CSV report and undo. `/st migrate` opens it as a window; the chat steps below do the same
without it.

## Evidence per cargo platform

| Source | Used for |
|---|---|
| Load/unload setting | Load → Supply, Unload → Demand. |
| Connected belts and pipes (`FSTPlatformAccess::CountConnections`) | Nothing connected → left Off. Load with no input, or unload with no output → conflict. |
| Connection trace (`STTrace`) | **Items: the source of truth.** From the machines behind the belts and pipes, even when the factory is empty or idle. |
| Platform inventory | Items, only when the trace finds nothing (backup). |
| Load filters (load-mode platforms) and unload filters (unload-mode platforms) of every timetable stop at the station | Items, only when the trace finds nothing (backup), if they fit the platform's cargo type. |
| Fluid in the platform's pipes | Last resort for fluid platforms. |

### Connection trace

A load platform is traced upstream to what produces, an unload platform downstream to what consumes. The walk goes
through belts, lifts, splitters, mergers, pipes, junctions and pumps and stops at the first machine with a recipe
(manufacturer, miner, pump). It reads the configured recipe and the clock speed, so the rate is a starting target.

- **Smart and programmable splitters:** their sort rules are followed per output (`FSTPlatformAccess::
  GetAttachmentOutputIndex` reads `mOutputs` through the access transformer). An output only passes its items; "Any
  undefined" passes everything the other outputs don't name; "Any" passes everything; "Overflow" passes everything
  and marks the path shared, since its share isn't known in advance.
- **Modular Load Balancers** (optional mod, read by reflection: `LBBuild_ModularLoadBalancer`, `GroupLeader`,
  `mGroupModules`, `mFilteredItems`): a balancer group moves items between its modules without belts, so the whole
  group is one building. Upstream through a filter module's output only its items pass; through any other output,
  none of the group's filtered items.
- **Lifts and floor holes:** a lift whose far end isn't connected continues on the other side of the floor hole it
  is snapped to (`GetSnappedPassthroughs`).
- **Trace limit:** buildings and belt segments one platform's trace may visit, default 5,000 (100 to 100,000). Set
  it in the migration window or with `/st migrate preview tracelimit <n>`. Reaching it marks the result incomplete.
- **One item per platform** (default): a machine with several inputs makes the trace find several items, and giving
  a platform all of them would let one flood the line while another runs short. Of several items the platform gets
  the one it holds most of now, else one its timetables send (DynamicTimeTable or vanilla), else the largest traced
  flow (marked medium, "check it"). The others are listed in the note. The window's "Items per platform" switch
  (or `allitems` in chat) keeps every traced item, for mixed (sushi) belts.
- **The trace decides the items** whenever it finds any. The platform's inventory and timetable filters are only
  used when the trace finds nothing; items they name that aren't on the belts are listed as ignored.
- Confidence: **high** for a clean trace, or one the platform's stock or timetables confirm; **medium** through
  merges, splits, overflow outputs, unrecognised buildings or the limit, and for backup items; **unclear** when
  nothing is found at all.
- The preview chat lines mark non-high platforms, e.g. `P2=Supply[Wire](medium)`. The CSV has the columns
  Confidence and Connection trace, with the path, e.g. `Belt x3 > SplitterSmart > ConstructorMk1`.

Not yet used:

- what is on the attached belts (belt contents);
- rates measured beforehand. Rates are only measured for platforms that have a role, so after migrating, observe
  only mode gives them before trains are handed over.

## Which trains and stations migrate

- **Route inference** (last fallback): a platform still without items gets them from the routes of the trains that
  stop at its station. A demand platform gets what their other stations supply. A supply platform gets what they
  take. It repeats while it fills platforms, so items carry along chains of routes. Marked medium confidence, "inferred
  from <train>'s route".
- A train is **pull-based** when at least one of its stops is a member of a DynamicTimeTable group. Only those are
  ticked.
- Other trains are **dedicated** (e.g. a raw quartz shuttle between two fixed stations). Apply marks them *keep
  vanilla* so later runs and the manage toggle leave them alone.
- A station used only by dedicated trains is flagged and gets no roles. Otherwise managed trains could take its
  items away from its shuttle.
- A station used by both kinds is flagged; apply it by number to give it roles anyway.
- A station no train uses is flagged.
- Without DynamicTimeTable every train that can dock (and isn't set to keep vanilla) is ticked: it joins the managed
  pool, and a train that used to shuttle on one route takes other trips when that route is stocked. Untick a train
  to keep it on its own timetable. No station is flagged for that reason.

## Changing a platform by hand

In the migration window's Stations tab, click a station's name or `[+]` to open its platforms. Then:
- click a platform's role to cycle it: supply, demand, off;
- click `[items]` to pick its items: a search field and every item that fits the platform, its own items ticked
  first. Clicking an item adds it, or removes a ticked one. A demand platform can also be a sink ("Any item").
  `[done]` closes the picker.

Each change goes to the server at once (`Server_MigratePlatform` -> `STMigration::SetPlatform`) and ticks the
station. Applying the station gives the platform that setting. The platform shows confidence "by hand".

Settings by hand are kept for the session and used again by later previews, until their station is applied.

Pieces that can't move freight or fluid show as "Empty platform" and can't be set. These are Modular Stations' empty
platforms, which are built on the cargo platform class. `STLayout::CanMoveCargo` is false for them: they have no
inventory, or no belt or pipe connectors. Whether anything is attached doesn't count, so a station not yet connected
to the factory can still be migrated. The same check refuses a role from the platform menu, a paste, `/st platform`
and the copy to the station's other platforms. A station with only such pieces isn't listed.
Changing the preview counts as an ordinary edit: with "Only admins change SmartTrains" on, only admins can.

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
