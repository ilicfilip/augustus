#include "act.h"

#include "agent/observe.h"
#include "building/building.h"
#include "building/construction.h"
#include "building/dock.h"
#include "building/menu.h"
#include "building/properties.h"
#include "building/roadblock.h"
#include "city/data_private.h"
#include "city/finance.h"
#include "city/labor.h"
#include "city/map.h"
#include "city/view.h"
#include "city/warning.h"
#include "core/encoding.h"
#include "figure/figure.h"
#include "game/undo.h"
#include "graphics/window.h"
#include "map/building.h"
#include "map/figure.h"
#include "map/grid.h"
#include "map/road_access.h"
#include "map/road_network.h"
#include "map/routing.h"
#include "map/terrain.h"
#include "map/tiles.h"
#include "map/water.h"
#include "window/city.h"

#include <stdio.h>
#include <string.h>

#define MAX_WARNINGS 5

static void write_error(json_writer *w, const char *message)
{
    json_bool(w, "ok", 0);
    json_string(w, "error", message);
}

static int is_drag_type(building_type type)
{
    switch (type) {
        case BUILDING_ROAD:
        case BUILDING_AQUEDUCT:
        case BUILDING_WALL:
        case BUILDING_HIGHWAY:
        case BUILDING_DRAGGABLE_RESERVOIR:
        case BUILDING_CLEAR_LAND:
        case BUILDING_HOUSE_VACANT_LOT:
        case BUILDING_PLAZA:
        case BUILDING_GARDENS:
            return 1;
        default:
            return 0;
    }
}

/** Cheap fingerprint of the map, to tell whether a construction actually changed anything */
static unsigned int map_fingerprint(void)
{
    unsigned int hash = 2166136261u;
    for (int y = 0; y < map_grid_height(); y++) {
        for (int x = 0; x < map_grid_width(); x++) {
            int grid_offset = map_grid_offset(x, y);
            hash = (hash ^ (unsigned int) map_terrain_get(grid_offset)) * 16777619u;
            hash = (hash ^ map_building_at(grid_offset)) * 16777619u;
        }
    }
    return hash;
}

static int in_map(int x, int y)
{
    return x >= 0 && y >= 0 && x < map_grid_width() && y < map_grid_height();
}

static void write_warnings(json_writer *w)
{
    json_array_start(w, "warnings");
    for (int i = 0; i < MAX_WARNINGS; i++) {
        const uint8_t *text = city_warning_get(i);
        if (text) {
            json_game_string(w, 0, text);
        }
    }
    json_array_end(w);
}

static void write_tile(json_writer *w, int x, int y)
{
    int building_id = map_building_at(map_grid_offset(x, y));
    json_object_start(w, "at_target");
    if (building_id) {
        building *b = building_main(building_get(building_id));
        json_int(w, "building_id", b->id);
        json_string(w, "type", agent_building_type_name(b->type));
        json_int(w, "x", b->x);
        json_int(w, "y", b->y);
    } else {
        char tile[2] = { agent_tile_char(map_grid_offset(x, y)), 0 };
        json_string(w, "tile", tile);
    }
    json_object_end(w);
}

static int is_road(int x, int y)
{
    return in_map(x, y) && map_terrain_is(map_grid_offset(x, y), TERRAIN_ROAD);
}

/** Road tiles in the rectangle with 3+ road neighbours: junctions where roaming walkers pick a random way */
static int count_junctions(int x1, int y1, int x2, int y2)
{
    int count = 0;
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            if (is_road(x, y) &&
                is_road(x - 1, y) + is_road(x + 1, y) + is_road(x, y - 1) + is_road(x, y + 1) >= 3) {
                count++;
            }
        }
    }
    return count;
}

/**
 * After a failed single-building placement: which footprint tiles are not buildable and why (map letter, or a
 * person/animal standing there) — the game's warning only says "Must build on cleared land".
 */
static void write_blocked_tiles(json_writer *w, building_type type, int x, int y)
{
    int size = type == BUILDING_WAREHOUSE ? 3 : building_properties_for_type(type)->size;
    if (size < 1) {
        size = 1;
    }
    json_array_start(w, "blocked");
    for (int yy = y; yy < y + size; yy++) {
        for (int xx = x; xx < x + size; xx++) {
            char what[256];
            if (!in_map(xx, yy)) {
                snprintf(what, sizeof(what), "outside the map");
            } else {
                int grid_offset = map_grid_offset(xx, yy);
                char c = agent_tile_char(grid_offset);
                if (c != '.' && c != ',') {
                    snprintf(what, sizeof(what), "tile '%c'", c);
                } else if (map_has_figure_at(grid_offset)) {
                    figure *f = figure_get(map_figure_at(grid_offset));
                    snprintf(what, sizeof(what), "%s", f->type == FIGURE_MAP_FLAG ?
                        "map flag (entry/exit/invasion/fishing point): permanent, build elsewhere" :
                        figure_is_herd(f) ? "animal herd: it wanders off; retry later" :
                        f->type == FIGURE_HOMELESS ? "evicted residents of a demolished house: they stand about a "
                            "day, then move to a free house or leave the city (demolishing occupied houses loses "
                            "those people); retry in 1-2 days" :
                        "a walker passing: retry next day");
                } else {
                    continue;
                }
            }
            json_object_start(w, 0);
            json_int(w, "x", xx);
            json_int(w, "y", yy);
            json_string(w, "by", what);
            json_object_end(w);
        }
    }
    json_array_end(w);
}

/** If the construction opened a confirmation popup (deleting forts, monuments, bridges), cancel it */
static int cancel_popup(void)
{
    if (window_get_id() == WINDOW_POPUP_DIALOG || window_get_id() == WINDOW_PLAIN_MESSAGE_DIALOG) {
        window_city_show();
        return 1;
    }
    return 0;
}

#define MAX_TRACKED 64

/** Main buildings with a tile in the rectangle (to report what a clear destroyed) */
static int buildings_in_rect(int x1, int y1, int x2, int y2, unsigned int *ids)
{
    int count = 0;
    for (int y = y1 < y2 ? y1 : y2; y <= (y1 < y2 ? y2 : y1); y++) {
        for (int x = x1 < x2 ? x1 : x2; x <= (x1 < x2 ? x2 : x1); x++) {
            if (!in_map(x, y)) {
                continue;
            }
            unsigned int id = map_building_at(map_grid_offset(x, y));
            if (!id) {
                continue;
            }
            building *b = building_main(building_get(id));
            if (building_properties_for_type(b->type)->shared) {
                continue; // aqueduct segments: shown as tiles, not buildings
            }
            int known = 0;
            for (int i = 0; i < count; i++) {
                known |= ids[i] == b->id;
            }
            if (!known && count < MAX_TRACKED) {
                ids[count++] = b->id;
            }
        }
    }
    return count;
}

#define MAX_ROAD_TILES 256

/** Road tiles in the rectangle, as grid offsets (to report what a clear removed) */
static int road_tiles_in_rect(int x1, int y1, int x2, int y2, int *offsets)
{
    int count = 0;
    for (int y = y1 < y2 ? y1 : y2; y <= (y1 < y2 ? y2 : y1); y++) {
        for (int x = x1 < x2 ? x1 : x2; x <= (x1 < x2 ? x2 : x1); x++) {
            if (in_map(x, y) && count < MAX_ROAD_TILES && map_terrain_is(map_grid_offset(x, y), TERRAIN_ROAD)) {
                offsets[count++] = map_grid_offset(x, y);
            }
        }
    }
    return count;
}

/**
 * Road access by the game's own rule for the type (building/maintenance.c check_rome_access): granaries and
 * warehouses use fixed entrance tiles, houses any road within 2 tiles, the rest an adjacent road. A roadblock
 * tile doesn't count as road for granaries, warehouses and ordinary buildings (map/road_access.c).
 */
static int has_game_road_access(building *b)
{
    if (b->type == BUILDING_GRANARY) {
        return map_has_road_access_granary(b->x, b->y, 0);
    }
    if (b->type == BUILDING_WAREHOUSE) {
        return map_has_road_access_warehouse(b->x, b->y, 0);
    }
    int size = b->size > 0 ? b->size : 1;
    if (b->house_size) {
        int rx, ry;
        return map_closest_road_within_radius(b->x, b->y, size, 2, &rx, &ry);
    }
    return map_has_road_access(b->x, b->y, size, 0);
}

/** Buildings near the rectangle that have road access now (a clear or a roadblock may take it away) */
static int buildings_with_road_near(int x1, int y1, int x2, int y2, unsigned int *ids)
{
    int min_x = (x1 < x2 ? x1 : x2) - 2, max_x = (x1 > x2 ? x1 : x2) + 2;
    int min_y = (y1 < y2 ? y1 : y2) - 2, max_y = (y1 > y2 ? y1 : y2) + 2;
    int count = 0;
    for (int id = 1; id < building_count() && count < MAX_TRACKED; id++) {
        building *b = building_get(id);
        if (b->state != BUILDING_STATE_IN_USE || building_main(b) != b || building_properties_for_type(b->type)->shared ||
            b->type == BUILDING_ROADBLOCK) {
            continue;
        }
        int size = b->size > 0 ? b->size : 1;
        if (b->x > max_x || b->x + size - 1 < min_x || b->y > max_y || b->y + size - 1 < min_y) {
            continue;
        }
        if (has_game_road_access(b)) {
            ids[count++] = b->id;
        }
    }
    return count;
}

#define MAX_HOUSES_TRACKED 2048

static int network_near(int network, int x, int y, int size, int radius)
{
    for (int yy = y - radius; yy < y + size + radius; yy++) {
        for (int xx = x - radius; xx < x + size + radius; xx++) {
            if (in_map(xx, yy) && map_terrain_is(map_grid_offset(xx, yy), TERRAIN_ROAD) &&
                map_road_network_get(map_grid_offset(xx, yy)) == network) {
                return 1;
            }
        }
    }
    return 0;
}

static int road_network_near_point(int x, int y)
{
    int rx, ry;
    return map_closest_road_within_radius(x, y, 1, 2, &rx, &ry) ? map_road_network_get(map_grid_offset(rx, ry)) : 0;
}

/**
 * Buildings on the city's main (largest) road network (map/road_network.c, refreshed now as the game does every
 * few ticks): walkers, carts, market buyers and labour seekers only use roads. Immigrants and traders also cross
 * open land, so a cut road doesn't evict houses (building/maintenance.c measures the entry distance over land).
 */
static int buildings_on_main_network(unsigned int *ids, int *exit_joined)
{
    map_road_network_update();
    const map_tile *entry = city_map_entry_point(), *exit = city_map_exit_point();
    int entry_network = road_network_near_point(entry->x, entry->y);
    *exit_joined = entry_network && road_network_near_point(exit->x, exit->y) == entry_network;
    int network = city_data.map.largest_road_networks[0].id;
    int count = 0;
    if (!network) {
        return 0;
    }
    for (int id = 1; id < building_count() && count < MAX_HOUSES_TRACKED; id++) {
        building *b = building_get(id);
        if (b->state != BUILDING_STATE_IN_USE || building_main(b) != b || building_properties_for_type(b->type)->shared ||
            (building_type_is_roadblock(b->type) && b->type != BUILDING_GRANARY && b->type != BUILDING_WAREHOUSE)) {
            continue;
        }
        if (network_near(network, b->x, b->y, b->size > 0 ? b->size : 1, b->house_size ? 2 : 1)) {
            ids[count++] = b->id;
        }
    }
    return count;
}

static void write_drag_failure(json_writer *w, building_type type, int x, int y, int x2, int y2)
{
    json_array_start(w, "ends");
    for (int i = 0; i < 2; i++) {
        int ex = i ? x2 : x, ey = i ? y2 : y;
        char tile[2] = { agent_tile_char(map_grid_offset(ex, ey)), 0 };
        json_object_start(w, 0);
        json_int(w, "x", ex);
        json_int(w, "y", ey);
        json_string(w, "tile", tile);
        json_object_end(w);
    }
    json_array_end(w);
    if (type == BUILDING_ROAD && is_road(x, y) && is_road(x2, y2)) {
        json_string(w, "why", "both ends are already road (removing a roadblock leaves its road): nothing new to "
            "build");
    } else {
        json_string(w, "why", "a drag builds nothing when an end tile can't take it (a building, rock '^', water "
            "'~', trees 't' — clear them first) or no route joins the two ends; see `ends`");
    }
}

/** Jobs in every placed building (the game's own total skips buildings its labour seekers haven't reached yet) */
static int total_jobs(void)
{
    int jobs = 0;
    for (int id = 1; id < building_count(); id++) {
        building *b = building_get(id);
        if ((b->state == BUILDING_STATE_IN_USE || b->state == BUILDING_STATE_CREATED) && building_main(b) == b) {
            jobs += model_get_building(b->type)->laborers;
        }
    }
    return jobs;
}

/**
 * After placing a workplace: jobs vs the city's workforce (city/labor.c: 60 % of people aged 20-49, times the
 * plebs share; about 42-45 % of the population in practice). Too many jobs = buildings stand empty, by priority.
 */
static void write_labour_balance(json_writer *w, building_type type)
{
    int needs = model_get_building(type)->laborers;
    if (needs <= 0) {
        return;
    }
    int workforce = city_data.labor.workers_available;
    int jobs = total_jobs();
    json_object_start(w, "labour");
    json_int(w, "this_needs", needs);
    int reached = 0;
    for (int i = 0; i < 9; i++) { // labour categories, city/labor.c
        reached += city_labor_category(i)->workers_needed;
    }
    json_int(w, "jobs_in_city", jobs);
    json_int(w, "jobs_not_reached", jobs - reached > 0 ? jobs - reached : 0);
    json_int(w, "workforce", workforce);
    json_int(w, "unemployed", city_data.labor.workers_unemployed);
    json_int(w, "population", city_data.population.population);
    json_object_end(w);
    if (jobs > workforce) {
        char text[400];
        snprintf(text, sizeof(text), "the city now has %d jobs for %d workers (workforce is about 45%% of the "
            "population): %d jobs will stay empty, lowest labour priorities first (jobs_not_reached = buildings no "
            "labour seeker has reached yet; they compete once reached). Build in waves: housing first, then only "
            "as many workplaces as people can staff; add industry when unemployment rises above ~10%%",
            jobs, workforce, jobs - workforce);
        json_string(w, "labour_warning", text);
    }
}

static void construct(json_writer *w, building_type type, int x, int y, int x2, int y2)
{
    int road_before[MAX_ROAD_TILES];
    int num_roads_before = type == BUILDING_CLEAR_LAND ? road_tiles_in_rect(x, y, x2, y2, road_before) : 0;
    int is_roadblock = building_type_is_roadblock(type) && type != BUILDING_GRANARY && type != BUILDING_WAREHOUSE;
    unsigned int roaded_ids[MAX_TRACKED];
    int num_roaded = num_roads_before || is_roadblock ? buildings_with_road_near(x, y, x2, y2, roaded_ids) : 0;
    static unsigned int reached_before[MAX_HOUSES_TRACKED];
    int exit_before = 0;
    int num_reached_before = num_roads_before ? buildings_on_main_network(reached_before, &exit_before) : 0;
    unsigned int before_ids[MAX_TRACKED];
    building_type before_types[MAX_TRACKED];
    int before_x[MAX_TRACKED], before_y[MAX_TRACKED];
    int num_before = type == BUILDING_CLEAR_LAND ? buildings_in_rect(x, y, x2, y2, before_ids) : 0;
    for (int i = 0; i < num_before; i++) {
        building *b = building_get(before_ids[i]);
        before_types[i] = b->type;
        before_x[i] = b->x;
        before_y[i] = b->y;
    }
    int treasury_before = city_finance_treasury();
    unsigned int map_before = map_fingerprint();
    // Junctions around a new road (routing may wander, so look at a margin around the drag)
    int jx1 = (x < x2 ? x : x2) - 3, jy1 = (y < y2 ? y : y2) - 3;
    int jx2 = (x > x2 ? x : x2) + 3, jy2 = (y > y2 ? y : y2) + 3;
    int junctions_before = type == BUILDING_ROAD ? count_junctions(jx1, jy1, jx2, jy2) : 0;
    city_warning_clear_all();

    building_construction_set_type(type, 0);
    building_construction_start(x, y, map_grid_offset(x, y));
    building_construction_update(x2, y2, map_grid_offset(x2, y2));
    building_construction_place();
    building_construction_clear_type();

    int needed_confirmation = cancel_popup();
    int cost = treasury_before - city_finance_treasury();
    int changed = cost != 0 || map_fingerprint() != map_before;

    if (changed) {
        // Let someone watching the game window see what the agent just did
        city_view_go_to_grid_offset(map_grid_offset(x, y));
    }
    json_bool(w, "ok", changed);
    if (needed_confirmation) {
        json_string(w, "error", "needs confirmation (fort, monument or bridge); cancelled");
    } else if (!changed) {
        json_string(w, "error", type == BUILDING_CLEAR_LAND ? "nothing to clear there (already clear, or rock/water)" :
            is_drag_type(type) ? "nothing was built; see why" : "nothing was built; see warnings");
        if (!is_drag_type(type)) {
            write_blocked_tiles(w, type, x, y);
        } else if (type != BUILDING_CLEAR_LAND) {
            write_drag_failure(w, type, x, y, x2, y2);
        }
    }
    json_int(w, "cost", cost);
    json_int(w, "treasury", city_finance_treasury());
    if (num_before) {
        // Clearing a rectangle demolishes every building touching it, including ones the agent built itself
        json_array_start(w, "removed");
        for (int i = 0; i < num_before; i++) {
            building *b = building_get(before_ids[i]);
            if (b->state != BUILDING_STATE_IN_USE && b->state != BUILDING_STATE_CREATED) {
                json_object_start(w, 0);
                json_string(w, "type", agent_building_type_name(before_types[i]));
                json_int(w, "x", before_x[i]);
                json_int(w, "y", before_y[i]);
                json_object_end(w);
            }
        }
        json_array_end(w);
    }
    int removed_roads = 0;
    for (int i = 0; i < num_roads_before; i++) {
        removed_roads += !map_terrain_is(road_before[i], TERRAIN_ROAD);
    }
    if (removed_roads || (is_roadblock && changed)) {
        if (removed_roads) {
            json_int(w, "removed_road_tiles", removed_roads);
        }
        int lost = 0;
        json_array_start(w, "lost_road_access");
        for (int i = 0; i < num_roaded; i++) {
            building *b = building_get(roaded_ids[i]);
            if (b->state == BUILDING_STATE_IN_USE && !has_game_road_access(b)) {
                lost++;
                json_object_start(w, 0);
                json_int(w, "id", b->id);
                json_string(w, "type", agent_building_type_name(b->type));
                json_int(w, "x", b->x);
                json_int(w, "y", b->y);
                json_object_end(w);
            }
        }
        json_array_end(w);
        if (removed_roads) {
            json_string(w, "road_note", "the clear removed road tiles: buildings in lost_road_access get no workers, "
                "carts or walkers until a road touches them again; others may now be cut off from the city (check "
                "storage_report and the labour numbers)");
        } else if (lost) {
            json_string(w, "road_note", "a roadblock doesn't count as road for road access (granaries and "
                "warehouses use only their entrance tiles): the buildings in lost_road_access just lost their only "
                "road. Undo and put the roadblock one tile further along");
        }
    }
    if (removed_roads && num_reached_before) {
        static unsigned int reached_after[MAX_HOUSES_TRACKED];
        int exit_after = 0;
        int num_after = buildings_on_main_network(reached_after, &exit_after);
        int houses = 0, others = 0;
        json_array_start(w, "cut_off");
        for (int i = 0; i < num_reached_before; i++) {
            int still = 0;
            for (int j = 0; j < num_after && !still; j++) {
                still = reached_after[j] == reached_before[i];
            }
            building *b = building_get(reached_before[i]);
            if (still || b->state != BUILDING_STATE_IN_USE) {
                continue;
            }
            if (b->house_size) {
                houses++;
            } else {
                others++;
            }
            if (houses + others <= 12) {
                json_object_start(w, 0);
                json_int(w, "id", b->id);
                json_string(w, "type", agent_building_type_name(b->type));
                json_int(w, "x", b->x);
                json_int(w, "y", b->y);
                json_object_end(w);
            }
        }
        json_array_end(w);
        json_int(w, "cut_off_houses", houses);
        json_int(w, "cut_off_buildings", others);
        if (houses + others) {
            json_string(w, "cut_off_note", "these are no longer joined BY ROAD to the city's main road network: "
                "service walkers, carts, market buyers and labour seekers use roads only, so this part can't "
                "exchange workers, food or goods with the rest (houses stay; immigrants walk over land). Undo, or "
                "rebuild the link");
        }
        if (exit_before && !exit_after) {
            json_bool(w, "exit_cut", 1);
            json_string(w, "exit_note", "the map entry's road no longer joins the exit's road: traders and "
                "migrants walk the gap over open land (slower); usually harmless");
        }
    }
    int tile_cost = model_get_building(type)->cost;
    if (changed && tile_cost > 0 && (type == BUILDING_ROAD || type == BUILDING_AQUEDUCT || type == BUILDING_WALL)) {
        // Like the mouse drag, these are routed around obstacles (trees, rocks, buildings): report detours
        int tiles = cost / tile_cost;
        int straight = (x2 > x ? x2 - x : x - x2) + (y2 > y ? y2 - y : y - y2) + 1;
        json_int(w, "tiles", tiles);
        json_int(w, "straight_tiles", straight);
        if (type == BUILDING_ROAD) {
            int new_junctions = count_junctions(jx1, jy1, jx2, jy2) - junctions_before;
            if (new_junctions > 0) {
                json_int(w, "new_intersections", new_junctions);
                json_string(w, "intersection_note", "every intersection makes roaming walkers (prefects, engineers, "
                    "priests, tax collectors, market ladies) choose randomly; never run a road right next to "
                    "another (double roads make a ladder of junctions); use single roads + roadblocks");
            }
        }
        if (tiles > straight) {
            json_bool(w, "detour", 1);
            json_string(w, "note", "routed around obstacles; for a straight line clear trees/rubble on the direct "
                "path first (clear), rocks and water can't be cleared");
        }
    }
    if (changed && !needed_confirmation && type != BUILDING_CLEAR_LAND) {
        write_labour_balance(w, type);
    }
    if (changed && (type == BUILDING_FOUNTAIN || type == BUILDING_BATHHOUSE)) {
        // The game's "needs access to a full reservoir" warning also appears while a reservoir merely isn't full
        // yet, so say whether the building is within any reservoir's range at all
        int reach = agent_reservoir_reach(x, y, type == BUILDING_BATHHOUSE ? 2 : 1);
        json_string(w, "water", reach == 2 ? "inside a filled reservoir's range: works once staffed" :
            reach == 1 ? "inside the range of a reservoir with NO water yet: it needs open water within 1 tile or an "
                "aqueduct from a full reservoir" :
            "OUTSIDE every reservoir's range: this will stay dry — move it, build a reservoir (+aqueduct) within "
            "reach, or use wells/latrines instead");
    }
    write_warnings(w);
    write_tile(w, x, y);
    window_invalidate();
}

void agent_act_build(json_writer *w, const char *args)
{
    char name[48];
    int x, y, x2, y2;
    int n = args ? sscanf(args, "%47s %d %d %d %d", name, &x, &y, &x2, &y2) : 0;
    if (n != 3 && n != 5) {
        write_error(w, "usage: build <type> <x> <y> [x2 y2]");
        return;
    }
    building_type type = agent_building_type_from_name(name);
    if (type == BUILDING_NONE || type == BUILDING_CLEAR_LAND) {
        write_error(w, "unknown or unbuildable type (use names from `buildings`, `housing` for housing plots, "
            "`clear` to clear land)");
        return;
    }
    if (!building_menu_is_enabled(type)) {
        write_error(w, "not available in this mission");
        return;
    }
    if (n == 3) {
        x2 = x;
        y2 = y;
    } else if (!is_drag_type(type)) {
        write_error(w, "x2 y2 only apply to roads, aqueducts, walls, highways, reservoirs, housing, plazas, gardens");
        return;
    }
    if (!in_map(x, y) || !in_map(x2, y2)) {
        write_error(w, "outside the map");
        return;
    }
    if (type == BUILDING_DRAGGABLE_RESERVOIR) {
        // The game's reservoir tool takes the centre tile of the 3x3; keep x,y = top-left like every other building
        construct(w, type, x + 1, y + 1, x2 + 1, y2 + 1);
        return;
    }
    construct(w, type, x, y, x2, y2);
}

void agent_act_clear(json_writer *w, const char *args)
{
    int x, y, x2, y2;
    int n = args ? sscanf(args, "%d %d %d %d", &x, &y, &x2, &y2) : 0;
    if (n != 2 && n != 4) {
        write_error(w, "usage: clear <x> <y> [x2 y2]");
        return;
    }
    if (n == 2) {
        x2 = x;
        y2 = y;
    }
    if (!in_map(x, y) || !in_map(x2, y2)) {
        write_error(w, "outside the map");
        return;
    }
    construct(w, BUILDING_CLEAR_LAND, x, y, x2, y2);
}

void agent_act_undo(json_writer *w)
{
    if (!game_can_undo()) {
        write_error(w, "nothing to undo (undo is only possible shortly after building)");
        return;
    }
    int treasury_before = city_finance_treasury();
    game_undo_perform();
    window_invalidate();
    json_bool(w, "ok", 1);
    json_int(w, "refund", city_finance_treasury() - treasury_before);
    json_int(w, "treasury", city_finance_treasury());
}

#define MAX_SITES 10

typedef struct {
    int x, y, distance, trees, road;
} site;

/**
 * Spots where the game would accept the building, nearest to x,y first: the footprint is free (trees allowed,
 * they get cleared) and the type's terrain need is met (building/construction.c can_place_on_terrain: water
 * within 2 tiles for clay pits, trees for timber yards, rock for quarries and mines, meadow for farms; the shore
 * check of construction_building.c for docks, wharves and shipyards). Nothing is built.
 */
void agent_act_sites(json_writer *w, const char *args)
{
    char name[48];
    int cx, cy, radius = 15;
    int n = args ? sscanf(args, "%47s %d %d %d", name, &cx, &cy, &radius) : 0;
    if (n < 3) {
        write_error(w, "usage: sites <type> <x> <y> [radius]");
        return;
    }
    building_type type = agent_building_type_from_name(name);
    if (type == BUILDING_NONE || type == BUILDING_CLEAR_LAND || is_drag_type(type)) {
        write_error(w, "unknown type, or a dragged one (roads, housing...): use names from `buildings`");
        return;
    }
    if (!building_menu_is_enabled(type)) {
        write_error(w, "not available in this mission");
        return;
    }
    if (radius < 1) {
        radius = 1;
    } else if (radius > 40) {
        radius = 40;
    }
    int size = type == BUILDING_WAREHOUSE ? 3 : building_properties_for_type(type)->size;
    if (size < 1) {
        size = 1;
    }
    int shore = type == BUILDING_DOCK || type == BUILDING_WHARF || type == BUILDING_SHIPYARD;
    site found[MAX_SITES];
    int num_found = 0, total = 0;
    building_construction_set_type(type, 0);
    for (int y = cy - radius; y <= cy + radius; y++) {
        for (int x = cx - radius; x <= cx + radius; x++) {
            if (!in_map(x, y) || !in_map(x + size - 1, y + size - 1)) {
                continue;
            }
            int trees = 0;
            if (shore) {
                int orientation_abs, orientation_rel;
                if (map_water_determine_orientation(x, y, size, 0, &orientation_abs, &orientation_rel, 1, 0) ||
                    (type == BUILDING_DOCK && !building_dock_is_connected_to_open_water(x, y))) {
                    continue;
                }
            } else {
                int warning_id;
                if (!map_tiles_are_clear(x, y, size, TERRAIN_ALL & ~(TERRAIN_TREE | TERRAIN_SHRUB), 0) ||
                    !building_construction_can_place_on_terrain(x, y, &warning_id)) {
                    continue;
                }
                for (int yy = y; yy < y + size; yy++) {
                    for (int xx = x; xx < x + size; xx++) {
                        trees += map_terrain_is(map_grid_offset(xx, yy), TERRAIN_TREE | TERRAIN_SHRUB) ? 1 : 0;
                    }
                }
            }
            total++;
            site s = { x, y, (x > cx ? x - cx : cx - x) + (y > cy ? y - cy : cy - y), trees,
                map_has_road_access(x, y, size, 0) };
            // keep the nearest MAX_SITES, fewest trees to clear first on ties
            int pos = num_found;
            while (pos > 0 && (found[pos - 1].distance > s.distance ||
                (found[pos - 1].distance == s.distance && found[pos - 1].trees > s.trees))) {
                pos--;
            }
            if (pos >= MAX_SITES) {
                continue;
            }
            if (num_found < MAX_SITES) {
                num_found++;
            }
            for (int i = num_found - 1; i > pos; i--) {
                found[i] = found[i - 1];
            }
            found[pos] = s;
        }
    }
    building_construction_clear_type();
    json_bool(w, "ok", 1);
    json_string(w, "type", agent_building_type_name(type));
    json_int(w, "size", size);
    json_int(w, "radius", radius);
    json_int(w, "sites_found", total);
    json_array_start(w, "sites");
    for (int i = 0; i < num_found; i++) {
        json_object_start(w, 0);
        json_int(w, "x", found[i].x);
        json_int(w, "y", found[i].y);
        json_int(w, "distance", found[i].distance);
        json_int(w, "trees_to_clear", found[i].trees);
        json_bool(w, "road_adjacent", found[i].road);
        json_object_end(w);
    }
    json_array_end(w);
    json_string(w, "note", shore ? "x,y = top-left; the shore check is the game's own. Build a road to it" :
        "x,y = top-left; terrain need checked as the game does (trees on the footprint: clear them first). "
        "Walkers passing can still block a tile on the day you build");
}
