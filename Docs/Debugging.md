# Debugging SmartTrains

The log is `%LOCALAPPDATA%\FactoryGame\Saved\Logs\FactoryGame.log`; every SmartTrains line starts with
`LogSmartTrains:`. More detail: add `LogSmartTrains Verbose` (or `VeryVerbose`, which includes every rejected fit) under
`[Core.Log]` in `%LOCALAPPDATA%\FactoryGame\Saved\Config\Windows\Engine.ini`, or type `log LogSmartTrains Verbose` in
the console.

## Lines to look for

| Line | Meaning |
|---|---|
| `Subsystem ready` | Loaded; counts and whether Freight Car Control / DynamicTimeTable were found. |
| `PLATFORM ... -> ...` | A platform's role or items changed, and by whom. |
| `MANAGE` / `UNMANAGE` | A train was taken over or given back (saved stop count, autopilot). |
| `JOB n <train>: ...` | A job was handed out, with its score parts. |
| `WRITE <train> job n: [...]` | The timetable written, and whether through Freight Car Control. |
| `JOB n ...: docked at ... for leg i/n, +x s against plan` | Arrival, and how late against the plan. |
| `JOB n ...: left ... after x s, loaded/unloaded ...` | What was actually moved. |
| `JOB n ... DONE` | Delivered amount against plan, total time. |
| `TRIP A -> B took x s (estimate was y s ...)` | Travel time learning; big differences mean the speed estimate is off. |
| `PARK` | A train's autopilot was switched off where it stands. |
| `WOULD JOB` | Observe-only mode: the job it would have given. |
| `EXTERNAL TIMETABLE CHANGE` | Someone else changed a managed train's timetable; the job was written back. |
| `DOCK MAP MISMATCH` | A train docked with different cars beside the platforms than expected. The new map is kept. |
| `NO FIT` (VeryVerbose) | Why a train can't do a supplier → consumer pair. |
| `MIGRATION` | Preview, apply and undo summaries; the CSV path. |
| `Test copy` / `Backup` | Save copies and the session name being put back. |

## Known gaps (0.1.0)

- No in-game menu yet (plan M2); everything is done with `/st`.
- Each job has one pickup and one delivery (milk runs are M4).
- A train that is parked counts as standing at the station where it last docked.
