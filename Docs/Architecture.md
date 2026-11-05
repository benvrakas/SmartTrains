# SmartTrains architecture

Satisfactory SML mod, game CL 513885. Prefix `ST`, log category `LogSmartTrains`. The design and its decisions are
in the plan (`C:\Users\benvr\.claude\plans\smarttrains.md`); this file records how the code implements them.

## Pieces

| File | What it does |
|---|---|
| `STTypes.h` | Saved data: platform settings (`FSTPlatformConfig`), station settings, train state and jobs, learned dock maps, trip and stay times, the migration undo record. |
| `STSubsystem.h`, `STSubsystem.cpp` | `ASTSubsystem` (server only, saved with the world): settings API, train monitoring, docking events, learning, rate sampling, readouts. |
| `STDispatch.cpp` | The rest of `ASTSubsystem`: supply/demand nodes, the fit model, dispatching, the flow plan, writing timetables. |
| `STTripPlanner.*` | Trip planning: car-by-car simulation, regret insertion, the exact train assignment. |
| `STMinCostFlow.*` | Minimum-cost flow for the flow plan. |
| `STThroughput.*` | The throughput monitor behind `/st monitor`. |
| `STLayout.*` | Station platform chains and train consists, walked the way the game docks. `FSTPlatformAccess` reads protected platform fields (access-transformer friend). |
| `STTravel.*` | Track distances between stations by a directed search over the track network. |
| `STBridges.*` | Freight Car Control and DynamicTimeTable, both reached by reflection only. |
| `STMigration.*`, `STSaveTools.cpp` | The migration tool and the save copy commands. |
| `STChatCommand.*` | `/st` (see `Commands.md`). |
| `STConfig.*` | Mods menu settings. |
| `STViews.h`, `STReadouts.cpp` | What the menus show: view structs, and the subsystem functions that fill them (`MakePlatformView`, `ExplainPlatform`, ...). |
| `STRemoteCallObject.*` | Carries menu edits to the server and readouts back. |
| `UI/*` | The panels added to vanilla menus, and the controls they are built from. |

## How trains are controlled

SmartTrains never drives a train. It writes the managed train's timetable for each job and lets the game (and Auto
Trains, if installed) drive and dock:

- Pickup stop: *fully load*, leave after **Pickup: longest wait** at the latest (`IsDurationAndRule = false`), load
  filter = the job's item, unload filter = `[None]`.
- Delivery stop: *fully unload*, leave after **Delivery: longest wait** at the latest, unload filter = the item,
  load filter = `[None]`.
- Drive-only stop (depot, moving out of the way): *load/unload once*, no wait, both filters `[None]`.

`[None]` is a filter list holding only a null class. `IsLoadUnloadBlockedByNoneFilter` treats it as "transfer
nothing" and such a platform doesn't hold the train. An empty list would mean "everything".

How a stop ends was read from `AFGBuildableTrainPlatformCargo::CanCompleteDocking` (CL 513885):

- *Fully load/unload*, duration *or* rule: the platform lets the train go when the car is full (loading) or empty
  (unloading), when the transfer is blocked by the none filter, or when the stop's duration has passed.
- *Fully*, duration *and* rule: both conditions must hold.
- *Load/unload once*: only the duration is checked, after the one transfer.

Field offsets used for this (platform): `+0x8d8` `mHasFullyLoadUnloadRule`, `+0x8dc` `mDockForDuration`, `+0x8e0`
`mMustDockForDuration`, `+0x8e4` `mCurrentDockForDuration`. Bits at `+0x838`: `0x10` freight full, `0x20` freight
empty. `+0x839` bit 0: blocked by none filter.

The timetable is written with `SetStops` and `SetCurrentStop(0)`, or through Freight Car Control's
`ExternalSetStops` when it is loaded. While a train is docked the station keeps its own copy of the docked stop's
rules (`mCurrentDockedWithRuleSet`), so rewriting the timetable during a dock is safe. The next job can be written
before the train leaves, and it departs straight for it.

Parking: an idle managed train gets self-driving switched off where it stands, right after it leaves its last stop
(the check runs every 0.25 s). It is switched back on with its next job.

## Fit model

The game docks as follows (`AFGBuildableRailroadStation::DockVehiclesToPlatforms`, disassembled):

1. It collects the vehicles behind the docking locomotive, starting with the one on its back coupler and continuing
   away from it. For each next vehicle it takes the back coupler when that vehicle faces the same way as the
   locomotive, else the front (`mIsOrientationReversed` at `+0x668`, couplers front `+0x658`, back `+0x660`). The
   locomotive itself is not in the list.
2. It walks the platforms from the station's output connection (`mConnectedTo` at `+0x270`, owner at `+0xa8`,
   `GetConnectionInOppositeDirection`) while there are fewer platforms than vehicles.
3. Platform *i* gets `NotifyTrainDocked(vehicle i)`.

The docking locomotive is the train's MU master (`AFGTrain::Dock` passes `+0x490` to `StartDocking`).

`STLayout::GetVehiclesBehind` and `GetPlatforms` repeat this, so the predicted dock map is exact for a train whose
master is known. A push-pull train may arrive either way round. Until it has been seen at a station, a job only uses
cars that fit both ways. Every real dock is read back from the platforms' `GetDockedActor` and stored per (train,
station). A stored map is used while the train still has those vehicles. A dock that differs from the expectation
logs `DOCK MAP MISMATCH`.

A car **fits** a job (item X, pickup S, delivery D) when, at S, it stops beside a Supply platform with X, and at D,
beside a Demand platform that accepts X. It must be empty or hold only X (it tops up), and the cargo type (solid or
fluid) must match. A car
that would **stray** sits at S beside a load-mode platform that has or supplies X but doesn't fit at D:

- with Freight Car Control the job sets it to Skip at S;
- without it the job is refused (decision 3 in the plan).

A loaded train only gets delivery jobs for what it carries. Cars carrying X that aren't beside a Demand platform for
X at D are handled the same way: skipped with Freight Car Control, otherwise the job is refused if they would unload
into another platform.

## Dispatching

Every **Dispatch interval**, as soon as a train docks for its last stop or leaves a stop, and after platform
settings change (`DispatchRound`):

1. **Nodes:** one per (station, item, role), summing its platforms. Each node holds stock, capacity, rate (measured
   or manual), whether it is starved, its priority, target and send-at. `Reserved` holds amounts promised to
   running trips (per-car work of the stops still ahead).
2. **Re-plans:** trips under way that left a stop since the last round, or all of them after a settings change,
   are planned again from where the train is and what its cars hold (`ReplanTrip`, below).
3. **Free trains:** managed trains with no trip; trains driving to a depot or out of the way; trains docked for
   their last stop. A train is free from its station at the time its stay should end.
4. **Moves** per free train (`FSTTripPlanner::BeginTrain`): supplier → consumer pairs of an item where at least one
   car stops beside a supply platform for it at one end and a demand platform for it at the other, whichever way
   the train docks; and deliveries of cargo already on board. A consumer must be below target at the rough arrival
   time, unless the supplier is nearly full. Any-item sinks only take from nearly full suppliers, or unwanted cargo
   (below). Each move gets a solo score: the score of a trip made of that move alone.
5. **Exact assignment** of trains to first moves (`STAssignment::Solve`, the Hungarian method). Rows are free
   trains, columns the best six moves of each train, two slots per move, values the solo scores. This stops one
   train taking a move another could do almost as well while a move only it can do waits.
6. **Trips**, best assigned train first: the assigned move, then **regret insertion** (`Improve`), then `Trim`.
   Each trip's amounts and station stays are reserved before the next train is planned.
7. Parked trains move to a depot, or else to the nearest station no trip uses, when:
   - a trip needs their station;
   - another train's timetable, managed or not, is heading there;
   - a self-driving train has stood within 150 m of them for 45 s (`BLOCKING`, at most every 5 minutes per
     parked train). This covers through-stations and parking spots on the line.

### Trip planning (`STTripPlanner.*`)

A trip is a list of stops (at most **Most stops per trip**, each station once, and with more than two stops no longer
than **Longest trip**: the limit only stops SmartTrains combining stops, a direct trip is never too long),
each with the items loaded and unloaded there. These become the stop's load and unload filters.

**Simulation** (`Simulate`): the stops' times come from travel estimates and stays, after the bookings of other
trains. Then each car, stop by stop, beside the platform it meets there (from the dock maps):

- At a loading platform, a car loads every filter item the platform holds or is set to supply, if it has room.
  Freight cars hold solids or one fluid, never both. Loading from a platform without a supply role is harm.
- At an unloading platform, a car unloads every filter item it carries. The platform must be a demand platform
  that takes all of them (or an any-item sink); otherwise it is harm.
- If the possible dock maps disagree, or the car would do harm, the car skips the stop through Freight Car Control.
  Without FCC the trip is invalid. Maps that put a car beside different platforms agree when those platforms are
  equivalent (same role, mode, form and items), as at a push-pull shuttle's station. The car's amounts then come from
  the smallest of their pools, and each is reduced by what it moves.
- Amounts come from the platform's stock plus its share of the node's production up to the end of the stay (a fluid
  platform fed by pipes keeps filling while the train loads), less what other trips reserved. Unloads are limited by the platform's free slots.
- Cargo loaded on the trip that no later stop unloads is stranded: with FCC that car skips the pickup and the
  simulation runs again; without FCC the trip is invalid.
- **Batch:** per item, what loads must reach the send-at share of the loading cars' room, unless a critical
  consumer is served or a supplier is nearly full.
- **Value** of each delivery: priority weight × urgency × the amount up to the consumer's room. Urgency is 1, plus up
  to 2 for the share below target, plus 2 if starved, plus up to 2 for the time since the last delivery there (an
  hour gives the full 2; fairness). Over target, it is worth 0.2 × weight for unwanted cargo or a nearly full
  supplier; sinks are worth 0.1.
- **Score** = value ÷ time from now to the end of the trip (at least 30 s).

**Regret insertion** (`Improve`, after Ropke and Pisinger's insertion heuristics for pickup and delivery):

- Every move not yet used is tried at every place. Its pickup goes at an existing stop of that station or as a new
  stop anywhere, and its drop at an existing stop after it or a new stop after it.
- Each round inserts the move with the largest regret: its best score minus the larger of its second best
  placement and the current score. A move with only one workable place goes in before it is lost.
- Only insertions that raise the trip's score count, so a trip grows only while its value per second rises. Planning
  stops when nothing improves, or when the round's time budget (25 ms) is used up; the rest wait for the next round.

**Turning a trip into a job** (`MakeLegs`): each stop gets per-car work (`FSTCarWork`: car, item, load or unload,
planned amount). With FCC, every car without work at a stop skips it. A stop is "load/unload once" when it both
loads and unloads, or when a later stop moves the same item (a first pickup or a first drop of a split load).
Otherwise it waits for full or empty cars, up to the load or unload wait.

**Re-planning** (`ReplanTrip`): after every stop (`bReplan`) and after a platform setting changes (`bReplanAll`).
The rest of the trip is checked without the trip limit, and additions may make the trip as long as its rest already
is, so a running trip is never dropped for being long.
The rest of the trip is simulated again from the cargo really on board, with its own reservations released first.
Items that would move nothing and empty stops are trimmed, then regret insertion may add moves, such as delivering
leftovers on the way. If the rest no longer works at all, it is planned again from the cargo on board. If nothing
is left to do, the trip ends and the train is settled when the round ends (next trip, depot or park).

**Unwanted cargo:** a free train carrying cargo that no trip delivers is marked (`UnwantedCargoSince`). After
**Unwanted cargo after** minutes, its cargo may go to consumers above target or to sinks. `/st status` warns when
more than half the managed trains carry unwanted cargo.

**Flow plan** (every **Flow plan interval**): for each item, consumers are taken by priority tier, highest first.
Each tier is an exact **minimum-cost flow** (`STMinCostFlow`, successive shortest paths with Bellman-Ford). Source
→ supplier edges carry what is left of each supplier's rate, supplier → consumer edges cost the travel time, and
consumer → sink edges carry the consumer's rate. Starved consumers count at 1.5 × their measured rate, and at least
1 per minute. Later tiers get what is left. The result gives the fleet advice in `/st status`: trains needed = Σ
flow × round trip ÷ average load of the managed trains that fit that route.

## Clock

`Now()` is the world's time plus `mClockBase`. The world's time starts at 0 each session. `mClockAtSave` (saved)
holds `Now()` at the last save, and `PostLoadGame` sets the base so the clock carries on from it. Every saved
timestamp (planned arrivals, departures, unwanted cargo) stays meaningful after a load. A save from before the clock
existed continues from the latest time its trains hold.

## Measuring

Every second, for each item of each configured platform:

- Supply: belts in = stock change + amount the docked car gained.
- Demand: belts out = amount the docked car lost − stock change.

Negative samples (measurement noise) count as 0. The rate is a moving average whose weight starts as 1 ÷ elapsed
time (a plain average during the first window), then 1 ÷ window. A rate counts as known after 2 minutes or the
window, whichever is shorter. A demand platform empty for 30 s is *starved*.

## Travel times

`FSTTravel` searches one source station per 0.5 s. Each search is Dijkstra over track exits: a node is "about to
leave a track through this connection". From connection C, the next nodes are `C->GetConnections()[i]->GetOpposite()`
at the cost of that track's length.

- A station departs through its track's far end. A station is reached when a search enters its track through the
  platform end, which is `GetStationOutputConnection()->GetRailroadConnectionReference()` or the station track's end
  nearest it.
- A second search starts from the platform end, for push-pull trains that back out. Those routes get 30 s added.

Time = distance ÷ **Speed estimate** + 20 s. It is replaced by the learned trip time once a train has driven that
pair: the first sample, then a 0.3-weight moving average. Stays per station are learned the same way.

**Push-pull arrivals.** A train with a locomotive at each end can also enter a station from its far end, station
building first, and dock with its rear locomotive. Each search therefore records arrivals through both ends
(`FRow::ForwardBack`, `ReverseBack`). `EstimateTravelSeconds(..., bPushPull)` uses the far-end arrivals only for such
trains (`IsPushPull`: more than one locomotive that can dock), so a single-locomotive train is never planned onto a
route it can't drive. A shuttle between two dead-end stations is the usual case.

## Freight Car Control

Optional and reached by reflection. The class `FCCRuleSubsystem` is found by name, and
`ExternalSetStops(TimeTable, Stops, SkipStops, SkipVehicles, Owner)` is called through `ProcessEvent`. A non-empty
Owner locks the train's car rules in FCC's menus. An empty one releases them and puts the player's earlier rules
back. If the call fails, the timetable is written directly.

## DynamicTimeTable

Read only, by reflection: `DynamicTimeTableSubsystem::GetStationGroups`. The returned array of group structs is
walked generically: a name-like field, plus an array of members. Each member is either a station object, or a struct
holding a station object or name and a slot count. The migration log line names the struct it read. A managed
train's timetable that DTT (or anyone) changes is noticed within 0.25 s, logged as `EXTERNAL TIMETABLE CHANGE`, and
the job is written back.

## UI

The subsystem exists on the server only, so every menu (the host's too) talks to it through
`USTRemoteCallObject`:

- A panel asks for its readout once a second while it is shown (`Server_Request*`). The server answers with a view
  struct (`Client_Platform`, `Client_Train`, `Client_Station`, `Client_Migration`).
- An edit is sent at once with the next edit number of that player (`NextEdit`). The server applies it, logs the
  player's name as the cause, and echoes the number in its views. Views made before the player's latest edit are
  dropped, so a readout already on its way can't undo a click. The platform panel also keeps its own copy of the
  settings until a view includes its edit.
- Refusals come back as `Client_Message` and show under the panel for 8 s.

Panels are C++ `UUserWidget`s (`USTPanel` and subclasses) built from the game's own widgets, as in Freight Car
Control: `Widget_StandardButton` (`USTButton`), `Widget_InputBox` (`USTTextInput`), `BPW_TrainMenuLabel` headings,
and `Widget_Window_ContentBackground_DarkMode` as a background. Texts copy the font of a neighbouring vanilla text.
Each control falls back to a plain UMG one, with a warning, if the game's widget is missing. They are added with SML
widget blueprint hooks at `POST_INITIALIZATION`, and not on dedicated servers:

| Vanilla widget | Panel | SmartTrains panel |
|---|---|---|
| `Widget_Train_DockingStation` (freight platform) | `CanvasPanel_149` at x 448 | `USTPlatformLauncher`, beside the window; opens `USTPlatformWindow` |
| `BPW_Train_DockingStation_Fluid` | `CanvasPanel_0` at x 765 | `USTPlatformLauncher` |
| `Widget_Locomotive_Menu` | `VerticalBox_1` (Timetable column), slot 3: under the stop list | `USTTrainPanel` |
| `Widget_TrainStationNew` | `VerticalBox_2` (under the train and station lists) | `USTStationPanel` |

Panel names and positions come from the starter project's widget assets, dumped with Telescreen's `TSDumpWidgets`
commandlet. It now also lists named-slot content, canvas slot layout and size box overrides
(`Tools/Spikes/SmartTrainsUI` holds the output). The platform panel sits right of the vanilla layout in an
auto-sized canvas slot, so the canvas, and with it the window, grows wider. The panel logs its geometry half a
second after it opens, to check that.

- **Platform window** (`USTPlatformWindow`, the `USTPlatformPanel` content in a window on the viewport). The launcher beside the platform menu shows the role, items and one readout line, and opens it. The window keeps the input mode as the platform menu set it, so the menu behind it works; Close, Escape, or closing the platform menu close it. Its panel reads the platform and the font from the menu (`USTPanel::SetMenu`).
- **Platform panel.** Role buttons (Off / Supply / Demand); per item: priority (click cycles, right click goes back),
  send-at or target in 10 % steps, and a manual rate (m³/min for fluids). The item picker lists the items on the
  platform first, then every item that available recipes make plus raw resources, of the platform's form; "Any item"
  only on demand platforms. Copy / Paste use the player's SmartTrains clipboard (below). "Copy to the station's other
  platforms" gives every platform of its kind in the station these settings once.
- **Clipboard** (`STClipboardHooks.cpp`). Each player has two slots on the server, freight and fluid
  (`USTRemoteCallObject::mFreightClipboard` / `mFluidClipboard`), as the game keeps one clipboard entry per building
  kind. The panel's Copy / Paste and the game's copy/paste-settings both use them: SML hooks after
  `AFGPlayerState::CopyFactoryClipboard` / `PasteFactoryClipboard` send `Server_CopyPlatformSettings` /
  `Server_PastePlatformSettings` when the object is a cargo platform and the player is local. Copying a platform
  without a role copies "Off", so pasting it removes the target's role, as the game's paste makes the target like
  the source. Through the game's key the load/unload setting is then left as the game pasted it.
- **Train panel.** In the Timetable column, inserted at slot 3 (`ParentSlotIndex`), under the stop list and above the
  cancel docking, edit and self-driving buttons. It has the managed switch, one status line and the job's per-stop
  lines, in a scroll box at most 230 px tall. (0.3.0 had it in the info column, where it covered the power and speed
  stats and ran off the bottom.) "Keep vanilla" is only in chat and the migration tool. While the train is managed,
  the vanilla "Edit timetable" button is disabled on every refresh, and enabled again when the menu closes.
- **Station panel.** Depot switch, layout, fleet status. The depot is per station, and a depot may have no platforms,
  so it stays in the station menu; everything per platform is in the platform menus.
- **Migration window** (`USTMigrationWindow`). Not part of any vanilla menu: `/st migrate` without a step sends
  `Client_OpenMigrationWindow` to the player, which adds the window to the viewport 0.2 s later (after the chat has
  closed and given the input back) and takes the input in UI-only mode. Close or Escape removes it and gives the
  input back to the game. A dimmed full-screen layer behind it catches clicks, so it keeps the keyboard focus. It
  runs the same steps as `/st migrate`. Its rows are the preview's stations and trains, ticked as proposed. Apply
  sends the ticked numbers. Without a test copy or backup this session, Apply takes a second click (the chat
  command's `confirm`).

## Without Freight Car Control

Every platform keeps its own role and items with or without Freight Car Control. The only difference is that without
it no car can skip a stop: each car loads or unloads at the platform it stops beside, within the stop's filters. The
planner's simulation already follows that rule. A trip where some car would load an item nobody planned, unload where
it shouldn't, or work differently depending on how the train docks is invalid without FCC. With FCC that car skips the
stop. 0.4.0 betas also had "Same for every platform in this station" (station linking); it was removed. The saved
field is skipped when an older save loads. `/st platform copy` and the platform window's copy button still give the
station's other platforms one platform's settings, once.

## Idle reasons

A free train that gets no trip has its reasons recorded per round (`FSTTripPlanner::DescribeProblems`): every
rejected candidate trip adds its reason, and the three most frequent are kept with their counts. The `IDLE` log line,
the train panel and the monitor's idle list show them.

## Throughput monitor

`FSTThroughput` samples every 10 s and judges the last 30 minutes:

- Fleet: managed trains driving a trip, at a stop of a trip, going to park, or idle. Busy = driving or at a stop.
- Nodes (station, item, role, from `BuildNodes`): below target, starved, full (≥ 90 %), nearly empty (≤ 10 %).
- Stations with roles: whether any train is docked. Arrivals of trip legs are recorded with their lateness against
  the plan (`RecordArrival` from `OnDocked`).

Verdict per item, first match:

1. no demand platforms: "No demand"; no supply platforms: "No supply";
2. less than a minute of samples: "Measuring";
3. consumers below target less than 25 % of the time: "OK";
4. no planned route, or no managed train fits one: "No train fits";
5. suppliers nearly empty half the time, or supply rate under 90 % of demand: "Supply short";
6. suppliers full 30 % of the time (or the flow plan needs more trains than fit) while the fleet is busy 80 % of the
   time: "Trains short";
7. one of its stations congested (a train docked 85 % of the time, or arrivals 120 s late on average): "Congested";
8. otherwise "Blocked": supply and trains are there but trips aren't made, so the idle reasons tell why.

The window has five tabs:
- **Overview:** tiles (managed trains, busy share, idle trains, limited items, congested stations), what limits
  deliveries with each row's advice, the fleet lines and the idle reasons.
- **Items:** a table with icon, limit, supply and demand per minute, trains needed / fitting, and time below target.
  It can be filtered to items that need a look.
- **Stations:** the docked share, arrivals and average lateness of every station with roles.
- **Platforms:** starved and backed-up platforms.
- **Trains:** every managed train with its state, last station and trip or idle reason.

Tab headings count the rows that need a look. The view (`FSTMonitorView`) goes to `USTMonitorWindow` through `Server_RequestMonitor` / `Client_Monitor` once a second
while the window is open. `/st monitor text` and the server console print the same report, and it is logged as
`MONITOR` lines every 5 minutes while trains are managed.

## Pool tags (removed in 0.4.0)

0.1.0 to 0.3.0 had pool tags on stations and trains. Matching by platform items makes them unnecessary. The saved
fields stay so older saves load. On load, each tag is logged and cleared.

## Dock maps

`GetDockMaps` returns the vehicle beside each platform for every way the train may dock:

- Learned maps (`LearnDockMap`, one per way round) are used while the train has the same vehicles in the same order
  (`FSTDockMap::Consist`).
- Ways round not seen yet stay predicted, from the vehicles behind each locomotive that can dock.
- If a learned map matches no prediction, only learned maps are used for that station.
- A car is planned at a stop only if it meets a suitable platform in every map. Otherwise FCC skips it, or the trip
  is invalid without FCC. The game reads a stop's rules when the train docks, so a wrong guess can't be fixed
  during that dock; the mismatch is logged and learned, and the next re-plan works from it.
