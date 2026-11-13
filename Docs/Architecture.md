# SmartTrains architecture

Satisfactory SML mod, game CL 513885. Prefix `ST`, log category `LogSmartTrains`. The design and its decisions are
in the plan (`C:\Users\benvr\.claude\plans\smarttrains.md`); this file records how the code implements them.

## Pieces

| File | What it does |
|---|---|
| `STTypes.h` | Saved data: platform settings (`FSTPlatformConfig`), station settings, train state and jobs, learned dock maps, trip and stay times, the migration undo record. |
| `STStationSetup.*` | Station set-up: platform roles and items from what is attached, the factory's product and theoretical rate, and station names. |
| `STGather.cpp` | The default dispatcher: trips built around one consumer station each. |
| `STGroups.cpp` | Item groups: groups from the flow plan, train homes, the dispatch round, group readouts and settings. |
| `STSubsystem.h`, `STSubsystem.cpp` | `ASTSubsystem` (server only, saved with the world): settings API, train monitoring, docking events, learning, rate sampling, readouts. |
| `STDispatch.cpp` | The rest of `ASTSubsystem`: supply/demand nodes, the fit model, dispatching, the flow plan, writing timetables. |
| `STTripPlanner.*` | Trip planning: car-by-car simulation, regret insertion. |
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
| `STAccess.*` | Who may change what (admin-gated controls). |
| `UI/*` | The panels added to vanilla menus, and the controls they are built from. |

## 0.12.0: consumer-anchored trips

Code: `STGather.cpp` (`ASTSubsystem::GatherRound`), `FSTTripPlanner::SetOnlyStation` and `GetConsumerStations`. The
only dispatcher since 0.13.0 (0.12.0 kept 0.10's item groups behind a switch for comparison). Why: the user asked (2026-11-09) for as few rules as possible at close to the best efficiency. The IB logs
showed planner sophistication didn't move throughput (0.7.1's horizon planner ran the same trips per 30 min as
0.6.10), and most bugs came from mechanisms interacting (services, homes, lending). The shape is the user's DTT setup
(one group per pull-based station, "[supplier group] -> factory") made automatic.

The rules:

1. **Need.** The moves `BeginTrain` lists already are the need: a consumer below target at the rough arrival time (or
   a nearly full supplier pushing), with a car that lines up at both ends.
2. **Build.** For each consumer station the train can deliver to (`GetConsumerStations`), regret insertion runs with
   only moves to that station (`SetOnlyStation`): pickups for the cars that line up with its platforms, from the
   suppliers that pay best, while the trip's value per second rises. Each car carries one item, so a factory with
   mixed inputs gets a gathering trip in its platform order; a one-item consumer gets a plain shuttle trip.
3. **Choose.** The train runs the consumer's trip with the best score (value per second, the same objective as
   since 0.8.0) that is worth running (value above `TrainSecondCost` x its time). Trains take turns as before (longest
   waiting first, every round plans at least one).
4. **After.** The job keeps its consumer (`FSTJob::AnchorStation`); re-plans after each stop only add deliveries
   there. Cargo for elsewhere stays on board and is a move (cargo on board) for the next trip. With nothing worth a
   trip the train settles (depot or in place).
5. **Cargo first** (0.16.0). A train with cargo on board (a delivery whose platform had less room than planned) first
   tries only consumer stations with a demand node for an item it carries, and only trips that unload some of it;
   any consumer only when none of those is worth a trip now. On the IB server 12 of 160 trips after
   `/st emptycars all` delivered under 90 % of their plan, and those trains then loaded for another factory beside
   the leftovers.

There are no homes and no lending (the groups are only a readout of the flow plan). A
consumer is served by whichever free train makes its trip pay best, so trains whose cars line up better and trains
nearby win by themselves. Train-first turns can give a far train a consumer a nearer, later train would have served
better; the reservations it makes lower that consumer's value for the others, and the next round corrects it.

## 0.11.0: station set-up

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
- **Preview and apply:** the depot bar under the station name (first tab of the station menu) has Rename station
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

## Car patterns (0.18.0; item sets since 0.20.0)

- **Pattern** (`FSTTrainState::CarSets`, saved, one `FSTCarItems` per freight car front to back): the items each car
  carries in its group; empty where none are given yet. Usually one item; a sushi platform makes it several. Reset when
  the number of cars changes. `DescribePattern`: "Iron | Wire + Cable | - | Wire". (0.18.0's one-item `CarPattern` is
  no longer read, so trains start open after updating.)
- **Lining up** (`FSTTripPlanner::MakeCarItems`): per car, the demand items of the platform beside it at the consumer
  (the same whichever way the train docks, else none). A car with pattern items beside a demand platform whose items
  aren't exactly the same set: the station is another group (false). A car may take its pattern items, else (open) the
  demand platform's items beside it, else what it holds. It counts as mismatched if it holds an item outside that set.
  The result goes to `SetCarItems`; in `SimulateWithSkips` a car beside a supply platform offering a planned item
  outside its set, or holding something outside it, counts as harm, so it skips (FCC) or the trip fails. Within its set
  a car may carry several items (loaded at one stop or several); without a set (tests) a car still never mixes.
- **Dispatch** (`GatherRound`): consumers that line up with the pattern and have no mismatched cargo are tried as before
  (one trip per consumer, the best runs). The pattern then takes, per car, the set the trip allowed it (`PatternAfter`),
  else keeps its set. With none found, `PatternIdleSince` starts; after `PatternIdleSeconds` (300 s), or at once for an
  open train, consumers are tried ignoring the pattern (still no mismatched cargo), and the pattern is rebuilt from that
  trip. If consumers were blocked only by cargo, a clearing trip runs: `SetNoLoads`, any consumer (`SetSinksForCargo`
  lets sinks take cargo a consumer exists for), no anchor; the pattern starts open. `ReplanTrip` applies the same car
  items from the job's anchor (no anchor: no loads, sinks allowed).
- **Parked trains in the way (0.31.0)**: a parked train's booking is marked (`FSTBooking::bParked`); when it makes a
  stop of any trip tried wait (`IsParkedInWay` in `SimulateWithSkips`), the planner notes the station
  (`GetParkedInWay`), the dispatcher keeps it in `mWantedStations` for `WantedStationSeconds` (60 s, WANTED line), and
  `GetNeededStations` counts it, so `ClearNeededBays` moves the parked train out; while it pulls out it holds the
  station for `ParkedLeaveSeconds`. Before, a station counted as needed only once a job had a stop there, but a
  parked train in a dead-end station holds it for `DeadEndHoldSeconds` (an hour), so every trip there was rejected
  ("would wait before ... behind 1 trains (room for 0 before its switch)") and no job ever needed it. IB save,
  2026-11-13: 32 of 38 trains idle, parked in stations for want of depot signals, suppliers full and consumers
  starved, about two trips a minute.
- **Parked in its own pickup station (0.31.1)**: a train can't dock at the station it stands in, so trips starting
  there are rejected ("first stop is the station the train stands in", 0.9.5); a train parked at its supplier stayed
  stuck (130 of the last 400 idle notes on the IB save, 2026-11-13). `WasKeptFromOwnStation` now marks its station
  wanted (`WantStation`), so it parks elsewhere and can come back for the trip.
- **Own group's queue rejections only (0.31.0)**: `bQueuedInGroup` (0.28.0) counted every queue rejection since the
  train was set up, so trains with "0 tried" kept a group they had no trips in and never changed; it now needs a
  consumer tried and rejections made during its own pattern's passes.
- **Queued work keeps the group (0.28.0)**: when a train finds no trip in its own pattern but trips there were
  turned away by the queue limit (`FSTTripPlanner::CountQueueRejections`: "would wait before ..." / "would queue at
  ..."), its group has work and a station is busy, so `PatternIdleSince` is reset instead of started and it doesn't
  change group by idle time; it tries again next round. Homeless and spare changes still apply (a spare leaves only
  while another train keeps the group). Why: on the IB save the only train of Dune Desert High-Speed Connector In 1's
  group was turned away for 5 minutes by trains waiting before Grass Fields Copper Ingot Out 1 (689 such rejections
  in that log), counted that as nothing to do, and moved to Crystal Oscillator In 1, leaving the starved station with
  no train; busy suppliers made trains drop busy groups across the network.
- **Spare trains (0.27.0)**: a train whose cars are empty, that finds no trip in its own pattern, and whose pattern
  another managed train shares (`SamePattern`, also back to front for a train turned round) changes group at once,
  but only to a consumer station no train serves (`TryConsumers(true, true)`, no second pass over all consumers); with
  none it waits as before. The last train of a group never leaves this way, so a group keeps a train and a train that
  just moved to an unserved station (no mate there) stays. Why: a group with more trains than its flow needs kept the
  extras parked for `STPatternIdleSeconds` while another station starved; re-picking a group after every trip would
  bring back mixed cargo, which the sticky patterns stopped.
- **Homeless patterns (0.26.0)**: a train finding no trip in its pattern, with no consumer station tried at all, checks
  (at most once a minute, `HasPatternHome`) whether any consumer station's demand platforms still take its pattern
  (`MakeCarItems` with `OutMatched` > 0: a patterned car beside a demand platform for the same items, none for other
  items). If none does, it changes group at once instead of after `STPatternIdleSeconds` (`bPatternHomeless`). Why:
  on the IB save a train took "Circuit Board | Steel | - | Steel" from Dune Desert Computer In 1 while the player was
  still setting its platforms; once they were set, no station took that pattern, its trip was dropped and it waited
  5 minutes for a group that no longer existed. The platform readout counts only free trains whose pattern can come
  (open, free to change group, homeless, or each patterned car with a platform for the same items at the station);
  before, "19 free trains fit; a job should follow" counted trains of other groups.
- **Unserved first (0.23.0)**: a train choosing a pattern (open, or changing group) first tries only consumers no
  train serves (`IsConsumerServed`: no managed train on a trip anchored there, none given since `ServedSeconds`, 600 s,
  `mLastTripTo` set in `CommitTrip`); only with no worthwhile trip among them, all consumers. Before, the best score
  won, so a small far consumer (IB: Dune Desert Crystal Oscillator In 1, Rubber starved for hours) lost every open and
  changing train to big near ones (a new train took a Uranium pattern).
- Why: platforms fill cars past the plan, so leftovers are normal; within a pattern they always line up.

## 0.17.0: depot signals, hold, new trains

- **Depot signal** (`STDepotSignal.*`): `ASTBuildableDepotSignal`, a C++ subclass of the native
  `AFGBuildableRailroadSignal`. At game instance post-initialization (`STDepots::ResolveVanillaAssets`) its class
  default gets every editable setting the vanilla `Build_RailroadBlockSignal_C` default has (declared in `AFGBuildable`
  and below: hologram, interact widget, block or path, sounds; plus `mRailroadSignalSparesDataCDO`, the left/right-handed meshes, which isn't editable and is copied by hand: without it the hologram crashes on spawn (0.19.1)). Actors of a native class don't take the default's runtime changes,
  so `ASTBuildableDepotSignal::PostInitProperties` copies the same settings from the default onto every new signal
  (0.19.2; without it `AFGBuildableRailroadSignal::BeginPlay` read null signal data and crashed the server), and the vanilla signal component's mesh,
  transform and materials. The vanilla blueprint has no components of its own beyond that one (dump:
  `Tools/dump_signal_assets.py`), and the lights are the native class's. Descriptor and recipe copy the vanilla block
  signal's (icons, build category, ingredients); since 0.19.0 it unlocks with the vanilla block signal: every 5 s the server makes its recipe available once the block
  signal's is (`STDepots::KeepUnlocked`, also for saves that unlocked block signals before). The 0.17.0 AWESOME Shop
  schematic is no longer registered; its class stays (a stub) so saves that bought it still load.
- **Siding check** (`STDepots::Check`): the approach exit is the guarded connection (or one joined to it) whose facing
  signal is this one; from there back along whole tracks until the siding's entry: a signal at a joint, or (since
  0.20.1, with Auto Trains installed: `STAt::IsInstalled`, also true on clients) its switch. A platform's track or a track end on the way makes it unusable. Shown in the look-at text (runs on every client: tracks
  and signals are replicated) and `/st depots`.
- **Dead ends and chains (0.21.0)**: `Check` first walks forward from the signal; if the track ends there without a
  switch the signal faces into a dead end (`bDeadEnd`): the spot runs to the next depot signal facing the same way, or
  the track end (`Goal`). Otherwise the passing-siding walk back runs as before, and a depot signal at the entry joint is the spot's
  `Neighbour`. `STDepots::Survey` links every spot to its neighbours (`Behind` toward the siding's entrance, `Ahead`
  deeper in) and lists the trains with a vehicle on its tracks (so it holds after a restart, when `mDepotTrips` is
  empty). `SendToDepotSignal` only offers a spot that is free, whose spots `Behind` are free (the way in), and whose
  spot `Ahead` is taken or doesn't fit the train (far end first); dead ends only for push-pull trains with
  **Dead-end depots** on. `GatherRound` skips a parked train for which `IsBoxedInAtDepot` finds a train (or a trip)
  on its way out: the spots `Ahead` on a passing siding, `Behind` on a dead end. `MonitorTrains` parks a depot-bound
  train near the spot's goal (`FDepotTrip::Goal`), not the signal, since a dead end's goal lies deep inside.
- **Driving (0.22.0)**: one drive per trip (`FDepotTrip`: goal, route, stop signal), started, followed and ended by
  `StartDepotDrive`, `GetDepotDriveState` and `EndDepotDrive`; everything else (spot choice, parking on arrival, the
  no-way pause `mDepotNoWay` for `STDepots::NoWayRetrySeconds`) is shared. With Auto Trains loaded it drives the train
  (`STAt::DriveTo` / `GetDriveState` / `CancelDrive`, its `ExternalDriveTo` API since 1.4.15: it empties the timetable,
  plans the leg around the other trains and stops the train at the goal). Without it SmartTrains drives: an empty
  timetable, the route as a path with no station and the game's follow-path state (`FSTPlatformAccess::FollowPath`;
  the game follows such a path only in that state), and the spot's stop signal (`FSTDepotSiding::StopSignal`: the
  depot signal on a passing siding, the signal at the far end of a dead end) held at stop (`STDepots::Hold`, kept there
  by a hook on `AFGBuildableRailroadSignal::UpdateAspect`), so the game's automatic control brakes for it; arrived once
  the train stands with an end within 60 m of the goal; the path given again every 5 s if the game dropped it. The
  siding rules without Auto Trains (an entry signal after a passing siding's switch, a signal facing in at a dead end's
  far end) make the spot a block of its own. A trip's timetable ends an Auto Trains drive by itself (`WriteJob` only
  lets the stop signal go); unmanaging cancels it. Neither drive is saved: after loading an idle train is sent again.
  Before 0.22.0 SmartTrains set the path without the follow-path state: the game ignores such a path, flags "no
  timetable", and Auto Trains didn't manage the train either; on the IB save most depot-bound trains stood on the main
  line and jammed the network.
- **Parking** (`SendToDepotSignal`, in `SettleIdle` and `ClearNeededBays`): free depot signals (no other train in `mDepotTrips`) whose
  siding fits the train (length + `STDepots::Margin`) on its track graph are goals of one Dijkstra over track exits from
  the MU master's exit ahead (`STDepots::FindRoute`). The train's timetable is emptied, then the route is given to its
  automatic control (`FSTPlatformAccess::SetTrainPath`, friend of `AFGTrain`: `mAtcData.SetPath`, no station, as Auto
  Trains' routes), autopilot on, `LastStation` cleared (so bookings and bay clearing ignore it). `MonitorTrains` parks it
  (autopilot off) once it stands still within 60 m of the signal or its path has run out. `WriteJob` (a new trip)
  removes it from `mDepotTrips`. Untested in game: whether the autopilot keeps a path that ends at no station.
- **Hold** (`FSTTrainState::bHeld`, saved): autopilot off; `GetFreeAt`, re-plans and `SettleIdle` skip it; `WriteJob`
  leaves the autopilot off. Resume writes the job again. Someone switching the autopilot on ends the hold.
  **Cancel job** and `/st emptycars` use `EndJob`.
- **Manage new trains** (setting): every 5 s, trains with no `FSTTrainState`, freight cars, no timetable stops and a
  locomotive that can dock are managed once their vehicle count hasn't changed for 30 s.
- **Other train mods** (`USTGameWorldModule::NoteConflictingMods`, 20 s after the world's post-initialization): SML's
  loaded mods matched by name; a warning in the log, and on clients a system chat line each.

## 0.10.0: item groups (a readout since 0.13.0)

Code: `STGroups.cpp`. 0.10.0 replaced 0.9's services with item groups that owned home trains (D'Hondt share-out,
`AllocateHomes`), planned home work first and lent idle trains out (`GroupRound`, `LEND`), with per-item least/most
limits and a per-train Stay switch. 0.12.0 replaced that dispatcher with consumer-anchored trips, which delivered about
19 % more car loads on the IB save copy; 0.13.0 removed homes, lending, the limits, Stay and the switch back.

What is left is a readout of the flow plan:

- **Groups** (`FSTGroup`, rebuilt from every flow plan by `UpdateGroups`, not saved): one per item the flow plan
  routes, with its routes (`FSTFlowRoute`: supplier, consumer, flow, round trip, trains needed, the trains that fit
  it), the trains its routes need (`Needed`, smoothed: each plan moves it `NeededSmoothing` (0.35) of the way), and the
  managed trains that fit them. A group whose routes leave the plan is kept for `GroupKeepSeconds` (15 min).
- **Short** (`GetGroupShortfall`): ceil(needed - 0.15) (`GetGroupTarget`) less the trains working on the item now
  (`CountWorkingFor`), and only while no train that fits it is idle. Idle fitting trains mean too little to move.
- **Readouts:** monitor Groups tab and Overview tiles, `/st group [item]`, the platform and station readouts' group
  lines. Logs: `GROUP <item> new|ends`.

## 0.9.1: congestion from Auto Trains

`STAt::GetTrackLoad` calls Auto Trains' `ExternalGetTrackLoad` (1.4.12): the busiest switch or crossing within 400 m of a
station, as booked time over the next 180 s / 180 s, and the seconds trains wait there now. `ASTSubsystem::GetTrackLoad`
caches it per station for 15 s (0 without Auto Trains). Used by the stop cost (`TrackLoadWeight` 2: a fully booked way in
counts like two more trains there) and by the station readout.

## 0.8.0: one way of deciding

0.7.x added a mechanism per problem the server logs showed (shuttles, requests, repeats, loops, sink rules), and each
fix exposed the next problem through how they interacted. 0.8.0 replaces them with general rules:

- **One objective.** A trip's score is `Value - TrainSecondCost x duration`
  (`STForecast::TrainSecondCost`, 0.01 car-load minutes per second), the same net the horizon search compares, so
  regret insertion, re-plans and the search all optimise one thing. Value is the forecast cost removed (shortfall x
  priority x `CostScale`, plus lost production x 0.25), less the stop cost, plus or minus tied cars. The over-target
  bonus (0.2) and sink value (0.1) are gone; a re-plan now has to beat the kept rest by `ReplanMarginValue` in score.
- **Idle trains park** (`SettleIdle`): a free depot signal (`SendToDepotSignal`); else it parks where it stands only
  when it stands in a station no train needs (`GetNeededStations`: in a managed trip or drive to park still ahead, or
  where another train is docked or parked). Moving, out on the line, or in a needed station, it drives to the nearest
  parking station (`FindParkingStation`: not needed, not blocked, not the one it is in) and parks there. Before 0.22.2
  a train still moving drove on to its timetable's current stop: after a one-stop trip that was the consumer it had
  just left, so it parked inside it and the next delivery waited at the door (IB, Dune Desert Crystal Oscillator In 1).
  `ClearNeededBays` moves a parked train (in a station, or out on the line) when a trip needs its station or Auto Trains
  reports a train waiting for it (to a free depot signal, else `FindParkingStation`). A train still moving is never
  parked where it is (autopilot off while under way leaves others planned around a train nobody drives): with no
  station free of trains it drives to the nearest other station. Depot drives park only once arrived. Station depots (`FSTStationConfig::bDepot`,
  `/st station depot`) were removed in 0.19.0; the saved `mStations` of older saves is ignored on load.
- **Waiting room** (`FSTTravel::WalkApproach`, `GetWaitingRoom`): from each station's platform end, the walk follows the
  track out until the first switch (a connection with more than one track), a track end or another station, adding
  the length of track not owned by platforms. Room = that length / (train length + 5 m). The queue limit is the smaller
  of **Most trains waiting before a station** and the room, so no trip is planned that would wait on more track than
  there is. A depot is no special case: a parked train books its station like any train, and a short siding has no
  room, so trains coming to unload into a depot's sink and trains coming to park there never queue behind each other.
- **Junctions:** the switch where the walk ends (`GetJunction`) is shared by stations reached through it; `CountNear`
  (stop cost) counts other trains booked at any of them, so trips spread over stations behind one switch.
- **Sinks per car** (`CarHasConsumer`, `ItemHasConsumer`): cargo on board may go to a sink only if that car can't unload
  it at any consumer of the item the train can reach (whichever way it docks there); a full supplier's goods only if no
  consumer of the item exists. `Simulate` rejects a sink stop that breaks this. (0.7.7's one-hour rule let cargo
  through whenever its consumers were full for now.)
- **Loads capped** (`Simulate`): after a valid simulation, what a car would still hold of an item loaded on this trip is
  taken off its pickups (`Caps`) and the trip simulated again, so cars load only what the trip unloads.
- **Recipe rates and supply chain** (below).
- **Snapshots** (below).

### Recipe rates and supply chain

- The machines come from `FSTPlatformConfig::Machines`, kept when the platform was set up from its belts and pipes
  (Autodetect, station set-up, migration). Until 0.14.0 a background trace (`StepTraces`, one platform a second, each
  again after 10 minutes) kept them, not saved.
- `BuildNodes` gives each node the union of its platforms' machines and `RecipeRate`: what their recipes make (supply)
  or use (demand) of the item at their clock speed (manufacturers only; extractors count 0).
- The flow plan uses the recipe rate where a measurement is only a lower bound: a starved consumer (before: measured x
  1.5) and a full supplier (on top of offering its stock).
- `LinkSupplyChain`: a consumer that has stock but shares a machine with another consumer that is starved can't use
  what it gets; its `CostScale` drops to `WaitingCostScale` (0.25) and `WaitsOn` names the missing item (monitor
  "Supply chain" lines). Example from the server: Uranium Fuel Rod RSC 1A waiting on Encased Uranium Cells while that
  cell's maker waits on sulfur.

### Snapshots and replay

`/st snapshot` sets `bSnapshotNext`; the next `GroupRound` adds each free train's `DescribeForReplay` (cars, cargo,
platforms, dock maps, start) after `BeginTrain`, and `WriteSnapshot` writes them with the nodes, bookings, travel
times, stays, items (form, stack) and settings to `Saved/SmartTrains/Snapshots`. The test `SmartTrains.Replay.Snapshots`
rebuilds that situation (Docs/Testing.md) and plans each train again.

## How trains are controlled

SmartTrains never drives a train. It writes the managed train's timetable for each job and lets the game (and Auto
Trains, if installed) drive and dock:

- Pickup stop: *fully load*, leave after **Pickup: longest wait** at the latest (`IsDurationAndRule = false`), load
  filter = the job's item, unload filter = `[None]`.
- Delivery stop: *fully unload*, leave after **Delivery: longest wait** at the latest, unload filter = the item,
  load filter = `[None]`.
- Drive-only stop (depot, moving out of the way): *load/unload once*, no wait, both filters `[None]`.

**Stays end when the work is done** (`ReleaseDockWhenDone`, every 0.25 s while a managed train is docked):
- The stay ends at once when every car has moved its planned amount for this stop.
- After the first 30 s, it also ends when nothing has moved for 20 s, e.g. a platform with no room left.
- It ends through the station's `ClearRuleSetTimeRequirement`: the stop's time drops to 0, so "fully load/unload,
  or N s" lets the train go. Logged as `DOCK <train> at <station>: ... the stay ends now`.
- Stops are also written with `IgnoreFullLoadUnloadIfTransferBlockedByFilters`. A car only moves its filtered items,
  so cargo kept for a later stop no longer holds up "fully".

Before this, a train that unloaded what the platform had room for waited out the whole delivery wait. With the
docking sequence that came to 3-4 minutes. Meanwhile it blocked the station for the trains behind it and locked its
platforms and locomotive menu. The long stays also made the learned stay times, and every trip estimate built on
them, far too long.

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

**No mixed cars** (trip planner, `Simulate`): a car comes out mixed only when an empty car loads several items at
one stop from a platform with several items. The planner counts it as harm, so with Freight Car Control the car
skips the stop and without it the trip is refused, when:
- a car holding any item (`HoldsOtherItem`) stops beside a loading platform that would give it a different one,
  even into free slots;
- a car stops beside an unloading platform and holds anything the stop doesn't unload there. The platform would
  take all of it.

A configured platform only gives its own items: stray stock of another item isn't planned as a pickup. Before
2026-11-06 a car with one free slot beside its aluminium ingots took a stack of concrete, so it would have unloaded
aluminium into the concrete consumer.

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
   time, unless the supplier is nearly full. Any-item sinks only take from nearly full suppliers, or unwanted cargo.
5. **Trips** (`GroupRound`, "0.10.0: item groups"): home work first, then any work; each trip's amounts and station
   stays are reserved before the next train is planned.
- A trip never starts at the station the train is docked at, stands in, or left in the last 60 s
  (`JustLeftAnchorSeconds`) unless it stands there parked.
- For 10 s after a timetable write, a current stop the game moved on by one (the departure that followed the write)
  is set back to the job's leg (`TIMETABLE` log line).
6. Parked trains move to a depot, or else to the nearest station no trip uses, when a trip needs their station, or
   (with Auto Trains) when a train has waited 20 s for them (`BLOCKING`).
   - With Auto Trains the way out is checked first (`ExternalCheckLeg`). If it isn't clear (`REROUTE`): that parking
     station is skipped for `RerouteAgainSeconds` (`FSTTrainState::ExitBlocked`), so the next round tries the next one;
     the station is offered to no new trip for as long (`mBlockedStations`, skipped in `GatherRound`); and the trips
     heading there end (`EndJob`), so their trains get a trip elsewhere or park out of the way and clear the door. With
     every way out blocked it waits; each report is logged once (`LastExitReport`). Before 0.22.2 those trips re-planned
     "from scratch", but only with deliveries to the same station, so they came straight back (and a train that couldn't
     get out logged the same line every 3 s for hours). An unreachable station ends the trip the same way.
   - A parked train books its station for 60 s. In a dead-end station (`IsDeadEnd`: no station reachable past its
     building) it books it until it leaves, since a train sent there could neither get in nor let it out.

### Forecast cost (`STForecast.*`, 0.7.0)

Every node's stock is forecast over the next hour (`HorizonSeconds`): piecewise linear between the events planned for
it (`FSTNode::Planned`: each job leg's arrival and planned amount; `Reserve` /
`Release` keep it in step with the reservations), at its use or production rate (`GetRate`; a nearly full supplier
can't measure production, so at least a car load per half hour), clamped to 0..capacity.

- **Demand cost**: priority weight x the integral of max(0, target - stock) over the hour, in car-load minutes
  (amount / car capacity for the item, so a car of sulfur counts like a car of concrete).
- **Supply cost**: production lost while full, x `LostLoadMinutes` (0.25 per car load): a tie-breaker only. The server
  log of 2026-11-07 had most suppliers full with nobody needing their goods; weighting that like demand would send
  trains to haul production to sinks instead of serving short platforms.
- **Value of a stop** (`Simulate`): the drop in the node's cost when the delivery or pickup is added at its arrival
  (`Gain`), on top of the events planned by other jobs and by earlier stops of the same trip. It replaced the urgency
  and fairness weights: a delivery that ends a shortfall sooner is worth more, one to a platform that stays above its
  target within the hour is worth nothing. Moves only go to consumers that fall below target within the hour
  (`Deadline`). Trips still score value / time; the send-at batch rule stays.
- **Automatic target (0.30.0)**: a demand item's `TargetPercent` 0 means automatic (the default; on loading an older
  save, targets at the old default 50 % become 0, `mTargetsVersion`). `ApplyAutoTargets` (end of `BuildNodes`) sets the
  node's target to its use (`GetDemandRate`: measured, or its machines' recipes when starved or still measuring) over
  the lead time, x `AutoTargetMargin` 1.25, as a share of capacity, kept within 10-90 % (50 % while no supplier is
  reachable). Lead time: from the nearest supplier of the item there and back plus a stay at each end
  (`EstimateTravelSeconds`, `EstimateStaySeconds`), cached per station and item for 5 minutes (`mLeadTimes`). Why:
  a fixed 50 % meant very different times per item: on the IB save a High-Speed Connector factory's Silica platform
  (9,600, at 324/min) counted as short with 15 minutes of use left, while its Copper (4,800, drained fast) counted only
  when nearly gone. Manual targets (1-100 %) still apply; the platform menu steps below 10 % to automatic.
- **Until the next train (0.29.0)**: a delivery's gain counts only the shortfall it prevents until another train
  could bring the item (`NextDeliveryTime`: one more trip as long as this one, at least `NextDeliveryMinSeconds`,
  10 minutes, after its arrival); pickups still use the whole hour. Over the whole hour a top-up looked valuable for
  any platform that would dip below target some time in it, as if no train would come before then, so trips detoured
  for slow items: on the IB save a High-Speed Connector trip went Copper -> Caterium -> Silica with Caterium about 18
  minutes from its target at 63/min, delaying the copper the factory was starving for. Slow items now get a stop when
  they would run short before the next train, i.e. close to their target and nearly a car load at a time.
- **Queue limit look-ahead** (0.7.4): other trains' bookings count towards **Queue trains** only for arrivals within
  `QueueLookSeconds` (5 minutes); later arrivals still wait for bookings but aren't rejected. In the 0.7.3 log trains
  stood still only 3 % of their trip time, while 292 trips were rejected for "would queue at RockPow- Sulfur SNK" in
  35 minutes: bookings of long plans 20 minutes ahead, whose times change anyway.
- **Least load** (0.7.5): a car loads an item only if it takes at least `MinLoadShare` (a twentieth) of a car of it.
  112 of 720 planned car transfers in the 0.7.4 log were 200 items or fewer, often top-ups like "loads 0 of 26".
- **Slow-draining platforms** (0.7.4): an unloading platform that took less than half of what was planned
  (`SlowDrainShare`) counts no drain in its predicted room for 30 minutes (`mSlowDrainUntil`, SLOWDRAIN lines). Room
  assumed the node's use drained each platform in proportion to its stock; RockPow Coal P1 sat full while P2 drained.
- **Stop cost** (0.7.3): each stop costs `StopCostValue` (1 car-load minute), once more for every other train booked
  at that station within `StopNearSeconds` (2 minutes) of the visit. The server log of 2026-11-08 showed why: value
  counted only how full each car got, so three trains bringing a car or two of sulfur each scored like one train
  bringing four, while each visit held the station and the others queued (98 trips rejected for the queue at RockPow
  Sulfur SNK in 40 minutes). A stop now has to carry enough to be worth holding its station.
- **Tied cars** (0.7.3): a car still holding cargo after a trip can only take that item next. Each car's change in
  `GetTiedShare` (0 empty, `NeededSoonShare` 0.25 when every item it holds is wanted by a demand platform within the
  hour (`mNeededSoon`), else 1) is worth `TiedCarValue` (2 car-load minutes): a trip that empties a car gains it, one
  that leaves leftovers pays it. It is a cost, not a rule, so no train detours just to dump a few items.

**Commitment** (0.7.1): a re-plan after a stop replaces the rest of the trip only if it scores `ReplanThreshold` (1.1)
times as much and is also worth `ReplanMarginValue` (5 car-load minutes) more, so small changes in the estimates don't
switch trips.

### Trip planning (`STTripPlanner.*`)

**Queue limit** (`QueueTrains`, 2 trains; only while `LeaveAtOnce` is on): when a stop's arrival is pushed back by
other trains' bookings or Auto Trains' station queue (`FirstFreeArrival`), the train waits on the line before the
station, and so do the trains ahead of it except the docked one. `Simulate` fails a trip ("would queue at X behind N
trains") when the trains ahead (`CountAhead`: bookings still there when it could get there, or docking before it can)
are more than the limit. The waiting itself isn't limited: it counts as trip time, so it lowers the score. (Until
0.6.8 the limit was a time, 60 s: each train ahead adds its stay and the arrival gap, so it meant one train ahead.)

**Unreachable stations** (0.7.6): when the game reports no way to the station a managed train is heading for (no
path, station unreachable, or unreachable with signals) for `UnreachableSeconds` (20 s), that station is left out for
that train for `UnreachableMinutes` (30) (`FSTTrainState::Unreachable`, checked in `CanServe`, so trips, depots and
depots all skip it), with an UNREACHABLE warning. A train driving to park settles again (another depot); a train on a
trip is planned again from the cargo on board. SmartTrains' own track
search doesn't know signals, so it can't see this beforehand; the game's pathfinder refuses a way only where it passes
a one-way signal from behind. In the 0.7.5 log a train drove for 17 minutes towards a depot the game called unreachable
with signals; the cause was the depot station built backwards: arriving in its direction meant driving the main line
against a one-way signal. The warning names station facing first.

**Delivery reservations per platform:** like pickups, a planned delivery records its platform. `GetPlatformRoom` is
the platform's free room plus what its machines use before the train arrives, less the deliveries promised to it and
its share of promises made at no known platform. Test: `SmartTrains.Planner.DeliveryReservations`.

**Trip times:** `LearnTrip` keeps a moving average (30% per trip). Once a pair has two samples, one sample counts at
most 1.5 times the estimate (`TripOutlierFactor`), so a trip held up in a queue doesn't skew the next plans.

**Supply reservations per platform:** a planned pickup records the platform the car loads from (`FSTCarWork::Platform`,
saved; `FSTPlanCarStep::LoadPlatform`). The supply node keeps those amounts per platform (`FSTNode::PlatformReserved`)
besides the station total (`Reserved`). `GetPlatformAvailable` gives a platform its stock and its share of the rate,
less the pickups promised there and its share of the promises with no known platform (jobs from older saves).
`GetAvailable` is the sum over the station's platforms. Before, every promise was shared out over all platforms, so
trains that could only reach platforms 1-3 used up platform 4's stock as well. Test:
`SmartTrains.Planner.PlatformReservations`.

A trip is a list of stops (at most **Most stops per trip**, each station once, and with more than two stops no longer
than **Longest trip**: the limit only stops SmartTrains combining stops, a direct trip is never too long),
each with the items loaded and unloaded there. These become the stop's load and unload filters.

**Simulation** (`Simulate`): the stops' times come from travel estimates and stays, after the bookings of other
trains. A booking (`FSTBooking`) runs from a train's arrival to the end of its stay. A train docked for its current
stop books from when it docked; one running late books from now. `FirstFreeArrival` finds the first slot that keeps
**Gap between trains at a station** to every other booking there. Then each car, stop by stop, beside the platform it meets there (from the dock maps):

- At a loading platform, a car loads every filter item the platform holds or is set to supply, if it has room.
  Freight cars hold solids or one fluid, never both. Loading from a platform without a supply role is harm.
- At an unloading platform, a car unloads every filter item it carries. The platform must be a demand platform
  that takes all of them (or an any-item sink); otherwise it is harm.
- If the possible dock maps disagree, or the car would do harm, the car skips the stop through Freight Car Control.
  Without FCC the trip is invalid. Maps that put a car beside different platforms agree when those platforms are
  equivalent (same role, mode, form and items), as at a push-pull shuttle's station. Each dock map keeps its own
  platform pools and room (`PoolKey`). A car plans the smallest of its amounts over the maps, and that amount comes
  off its platform in every map.
- Amounts come from the platform's stock plus its share of the node's production up to the end of the stay (a fluid
  platform fed by pipes keeps filling while the train loads), less what other trips reserved. Unloads are limited by
  the platform's free slots (a fluid platform also by the rest of its one slot; a train doesn't top up part-full
  solid stacks). A car unloads at least 2 car slots of an item, or all it holds of it when that is less; a smaller
  amount counts as nothing, since the stay ends at once when the platform has no room the moment the train docks.
- A car's room comes from its wagon type (`STLayout::GetCarCapacity`): slots × stack for a solid, the wagon's fluid
  capacity for a fluid. Its inventory now can't be used: a car set up for a fluid has one big slot.
- A train docked at the last stop of its trip is planned only once it leaves, if that stop moves cargo
  (`GetFreeAt`): platforms keep loading and unloading while it is docked, so what the stop moved is known only
  afterwards. Its next trip starts from what the cars really hold. A last stop that moves nothing is planned while
  docked, as before.
- Cargo loaded on the trip that no later stop unloads is stranded: with FCC that car skips the pickup and the
  simulation runs again; without FCC the trip is invalid.
- **Batch:** per item, what loads must reach the send-at share of the loading cars' room, unless a critical
  consumer is served or a supplier is nearly full.
- **Value** of each delivery: priority weight × urgency × the share of a car it fills (the amount up to the
  consumer's room ÷ the car's capacity for that item). Only how full the car is counts: 100 sulfur is worth 500
  concrete, and a full car of fluid is worth a full car of any solid. Urgency is 1, plus up
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
planned amount). With FCC, every car without work at a stop skips it. A stop is "load/unload once" in three cases: it only unloads fluids (a fluid car empties in one transfer); it both
loads and unloads; or a later stop moves the same item (a first pickup or a first drop of a split load).
Otherwise it waits for full or empty cars, up to the load or unload wait.

**Re-planning** (`ReplanTrip`): after every stop (`bReplan`) and after a platform setting changes (`bReplanAll`).
The rest of the trip is checked without the trip limit, and additions may make the trip as long as its rest already
is, so a running trip is never dropped for being long.
The rest of the trip is simulated again from the cargo really on board, with its own reservations released first.
Items that would move nothing and empty stops are trimmed, then regret insertion may add moves, such as delivering
leftovers on the way. If the rest no longer works at all, it is planned again from the cargo on board. If nothing
is left to do, the trip ends and the train is settled when the round ends (next trip, depot or park).

**Sinks** (`ESTRole::Sink`, 0.6.9): a sink platform's config has one rule without an item. `BuildNodes` makes it a
Demand node with `bAnyItem`.

**Unwanted cargo:** a free train carrying cargo that no trip delivers is marked (`UnwantedCargoSince`). After
**Unwanted cargo after** minutes, its cargo may go to consumers above target or to sinks. `/st status` warns when
more than half the managed trains carry unwanted cargo.

**Sinks keep what is needed** (0.7.7): a sink only gets an item no consumer needs within the forecast horizon
(`mNeededSoon`): move generation skips sinks for such items (leftovers and full suppliers alike), and `Simulate` rejects
a stop that would unload one into a sink (kept stops of older plans included). "Unwanted" only means
no stop of the current trip unloads the item, so before 0.7.7 every leftover went to the sinks once the depots on the
server became reachable: in 35 minutes 25 sink stops destroyed 19,500 coal, 11,000 concrete, 7,000 copper sheets,
2,300 sulfur and 2,000 AI Limiters while their consumers waited, and trains queued onto the main line for the depot
stations the sinks were in. Emptying a car also gains the tied-car value, which made dumping score above delivering
later.

**Flow plan** (every **Flow plan interval**): for each item, consumers are taken by priority tier, highest first.
Each tier is an exact **minimum-cost flow** (`STMinCostFlow`, successive shortest paths with Bellman-Ford). Source
→ supplier edges carry what is left of each supplier's rate, supplier → consumer edges cost the travel time, and
consumer → sink edges carry the consumer's rate. Starved consumers count at 1.5 × their measured rate, and at least
1 per minute. A supplier at least `FullSupplyShare` (90 %) full also offers its stock spread over `FullStockSeconds`
(30 minutes), 0.7.5: a full platform takes nothing from its belts, so its measured rate is only what trains took. In
the 0.7.4 log Coal SRC A was full 94 % of the time and counted 207/min against 685/min of demand, so the flow plan
saw coal as 0.2 trains and coal went a few hundred at a time on other trips. Later tiers get what is left. Each
route with a train that fits it becomes an `FSTFlowRoute` of its item's group: trains needed = flow × round trip ÷
average load of the managed trains that fit it.

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

## Auto Trains

Optional, by reflection (`STAt` in `STBridges.*`, Auto Trains' `Docs/API.md`). The actor of class
`ATTrafficSubsystem`, through the subsystem actor manager.

| Call | Used for |
|---|---|
| `ExternalTimeTableChanged` | After every real timetable write (`WriteJob`). |
| `ExternalGetWaitingFor` | `ClearNeededBays`: a parked managed train moves when a train has waited 20 s for it. |
| `ExternalGetStationQueue` | Each dispatch round, for stations with roles (asked every 15 s per station): a booking from now until the queue's last train should have left. Covers trains SmartTrains doesn't manage. |
| `ExternalEstimateTravelSeconds` | `EstimateTravelSeconds` when SmartTrains' own search finds no way (cached 10 minutes; logged as `TRAVEL`). |
| `ExternalCheckLeg` | Holding (dispatch step 7): before a trip of a train standing in a station is committed, its first leg is checked. Pending: try again next round. Blocked: hold, with Auto Trains' reason. Clear, or can't be asked: commit. |
| `ExternalGetTroubledTrains` | The monitor's Trains tab: deadlocked, stuck, lost or stalled trains. |

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
  - The monitor view goes unreliably in one message, trimmed to about 48 KiB (`FitMonitor`). The details of the
    longest lists' last rows go first, then those rows, with a warning in the log.

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
| `Widget_TrainStationNew` | `VerticalBox_0` (first tab, under the name plate) | `USTStationDepotBar` (depot switch only) |

Panel names and positions come from the starter project's widget assets, dumped with Telescreen's `TSDumpWidgets`
commandlet. It now also lists named-slot content, canvas slot layout and size box overrides
(`Tools/Spikes/SmartTrainsUI` holds the output). The platform panel sits right of the vanilla layout in an
auto-sized canvas slot, so the canvas, and with it the window, grows wider. The panel logs its geometry half a
second after it opens, to check that.

- **Platform window** (`USTPlatformWindow`, the `USTPlatformPanel` content in a window on the viewport). The launcher beside the platform menu shows the role, items and one readout line, and opens it. The window keeps the input mode as the platform menu set it, so the menu behind it works; Close, Escape, or closing the platform menu close it. Its panel reads the platform and the font from the menu (`USTPanel::SetMenu`).
- **Platform panel.** One role button (Off / Supply / Demand / Sink; click steps forward, right click back); per item: priority (click cycles, right
  click goes back), least per visit or target in 10 % steps, and a manual rate (m³/min for fluids). Sinks have no item list.
  The item picker lists the items on the platform first, then every item that available recipes make plus raw
  resources, of the platform's form. Copy / Paste use the player's SmartTrains clipboard (below), and under them
  "Copy to the station's other platforms" gives every platform of its kind in the station these settings once; all
  three carry role and items.
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
5. suppliers nearly empty half the time, or supply rate under 90 % of demand: "Supply short"; consumers starved half
   the time while suppliers aren't full and one freight car of the item takes over 30 minutes of supply to fill: "Low
   supply" (starved consumers use nothing, so their demand can't be measured);
6. suppliers full 30 % of the time (or the flow plan needs more trains than fit) while the fleet is busy 80 % of the
   time: "Trains short";
7. one of its stations congested (a train docked 85 % of the time, or arrivals 120 s late on average): "Congested";
8. otherwise "No trips planned": supply and trains are there but trips aren't made, so the idle reasons tell why.

Starvation or congestion: each demand sample records whether a delivery to it is on its way (`OnTheWay`: the node has
reserved deliveries). A demand group (a station's platforms for one item) with a platform empty for 30 s counts as starved in that sample, and `AllEmpty` when all of them are empty. One starved half the window is "Starved", or "Partly starved" unless all were empty half the window; then ": trains held up" when a delivery was on its way
for at least half of the time it was empty (congestion; with the station's average lateness), else ": no
trips" (starvation; it points at the item's verdict). The item's details give the same split over its consumers.

The window has six tabs:
- **Overview:** tiles (item groups, groups short of trains, managed trains, busy share, idle trains, limited items,
  congested stations), what limits deliveries with each row's advice, the fleet lines and the idle reasons.
- **Groups:** every item the flow plan routes, with trains needed, trains working on it now and routes; details
  list its routes and the trains moving it. Short groups first.
- **Items:** a table with icon, limit, supply and demand per minute, trains needed / fitting, and time below target.
  It can be filtered to items that need a look.
- **Stations:** the docked share, arrivals and average lateness of every station with roles.
- **Platforms:** starved and backed-up platforms.
- **Trains:** every managed train with its state, last station and trip or idle reason.

Tab headings count the rows that need a look. The view (`FSTMonitorView`) goes to `USTMonitorWindow` through `Server_RequestMonitor` / `Client_Monitor` once a second
while the window is open. `/st monitor text` and the server console print the same report, and it is logged as
`MONITOR` lines every 5 minutes while trains are managed.

**Monitor additions (0.7.2)**: **Unreachable** rows (`AddUnreachable`) for supply platforms holding stock and demand platforms that ran
out that no managed train has a car of their kind beside, in any way it docks (learned or predicted dock maps), such as
Nitric Acid SRC P2/P3 on the server; and a **Fleet saturated** line when trains are on trips 85 % of the time or more.

**Congestion (0.7.3)**: a driving train slower than `StandingSpeed` (1 m/s) counts as standing still (queueing for a
station or held at signals). Busy means moving on a trip or at a stop; standing is shown apart. **Congested** shows
when trains stand still on trips 15 % of the time or more (`StandingShare`); **Fleet saturated** only when they are not
congested and busy 85 % of the time or more. 0.7.2 counted standing as busy and said to build more trains while the
fleet queued at a few stations.

## Dock maps

`GetDockMaps` returns the vehicle beside each platform for every way the train may dock:

- Learned maps (`LearnDockMap`, one per way round) are used while the train has the same vehicles in the same order
  (`FSTDockMap::Consist`). A map records the locomotive that docked (`FSTDockMap::Locomotive`, the train's lead), and
  replaces the prediction for that locomotive. Maps from older saves are told apart by their first docked vehicle.
- Ways round not seen yet stay predicted, from the vehicles behind each locomotive that can dock.
- If a learned map matches no prediction, only learned maps are used for that station.
- A car is planned at a stop only if it meets a suitable platform in every map. Otherwise FCC skips it, or the trip
  is invalid without FCC. The game reads a stop's rules when the train docks, so a wrong guess can't be fixed
  during that dock; the mismatch is logged and learned, and the next re-plan works from it.

## Docking

A train's dock begins in `NoteDockStarted`, called by an SML hook after `AFGBuildableRailroadStation::StartDocking`
(`STDockHooks.cpp`, server; it returns a bool, so it is safe on Linux servers). `MonitorTrains` checks `IsDocked`
every 0.25 s for the end of the dock, and as a fallback for a start the hook missed (logged at Verbose). Until 0.6.8
the check alone found docks: one with nothing to move lasts about 0.3 s, a few frames on a busy server, so some were
missed, the job's leg stayed a stop behind, and the next timetable sent the train past a stop ("legs were missed").
