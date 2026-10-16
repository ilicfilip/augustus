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
#include "game/undo.h"
#include "graphics/window.h"
#include "map/building.h"
#include "map/grid.h"
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

/** If the construction opened a confirmation popup (deleting forts, monuments, bridges), cancel it */
static int cancel_popup(void)
{
    if (window_get_id() == WINDOW_POPUP_DIALOG || window_get_id() == WINDOW_PLAIN_MESSAGE_DIALOG) {
        window_city_show();
        return 1;
    }
    return 0;
}

static void construct(json_writer *w, building_type type, int x, int y, int x2, int y2)
{
    int treasury_before = city_finance_treasury();
    unsigned int map_before = map_fingerprint();
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
        json_string(w, "error", "nothing was built; see warnings");
    }
    json_int(w, "cost", cost);
    json_int(w, "treasury", city_finance_treasury());
    int tile_cost = model_get_building(type)->cost;
    if (changed && tile_cost > 0 && (type == BUILDING_ROAD || type == BUILDING_AQUEDUCT || type == BUILDING_WALL)) {
        // Like the mouse drag, these are routed around obstacles (trees, rocks, buildings): report detours
        int tiles = cost / tile_cost;
        int straight = (x2 > x ? x2 - x : x - x2) + (y2 > y ? y2 - y : y - y2) + 1;
        json_int(w, "tiles", tiles);
        json_int(w, "straight_tiles", straight);
        if (tiles > straight) {
            json_bool(w, "detour", 1);
            json_string(w, "note", "routed around obstacles; for a straight line clear trees/rubble on the direct "
                "path first (clear), rocks and water can't be cleared");
        }
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
