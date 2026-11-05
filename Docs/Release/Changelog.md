# Changelog

## 0.4.0 (beta, not released)

- **Migration window redesigned**, in the monitor's style:
  - **Steps:** Preview, Apply ticked and Undo stay on top, with the trace limit and items-per-platform options.
  - **Overview tab:** counts (stations, ticked, to check, platforms with a role, trains ticked) and a list of what
    to check before applying.
  - **Stations tab:** filters (All, To check, Ticked, Not ticked) and tick or untick everything shown. Each station
    shows a tick, its platform roles, and Ready, Check or Not ticked by default. "+" opens its platforms: role in
    colour, items with icons, confidence in colour, notes and problems.
  - **Trains tab:** the same filters; each train shows whether it will be managed, its route and why.
  - **Last result tab:** what the last step said.

- **Monitor verdicts checked directly:**
  - "No train fits" now comes from a direct check: whether any managed train can reach the consumer that waits most
    from a supplier, and fit at both ends. It names the consumer and the first fit problem. "Unreachable" means no
    train can drive there.
  - It used to come from the flow plan, which plans nothing when full suppliers measure 0/min.
  - "Trains short" also needs few idle trains.
  - Every verdict has a "What to do" line, and the overview shows it instead of "Waits most".
- The empty-loop guard keeps its record per train (the first version didn't stop the Sulfur loop); a skipped move is
  logged at Verbose.

- **Automated tests** for the trip planner, the flow plan and the assignment (6 tests, editor only; see Testing.md).

- **Parked trains hold their station:** a parked train books its station for the minute it takes to move it out, so
  trips prefer free stations. A trip held because its first stop is busy counts as needing that stop, so a train
  parked there is moved out.
- **Auto Trains' times:**
  - A running trip's next stop is booked from Auto Trains' planned arrival.
  - Station pairs not driven yet get Auto Trains' travel time (a few new pairs per round, kept 10 minutes) instead of
    distance divided by average speed.
- **Dock maps by docking locomotive:** a learned dock map now records the locomotive that docked, which tells the ways
  round apart for trains with locomotives in the middle or at both ends. False `DOCK MAP MISMATCH` lines came from
  telling them apart by the first docked vehicle.
- **Less planning work:** a free train's setup from scoring its options is reused when its trip is planned in the same
  round, instead of being worked out twice.

- **Fluid deliveries don't wait out the timeout:** a stop that only unloads fluids is "unload once". A fluid car empties
  into its platform in one transfer (about 27 s). "Fully unload" then held the train up to "Delivery: longest wait"
  (120 s) whenever a platform wasn't quite empty, while its next trip, going to refill, was already chosen. Fluid
  pickups keep their fill wait, since the platform keeps filling from its pipes while the train loads.

- **No more empty loops:** a stop where a train moved none of an item it was planned to move is logged
  (`NOPROGRESS`). That train doesn't plan that item at that station again for 10 minutes. Seen with leftover Sulfur
  "delivered" to a full sink 7 times in 2 minutes.

- **Route check before leaving (Auto Trains):** before a train standing in a station starts a trip, SmartTrains asks
  Auto Trains whether the first leg can be driven now, planned around every other train, including trains coming the
  other way on a single track. If not, the train holds: "holding at ...: Auto Trains sees no way to ... now". The
  check takes one dispatch round.

- **Stuck nose to nose (stopgap):** with Auto Trains, when two managed trains on one network have been stuck or
  deadlocked for 30 s, the one whose trip started last goes back to the station it came from (push-pull trains only,
  at most every 3 minutes). The others plan their trips again from scratch. Logged as `STUCK`. Seen with the two
  water shuttles on one shared track: each headed for the station the other had just left.

- **Works with Auto Trains' API** (optional, by reflection):
  - Auto Trains is told after every timetable rewrite.
  - A parked train moves when Auto Trains reports a train waiting 20 s for it, replacing the distance guess.
  - Station queues, trains SmartTrains doesn't manage included, count as station bookings.
  - Where SmartTrains' own route search finds no way, Auto Trains' travel time is used.
  - Deadlocked, stuck, lost and stalled trains show in the monitor's Trains tab.

- **Trains are spaced at stations:** trips are timed so the next train arrives at least **Gap between trains at a
  station** (Mods menu, 30 s) after the last one left. Bookings now have a start and an end, so a trip can use a free
  slot between two others. A train standing in a station whose first stop is booked waits there. It leaves later
  instead of queueing on the line, where it would block junctions. Its idle reason says "holding at ...".

- **Fewer timetable rewrites:** a re-plan that leaves the stops, rules and skipped cars as the train already has them
  no longer writes the timetable again. Each write made the game, and Auto Trains, plan the route again, often while
  the train pulled out of a station.

- Monitor Trains tab: trains show the name the game shows (with train names on, the "ST: ..." trip name), not the
  name they had before SmartTrains managed them.

- **Push-pull platform room counted once:** a car that can stop beside either of two identical platforms used to
  take its amount from both, so half a push-pull train's cars found no room and were skipped while still full. Each
  way the train may dock now has its own platform room, and a car plans the smaller of its amounts.
- **No waiting out the timeout:** a stop where a car can't unload everything it carries (the platform has no room
  for the rest) is "unload once". It used to wait for empty cars until "Delivery: longest wait".

- **Fluid trips were tiny:** the planner sized fluid cars and platforms by a fluid's stack (50 m3) instead of their
  real slot (1,600 m3 per car, 2,400 m3 per platform). Fluid trips were planned at a few dozen m3, and cars and
  platforms looked full after one stack. Solids were not affected.
- **Parked trains move out of the way:** a parked managed train moves to a depot or a free station in three cases:
  a trip needs its station; another train's timetable (managed or not) is heading there; or a self-driving train has
  stood within 150 m of it for 45 s. The last case happens at most every 5 minutes per train, and is logged as
  `BLOCKING`.
- **Monitor:** full supply platforms stop their belts, so their measured rate is only a lower bound. Items whose
  suppliers are full are no longer called "Supply short". Platform counts say how many stations they span.
- Diagnostics: "can't be reached" names the station it was tried from and whether the train counts as push-pull.
  With Detailed log on, the route search names the stations it reaches on small networks.

- **Item icons** beside the platform menu: the SmartTrains section lists the platform's items with their in-game icons.
- **Clock carries on across saves.** SmartTrains' times used to restart at 0 on every load, so after loading, ETAs
  and "against plan" were off by the length of the last session, the unwanted-cargo timer stalled, and learned trip
  times could be spoiled. The clock is now saved. An older save continues from the latest time its trains hold.
- **Push-pull shuttles run:** a train that can dock either way round no longer has every car skipped at stations whose
  platforms are identical, the reason the water shuttles never moved.
- **Platform readouts** count a car holding the same item as fitting (it tops up), not only empty cars.
- **Idle reasons:** a train with no trip lists its three most frequent rejection reasons with counts, not just the
  last.
- **`/st monitor`:** a throughput monitor window with five tabs: Overview, Items, Stations, Platforms and Trains. Per item it shows whether supply, trains, fit or a congested
  station limits deliveries over the last half hour. It also lists starved and backed-up platforms, and why idle
  trains wait. Logged as `MONITOR` lines every 5 minutes.
- **Station linking removed:** platforms always keep their own settings, with or without Freight Car Control. The
  platform window's **Copy to the station's other platforms** button copies settings once. Without Freight Car
  Control the only difference is that no car can skip a stop, which the planner checks.
- **Migration route inference:** platforms the trace, stock and timetables can't settle get their items from the
  routes of trains that stop there.
- Idle trains are named "ST: parked at <station>" (with train names on).
- **Locomotive menu:** the SmartTrains section moved to the Timetable column, under the stop list, and is smaller:
  the managed switch, a status line and the job. It no longer covers the power and speed stats or runs off the
  bottom. "Keep vanilla" is set with `/st train keepvanilla` only.
- **Pool tags removed.** Trains go wherever platform items match. Tags in older saves are logged and dropped.
- **Platform-to-platform trips.** One trip can now serve several platforms and items: each car has its own plan
  (load this here, unload it there, skip the rest). Every platform has its own items; without Freight Car Control a
  trip only works when every car fits the platform it stops beside. Trips have up to 4 stops and 45 minutes (Mods menu). They pick up
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
