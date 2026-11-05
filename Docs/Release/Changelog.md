# Changelog

## 0.4.0 (beta, not released)

- **Locomotive menu:** the SmartTrains section moved to the Timetable column, under the stop list, and is smaller:
  the managed switch, a status line and the job. It no longer covers the power and speed stats or runs off the
  bottom. "Keep vanilla" is set with `/st train keepvanilla` only.
- **Pool tags removed.** Trains go wherever platform items match. Tags in older saves are logged and dropped.
- **Platform-to-platform trips.** One trip can now serve several platforms and items: each car has its own plan
  (load this here, unload it there, skip the rest). With Freight Car Control every platform can have its own items;
  without it a station works as one big platform. Trips have up to 4 stops and 45 minutes (Mods menu). They pick up
  where they deliver when that fits, and deliver leftovers on the way.
- **Re-planned after every stop** from what the cars really hold, so short pickups, full consumers and hand-moved
  cargo are handled at the next stop.
- **Smarter choices:** trips are built by regret insertion, trains get their first job by an exact assignment, and
  the flow plan behind the fleet advice is an exact minimum-cost flow.
- **Fairness:** a consumer gets more urgent the longer it goes without a delivery.
- **Unwanted cargo:** after 30 minutes (Mods menu) it may go to any consumer with room or a sink. `/st status`
  warns when many trains carry some.
- **Dock maps** are learned per way round and dropped when the train's cars change.
- Milk runs (0.3.0) are part of the new planner; their Mods menu switch is gone.
- **Mods menu:** "Detailed log" (planner rounds, re-plans, traces and dock maps in the log), "Planning time per
  round" (25 ms) and "Re-plan threshold" (1.1 x, replaces the unused "Retarget threshold"). All settings are logged
  at start and whenever one changes.
- **Migration:** the connection trace is now the source of truth for platform items (stock and timetables are the
  backup). It follows smart splitter rules, Modular Load Balancer groups and filters, and lifts through floor holes,
  with a much larger limit (5,000, settable in the window). The window lists stations and trains as rows with one
  line per platform, its confidence and notes, and tick all / untick all per section.
- **Migration gives each platform one item** by default (the one it holds most of, else the one its timetables
  send, else the largest traced flow), so machines with several inputs aren't flooded. "Items per platform" in the
  window keeps every traced item for mixed belts.
- **Platform menus:** the settings open in their own window ("Open SmartTrains settings" in the small SmartTrains
  section beside the platform menu), instead of a tall panel beside it.
- Trains managed out on the track with nothing to do drive to the nearest station before parking, instead of
  stopping on the line. Re-plans no longer drop the rest of a trip for being over the trip limit.
- **Push-pull trains** can be sent to stations they can only enter from the far end (dead-end shuttles), which the
  route search used to count as unreachable.
- The trip limit no longer blocks direct trips (one pickup, one delivery) and never drops the rest of a running trip;
  it only limits combining stops.
- Pickups count what the platform makes while the train is docked (fluids fed by pipes).
- **Migration without DynamicTimeTable** ticks every train that can dock (unless set to keep vanilla), so the whole
  fleet joins the pool; untick any to keep.
- **Train names** (setting): SmartTrains' name only, `ST: <trip>` or `ST: Idle`; a name a player gives a managed
  train is the one put back later.
- Trips are re-planned when a station or a platform with settings is dismantled. Unwanted cargo means an item on
  board that no stop ahead unloads.
- DynamicTimeTable is found again since its rewrite (`DTTSubsystem`), and its rewrites of managed trains are named in
  the log.
- **Why no trip:** a free managed train logs, and shows in its menu and `/st jobs`, why it got nothing.
- **"Same for every platform in this station"** in the platform menu (and `/st station same`). Editing one platform
  then sets every platform of that kind in the station. By default it is on without Freight Car Control (where a
  station works as one big platform) as long as the platforms match, and off with it. The "To station" button is gone.

## 0.3.0 (beta, not released)

- **Milk runs** (Mods menu, on by default): a train may pick up at two suppliers when neither has a full load, or
  split a load between two consumers when neither has room for all of it. The first stop takes or leaves what it
  can and moves on.

## 0.2.0 (beta, not released)

- **Menus.** SmartTrains settings are now in the game's menus as well as in chat:
  - Freight and fluid platform menus get a panel beside the window: role, items (with an item picker), send-at batch
    or stock target, priority and manual rate. It also has Copy / Paste, "To station", and a live readout of stock,
    rate and inbound trains. When nothing is coming, it says why.
  - The locomotive menu has the managed toggle, keep vanilla, the pool tag and the current job. A managed train's
    timetable shows read-only.
  - The station menu has the depot toggle, the pool tag, the station's platform roles and the fleet status.
  - `/st migrate` opens the migration window: preview with tickable stations and trains, apply, undo, test copy and
    backup.
- The game's copy/paste-settings on a platform also carries its SmartTrains settings, and shares one clipboard with
  the panel's Copy / Paste.
- In multiplayer, every player's edits go to the host and are logged with their name.
- `/st platform` also explains why no train is coming.

## 0.1.0 (beta, not released)

- **Platform roles.** `/st platform supply|demand <items>` gives a freight or fluid platform a role, with a target,
  send-at batch, priority and optional manual rate per item. `demand any` makes a sink.
- **Managed trains.** `/st train manage` hands a train to SmartTrains. Its old timetable comes back with `unmanage`.
- **Dispatching** by measured stock and rates, travel time, priority and a flow plan, with fleet advice in `/st status`.
- **Fit model**: the cars that stop beside the right platforms at both ends, worked out the way the game docks.
  Docks are learned per train and station.
- **Freight Car Control** (1.1.0 or later, optional): cars that don't fit a job skip it.
- **Depots and parking**: idle trains park at the nearest depot, or where they stand. A parked train is moved when
  a job needs its station.
- **Migration tool**: `/st migrate preview|apply|undo`, reading platform modes, belts, inventories, timetable
  filters and DynamicTimeTable groups. Dedicated trains are kept vanilla.
- **Beta tools**: `/st testcopy` (save as a separate session) and `/st backup`.
- **Observe only** mode in the Mods menu.
