# Changelog

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
