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
| `city` | `scenario`, `date`, `paused`, `difficulty` (very_easy…very_hard), `victory` (none/won/lost), `finance`, `population`, `labour` (`workers_short` = unfilled jobs), `food` (stored, needed_per_month, produced_last_month, eaten_last_month — units, 100 = 1 cartload — supply_months, types_eaten, percentage_produced), `ratings`, `win_criteria` (enabled criteria only), `labour_categories` (needed/allocated/priority) |
| `map [x y w h] [layer…]` | `map_width/height`, region, `entry`/`exit` points, `layers[]` each with `name`, `legend`, `rows` (one string per y, one char per tile). Layers: `terrain` (default; terrain + building letters), `desirability` (tens), `water` (fountain/reservoir range), `fire` / `damage` (per-building risk in tenths of the way to fire (>100) / collapse (>200); `-` fireproof, `!` burning) |
| `risks [min percent]` | buildings incl. houses at or above the threshold (default 60 %) of the way to fire or collapse, worst first: `at_risk` [{id, type, x, y, fire_percent, collapse_percent}], `burning` [{x, y}] (burning ruins), `burning_now` |
| `buildable` | building types available in this mission: `type` (name accepted by `build`), `cost`, `size`, `workers`, `desirability_by_distance` (effect at 1..range tiles, from the building model as map/desirability.c applies it) |
| `buildings [type]` | all non-house buildings (or only `type`, which may also be a house level): `id`, `type`, `x`, `y`, `size`, `workers`/`workers_needed`, `road_access`, `fire_risk`/`damage_risk`, `stock` + `stock_unit` (granaries and warehouses: cartloads; markets, temples, houses, workshops: units, 100 = 1 cartload), flags; unfinished monuments (oracles, large/grand temples, colosseum, …) have `construction` {phase, phases, halted, materials_still_needed, workcamps} instead of `stock`. Shared buildings (one record for all aqueduct tiles) aren't listed. Farms, raw-material producers and workshops have `production` {output, progress_percent, efficiency (% of normal; absent if under a month old), average_per_month, has_raw_materials (workshops), stockpiling, city_stockpiled} |
| `houses [x y w h]` | per house: `level`, `x`, `y`, `size`, `population`, `room`, `desirability`, `evolve_at_desirability` / `devolve_at_desirability` (this level's thresholds), `happiness`, water (`fountain`, `well`), `foods`, `stock`, `access` (service counters, 0 = none), `evolve_id` + `evolve` (the house window's "why it won't evolve" sentence), `worst_desirability_building` |
| `build <type> <x> <y> [x2 y2]` | places like the UI does (x,y = top-left, also for reservoirs — the game's own tool uses their centre) (same costs, rules, warnings). `type` = names from `buildings` (`housing` for housing plots); `x2 y2` drags roads, aqueducts, walls, highways, reservoirs, housing, plazas, gardens. Reply: `ok` (something changed), `cost`, `treasury`, `warnings` (the game's messages, e.g. "Must build on cleared land"), `at_target` (building there, or `tile` letter); fountains and bathhouses add `water` (inside / OUTSIDE every reservoir's range — the game's own warning also fires while a reservoir merely isn't full yet); a failed single-building placement adds `blocked` [{x, y, by}] — footprint tiles that aren't clear (map letter) or have a figure on them (`by` says which: a map flag is permanent, a walker or herd moves on). Roads/aqueducts/walls are routed around obstacles like the mouse drag; their reply adds `tiles`, `straight_tiles` and `detour`+`note` when the path is longer than the straight line; roads also report `new_intersections` (+`intersection_note`) when they add junctions within 3 tiles |
| `clear <x> <y> [x2 y2]` | clear land; same reply, plus `removed` [{type, x, y}] — every building touching the rectangle is demolished — and, when road tiles went, `removed_road_tiles`, `lost_road_access` [{id, type, x, y}] (buildings next to the rectangle that touched a road before and don't now), `road_note`. Clearing forts, monuments or bridges needs a confirmation the agent cancels (error) |
| `undo` | undo the last build/clear while the game allows (shortly after building): `refund`, `treasury` |
| `set tax <0-25>` / `set wages <0-100>` / `set priority <category> <0-9>` | new values; priority replies with `labour_categories` (0 = no priority) |
| `storage <id> [<resource> <accept\|get\|refuse\|keep> [4-32]]` | warehouse/granary orders (quantity in cartloads, steps of 4; `keep` = "retaining", only cart depots take from it) → `orders` (non-refused resources: "state quantity"), `permissions` |
| `storage <id> permission <name> <on\|off>` | who may take goods: market, traders, dock, quartermaster, worker, barkeep, caravanserai, lighthouse, armoury, workcamp, natives, caesar |
| `market <id> [<resource> <on\|off>]` | which goods a market (or other distributor) fetches → `fetches` |
| `roadblock <id> [<walker> <on\|off>]` | which roaming walkers pass (maintenance, priest, market, entertainer, education, medicine, tax_collector, labor_seeker, missionary, watchman) → `lets_through`; new roadblocks block all |
| `trade` | `resources` [{resource, stock (cartloads), can_produce, status (none/import/export/import_export), import_up_to, export_over, stockpiled, industry_mothballed, importable_now / importable_if_route_opened, exportable_now / exportable_if_route_opened, prices {import_land, import_sea, export_land, export_sea}}], `partners` [{city_id, name, open, available, route land/sea, cost_to_open, goods_to_open, sells_to_us / buys_from_us [{resource, per_year, traded_this_year}]}], `conditions` {working_dock, river_entry, land/sea_trade_disrupted} |
| `trade <resource> <import\|export\|stockpile\|mothball> <on\|off>` | import/export refused if no open route sells/buys it; turning export on un-stockpiles (game rule) |
| `trade <resource> <import_up_to\|export_over> <0-100>` | thresholds in cartloads (0 = no limit / export everything) |
| `open_route <city id>` | opens like the empire screen: checks treasury and `goods_to_open` in storage → `opened`, `cost` |
| `walker_preview <type> <x> <y>` | simulates a roaming-service building's walkers at x,y (existing or planned) with the game's roamer preview (figure/roamer_preview.c): `route_tiles`, `rows` (* = walked road, @ = building), `missed_houses` / `missed` / `houses` within 15 tiles; for workplaces whose only roamer is the labour seeker (farms, workshops, storage, docks...): `walker: "labor_seeker"`, `houses_passed`, `labour` verdict instead |
| `supply <distributor id> [<resource>]` | why a market/temple/tavern/mess hall gets a good or not: `staffing_percent`, `problem`, `buyer` {out, fetching, destination_id, x, y} (markets), `resources` [{resource, stock_units, fetching, problem/note, sources [{id, type, x, y, cartloads, distance, usable, why_not \| road_trip_tiles, slow}], verdict}] — mirrors building/distribution.c (`distance` is the straight range check, max 40 for markets; `road_trip_tiles` the buyer's real one-way walk) |
| `layout_review [x y w h]` | per service (prefect, engineer, religion, market, tax_collector, bathhouse, barber, doctor, school, library): `buildings`, `houses`, `missed`, `missed_area`, `missed_houses`; `leaks` (junctions next to houses with a suggested roadblock tile); `roadblocks_blocking_services` (roadblocks near houses and the services they block); `dry_water_buildings` (fountains/bathhouses outside every reservoir's range or without workers). A walker serves buildings within 2 tiles of its route. The preview display option is switched on in memory only |
| `ratings` | `culture` {rating, parts [{part, coverage_percent, points, max_points, next_step_at_coverage, points_at_next_step}], monuments_counted}, `prosperity` {rating, cap_from_housing, yearly_factors_now {…}, percent_in_tents_shacks, percent_in_villas_palaces}, `peace` {rating, calm_years_in_a_row, next_yearly_gain_if_calm, this_year_so_far}, `favor` {rating, penalties, months_since_gift} — mirrors city/ratings.c |
| `finance` | `treasury`, `this_year` / `last_year` {income {taxes, exports, donated, total}, expenses {imports, wages, construction, interest, salary, sundries, tribute, levies, total}, net} |
| `storage_report` | `storage` [{id, type, x, y, staffing_percent, road_access, contents, stored, free_space, accepts}], `stuck_producers` [{id, type, x, y, waiting_with, reason}], `no_labour_access` [{id, type, x, y}] (workplaces whose labour seeker passes no house) — producers whose cart found no storage (game flag `min_max_seen`) |
| `depot <id> [<resource> <source id> <destination id> [always\|never\|source_over N\|dest_under N]]` | show/set a cart depot's order (storages must differ; granaries food only; N 0-32 cartloads) → `order`, `carts`, `staffing_percent` |
| `rome` | `favor`, `player_rank`/`salary_rank` {rank, title, salary}, `personal_savings`, `months_since_gift`, `gifts` [{size, cost, affordable}], `requests` [{id, resource, amount, state, months_left, favor, can_dispatch}] |
| `salary <rank 0-10>` | sets the governor's salary like the advisor window; `above_rank` (paying above your rank costs favour) |
| `gift <modest\|generous\|lavish>` | gift to Caesar from personal savings → `cost`, `personal_savings`, `favor_change` |
| `dispatch <request id>` | sends a request's goods/denarii if available → `state`, `treasury_change` |
| `festival [<god> <small\|large\|grand>]` | gods (ceres, neptune, mercury, mars, venus): happiness, wrath/happy bolts, months since festival; `festival_costs`; or schedules a festival (cost paid now, held 2-4 months later; one at a time) |
| `victory <accept\|continue_2\|continue_5>` | answers the "mission won" dialog (window `victory_dialog`, appears when all goals are met): accept the promotion, or govern 24/60 more months |
| `next_mission [choice]` | after accepting (windows `video`/`intermezzo`/`mission_end`): advances the campaign like the end screen; if the next mission offers two maps returns `choice_needed` + `choices` [{choice, name, description}] and shows the selection screen; with a choice (or when there's only one map) loads and starts it like the briefing's start button → `scenario`, `campaign_mission`; `campaign_complete` after the last mission |
| `test_win` | forces a win — only exists when the game runs with `AUGUSTUS_AGENT_TEST_MODE=1` (the test suite) |
| `briefing` | the mission briefing as plain text: `title`, `subtitle`, `text`, `original_campaign`, `campaign_mission` (original campaign text, or the custom scenario's intro message) |
| `message` | the open message popup: `type`, `urgent`, `year`, `month`, `param1/2` (shown beside the text by the game, e.g. request amounts), `title`, `subtitle`, `text` (plain text) |
| `save <agent_name>` | saves `agent_name.svx` in the savegame directory (name must start with `agent_`, so the player's saves are never overwritten) or to an absolute path → `file`. Also works while the victory dialog is open (after accepting, the city is gone) |
| `screenshot [city\|minimap\|display]` | saves a PNG (default: whole city); `directory` = where; newest PNG there is it |
| `step [days=1]` | `days_requested`, `ticks_run`, `date`; `stopped_by: "window_changed"` + `window` if a popup/screen opened mid-step |
| `play <days> [speed]` | runs the game **in real time** through the normal game loop at `speed` % (10-100 step 10, 200, 300, 500; default 100 ≈ 0.8 s/day) so a person can watch; answers only when done (same fields as `step`) or when a popup opens (`stopped_by`). Commands sent meanwhile are queued. Pauses again afterwards |
| `load <savegame>` | `scenario`, `date` — loads from the savegame directory (or an absolute path), shows the city, pauses |
| `close` | closes a message dialog, message list, building info, advisors and other in-game panels a watching player may open (labour priorities, salary, gifts, trade, empire, festival, overlay menu) and returns to the city |
| `pause` / `resume` | `paused` |
| `help` | `commands` |

`step` stops early whenever the city stops being the active window (e.g. a news message pops up).
It gives every simulated tick its own millisecond on the game clock (house evolution and some caches
skip work "already done this millisecond"), and the main loop adds `agent_time_offset()` so the game
clock stays monotonic afterwards.

Runs are **not fully reproducible**: Augustus seeds some gameplay randomness from the wall clock
(`random_from_stdlib` in `core/random.c`: entertainer venues, crime, strikes, requests, invasions).
Typical loop: `step N` → if `stopped_by`, inspect, `close`, step the remaining days.

`load` resets the view orientation to north so multi-tile placement is always anchored at the top-left tile.
Buildings placed this tick are listed (state "created") before the next tick makes them active.

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
- `observe.c` — `map`, `buildings`, `houses`, building type names
- `act.c` — `build`, `clear`, `undo`
- `settings.c` — `set`, `storage`, `market`, `roadblock`, `message`, `save`
- `layout.c` — `walker_preview`, `layout_review`
- `ratings.c` — `ratings`, `finance`
- `rome.c` — `rome`, `salary`, `gift`, `dispatch`, `festival`
- `trade.c` — `trade`, `open_route`
- `campaign.c` — `victory`, `next_mission`, `test_win`
- `logistics.c` — `storage_report`, `depot`, `supply`

Hooks outside `src/agent/`: `platform/SDL2/augustus.c` (poll, clock offset, shutdown after `game_exit`),
`platform/arguments.*` (`--agent-port`), `window/message_dialog.*` (`window_message_dialog_current()`
read-only accessor), `game/game.c` (no settings/config saving in agent mode), `game/file.c` (no mission-start
autosave in agent mode — it is named after the player's rank and would overwrite e.g. `Architect.svx`),
CMakeLists.

Agent mode never writes the player's files: settings (`c3.inf`, `augustus.ini`) aren't saved on exit (so
`--windowed` doesn't become the player's display setting), there is no mission-start autosave, and `save`
only accepts `agent_*` names. `build` moves the camera to what was built, for anyone watching.

Not handled yet: mission briefing (new mission start) and mission end screens — `step` stops with the
`mission_end` window and `city.victory` says won/lost, but the agent can't continue to the next mission.
- `json.c` — minimal JSON writer (no external dependencies)
