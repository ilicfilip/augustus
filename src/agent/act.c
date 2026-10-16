#include "act.h"

#include "agent/observe.h"
#include "building/building.h"
#include "building/construction.h"
#include "building/menu.h"
#include "building/properties.h"
#include "city/finance.h"
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
#include "map/terrain.h"
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
            char what[128];
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

/** Buildings next to the rectangle that touch a road now (a clear may take their only road away) */
static int buildings_with_road_near(int x1, int y1, int x2, int y2, unsigned int *ids)
{
    int min_x = (x1 < x2 ? x1 : x2) - 1, max_x = (x1 > x2 ? x1 : x2) + 1;
    int min_y = (y1 < y2 ? y1 : y2) - 1, max_y = (y1 > y2 ? y1 : y2) + 1;
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
        map_point road;
        if (map_has_road_access(b->x, b->y, size, &road)) {
            ids[count++] = b->id;
        }
    }
    return count;
}

static void construct(json_writer *w, building_type type, int x, int y, int x2, int y2)
{
    int road_before[MAX_ROAD_TILES];
    int num_roads_before = type == BUILDING_CLEAR_LAND ? road_tiles_in_rect(x, y, x2, y2, road_before) : 0;
    unsigned int roaded_ids[MAX_TRACKED];
    int num_roaded = num_roads_before ? buildings_with_road_near(x, y, x2, y2, roaded_ids) : 0;
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
            "nothing was built; see warnings");
        if (!is_drag_type(type)) {
            write_blocked_tiles(w, type, x, y);
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
    if (num_roads_before) {
        int removed_roads = 0;
        for (int i = 0; i < num_roads_before; i++) {
            removed_roads += !map_terrain_is(road_before[i], TERRAIN_ROAD);
        }
        if (removed_roads) {
            json_int(w, "removed_road_tiles", removed_roads);
            json_array_start(w, "lost_road_access");
            for (int i = 0; i < num_roaded; i++) {
                building *b = building_get(roaded_ids[i]);
                map_point road;
                if (b->state == BUILDING_STATE_IN_USE && !map_has_road_access(b->x, b->y, b->size, &road)) {
                    json_object_start(w, 0);
                    json_int(w, "id", b->id);
                    json_string(w, "type", agent_building_type_name(b->type));
                    json_int(w, "x", b->x);
                    json_int(w, "y", b->y);
                    json_object_end(w);
                }
            }
            json_array_end(w);
            json_string(w, "road_note", "the clear removed road tiles: buildings in lost_road_access get no workers, "
                "carts or walkers until a road touches them again; others may now be cut off from the city (check "
                "storage_report and the labour numbers)");
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
