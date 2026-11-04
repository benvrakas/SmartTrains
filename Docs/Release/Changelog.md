# Changelog

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
