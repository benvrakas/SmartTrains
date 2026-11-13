# SmartTrains: methods and emergent behaviour

State: 0.33.0 beta (groups from cargo, automatic targets; reviewed against the code and the IB server log of 2026-11-13). Architecture.md has the implementation details; this file lists the methods in one
place and the behaviour that comes out of them working together.

Status tags for behaviour: **seen** (in a test log), **designed** (intended, not yet seen), **risk** (possible
problem, not seen), **bug?** (seen symptom, cause not confirmed).

## 1. Methods

### Framing

| Method | Where | What it does here |
|---|---|---|
| Inventory routing problem (IRP), maritime inventory routing flavour | whole planner | Platforms are inventories with rates and capacities. Trains are vehicles with compartments (cars). The planner decides when, where and how much to move, not just routes. |
| Order-up-to level with minimum batch, like an (s, S) inventory policy | node targets, send-at | Demand platforms are served while below **target**. A pickup only goes when it fills the **send-at** share of the loading cars. |
| Inventory forecast with a shortfall cost (stock-out integral) over a one-hour horizon | `STForecast` (0.7.0) | Each delivery or pickup is worth the drop in its node's forecast cost: the shortfall below target over the next hour, in car-load minutes, weighted by priority. Replaces urgency and fairness weights. |
| Proportional allocation by highest averages (D'Hondt) | standard | 0.10.0 to 0.12.0 only (removed in 0.13.0): home groups for the trains in proportion to what each item group needs. |
| Rolling horizon (plan, act one step, plan again), as in model predictive control | `ReplanTrip` | Only the current trip is fixed. It is planned again after every stop and after any settings change, from the cargo really on board. |
| Anytime planning with a time budget | `IsOverBudget`, 25 ms per round | Planning stops when the round's budget is used. Work left over waits for the next round. |

### Algorithms

| Method | Source | Where |
|---|---|---|
| Discrete event simulation, car by car and stop by stop | own | `FSTTripPlanner::Simulate`: times from travel estimates and stays, then each car beside the platform it meets, with stock, production during the stay, reservations, slots and cargo type. |
| Regret insertion for pickup and delivery | Ropke and Pisinger (2006), the insertion heuristics of ALNS | `Improve`: inserts the move with the largest regret (best place minus second best) while the trip's value per second rises. |
| Minimum-cost flow by successive shortest paths with Bellman-Ford, one priority tier after another | Ford and Fulkerson; Busacker and Gowen | `STMinCostFlow`: the flow plan behind the item groups and the fleet advice. |
| Dijkstra on a directed graph of track exits | Dijkstra (1959) | `FSTTravel`: distances between stations, separately for departing forward, backing out (push-pull), and arriving at the far end (push-pull). |
| Exponential moving averages | standard | Rates (window = **Rate window**), trip times and stays (weight 0.3, first sample taken as is). |
| Observation overrides prediction | own | Dock maps: predicted from couplers and platform chains, replaced by what a real dock showed, per orientation and consist. |
| Unserved first, spare trains | own (0.23.0, 0.27.0) | A train choosing a car pattern first tries consumer stations no train serves; a spare train (empty, nothing to do, its group kept by another) moves to one at once. Replaced the urgency aging removed with the forecast cost (0.8.0). |
| Resource booking | own | `Reserved` amounts per node and `BusyUntil` per station, so trips planned later see what earlier trips promised. |
| Graph search over belts and pipes, with filter propagation | adapted from FlowDirector's trace | `STTrace`: from a platform to the machines and their recipes, through splitters, mergers, lifts, floor holes, smart splitters and Modular Load Balancer filters. |
| Evidence ranking with confidence | own | Migration: trace first, then platform stock, then timetable filters, then pipe contents. |

### Optional later (agreed, not built)

- ALNS (adaptive large neighbourhood search): destroy and repair whole trips, not only insert.
- ADP (approximate dynamic programming): learn the future value of leaving a train in a place.

## 2. Emergent behaviour

### How trips form

1. **Shortest trips win first** (designed). Score = value ÷ time, so near pairs are served before far ones of equal
   need.
2. **Far starving consumers catch up** (designed). Its shortfall cost grows the longer it is short, and trains
   choosing a pattern go to stations no train serves first (0.23.0), so a far consumer isn't left out.
3. **Milk runs** (designed). Regret insertion adds a second pickup of the same item when one supplier can't fill the
   cars, or a second drop when one consumer can't take it all.
4. **Mixed trips through one station** (designed). With Freight Car Control, cars for different items or stops skip
   platforms they have no work at. One trip can serve several platforms and items.
5. **Backhaul at the same station** (designed). A stop that unloads and loads becomes "load/unload once", so a train
   drops one item and picks up another in one visit.
6. **Trips grow and shrink on the way** (designed). After each stop the rest is planned again: stops that would move
   nothing are trimmed, and leftovers may be delivered on the way.
7. **Long direct trips, short combined trips** (designed, since aaa465e). The trip limit only applies with more than
   two stops. A single far delivery always works, but stops are combined only into trips shorter than **Longest
   trip**.
8. **No out-and-back to one station** (designed). Each station appears once per trip, so A → B → A is two trips.
9. **Trains spread over suppliers** (designed). Reservations make a supplier look emptier to trains planned after
   it, so they pick other suppliers.
10. **One consumer per trip** (seen, 0.12.0). Each trip ends at one consumer station; the train fills the cars that
    line up there, from one or more suppliers (gathering trips for factories with several inputs). On the IB save copy
    this delivered about 19 % more car loads in 20 minutes than 0.10's item groups (home groups and lending), from the
    same start, full or emptied trains: 86 against 73 and 80 against 67. Trips that delivered cargo went from 31 to
    52, each unloading fewer cars, since most consumers there take one item at one or two platforms.

### Inventory patterns

11. **Pulse deliveries around the target** (designed). A consumer is served below its target, filled past it, then
    left until it drops again. The target is automatic by default (0.30.0): its use over the time a train needs to
    bring more. A delivery counts only until the next train could come (0.29.0), so slow items aren't topped up early.
12. **Trains wait at slow suppliers** (designed). Pickups are "fully load" up to **Pickup: longest wait**. With a
    low send-at, a train may leave part-full and come back sooner.
13. **Full suppliers push** (designed). A nearly full supplier ignores the batch rule and may go to consumers over
    target or to sinks, so production doesn't stall.
14. **Fluid platforms count as filling during the stay** (designed, since aaa465e). The planner expects a pipe-fed
    platform to keep filling while the train loads, so it plans bigger fluid pickups.
15. **Unwanted cargo finds a home** (seen stuck, 2026-11-13). Cargo no trip delivers goes to a consumer of it or to
    a Sink platform; with neither reachable the train keeps it and can't take a new pattern (six open trains on the IB
    save held such cargo). A network needs at least one Sink platform per cargo type.

### Fleet behaviour

16. **Dedicated trains join the pool** (seen). Without DynamicTimeTable the migration ticks every train. A former
    shuttle takes whichever consumer's trip pays best, often its old route when its cars line up only there.
17. **Parking in a station, then making room** (seen). An idle train stops where its last stop ended, often inside
    a station. When a trip needs that station, the parked train is sent to a depot signal or the nearest unused station
    (log: "drive to Alclad Aluminum Sheet SRC (4680) and park (making room at Heavy Oil Residue SNK (640))"). Since
    0.22.2 an idle train doesn't park in a station trips are heading for in the first place.
18. **Eviction chains** (seen, 2026-11-13). A train sent away to make room may park in a station the next trip needs.
    Without depot signals 32 of 38 trains stood in stations; until 0.31.0 a parked train kept every trip that would
    arrive within a minute from being planned at a station with no waiting room, which stalled the network. Now a station a trip is kept from counts as
    needed and its parked train moves, also when it is the parked train's own pickup station (0.31.1). Depot
    signals prevent the shuffling. Moves because a train stood near a parked one happen at most every 5 minutes per parked train.
18a. **Parked trains give way** (designed, after 2026-11-05). With Auto Trains, a parked train moves when a train has
    waited 20 s for it, after checking its way out. In a dead-end station a parked train keeps the station until it
    leaves, so a second train isn't sent into a bay it couldn't enter.
19. **Traffic behind parked trains** (seen, with Auto Trains). Auto Trains holds a train before a track a parked
    SmartTrains train occupies, until the parked train has moved away.
20. **Train not at a station goes to the nearest one first** (seen). A train managed in the middle of the line
    drives to the nearest station, then gets trips from there.
21. **Learned times replace guesses** (designed). After one trip on a pair, its real time is used. One slow trip
    (traffic, a long wait) makes that pair look slow for a while (weight 0.3).
22. **Idle names carry the station** (designed, fixed after 2026-11-05). With JobNames on, an idle train is "ST: parked
    at <station>". It used to be plain "ST: parked" for all of them.

18b. **Trains wait their turn in a station** (designed, after 2026-11-05). A train whose first stop is booked holds
    where it is, and others are timed a gap apart. Fewer trains queue on the line in front of busy stations. Risk: a
    held train parked in a bay another trip needs gets moved (item 18). A busy station's trips get later arrivals and
    lower scores, so they spread to other stations.

### Push-pull and orientation

23. **Push-pull trains back out and may arrive from the far end** (designed, since aaa465e). Trains with a
    locomotive at each end can use routes a one-locomotive train can't.
24. **Unseen orientations restrict a train until it has docked both ways** (designed). Until then a car is only
    planned if it fits whichever way round the train arrives. At stations whose platforms differ along their length
    this can block trips without Freight Car Control.
25. **Push-pull water shuttles** (bug, partly fixed).
    - **Identical platforms, fixed (kept):** a push-pull train may dock either way round, so each car meets a
      different platform depending on the way. The planner treated that as "works differently" and skipped every
      car. After the fix the B shuttle got trips (job 29, all four cars planned), so the fix stays.
    - **Tiny fluid amounts, fixed:** fluid capacity came from the item stack (50 m3).
    - **Still open:** the A shuttle at Water SNK A rejects every station as unreachable, although the route search
      reaches two stations from there. The next log names them.
26. **Freight Car Control is the executor** (seen). SmartTrains sets per-car skips through `ExternalSetStops` with
    owner "SmartTrains", which locks the car rules in FCC's menus. Unmanaged trains keep the player's FCC rules.
27. **Without Freight Car Control a car can't skip a stop** (designed). Every car beside an active platform moves
    what the stop allows. Trips where a car would load or unload the wrong thing are rejected, so fewer trips
    work. Platforms keep their own settings either way (station linking was removed).
28. **External timetable changes are fought** (seen with DynamicTimeTable). A managed train's timetable changed by
    anything else is noticed within 0.25 s and written back. With DynamicTimeTable installed this looped; it is now
    uninstalled.
29. **Settings changes re-plan everything** (designed). Any platform edit re-plans all trips under way, so a change
    shows up at once but costs one heavy round.

36. **Empty-trip loops are cut** (designed, after 2026-11-05). With unwanted cargo allowed anywhere, a train could
    trip to a full sink, move nothing, and plan the same trip again every few seconds. A stop that moves nothing now
    bars that move for that train for 10 minutes.

### Planning limits

30. **Big networks plan in pieces** (risk). With the 25 ms budget, trains planned late in a round may get only
    their first move. The rest is added at later stops. A throttled warning logs when the budget runs out.
31. **Idle reasons with counts** (designed, after 2026-11-05). The idle message lists the three most frequent reasons
    trips were rejected, with counts. It used to keep only the last one, which hid the water shuttles' blocker.
32. **Throughput verdicts** (designed). `/st monitor` names one limit per item from the last half hour: supply, trains,
    fit, congestion, or "No trips planned" when supply and trains are there but no trips are made. A starved
    platform says "trains held up" (deliveries on the way but late: congestion) or "no trips" (none sent: starvation). A verdict needs a minute of
    samples, and the thresholds are shares of the window, so a short quiet spell doesn't flip it.

### Station set-up and migration

33. **What a platform holds decides its item** (designed, 0.15.1). The trace to the machines only gives an empty
    platform its item; platforms feeding different inputs of the same machines get different items, the most used one
    where the most belts go (lanes, 0.25.0). Timetables and train routes aren't used.
34. **One item per platform** (designed). A platform fed by a multi-input machine gets the largest traced flow; the
    platforms of one role reaching the same machines share the items out, so a factory's four input platforms get
    its four inputs.
35. **Direction from the belts** (designed). A platform with belts only going in loads, only coming out unloads,
    whatever its load/unload switch said; with both, the switch decides.

### Car patterns (0.18.0 on)

37. **Cargo decides the group** (designed, 0.33.0). A train holding cargo keeps its pattern and serves the stations
    that line up with it, or delivers what it holds; an empty train has no group and takes the best trip anywhere,
    unserved stations first. This replaced the idle clock and the homeless, spare and queued rules (0.26.0 to 0.32.0),
    which patched one another.
38. **Patterns taken from a half-set station** (seen, 2026-11-12) no longer matter once the train is empty: it has no
    pattern then.
