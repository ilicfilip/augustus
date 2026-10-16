# Agent control server

Lets an external program (e.g. an MCP server driving an AI agent) observe and control the game.

- Start the game with `--agent-port PORT`; it listens on `127.0.0.1:PORT` only. Without the flag nothing changes.
- One client at a time. Commands run on the main thread between frames (`agent_poll()` in the SDL2 main loop).
- Protocol: one text command per line in, one JSON object per line out. Every reply has `command` and
  `ok`; failures add `error`.
- The game is **paused** while a client is connected; time only advances via `step`.
- 50 ticks = 1 day, 16 days = 1 month, 12 months = 1 year.
- SDL2 platform only for now (`src/platform/SDL2/augustus.c`); the SDL3 entry point is not hooked up.
  Windows builds get no-op stubs.

## Commands

| Command | Reply fields |
|---|---|
| `ping` | `version`, `window` {id, name}, `in_city`, `paused` |
| `city` | `scenario`, `date`, `paused`, `finance`, `population`, `labour` (`workers_short` = unfilled jobs), `food` (stored, needed_per_month, supply_months, types_eaten, percentage_produced), `ratings`, `win_criteria` (enabled criteria only) |
| `map [x y w h] [layer…]` | `map_width/height`, region, `entry`/`exit` points, `layers[]` each with `name`, `legend`, `rows` (one string per y, one char per tile). Layers: `terrain` (default; terrain + building letters), `desirability` (tens), `water` (fountain/reservoir range) |
| `buildings [type]` | all non-house buildings (or only `type`, which may also be a house level): `id`, `type`, `x`, `y`, `size`, `workers`/`workers_needed`, `road_access`, `fire_risk`/`damage_risk`, `stock` (granary: units, 100 = 1 load; warehouse: cartloads), flags |
| `houses [x y w h]` | per house: `level`, `x`, `y`, `size`, `population`, `room`, `desirability`, `happiness`, water (`fountain`, `well`), `foods`, `stock`, `access` (service counters, 0 = none), `evolve_id` + `evolve` (the house window's "why it won't evolve" sentence), `worst_desirability_building` |
| `step [days=1]` | `days_requested`, `ticks_run`, `date`; `stopped_by: "window_changed"` + `window` if a popup/screen opened mid-step |
| `load <savegame>` | `scenario`, `date` — loads from the savegame directory, shows the city, pauses |
| `close` | closes a message dialog / message list / building info window and returns to the city |
| `pause` / `resume` | `paused` |
| `help` | `commands` |

`step` stops early whenever the city stops being the active window (e.g. a news message pops up).
Typical loop: `step N` → if `stopped_by`, inspect, `close`, step the remaining days.

Coordinates are map tiles (0..map_width-1, 0..map_height-1), the same in all commands. A building's
`x`,`y` is its top-left tile in that grid.

## Example

```
$ printf 'load Engineer.svx\nstep 30\ncity\n' | nc 127.0.0.1 7777
{"command":"load","ok":true,"scenario":"map3","date":{"year":-348,"month":5,"day":10}}
{"command":"step","ok":true,"days_requested":30,"ticks_run":1500,"date":{...}}
{"command":"city","ok":true,...}
```

## Files

- `agent.c` — socket handling and commands
- `observe.c` — `map`, `buildings`, `houses`
- `json.c` — minimal JSON writer (no external dependencies)
