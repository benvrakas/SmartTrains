# Migration tool

Sets up a whole existing network at once: every station's platforms (role and item, from the machines their belts and
pipes reach), optionally every station's name, and which trains SmartTrains manages, with a preview, a CSV report and
undo. `/st migrate` opens it as a window; the chat steps below do the same without it.

Since 0.15.0 it is the station set-up (Architecture.md, "Station set-up") run over every station, plus the trains.
Until then it had its own evidence pipeline (load/unload switch, stock, timetable filters, route inference,
DynamicTimeTable groups, platforms edited by hand in the window); the user agreed on 2026-11-10 that only the
platforms' roles and items and the trains need migrating, and that the station set-up does the platforms better.

## What the preview proposes

- **Stations:** for every station with cargo platforms, `STSetup::ProposeStation`: the role from what is attached
  (belts or pipes only into the platform: Supply; only out: Demand; both: its load/unload switch; nothing: Off) and
  one item per platform from the connection trace (below), shared out between platforms of one role that reach the
  same machines. No machine reached: what the platform holds, then the fluid in its pipes. A platform left without an
  item flags its station ("platform N: no item found"), and a flagged station isn't ticked by default.
- **Names:** every station's set-up name (product, map area, number, rate), worked out a few stations at a time
  after the preview appears (`StepNaming`). With **Rename stations** on (window) or `rename` (chat), apply renames the
  ticked stations.
- **Trains:** every train with a timetable that SmartTrains doesn't manage yet. Ticked when it has a locomotive that
  can dock and isn't kept vanilla. A train that used to shuttle simply takes other trips when its route is stocked;
  untick any you want to keep on its own timetable.
- **DynamicTimeTable:** not read any more. If it is installed, the preview says to uninstall it before managing
  trains, as both would write the same timetables.

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
- **Sharing out between platforms** (`ShareOutItems`): platforms whose one item wasn't settled by their own stock
  share out the items their traces found.
  - A group is the platforms of one role whose traces reach a machine in common, across stations. Two platforms
    feeding the same cable machines are one group, even at different stations.
  - A platform whose trace reached no machine groups with its station's platforms of that role.
  - Each platform takes the best item no other platform of its group has yet. Platforms settled by their stock keep
    their item and count as taken.
  - A group with more platforms than items gives the rest their best item again.
  - A changed platform is marked medium, "one no other platform for the same machines has yet; check it", and logged
    as `MIGRATION <station> platform <n>: <item> instead of <item>`.
  - Without it, two empty platforms whose belts reach machines using Copper Ingot and Caterium Ingot would both get
    the same item.
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


## Undo

Apply stores each touched platform's previous settings and load/unload mode, the old names of renamed stations, and
the trains it managed. Undo puts the platforms and names back and unmanages those trains, which restores their saved
timetables and autopilot. Keep-vanilla marks stay.

## Testing flow

1. `/st testcopy`, then load the copy from the main menu.
2. `/st migrate preview`, and read the chat lines and the CSV in `%LOCALAPPDATA%\FactoryGame\Saved\Logs`.
3. Turn on Observe only in the Mods menu, then `/st migrate apply all`. Trains keep running their old timetables
   while the log shows what SmartTrains would do (`WOULD JOB`).
4. Turn Observe only off to let it drive.
