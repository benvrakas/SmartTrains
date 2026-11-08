# SmartTrains

A Satisfactory mod (SML 3.12, game build 513885 or later): trains without fixed timetables.

Give freight and fluid platforms a role (supply or demand) and the items they handle. Managed trains are then sent by
stock, rate and distance from where items pile up to where they're needed. You don't need a train per factory. Train
length, car positions and platform positions are checked for every job. Everything is opt-in: unmanaged trains and
platforms without a role work as in vanilla.

Status: 0.7.5 beta ([changelog](Docs/Release/Changelog.md)). Settings are in the platform, locomotive and station menus, and in chat (`/st help`).

- Commands: [Docs/Commands.md](Docs/Commands.md)
- Moving an existing network over: [Docs/Migration.md](Docs/Migration.md)
- How it works: [Docs/Architecture.md](Docs/Architecture.md)
- Logs and troubleshooting: [Docs/Debugging.md](Docs/Debugging.md)
- Tests: [Docs/Testing.md](Docs/Testing.md)
- Notes from the unreleased betas: [Docs/DevLog.md](Docs/DevLog.md)

Highly recommended (both optional): Auto Trains 1.3.9+ (traffic without signals; SmartTrains checks a train's way
with it before it leaves) and Freight Car Control 1.1.0+ (cars can skip stops, so trips can use only some cars). Also
works with Modular Stations. DynamicTimeTable's station groups are read for migration; don't keep it installed with
managed trains.
