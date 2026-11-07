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
| `STAccess.*` | Who may change what (admin-gated controls). |
| `UI/*` | The panels added to vanilla menus, and the controls they are built from. |

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
   time, unless the supplier is nearly full. Any-item sinks only take from nearly full suppliers, or unwanted cargo
   (below). Each move gets a solo score: the score of a trip made of that move alone.
5. **Exact assignment** of trains to first moves (`STAssignment::Solve`, the Hungarian method). Rows are free
   trains, columns the best six moves of each train, two slots per move, values the solo scores. This stops one
   train taking a move another could do almost as well while a move only it can do waits.
6. **Trips**, best assigned train first: the assigned move, then **regret insertion** (`Improve`), then `Trim`.
   Each trip's amounts and station stays are reserved before the next train is planned.
7. **Holding:** a trip is not started yet when the train is standing in a station (docked, or parked with
   self-driving off) and its first stop is free more than 20 s after the train could get there. The train waits
   where it is, and later rounds plan it again ("holding at ..."). A train queued on the line blocks the junctions
   behind it; one waiting in a station blocks nobody. **Off while "Trains leave stations at once" is on** (the
   default since 0.6.3), together with the Auto Trains way-out check before leaving: a train starts its trip at once.
- A trip never starts at the station the train is docked at, or left in the last 60 s (`JustLeftAnchorSeconds`)
  unless it stands there parked. A train with leftovers for a consumer it just served would otherwise dock there
  again at once, over and over.
- For 10 s after a timetable write, a current stop the game moved on by one (the departure that followed the write)
  is set back to the job's leg (`TIMETABLE` log line).
8. **Repeat instead of parking** ("Repeat the last job instead of parking", on by default since 0.6.3, `RepeatLastJob`):
   - A train left with no new trip runs its last trip again (`LastWorkJob`, saved): the whole trip as committed, if it
     picks up and delivers, from the stop after the one it is at, with planned arrivals from the travel and stay
     estimates. Only a train that never had such a trip parks.
   - The monitor counts repeating trains as free; their arrivals don't count towards station lateness.
   - A repeat (`FSTJob::bRepeat`, summary "again: ...") reserves no stock or room.
   - It isn't planned again after each stop.
   - The train counts as free in every dispatch round, so the first trip worth running replaces it.
   - Logged as `JOB <id> <train>: repeats its last job (...)`.
9. Parked trains move to a depot, or else to the nearest station no trip uses, when a trip needs their station
   (held trips included), or (with Auto Trains) when a train has waited 20 s for them (`BLOCKING`).
   - With Auto Trains the way out is checked first (`ExternalCheckLeg`). If it isn't clear, the train stays, and the
     trips heading there re-plan from scratch (`REROUTE`).
   - A parked train books its station for 60 s. In a dead-end station (`IsDeadEnd`: no station reachable past its
     building) it books it until it leaves, since a train sent there could neither get in nor let it out.

### Trip planning (`STTripPlanner.*`)

**Queue limit** (`QueueTrains`, 2 trains; only while `LeaveAtOnce` is on): when a stop's arrival is pushed back by
other trains' bookings or Auto Trains' station queue (`FirstFreeArrival`), the train waits on the line before the
station, and so do the trains ahead of it except the docked one. `Simulate` fails a trip ("would queue at X behind N
trains") when the trains ahead (`CountAhead`: bookings still there when it could get there, or docking before it can)
are more than the limit. The waiting itself isn't limited: it counts as trip time, so it lowers the score. (Until
0.6.8 the limit was a time, 60 s: each train ahead adds its stay and the arrival gap, so it meant one train ahead.) Repeats skip stations `IsStationQueued` (2 or more managed trains on their way, or an
Auto Trains queue), starting at another stop of the trip, or from the stop after the one it is at when all are queued.
A repeat is simulated like a new trip (`RepeatLastJob`: `BuildNodes`, `BuildBookings`, `BeginTrain`, `Simulate`) from
the cargo on board and the stations now; stops whose cars move nothing are dropped and it is simulated again. It isn't
run when nothing is left, the simulation fails (queue limit, batch), or another managed train's remaining stops are
the same stations. Repeats book nothing (`BuildBookings`) and reserve nothing, and a repeat ends at every stop it
leaves, so the train is planned again from there. When the repeat isn't run, the train goes to a depot if there is
one and tries again from there every 30 s (`RepeatRetrySeconds`). With no depot it keeps running its last trip as it
was until it gets a new one (`KeepRunningLastJob`, no simulation; like any repeat it books nothing, a new trip can
take the train at any time, and it ends at the next stop it leaves), so it keeps moving: a waiting train has self-driving off, and Auto
Trains treats it as standing there for good, so on a through line or single track it would block every train
passing.

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

**Requests** (`ServeRequests`, `FSTTripPlanner::PlanRequest`; platform setting `ESTRequest`, node `FSTNode::Request`).
Regret insertion adds one move at a time and keeps it only if the trip's score rises. A car that fits a consumer's
platform but holds another item is blocked: the pickup scores nothing alone, and emptying the car is added only if
that delivery is worth something itself. So a consumer whose only trains hold other cargo got none. After the round's
normal trips (whose reservations are in the nodes by then), a demand node asks (`GetRequestNeed`): **Automatic**
when stock + reserved − use rate × `RequestLookaheadMinutes` (10) is below its target, **On** while stock + reserved
is below 90 % of its capacity (repeats reserve nothing, so they don't count). Until 0.6.10 it asked only when
starved (empty 30 s) with no delivery planned at all. It also doesn't ask while `HasFittingTrain` finds a managed
train on its network (busy or not) with a car that `ComputeFit`, counting cargo, fits from one of its suppliers:
that train serves it by a normal trip. Unclaimed asking nodes, by
priority then fill, take the free train (no job, a repeat, or a no-work depot run) whose request trip delivers
first. `PlanRequest` takes up to 3 suppliers (nearest first) with cars that fit the supplier's loader and the
consumer's platform; their cargo of other items is unloaded at the 3 nearest consumers of those items and the 2
nearest sinks. It simulates none, one or two of them (both orders) before the pickup and the delivery. The trip that
empties the cars and delivers soonest wins, consumers and sinks alike; if none fits in `MaxStops`, the emptying alone
that leaves the cars empty soonest does (or, if none empties them, the one that unloads the most), and the train holds a claim (`FSTTrainState::RequestStation`/`RequestItem`, runtime only): it gets
no other trips (it is left out of the round's free trains) and `ServeRequests` plans its next part once it is free,
or drops the claim when the node no longer asks. Request trips (`FSTJob::bRequest`) run with a 60-minute trip limit,
are not re-planned after each stop (their emptying stops are worth nothing to a re-plan), and may get moves for
other cars by regret insertion as long as the delivery and the amount moved don't shrink. REQUEST log lines say who
serves which request, and why no free train can.

**Sinks** (`ESTRole::Sink`, 0.6.9): a sink platform's config has one rule without an item. `BuildNodes` makes it a
Demand node with `bAnyItem`, as "Any item" demand platforms were before; `PostLoadGame` turns those into Sink.

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
- **Platform panel.** One role button (Off / Supply / Demand / Sink; click steps forward, right click back); on
  demand platforms a request button (Automatic / On / Off, the same way); per item: priority (click cycles, right
  click goes back), send-at or target in 10 % steps, and a manual rate (m³/min for fluids). Sinks have no item list.
  The item picker lists the items on the platform first, then every item that available recipes make plus raw
  resources, of the platform's form. Copy / Paste use the player's SmartTrains clipboard (below), and under them
  "Copy to the station's other platforms" gives every platform of its kind in the station these settings once; all
  three carry role, items and request setting.
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
  - It has tabs: Overview, Stations, Trains and Last result. A station "to check" has a flag, or a platform with a
    conflict, no item, or low or unclear confidence.
  - The view carries each station's platforms (`FSTMigrationPlatformRow`: number, role, items, confidence, note,
    conflict), so the window shows item icons.
  - Ticks, opened stations, the tab and the filters live in the window. A fresh preview resets the ticks to the
    proposals and opens the Overview.

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
