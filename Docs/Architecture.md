# SmartTrains architecture

Satisfactory SML mod, game CL 513885. Prefix `ST`, log category `LogSmartTrains`. The design and its decisions are
in the plan (`C:\Users\benvr\.claude\plans\smarttrains.md`); this file records how the code implements them.

## Pieces

| File | What it does |
|---|---|
| `STTypes.h` | Saved data: platform settings (`FSTPlatformConfig`), station settings, train state and jobs, learned dock maps, trip and stay times, the migration undo record. |
| `STSubsystem.h`, `STSubsystem.cpp` | `ASTSubsystem` (server only, saved with the world): settings API, train monitoring, docking events, learning, rate sampling, readouts. |
| `STDispatch.cpp` | The rest of `ASTSubsystem`: supply/demand nodes, the fit model, dispatching, the flow plan, writing timetables. |
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
beside a Demand platform that accepts X. It must also be empty, and the cargo type (solid or fluid) must match. A car
that would **stray** sits at S beside a load-mode platform that has or supplies X but doesn't fit at D:

- with Freight Car Control the job sets it to Skip at S;
- without it the job is refused (decision 3 in the plan).

A loaded train only gets delivery jobs for what it carries. Cars carrying X that aren't beside a Demand platform for
X at D are handled the same way: skipped with Freight Car Control, otherwise the job is refused if they would unload
into another platform.

## Dispatching

Every **Dispatch interval**, and as soon as a train docks for its last leg or leaves a stop without a job:

1. **Nodes:** one per (station, item, role), summing its platforms. Each node holds stock, capacity, rate (measured
   or manual), whether it is starved, its priority, target and send-at. `Reserved` holds amounts promised to
   running jobs.
2. **Free trains:** managed trains with no job; trains driving to a depot or out of the way; and trains docked for
   their last leg. A train is free from its station at the time its stay should end.
3. **Candidates:**
   - For a loaded train: deliveries of each carried item.
   - For an empty train: every supplier → consumer pair of the same item. The supplier must have at least a batch
     (send-at share of the fitting capacity) by the time the train gets there. The consumer's projected stock at
     arrival must be below target, unless the supplier is nearly full.
   - Any-item sinks only take items from nearly full suppliers.
4. **Score** = priority weight × urgency × flow-plan factor × amount ÷ time to the end of the delivery.
   - Priority weights: Low 0.5, Normal 1, High 2, Critical 8.
   - Urgency is 1, plus up to 2 for the share below target, plus 2 if starved; at least 1.5 when the supplier is
     nearly full.
   - The plan factor is 1 for routes in the flow plan, 0.6 otherwise, and 0.1 for sinks.
5. Greedy assignment by score, rechecking supply and room after each assignment. Station bays are booked by arrival
   plus expected stay, and a later arrival waits behind them.
6. Parked trains standing at a station a job needs are sent to a depot, or else to the nearest station no job uses.

**Flow plan** (every **Flow plan interval**): for each item, consumers are taken by priority. Within a priority,
the cheapest open supplier → consumer route by travel time is filled repeatedly, a greedy transportation plan.
Starved consumers count at 1.5 × their measured rate, and at least 1 per minute. The result gives the plan factor
and the fleet advice in `/st status`: trains needed = Σ flow × round trip ÷ average load of the managed trains that
fit that route.

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
| `Widget_Train_DockingStation` (freight platform) | `CanvasPanel_149` at x 448 | `USTPlatformPanel`, beside the window |
| `BPW_Train_DockingStation_Fluid` | `CanvasPanel_0` at x 765 | `USTPlatformPanel` |
| `Widget_Locomotive_Menu` | `VerticalBox_0` (info column) | `USTTrainPanel` |
| `Widget_TrainStationNew` | `VerticalBox_2` (under the train and station lists) | `USTStationPanel` |

Panel names and positions come from the starter project's widget assets, dumped with Telescreen's `TSDumpWidgets`
commandlet. It now also lists named-slot content, canvas slot layout and size box overrides
(`Tools/Spikes/SmartTrainsUI` holds the output). The platform panel sits right of the vanilla layout in an
auto-sized canvas slot, so the canvas, and with it the window, grows wider. The panel logs its geometry half a
second after it opens, to check that.

- **Platform panel.** Role buttons (Off / Supply / Demand); per item: priority (click cycles, right click goes back),
  send-at or target in 10 % steps, and a manual rate (m³/min for fluids). The item picker lists the items on the
  platform first, then every item that available recipes make plus raw resources, of the platform's form; "Any item"
  only on demand platforms. Copy / Paste use a clipboard inside the mod. "To station" is `CopyPlatformToStation`. The
  game's own copy/paste-settings key isn't hooked yet: the platform implements it in Blueprint.
- **Train panel.** Managed toggle, keep vanilla (only while unmanaged), pool tag, job legs and cargo. While the train is
  managed, the vanilla "Edit timetable" button is disabled on every refresh, and enabled again when the menu closes.
- **Station panel.** Depot toggle, pool tag, layout, fleet status. Depot and pool are per station, and a depot may
  have no platforms, so they stay in the station menu; everything per platform is in the platform menus.
- **Migration window** (`USTMigrationWindow`). Not part of any vanilla menu: `/st migrate` without a step sends
  `Client_OpenMigrationWindow` to the player, which adds the window to the viewport 0.2 s later (after the chat has
  closed and given the input back) and takes the input in UI-only mode. Close or Escape removes it and gives the
  input back to the game. A dimmed full-screen layer behind it catches clicks, so it keeps the keyboard focus. It
  runs the same steps as `/st migrate`. Its rows are the preview's stations and trains, ticked as proposed. Apply
  sends the ticked numbers. Without a test copy or backup this session, Apply takes a second click (the chat
  command's `confirm`).
