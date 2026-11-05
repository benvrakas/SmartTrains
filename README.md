# SmartTrains

A Satisfactory mod (SML 3.12, game build 513885 or later): trains without fixed timetables.

Give freight and fluid platforms a role (supply or demand) and the items they handle. Managed trains are then sent by
stock, rate and distance from where items pile up to where they're needed. You don't need a train per factory. Train
length, car positions and platform positions are checked for every job. Everything is opt-in: unmanaged trains and
platforms without a role work as in vanilla.

Status: 0.4.0 beta. Settings are in the platform, locomotive and station menus, and in chat (`/st help`).

- Commands: [Docs/Commands.md](Docs/Commands.md)
- Moving an existing network over: [Docs/Migration.md](Docs/Migration.md)
- How it works: [Docs/Architecture.md](Docs/Architecture.md)
- Logs and troubleshooting: [Docs/Debugging.md](Docs/Debugging.md)
- In-game tests: [Docs/Testing.md](Docs/Testing.md)

Works with Freight Car Control (optional: cars can skip stops, so trips can use only some cars), Auto Trains (traffic),
Modular Stations and DynamicTimeTable (its groups are read for migration; managed trains are protected from it).
