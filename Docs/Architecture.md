# SmartTrains architecture

Satisfactory SML mod. Prefix `ST`, log category `LogSmartTrains`. What the mod is meant to be is in the plan
(`C:\Users\benvr\.claude\plans\trains-end-state.md`); this file records how the code implements it. The per-trip
dispatcher of 0.6 to 0.43 (trip planner, flow plan, forecasts, parking, depot signals) was removed in 0.50.0; its
notes are in the git history of this file.

## Pieces

| File | What it does |
|---|---|
| `STTypes.h` | Saved data: platform settings (`FSTPlatformConfig`), train state and jobs, learned dock maps, trip and stay times, the migration undo record. |
| `STLoops.cpp` | The loop plan: factory scans, consumers and suppliers, the split of the trains, each train's job, writing timetables, the monitor view. |
| `STLoopMath.*` | The plan's arithmetic (largest-remainder split, supplier allocation), apart from the game for the tests. |
| `STSubsystem.h`, `STSubsystem.cpp` | `ASTSubsystem` (server only, saved with the world): settings API, train monitoring, docking events, learning, readouts. |
| `STStationSetup.*`, `STTrace.*`, `STMinCostFlow.*` | Station set-up: platform roles and items from what is attached, the factory's product and theoretical rates (belt networks by maximum flow), and station names. |
| `STLayout.*` | Station platform chains and train consists, walked the way the game docks. `FSTPlatformAccess` reads protected platform fields (access-transformer friend). |
| `STTravel.*` | Track distances between stations by a directed search over the track network. |
| `STBridges.*` | Freight Car Control, DynamicTimeTable and Auto Trains, reached by reflection only. |
| `STMigration.*`, `STSaveTools.cpp` | The migration tool and the save copy commands. |
| `STChatCommand.*` | `/st` (see `Commands.md`). |
| `STConfig.*` | Mods menu settings. |
| `STViews.h`, `STReadouts.cpp` | What the menus show: view structs, and the subsystem functions that fill them. |
| `STRemoteCallObject.*` | Carries menu edits to the server and readouts back. |
| `STAccess.*` | Who may change what (admin-gated controls). |
| `STDockHooks.*`, `STClipboardHooks.*` | SML hooks: dock start, and the game's copy/paste of settings. |
| `UI/*` | The panels added to vanilla menus, the platform, migration and monitor windows, and the controls they are built from. |

## Loops

`CheckLoops` runs every 5 s (server):

1. **One factory scan** (`StepRateScan`): the station with Supply or Demand platforms scanned longest ago (or whose
   platform settings changed, `mRatesDirty`), at most once per `RateScanSeconds` (30 min) each, through
   `STSetup::ProposeStation` and `AnalyzeFactory`: `FFactory::InputRates` (what the factory uses at its most, this
   station's share) and `Product` / `Rate` (what it makes at its most). Fluids come in m³ and are kept in litres
   like inventories (`FStationRates`). A `FACTORY` line logs each scan.
2. **The model** (`BuildLoopModel`, at most every 30 s or when something changed): platforms grouped by station.
   - A **supplier** (`FSTSupplier`) per station and Supply item: the scan's rate for its product, else what the
     machines behind its platforms make by their recipes (each machine once), in car loads per minute
     (`GetCarCapacity`); unknown (-1) counts as enough.
   - A **consumer** (`FSTConsumer`) per station with Demand platforms: a slot (`FSTLoopSlot`) per Demand platform in
     platform order, its items (several: an item group), each item's rate (the scan's use, else the machines'
     recipes, shared over the station's slots taking it) and the slot's rate (their sum). The consumer's rate is its
     busiest slot's: a train brings one car per slot per round.
   - **Sources** (`FSTLoopSource`): biggest consumers first, each slot item takes its rate from the suppliers of the
     item on the same rail network, nearest first, within their spare rate (`STLoopMath::Allocate`).
   - **Round trip:** from the consumer to the nearest supplier still to visit, and so on, then back, plus the stays
     (`EstimateTravelSeconds`, `EstimateStaySeconds`). **Need** = rate x round trip.
3. **The split** (`AssignTrains`): the available trains (managed, not held, not driven by hand) and the consumers
   each fits (`FitsConsumer`). The targets are `STLoopMath::SplitByPriority` of the trains that fit some consumer
   over the needs of the consumers some train fits (see Priority below); consumers left with none are unserved
   (`FSTConsumer::Problem`, and `Cause` in a few words for the monitor's row).
4. **Each train's job** (`UpdateTrainJob`): its loop, unless it is switching patterns.

### Priority

Each consumer station has a priority (`FSTStationConfig::Priority` in `mStationConfigs`: Low, Medium by default,
High), set on its station menu's consumer card; it is the station's, since its whole pattern is one loop. A consumer's **full need** is its need rounded up (a need within 0.05 of a whole train
rounds down), at least one and at most the available trains that fit it. The tiers take turns from High down: a tier
whose full needs the trains left cover gets them all; otherwise it splits what is left in proportion to its full
needs (`Split`: one each while there are enough, then largest remainder) and the lower tiers get none. So High takes
trains from Medium and Low loops until it has enough for its factory's theoretical input, and Low only gets trains
once every Medium loop has its full need. Trains left once every full need is met go by need to consumers more
trains fit. With every consumer on Medium this is the plain split by need. Priority is part of the split's inputs,
so changing it splits again.

### Rebalancing

The trains are only split again when the split's inputs change (`mSplitInputs`): the consumers, their rates and
number of sources, and the available trains with what each fits. Round trips are left out, so learned travel times
don't move trains. On a new split, each train keeps its consumer while it fits there and the consumer isn't over its
target; consumers short of their target take free trains that fit them, empty ones first, then the nearest
(`EstimateTravelSeconds` from its last station); trains left over go where they fit furthest below their share. A
train whose consumer changes logs a `LOOP` line; each split logs a `SPLIT` line.

### A train's loop

`MakeLoopJob`: the car beside each slot's platform (`FitsConsumer`, the first dock map), the item it carries (a
group's item, and the supplier where there are several, picked by the train's rank among the consumer's trains so
the loop's trains share them by rate, `PickByRank`), and the supplier it loads at: its source, else any source of the
item where that car stands beside a Supply platform of it. Pickups are ordered nearest first from the consumer; the
consumer is the last stop. Every car that has no work at a stop skips it (Freight Car Control). `FSTTrainState::CarSets`
holds the pattern. `UpdateTrainJob` leaves a loop alone while its stops, cars and items stay the same.

### Switching patterns

A train given another consumer first gets rid of cargo its new loop doesn't carry in that car (`HoldsOtherCargo`):
- **Deliver** (`ESTJobKind::Deliver`, `MakeUnloadJob`): at its old consumer (its loop's `AnchorStation`; without a
  loop, the nearest consumer whose platforms take its cargo), load/unload once, so what doesn't fit stays.
- **Sink:** after the Deliver stop, if it still holds such cargo, the nearest station with a Sink platform beside a car
  holding cargo, unloading once.
- Then its loop (after a Sink stop without another check). With no consumer or Sink platform for its cargo it starts
  its loop with it, with a warning (`/st emptycars` clears it).

### Timetables

A loop is written once: all its stops, which the game's timetable repeats by itself. Every stop is *load/unload once*:
the train moves what is there in one go and goes on, so it never holds a platform waiting for cargo or room. Filters
are set to the stop's items, `IgnoreFullLoadUnloadIfTransferBlockedByFilters` on. With the **Wait for full cars**
setting (`bWaitForFullCars`, off by default) a loop's pickup and delivery stops are *fully load/unload* with *duration
and rule* and a duration of 0 instead (it leaves only with full or empty cars); changing it rewrites every loop.

The game moves a timetable's current stop on when the train docks (`AFGTrain::Dock` calls
`AFGRailroadTimeTable::IncrementCurrentStop` before `StartDocking`; nothing else does). So a job written while the
train is docked at one of its stops starts from the stop after it (`WriteJob`); `MonitorTrains` reads the stop docked
at as the one before the current. While a train is docked the station keeps its own copy of the docked stop's rules
(`mCurrentDockedWithRuleSet`), so rewriting during a dock doesn't change the stay. Writing the same stops again is
skipped (`WrittenStopKeys`). With Freight Car Control the stops go through its `ExternalSetStops` with the skipping
cars; Auto Trains is told (`ExternalTimeTableChanged`).

The stay ends by the game's rules (`AFGBuildableTrainPlatformCargo::CanCompleteDocking`): *fully load/unload* lets
the train go when the car is full (loading) or empty (unloading), or the transfer is blocked by the none filter;
*duration and rule* needs both. `[None]` is a filter list holding only a null class, "transfer nothing"; an empty list
would mean "everything".

- **Docking in place (0.39.0)**: a train standing in a station (not docked) can't dock by driving there.
  The game reports a stopped train that stands in a station as `ETrainDockingState::TDS_ReadyToDock` (with self-driving
  off too: 45 of 81 such cases, the OWNSTATION log line); `AFGTrain::Dock()` then docks it there, as the player's dock
  button does. `WriteJob` calls `Dock()` after writing the timetable when the first stop is the station the train
  stands in and it is ready (`DOCK ... docks where it stands` log line). `AFGTrain::TickAtc` ends with
  `UpdateReadyToDock`, which sets ready to dock when the next connection ahead belongs to a station that `CanDock`s the
  multiple-unit master, its distance is within `DOCK_DISTANCE` of `DOCK_STOP_OFFSET` and the speed is below
  `DOCK_SPEED`. Trains standing too far out of the platforms (not ready) leave and come back.

A train without a loop (it fits no consumer, its consumer went away, or no stop could be made) gets self-driving
switched off where it stands (`Park`) with the reason kept for readouts (`FSTTrainState::IdleReason`).

## Car patterns

A consumer station's Demand platforms, in order from the station, are its pattern: platform *i* gives the item (or
item group) of the car beside it when the train docks. A train fits a consumer when, in its first dock map there, a
freight car stands beside every Demand platform, fluid platforms included: a freight car carries items or fluid,
whichever it is given, and cargo its new loop doesn't carry is unloaded first (`HoldsOtherCargo`), so the fit
ignores what a car holds (it would otherwise change every time a car empties, and split the trains again). Its loop gives each of those cars
one item, and only those cars carry anything; the others skip every stop. `/st patterns` and the monitor's Consumers
tab show every consumer's pattern and its trains.

## Saves

0.50.0 dropped the old dispatcher's saved fields (platform targets, least per visit, priority and manual rates; jobs'
trip fields; trains' unwanted-cargo time); unknown properties are skipped when an older save loads. `mLoopsVersion`
(0 in older saves) makes `PostLoadGame` drop every train's job, consumer and pattern once, so each managed train gets
its loop. The Depot Signal class is gone: a save holding them loads without them.

## Station set-up (0.11.0)

Code: `STStationSetup.cpp` (namespace `STSetup` and `ASTSubsystem::SetUpStation`, `SetUpAllStations`, `SetNaming`).
The player asked for it on 2026-11-09: build a station, connect it, and have SmartTrains set the platforms and name
it from the factory, its map area and its rate. Only on request (station menu button, `/st setup`, the migration
window's Rename stations); never automatic.

- **Platforms** (`ProposeStation`): role from what is attached (`FSTPlatformAccess::CountConnections`): only inputs:
  Supply; only outputs: Demand; both: its load mode; nothing: Off. **Item: what the platform holds** (the item or
  fluid it holds most of that fits it); since 0.18.1 the trace never overrides it, and a note shows when its machines
  take or make something else. Only an empty platform gets its item from `STTrace::Trace`, the largest traced flow
  first; platforms of one role reaching the same machines share the items out (`STMigration::ShareOutItems`, moved
  out of the migration's private helpers). Empty and no machine reached: the fluid in its pipes. The proposal is a `STMigration::FStationProposal`, so the migration preview and the
  set-up share one shape. Applied with `SetPlatformConfig` (which also sets the load mode); platforms already set the
  same are left alone, so nothing re-plans for them.
  - **0.16.0, kept platforms:** a platform that already has a role (`FSTPlatformConfig`) is proposed as it is
    (`FPlatformProposal::bKept`, confidence "set up", its saved machines for grouping) and `ApplyProposal` skips it.
    Only the platform menu's Autodetect redoes its own platform (`ProposeStation`'s `Redo`).
  - **0.16.0, made here:** one upstream flood from the machines the station's unloading platforms reach; an item those
    machines use that the flood's machines and extractors make at least `LocalSupplyShare` (0.9) as fast is left out
    of the unloading platforms' choices (a rocket fuel plant that makes its own Fuel, a factory with its own miner).
  - **0.16.0, share-out by rate:** `ShareOutItems` groups platforms of one role reaching the same machines, across
    stations (the migration proposes every station with `bShare` off, then shares out once over the preview). Each
    group's platforms go to items in proportion to car loads per minute (`SpreadRates` / car capacity; at least one
    each while there are enough; largest remainder), counting settled platforms (one item, what they hold, kept). The
    rest, in station and platform order, take the item furthest below its share, then one already at that station,
    then the faster item. Test: `SmartTrains.Setup.ShareOutByRate`.
  - **0.25.0, lanes (elimination by machine input):** the trace records which machine connection it reaches
    (`FResult::MachineInputs`, "<machine path>|<connection>", before the visited check so a second input of a machine
    counts too), and set-up records each platform's belt or pipe rate on the factory side (`BeltRate`). Kept platforms
    with a role are traced as well, so a single Autodetect knows its neighbours. Within a group in which every
    platform was traced, platforms reaching a connection in common form a lane. With two lanes or more: a lane with
    one settled item gives it to its free platforms; the open lanes, by belt rate, take the remaining items by use per
    minute (most used to most belts) when there are as many items left as open lanes (or one item for all). Platforms
    left free go on to the car-load share-out. Why: on the IB save Dune Desert Computer In 1 feeds its assemblers
    through two balancers, three platforms into input 1 (Circuit Board, 810/min) and one into input 2 (Crystal
    Oscillator, 270/min); every trace reached the same machines with both items, so the share-out alternated them and
    the player had to clear three platforms. Test: `SmartTrains.Setup.ShareOutLanes`.
- **Factory** (`AnalyzeFactory`), in flood fills (`STTrace::Flood`): one walk from a set of machines through belts,
  pipes, splitters, storage and machines, downstream or upstream, that visits each building once and stops at train
  platforms (noting the machine it last passed before each, `PlatformsByMachine`), up to 50,000 buildings and belts.
  The first version started a separate trace from every machine, so a manifold feeding hundreds of machines was
  walked hundreds of times: one station (RockPow- Sulfur SNK) took 15.6 s on the VM, now 35-160 ms, and naming all
  112 stations of the IB save takes about 3 s. A machine reached through the wrong side of a pipe (its input while
  walking upstream) is not marked visited, since another path may reach it the right way (marking it lost the
  refineries of every fluid factory in the first flood version). Machines are grouped by recipe and production boost (`FSetupMachineGroup`: runs per minute
  from the machine's cycle time at its clock speed, products per run times its production boost (somersloops),
  ingredients per run). Extractors are a group of their resource at `GetExtractionPerMinute` (node purity, tier and
  clock included).
  - **Product:** for a station that only loads, the item most of its platforms load. Otherwise, walking downstream
    from its unloading platforms: what the machines make and don't use themselves, preferring what goes on to a train
    platform, then the one made by the most machines. No machine makes it (power plants, storage): the item it
    unloads most.
  - **The factory for the rate** is the upstream walk from the machines making the product (for a station that only
    loads, the machines its platforms reach), through their inputs to train platforms and extractors. A first version
    walked every machine both ways, which merged whole refinery complexes joined by shared belts into one 600-machine
    "factory" (IB save, 2026-11-10).
  - **Rate:** the most of the product per minute, found by bisection: a target rate is shared out over the machine
    groups making each item in proportion to the capacity they have left, recursively through the ingredients (depth
    12), taking byproducts made earlier in the same step first (canister and water loops) and then what trains
    bring. A share proportional to full capacity made the weakest recipe of a loop bind early (plastic and rubber
    factories at a third of their rate on the IB save). What trains bring is each unloading platform's belts or pipes out of it (belt speed x 60 / item
    spacing, pipe flow limit x 60), for the items it is set to, and for belts no more than they carry on to the
    machines that use the item (0.24.0, `STTrace::BeltCapacity`): a maximum flow downstream from all platforms bringing
    the item, each run of belts and lifts one link as fast as its slowest belt, splitters, mergers, storage and
    balancers joining links, the machines using the item as sinks (`FSTMinCostFlow::SolveMaxFlow`, breadth-first
    augmenting paths; the min-cost solver's Bellman-Ford is too slow on belt networks). The minimum cut names the
    limit ("2 belts at 780/min on the way from ..."). Without it, platforms with four belts out merged onto two Mk5
    belts into a manifold counted as four (IB: Dune Desert Circuit Board Out 1 named 945 where 874 can run). Splitter
    filters aren't followed, so it is an upper bound; pipes keep the platform's own limit (pipe networks have no
    direction to follow). The target fails when a machine group would run over
    its capacity or a train-fed item over its belts. The first failing one is the limit named in the readout.
    Ingredients nothing in the factory makes and no train brings are assumed to arrive (noted). A station that only
    loads is also capped by the belts or pipes into its platforms. Unloading into machines that make nothing (power
    plants), the rate is the station's own belts or pipes; loading what no reached machine makes, it is unknown and
    the name has no rate.
  - **Scan depth** (`ScanDepth` setting, 50,000 buildings and belt segments, 1,000 to 500,000): the limit of every
    flood fill, platform trace and belt capacity walk in set-up (`SetupScanDepth`); a walk cut short says so in the
    set-up notes.
- **Which item names a station:** what it loads (only loading); the one item it unloads, with
  `FFactory::InputRate`, how much of it the factory uses at its most rate (its share of all trains bringing that
  item, within its own belts), or its own belts when nothing is made (power plants); several unloaded items: the
  factory's product and rate. A loading station's rate is its share of the factory: its belts or pipes into its
  platforms against all loading platforms for the product that the factory's machines feed (`DownPlatforms`), so
  two stations loading from one factory don't both claim all of it.
- **Set-up bar layout**: the bar sits in the name plate's box, which takes its width from its widest child, so its two
  buttons ask for at most 120 px each (`USTButton::SetFitToSlot`, a size box's max desired width) and share whatever
  the menu gives them; their text stays on one line and ends in "..." (full text in the tooltip). A long "Rename to:
  ..." widened the menu past its frame (0.23.0). The bar shows no message line (it covered the menu below): the
  Autodetect button shows "Done: N platforms changed" or the error for 8 s; the SmartTrains section further down
  shows the full message.
- **Autodetect all platforms** (station menu, `USTStationSetupBar`, `Server_AutodetectStation`,
  `ASTSubsystem::AutodetectStation`): `ProposeStation` with `bRedoAll` (every cargo platform proposed afresh, as a
  platform's own Autodetect does for one), then `ApplyProposal` without renaming. The first click arms the button.
- **Name** (`NameStation`): `FSTNamingConfig` (saved per world): `Format` with `{area}`, `{product}`, `{role}`,
  `{n}`, `{rate}`; `{role}` from `SupplyWord` (only loads), `FactoryWord` (unloads several items: a factory's inputs),
  `ConsumerWord` (unloads one item), `MixedWord` (both). `{rate}`: the one item's use for a station unloading one
  item, none for one unloading several (the factory's output isn't what it takes in; 0.22.1), else the product's
  rate. Empty values drop out with their brackets (`FormatName`).
  - **Area:** `AFGWorldSettings::GetMinimapCaptureActor()->GetMapAreaTexture()->GetMapAreaForWorldLocation()`, the
    game's fog-of-war areas, named by `UFGMapArea::GetUserSetAreaDisplayName` or `GetAreaDisplayName`; else a
    `AFGVolumeMapArea` that contains the station (`mMapArea` by reflection). The name note says which one was used.
    The texture learns the map's scale and offset when its capture actor starts; if the first lookup finds nothing,
    `InitMapAreaTexture` is called once and the lookup tried again (dedicated servers, `SETUP map areas` log line).
  - **Number** (`PickNumber`): the number the station's name already has, if no other station has it; else the
    smallest from 1 that no other station's name has for the same area, product and role, with any rate
    (`MatchesName`). Setting up again keeps names stable while only the rate changes.
- **Preview and apply:** the set-up bar under the station name (first tab of the station menu) has Rename station
  (`Server_SetUpStation` mode 0 previews the name, 1 renames). The lines come back in `FSTStationView::SetupLines`;
  `SetupReady` (1 while a name preview from the last 2 min can be applied) turns the button into its confirm step.
  Platforms are set one at a time (0.15.1: the station-wide Autodetect button is gone; `/st setup` and the migration
  still set every platform). The platform panel's Autodetect (`Server_AutodetectPlatform`) proposes the whole
  station (items are shared out between platforms serving the same machines) and applies only that platform, at once.
  Applying is an Edit change (admin-gated with Only admins change SmartTrains).
- **Staged naming:** naming every station in one frame froze the IB server for about 18 s per migration preview
  (2026-11-10; the player clicked Preview three times and every other request, platform menus included, waited 55 s).
  The migration preview now leaves `NewName` empty and `STMigration::StepNaming` names a few stations per call;
  `/st setup all` does the same (`StepSetupAll`). Both run from one timer (`mSetupTimer`, every 0.05 s, about 15 ms,
  at least one station; `bMaxOncePerFrame`, so a slow step doesn't make the timer catch up within the same frame).
  When the preview's names are done the window that asked for it
  gets a fresh view (`USTRemoteCallObject::PushMigrationView`); an apply with Rename stations finishes them first.
- **Migration:** the preview names every station (`FStationProposal::NewName`, `NameNote`; a `name` row in the CSV,
  a note under each station in the window). With Rename stations on, apply renames the ticked stations and the undo
  record keeps the old names (`FSTMigrationUndo::RenamedStations`, `OldNames`).
- **Machines kept:** whatever sets a platform from its trace (Autodetect, station set-up, the migration) keeps the
  machines that trace reached in `FSTPlatformConfig::Machines` (saved), through `SetPlatformMachines`. Nothing traces
  platforms in the background any more (the user, 2026-11-10: once a factory is built its theoretical rate doesn't
  change); Autodetect again after rebuilding a factory. A save from before 0.14.0 (`bMachinesFilled` false) has each
  platform with a role traced once after loading (`StepMachineFill`, on the set-up timer).
- **Console:** `SmartTrains.Setup [apply] [keepname]` (an `FAutoConsoleCommandWithWorldAndArgs`) runs the
  all-stations set-up from a dedicated server's console, for headless tests.

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
master is known. A push-pull train may arrive either way round. Every real dock is read
back from the platforms' `GetDockedActor` and stored per (train, station). A stored map is used while the train still has those vehicles. A dock that differs from the expectation
logs `DOCK MAP MISMATCH`.

## Clock

`Now()` is the world's time plus `mClockBase`. The world's time starts at 0 each session. `mClockAtSave` (saved)
holds `Now()` at the last save, and `PostLoadGame` sets the base so the clock carries on from it. Every saved
timestamp (departures, job starts) stays meaningful after a load. A save from before the clock
existed continues from the latest time its trains hold.

## Travel times

`FSTTravel` searches one source station per 0.5 s. Each search is Dijkstra over track exits: a node is "about to
leave a track through this connection". From connection C, the next nodes are `C->GetConnections()[i]->GetOpposite()`
at the cost of that track's length.

- A station departs through its track's far end. A station is reached when a search enters its track through the
  platform end, which is `GetStationOutputConnection()->GetRailroadConnectionReference()` or the station track's end
  nearest it.
- A second search starts from the platform end, for push-pull trains that back out. Those routes get 30 s added.
- Backing out (0.34.3): only push-pull trains may start a route by backing out of a station. `GetDistance` used to fall
  back to backing-out routes for every train when no forward route existed, so stations a one-locomotive train can't
  reach got a travel time.
- One-way signals (0.34.1): a connection with a trailing signal and no facing signal is not driven through (the game's
  one-way rule), so routes hold on signalled networks without Auto Trains. Before, the search ignored signals.

Time = distance ÷ **Speed estimate** + 20 s. It is replaced by the learned trip time once a train has driven that
pair: the first sample, then a 0.3-weight moving average. Stays per station are learned the same way. A pair the search
finds no way between for that kind of train (`GetDistance` -2) is unreachable before any learned time is looked at
(0.34.6): learned times are shared by all trains and once kept a push-pull Water train trying for the bay beside its own
on the IB server, 40 refusals a day.

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

## Auto Trains

Optional, by reflection (`STAt` in `STBridges.*`, Auto Trains' `Docs/API.md`). The actor of class
`ATTrafficSubsystem`, through the subsystem actor manager.

| Call | Used for |
|---|---|
| `ExternalTimeTableChanged` | After every real timetable write (`WriteJob`). |
| `ExternalEstimateTravelSeconds` | `EstimateTravelSeconds` for a pair no train has driven yet: Auto Trains' time replaces SmartTrains' estimate (cached 10 minutes, a few per loop check). Never a "no way" from SmartTrains' search, which is checked first. |

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
- The engine refuses any network message over 64 KiB, which only matters with a remote server: a listen server's
  host calls its own `Client_*` functions directly.
  - Migration answers go in parts (`SendMigration`): `Client_MigrationStart` carries the view without its rows and
    their count, then `Client_MigrationRows` sends batches of about 24 KiB. The client shows the view once every row
    has arrived. A preview of 101 stations and 39 trains came to 104 KB, so on a dedicated server Preview did nothing.
  - The monitor view goes unreliably in one message without the rows' details (`bHasDetails` says a row has some),
    trimmed to about 48 KiB (`FitMonitor`) by dropping the longest list's last rows, with a warning in the log. A row's
    details come on request (`Server_RequestMonitorDetails`, keyed by `MonitorKey`) while its More toggle is open.

### Admin-gated controls

`STAccess::MayChange` is checked on the server before every change, in the `Server_*` edit handlers and the chat
command:
- **`EChange::Network`** is always admin only: server settings (`/st set`), migration apply and undo, test copies and
  backups.
- **`EChange::Edit`** is admin only while `AdminsOnly` is on: platform, station and train settings, pastes and test
  jobs.

`STAccess::IsAdmin` (the same rule as Auto Trains' `/at`) counts as admins:
- the host (the server's local controller);
- a player whose `AFGPlayerState::IsServerAdmin()` is set;
- on a dedicated server, a player whose controller's `GetDSPrivilegeLevel()` is `Administrator` (or `InitialAdmin`).
  The game sets it on the server from the player's join token, so a client can't fake it;
- no controller (the server console).

A refused edit handler sends the current view back, so the menu shows the unchanged state again.

Settings are SML configuration. A dedicated server reads its own `Configs/SmartTrains.cfg`, and a client's Mods menu
never reaches it. `USTConfig::Write` sets the property in the server's registered configuration, marks it dirty and
flushes SML's pending saves. `ReadSettings` picks it up within a second.

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
| `Widget_TrainStationNew` | `VerticalBox_0` (first tab, under the name plate) | `USTStationSetupBar` (set-up buttons) |

Panel names and positions come from the starter project's widget assets, dumped with Telescreen's `TSDumpWidgets`
commandlet. It now also lists named-slot content, canvas slot layout and size box overrides
(`Tools/Spikes/SmartTrainsUI` holds the output). The platform panel sits right of the vanilla layout in an
auto-sized canvas slot, so the canvas, and with it the window, grows wider. The panel logs its geometry half a
second after it opens, to check that.

- **Platform window** (`USTPlatformWindow`, the `USTPlatformPanel` content in a window on the viewport). The launcher beside the platform menu shows the role, items and one readout line, and opens it. The window keeps the input mode as the platform menu set it, so the menu behind it works; Close, Escape, or closing the platform menu close it. Its panel reads the platform and the font from the menu (`USTPanel::SetMenu`).
- **Platform panel.** One role button (Off / Supply / Demand / Sink; click steps forward, right click back) and the
  item list with a readout line per item (stock, and the traced most rate in car loads per minute with what the loops
  take). Sinks have no item list. The item picker lists the items on the platform first, then every item that
  available recipes make plus raw resources, of the platform's form. Copy / Paste use the player's SmartTrains
  clipboard (below), and under them "Copy to the station's other platforms" gives every platform of its kind in the
  station these settings once; all three carry role and items.
- **Clipboard** (`STClipboardHooks.cpp`). Each player has two slots on the server, freight and fluid
  (`USTRemoteCallObject::mFreightClipboard` / `mFluidClipboard`), as the game keeps one clipboard entry per building
  kind. The panel's Copy / Paste and the game's copy/paste-settings both use them: SML hooks after
  `AFGPlayerState::CopyFactoryClipboard` / `PasteFactoryClipboard` send `Server_CopyPlatformSettings` /
  `Server_PastePlatformSettings` when the object is a cargo platform and the player is local. Copying a platform
  without a role copies "Off", so pasting it removes the target's role, as the game's paste makes the target like
  the source. Through the game's key the load/unload setting is then left as the game pasted it.
- **Train panel.** In the Timetable column, inserted at slot 3 (`ParentSlotIndex`), under the stop list and above the
  cancel docking, edit and self-driving buttons. It has the managed switch, Hold / Resume, one status line (its loop, or why it has
  none), its pattern and its loop's stops, in a scroll box at most 230 px tall. (0.3.0 had it in the info column, where it covered the power and speed
  stats and ran off the bottom.) "Keep vanilla" is only in chat and the migration tool. While the train is managed,
  the vanilla "Edit timetable" button is disabled on every refresh, and enabled again when the menu closes.
- **Station panel.** Layout, the station's loop (as a consumer) and the network's status; everything per platform is
  in the platform menus.
- **Migration window** (`USTMigrationWindow`). Not part of any vanilla menu: `/st migrate` without a step sends
  `Client_OpenMigrationWindow` to the player, which adds the window to the viewport 0.2 s later (after the chat has
  closed and given the input back) and takes the input in UI-only mode. Close or Escape removes it and gives the
  input back to the game. A dimmed full-screen layer behind it catches clicks, so it keeps the keyboard focus. It
  runs the same steps as `/st migrate`. Its rows are the preview's stations and trains, ticked as proposed. Apply
  sends the ticked numbers. Without a test copy or backup this session, Apply takes a second click (the chat
  command's `confirm`).
  - One toolbar (Preview, Apply ticked, Undo, what Apply sets, and with beta tools Test copy and Backup) above three
    tabs: Stations, Trains and Result (0.15.2; the Overview tab and the separate option rows are gone). The Stations
    tab is a table with headings: tick, station (click to open its platforms), New name while Rename stations is on
    (`FSTMigrationRow::NewName`; "stays" when the name doesn't change, "..." while names are still worked out), platforms
    and status. What Apply sets cycles on click: platforms only (no extra word), platforms and names (`rename`), names
    only (`namesonly`: the proposals' platforms are left out, as the station menu's Rename does; 0.15.3). A station "to check" has a flag, or a platform with a conflict, no item, or low or unclear confidence.
  - The view carries each station's platforms (`FSTMigrationPlatformRow`: number, role, items, confidence, note,
    conflict), so the window shows item icons.
  - Ticks, opened stations, the tab and the filters live in the window. A fresh preview resets the ticks to the
    proposals and opens the Stations tab.

### Style (UI/STStyle.h)

One look for every panel: `STStyle` colours (FICSIT orange on dark panels; references, never copies, so other files'
statics can't read them before they are set), rounded brushes, and the game's font copied from the menu a panel sits
in (`USTPanel::GetFont`; a text made with `MakeTinted` keeps its own colour and weight when the font arrives). Drawn
widgets: `USTLogo` (the hexagon mark), `USTBar` (a fill bar easing to its value over 0.35 s, glowing with `SetPulse`),
`USTRing` (the same as a ring). Controls: `USTSegment` (one of a row of choices), `USTIconTile` (an item as its icon,
its name in the tooltip). Panels fade and rise in over 0.22 s (`USTPanel::NativeTick`). Views carry structured data
for them (`FSTCarView`, `FSTStopView`, `FSTPlatformChip`, `FSTSourceView`, and the pattern and train counts of
`FSTMonitorRow`); the text lines are left for `/st status` and the log.

### Ride HUD

`USTRideHud` (a `USTTrainDisplay`, like the locomotive panel): a core ticker every 0.5 s (`USTRideHud::Install`, from
`STUIHooks::Register`) finds the train the local player sits in (the possessed `AFGLocomotive`, or the character's
driven vehicle) and shows the card 170 units down at the top right, under Auto Trains' strip; hidden while the train
isn't managed. It copies the font from a hidden vanilla label and never takes input.

## Depot signals

`ASTBuildableDepotSignal` (STDepotSignal.h, back in 0.52.0): a block signal that also marks the passing siding behind
it as a parking spot (`STDepots::Check`); dead-end sidings aren't used. Spare trains wait there:

- **Use** (`UpdateUse`, every loop check): the share of a consumer's traced machines (its Demand platforms'
  `Machines`) that want input: not on standby, powered (manufacturers), output not backed up (no full output stack).
  Averaged with a 300 s time constant, kept across loop models (`mUse`). Without traced machines it counts as 1.
- **Wanted** = its trains times its use, rounded up, at least one. After a round (leaving its consumer, cars empty) a
  train of a consumer running more than Wanted goes to the nearest free fitting spot (`MaybeSendToDepot`,
  `SendToDepotSignal`): far end first, the way in clear. It keeps its consumer (`LoopConsumer`), marked `bDepot`
  (saved), so it still counts for its share; Auto Trains drives it (`STAt::DriveTo`), or without Auto Trains the game
  along a path with the signal held at stop. Arrived, its autopilot goes off (Parked).
- **Back** (`UpdateUse`): while a consumer runs fewer than Wanted, its depot trains with a clear way out
  (`IsBoxedInAtDepot`) go back to their loop (`WakeFromDepot`), as does one whose consumer changed. A drive that ends
  elsewhere also puts the train back on its loop, and that spot isn't offered to it for 10 minutes.

## Monitor

`MakeMonitorView` builds the view from the loop plan as last worked out: tiles (managed trains, on their loop,
switching patterns, without a loop, at depot signals, consumers, unserved consumers), fleet lines, and three tables:
- **Consumers:** pattern, rate (car loads per minute), round trip, share of the need, trains; details: pattern,
  suppliers taken, and the consumer's problem. Unserved first.
- **Suppliers:** per station and item, its most rate and what the loops take; "Over its rate" when they take more.
- **Trains:** state (on its loop, switching, no loop, held), its consumer, and what it does (`DescribeWork`: the stop
  it heads for).

The view goes to `USTMonitorWindow` (Overview, Consumers, Suppliers and Trains tabs) through
`Server_RequestMonitor` / `Client_Monitor` once a second while the window is open. `/st monitor text` and the server
console print the same report, logged as `MONITOR` lines every 5 minutes while trains are managed.

## Dock maps

`GetDockMaps` returns the vehicle beside each platform for every way the train may dock:

- Learned maps (`LearnDockMap`, one per way round) are used while the train has the same vehicles in the same order
  (`FSTDockMap::Consist`). A map records the locomotive that docked (`FSTDockMap::Locomotive`, the train's lead), and
  replaces the prediction for that locomotive. Maps from older saves are told apart by their first docked vehicle.
- Ways round not seen yet stay predicted, from the vehicles behind each locomotive that can dock.
- If a learned map matches no prediction, only learned maps are used for that station.
- Loops use the first map: the way round the train was seen docking, else its lead locomotive's. The game reads a
  stop's rules when the train docks, so a wrong guess can't be fixed during that dock; the mismatch is logged and
  learned, and the next loop check works from it.

## Docking

A train's dock begins in `NoteDockStarted`, called by an SML hook after `AFGBuildableRailroadStation::StartDocking`
(`STDockHooks.cpp`, server; it returns a bool, so it is safe on Linux servers). `MonitorTrains` checks `IsDocked`
every 0.25 s for the end of the dock, and as a fallback for a start the hook missed (logged at Verbose). Until 0.6.8
the check alone found docks: one with nothing to move lasts about 0.3 s, a few frames on a busy server, so some were
missed, the job's leg stayed a stop behind, and the next timetable sent the train past a stop ("legs were missed").
