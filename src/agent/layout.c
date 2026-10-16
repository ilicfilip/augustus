#include "layout.h"

#include "agent/observe.h"
#include "building/building.h"
#include "building/roadblock.h"
#include "core/config.h"
#include "figure/roamer_preview.h"
#include "map/building.h"
#include "map/grid.h"
#include "map/terrain.h"

#include <stdio.h>
#include <string.h>

/*
 * Uses the game's "preview roaming paths" simulation (figure/roamer_preview.c): it walks a building's roaming
 * walkers over the real road network with the real movement code (junction choices, roadblock permissions, roam
 * length), starting in each of 4 directions. A walker serves buildings within 2 tiles of its tile
 * (figure/service.c provide_culture: map_grid_get_area(x, y, 1, 2)).
 */

#define SHOWN_BUILDING_MARK 12 // roamer_preview.c SHOWN_BUILDING_OFFSET: the building's own origin tile
#define SERVICE_RADIUS 2
#define MAX_LISTED 12

static unsigned char covered[GRID_SIZE * GRID_SIZE];

static void write_error(json_writer *w, const char *message)
{
    json_bool(w, "ok", 0);
    json_string(w, "error", message);
}

static int in_map(int x, int y)
{
    return x >= 0 && y >= 0 && x < map_grid_width() && y < map_grid_height();
}

static int is_house_type(building_type type)
{
    return type >= BUILDING_HOUSE_VACANT_LOT && type <= BUILDING_HOUSE_LUXURY_PALACE;
}

static int travelled(int grid_offset)
{
    int value = figure_roamer_preview_get_frequency(grid_offset);
    return value > 0 && value != SHOWN_BUILDING_MARK;
}

/** Clears the preview, simulates the given buildings' walkers, then marks every tile they serve */
static int simulate(const building_type *types, int num_types, int hypothetical_type, int hx, int hy)
{
    int saved = config_get(CONFIG_UI_SHOW_ROAMING_PATH);
    config_set(CONFIG_UI_SHOW_ROAMING_PATH, 1); // in memory only: agent mode never saves the config
    figure_roamer_preview_reset_building_types();
    for (int i = 0; i < num_types; i++) {
        for (building *b = building_first_of_type(types[i]); b; b = b->next_of_type) {
            if (b->state == BUILDING_STATE_IN_USE || b->state == BUILDING_STATE_CREATED) {
                figure_roamer_preview_create(b->type, b->x, b->y);
            }
        }
    }
    if (hypothetical_type) {
        figure_roamer_preview_create(hypothetical_type, hx, hy);
    }
    memset(covered, 0, sizeof(covered));
    int travelled_tiles = 0;
    for (int y = 0; y < map_grid_height(); y++) {
        for (int x = 0; x < map_grid_width(); x++) {
            if (!travelled(map_grid_offset(x, y))) {
                continue;
            }
            travelled_tiles++;
            for (int yy = y - SERVICE_RADIUS; yy <= y + SERVICE_RADIUS; yy++) {
                for (int xx = x - SERVICE_RADIUS; xx <= x + SERVICE_RADIUS; xx++) {
                    if (in_map(xx, yy)) {
                        covered[map_grid_offset(xx, yy)] = 1;
                    }
                }
            }
        }
    }
    config_set(CONFIG_UI_SHOW_ROAMING_PATH, saved);
    return travelled_tiles;
}

static int house_is_covered(const building *b)
{
    for (int y = b->y; y < b->y + b->size; y++) {
        for (int x = b->x; x < b->x + b->size; x++) {
            if (in_map(x, y) && covered[map_grid_offset(x, y)]) {
                return 1;
            }
        }
    }
    return 0;
}

static int in_rect(const building *b, int x, int y, int w, int h)
{
    return b->x >= x && b->y >= y && b->x < x + w && b->y < y + h;
}

static int is_listed_house(const building *b)
{
    return (b->state == BUILDING_STATE_IN_USE || b->state == BUILDING_STATE_CREATED) && is_house_type(b->type) &&
        b->house_size;
}

/** Houses in the rectangle not served: count, bounding box, a few positions */
static void write_missed_houses(json_writer *w, int x, int y, int w_tiles, int h_tiles, int *total_out)
{
    int missed = 0, total = 0, x1 = 9999, y1 = 9999, x2 = -1, y2 = -1;
    json_array_start(w, "missed_houses");
    for (int id = 1; id < building_count(); id++) {
        building *b = building_get(id);
        if (!is_listed_house(b) || !in_rect(b, x, y, w_tiles, h_tiles)) {
            continue;
        }
        total++;
        if (house_is_covered(b)) {
            continue;
        }
        missed++;
        if (b->x < x1) x1 = b->x;
        if (b->y < y1) y1 = b->y;
        if (b->x > x2) x2 = b->x;
        if (b->y > y2) y2 = b->y;
        if (missed <= MAX_LISTED) {
            json_object_start(w, 0);
            json_int(w, "x", b->x);
            json_int(w, "y", b->y);
            json_object_end(w);
        }
    }
    json_array_end(w);
    json_int(w, "houses", total);
    json_int(w, "missed", missed);
    if (missed) {
        char box[48];
        snprintf(box, sizeof(box), "x %d-%d, y %d-%d", x1, x2, y1, y2);
        json_string(w, "missed_area", box);
    }
    if (total_out) {
        *total_out = total;
    }
}

void agent_layout_walker_preview(json_writer *w, const char *args)
{
    char name[48];
    int x, y;
    if (!args || sscanf(args, "%47s %d %d", name, &x, &y) != 3 || !in_map(x, y)) {
        write_error(w, "usage: walker_preview <type> <x> <y> (x,y = top-left, existing or planned building)");
        return;
    }
    building_type type = agent_building_type_from_name(name);
    if (!type) {
        write_error(w, "unknown building type");
        return;
    }
    // An existing building of that type at x,y is simulated as itself; otherwise as a planned one
    int tiles = simulate(0, 0, type, x, y);
    if (!tiles) {
        write_error(w, "no walker route: not a roaming-service building, or no road within reach of that spot");
        return;
    }
    // Rows covering the route, with a margin
    int x1 = 9999, y1 = 9999, x2 = -1, y2 = -1;
    for (int yy = 0; yy < map_grid_height(); yy++) {
        for (int xx = 0; xx < map_grid_width(); xx++) {
            if (travelled(map_grid_offset(xx, yy))) {
                if (xx < x1) x1 = xx;
                if (yy < y1) y1 = yy;
                if (xx > x2) x2 = xx;
                if (yy > y2) y2 = yy;
            }
        }
    }
    x1 = x1 - 3 < 0 ? 0 : x1 - 3;
    y1 = y1 - 3 < 0 ? 0 : y1 - 3;
    x2 = x2 + 3 >= map_grid_width() ? map_grid_width() - 1 : x2 + 3;
    y2 = y2 + 3 >= map_grid_height() ? map_grid_height() - 1 : y2 + 3;
    json_bool(w, "ok", 1);
    json_int(w, "route_tiles", tiles);
    json_int(w, "x", x1);
    json_int(w, "y", y1);
    json_string(w, "legend", "*=road the walkers use, letters = buildings/terrain as in the terrain layer, @=this building");
    char row[GRID_SIZE + 1];
    json_array_start(w, "rows");
    for (int yy = y1; yy <= y2; yy++) {
        int n = 0;
        for (int xx = x1; xx <= x2; xx++) {
            int grid_offset = map_grid_offset(xx, yy);
            row[n++] = xx == x && yy == y ? '@' : travelled(grid_offset) ? '*' : agent_tile_char(grid_offset);
        }
        row[n] = 0;
        json_string(w, 0, row);
    }
    json_array_end(w);
    // Houses around it: served or not
    int rx = x - 15 < 0 ? 0 : x - 15, ry = y - 15 < 0 ? 0 : y - 15;
    write_missed_houses(w, rx, ry, 31, 31, 0);
    json_string(w, "houses_scope", "houses within 15 tiles; 'missed' = this building's walkers don't pass them");
    figure_roamer_preview_reset_building_types();
}

static const struct {
    const char *service;
    building_type types[12];
    int num_types;
} SERVICES[] = {
    { "prefect", { BUILDING_PREFECTURE }, 1 },
    { "engineer", { BUILDING_ENGINEERS_POST }, 1 },
    { "religion", { BUILDING_SMALL_TEMPLE_CERES, BUILDING_SMALL_TEMPLE_NEPTUNE, BUILDING_SMALL_TEMPLE_MERCURY,
        BUILDING_SMALL_TEMPLE_MARS, BUILDING_SMALL_TEMPLE_VENUS, BUILDING_LARGE_TEMPLE_CERES,
        BUILDING_LARGE_TEMPLE_NEPTUNE, BUILDING_LARGE_TEMPLE_MERCURY, BUILDING_LARGE_TEMPLE_MARS,
        BUILDING_LARGE_TEMPLE_VENUS }, 10 },
    { "market", { BUILDING_MARKET }, 1 },
    { "tax_collector", { BUILDING_FORUM, BUILDING_SENATE }, 2 },
    { "bathhouse", { BUILDING_BATHHOUSE }, 1 },
    { "barber", { BUILDING_BARBER }, 1 },
    { "doctor", { BUILDING_DOCTOR }, 1 },
    { "school", { BUILDING_SCHOOL }, 1 },
    { "library", { BUILDING_LIBRARY }, 1 },
};

static int road_at(int x, int y)
{
    return in_map(x, y) && map_terrain_is(map_grid_offset(x, y), TERRAIN_ROAD);
}

static int houses_near(int x, int y, int radius)
{
    int count = 0;
    for (int yy = y - radius; yy <= y + radius; yy++) {
        for (int xx = x - radius; xx <= x + radius; xx++) {
            if (!in_map(xx, yy)) {
                continue;
            }
            unsigned int id = map_building_at(map_grid_offset(xx, yy));
            if (id && is_house_type(building_get(id)->type)) {
                count++;
            }
        }
    }
    return count;
}

static int is_roadblock_at(int x, int y)
{
    unsigned int id = in_map(x, y) ? map_building_at(map_grid_offset(x, y)) : 0;
    return id && building_type_is_roadblock(building_main(building_get(id))->type);
}

void agent_layout_review(json_writer *w, const char *args)
{
    int x = 0, y = 0, w_tiles = map_grid_width(), h_tiles = map_grid_height();
    if (args && *args && sscanf(args, "%d %d %d %d", &x, &y, &w_tiles, &h_tiles) != 4) {
        write_error(w, "usage: layout_review [x y w h]");
        return;
    }
    json_bool(w, "ok", 1);

    json_array_start(w, "services");
    for (unsigned int s = 0; s < sizeof(SERVICES) / sizeof(SERVICES[0]); s++) {
        int buildings = 0;
        for (int t = 0; t < SERVICES[s].num_types; t++) {
            for (building *b = building_first_of_type(SERVICES[s].types[t]); b; b = b->next_of_type) {
                buildings += b->state == BUILDING_STATE_IN_USE || b->state == BUILDING_STATE_CREATED;
            }
        }
        json_object_start(w, 0);
        json_string(w, "service", SERVICES[s].service);
        json_int(w, "buildings", buildings);
        simulate(SERVICES[s].types, SERVICES[s].num_types, 0, 0, 0);
        write_missed_houses(w, x, y, w_tiles, h_tiles, 0);
        json_object_end(w);
    }
    json_array_end(w);
    figure_roamer_preview_reset_building_types();

    // Junctions next to housing: roamers pick randomly there; a roadblock on the branch away from the houses keeps
    // them on the block
    json_array_start(w, "leaks");
    int leaks = 0;
    for (int yy = y; yy < y + h_tiles && leaks < MAX_LISTED; yy++) {
        for (int xx = x; xx < x + w_tiles && leaks < MAX_LISTED; xx++) {
            if (!road_at(xx, yy) || is_roadblock_at(xx, yy) || !houses_near(xx, yy, 2)) {
                continue;
            }
            static const int dx[4] = { 0, 1, 0, -1 }, dy[4] = { -1, 0, 1, 0 };
            int branches = 0, best = -1, best_houses = 9999;
            for (int d = 0; d < 4; d++) {
                int nx = xx + dx[d], ny = yy + dy[d];
                if (!road_at(nx, ny)) {
                    continue;
                }
                branches++;
                if (is_roadblock_at(nx, ny)) {
                    continue;
                }
                int near = houses_near(nx + dx[d], ny + dy[d], 2);
                if (near < best_houses) {
                    best_houses = near;
                    best = d;
                }
            }
            if (branches < 3 || best < 0) {
                continue;
            }
            leaks++;
            json_object_start(w, 0);
            json_int(w, "junction_x", xx);
            json_int(w, "junction_y", yy);
            json_int(w, "branches", branches);
            json_int(w, "suggest_roadblock_x", xx + dx[best]);
            json_int(w, "suggest_roadblock_y", yy + dy[best]);
            json_object_end(w);
        }
    }
    json_array_end(w);
    json_string(w, "leaks_note", "junctions next to houses: roamers choose randomly there. A roadblock on the branch "
        "that leads away from the houses keeps them on the block — then allow the services that need to pass");

    // Roadblocks next to housing that stop services the houses need
    static const struct {
        const char *walker;
        roadblock_permission permission;
    } CHECKED[] = {
        { "maintenance (prefects, engineers)", PERMISSION_MAINTENANCE }, { "priest", PERMISSION_PRIEST },
        { "market", PERMISSION_MARKET }, { "medicine", PERMISSION_MEDICINE }, { "education", PERMISSION_EDUCATION },
        { "tax_collector", PERMISSION_TAX_COLLECTOR },
    };
    json_array_start(w, "roadblocks_blocking_services");
    for (int id = 1; id < building_count(); id++) {
        building *b = building_get(id);
        if (b->state != BUILDING_STATE_IN_USE || !building_type_is_roadblock(b->type) ||
            !in_rect(b, x, y, w_tiles, h_tiles) || !houses_near(b->x, b->y, 3)) {
            continue;
        }
        int blocked = 0;
        for (unsigned int c = 0; c < sizeof(CHECKED) / sizeof(CHECKED[0]); c++) {
            blocked += !building_roadblock_get_permission(CHECKED[c].permission, b);
        }
        if (!blocked) {
            continue;
        }
        json_object_start(w, 0);
        json_int(w, "id", b->id);
        json_string(w, "type", agent_building_type_name(b->type));
        json_int(w, "x", b->x);
        json_int(w, "y", b->y);
        json_array_start(w, "blocks");
        for (unsigned int c = 0; c < sizeof(CHECKED) / sizeof(CHECKED[0]); c++) {
            if (!building_roadblock_get_permission(CHECKED[c].permission, b)) {
                json_string(w, 0, CHECKED[c].walker);
            }
        }
        json_array_end(w);
        json_object_end(w);
    }
    json_array_end(w);
}
