#include "observe.h"

#include "building/building.h"
#include "building/house_evolution.h"
#include "building/count.h"
#include "building/menu.h"
#include "building/monument.h"
#include "building/properties.h"
#include "core/encoding.h"
#include "core/lang.h"
#include "game/resource.h"
#include "map/building.h"
#include "map/desirability.h"
#include "map/grid.h"
#include "map/terrain.h"
#include "scenario/map.h"
#include "translation/translation.h"

#include <stdio.h>
#include <string.h>

#define MAX_LAYERS 4

typedef enum {
    LAYER_TERRAIN,
    LAYER_DESIRABILITY,
    LAYER_WATER
} map_layer;

static const char *LAYER_NAMES[] = { "terrain", "desirability", "water" };

static void write_error(json_writer *w, const char *message)
{
    json_bool(w, "ok", 0);
    json_string(w, "error", message);
}

static int is_house(building_type type)
{
    return type >= BUILDING_HOUSE_VACANT_LOT && type <= BUILDING_HOUSE_LUXURY_PALACE;
}

static int is_menu_group(building_type type)
{
    switch (type) {
        case BUILDING_ANY:
        case BUILDING_MENU_FARMS:
        case BUILDING_MENU_RAW_MATERIALS:
        case BUILDING_MENU_WORKSHOPS:
        case BUILDING_MENU_FORT:
        case BUILDING_MENU_SMALL_TEMPLES:
        case BUILDING_MENU_LARGE_TEMPLES:
        case BUILDING_MENU_GRAND_TEMPLES:
        case BUILDING_MENU_TREES:
        case BUILDING_MENU_PATHS:
        case BUILDING_MENU_PARKS:
        case BUILDING_MENU_STATUES:
        case BUILDING_MENU_GOV_RES:
        case BUILDING_MENU_SHRINES:
        case BUILDING_MENU_GARDENS:
            return 1;
        default:
            return 0;
    }
}

/**
 * The game's names come from scenario-event attributes, which may be alias lists like
 * "goddess_statue|small_statue_alt". Copies alias number `index` into `out`; returns 0 if there is none.
 */
static int get_alias(building_type type, int index, char *out, size_t size)
{
    const building_properties *props = building_properties_for_type(type);
    if (!props || !props->event_data.attr) {
        return 0;
    }
    const char *start = props->event_data.attr;
    for (int i = 0; i < index; i++) {
        start = strchr(start, '|');
        if (!start) {
            return 0;
        }
        start++;
    }
    size_t len = strcspn(start, "|");
    if (len == 0 || len >= size) {
        return 0;
    }
    memcpy(out, start, len);
    out[len] = 0;
    return 1;
}

const char *agent_building_type_name(int type)
{
    static char name[48];
    if (type == BUILDING_HOUSE_VACANT_LOT) {
        return "vacant_lot";
    }
    if (type > BUILDING_NONE && type < BUILDING_TYPE_MAX && get_alias(type, 0, name, sizeof(name))) {
        if (is_house(type)) {
            // e.g. the small tent's aliases are "vacant_lot|house_small_tent|housing"
            char alias[48];
            for (int i = 0; get_alias(type, i, alias, sizeof(alias)); i++) {
                if (strncmp(alias, "house_", 6) == 0) {
                    strcpy(name, alias);
                    break;
                }
            }
        }
        return name;
    }
    snprintf(name, sizeof(name), "type_%d", type);
    return name;
}

const char *agent_resource_name(int resource)
{
    // Resource names can be alias lists too ("timber|wood"); use the first
    static char name[32];
    const char *attr = resource_get_data(resource)->xml_attr_name;
    size_t len = strcspn(attr, "|");
    if (len >= sizeof(name)) {
        len = sizeof(name) - 1;
    }
    memcpy(name, attr, len);
    name[len] = 0;
    return name;
}

int agent_resource_from_name(const char *name)
{
    size_t len = strlen(name);
    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        const char *attr = resource_get_data(r)->xml_attr_name;
        while (attr && *attr) {
            size_t alias_len = strcspn(attr, "|");
            if (alias_len == len && strncmp(attr, name, len) == 0) {
                return r;
            }
            attr = attr[alias_len] ? attr + alias_len + 1 : 0;
        }
    }
    return RESOURCE_NONE;
}

int agent_building_type_from_name(const char *name)
{
    if (strcmp(name, "housing") == 0 || strcmp(name, "vacant_lot") == 0) {
        return BUILDING_HOUSE_VACANT_LOT;
    }
    if (strncmp(name, "house_", 6) == 0) {
        return BUILDING_NONE; // houses grow from housing plots, they can't be built directly
    }
    // The first match wins, e.g. "reservoir" is the draggable reservoir the build menu uses
    char alias[48];
    for (int type = BUILDING_NONE + 1; type < BUILDING_TYPE_MAX; type++) {
        if (is_menu_group(type) || is_house(type)) {
            continue;
        }
        for (int i = 0; get_alias(type, i, alias, sizeof(alias)); i++) {
            if (strcmp(alias, name) == 0) {
                return type;
            }
        }
    }
    return BUILDING_NONE;
}

static char building_char(building_type type)
{
    if (is_house(type)) {
        return 'H';
    }
    switch (type) {
        // Augustus models these as buildings; give them their terrain letters
        case BUILDING_AQUEDUCT:
            return 'a';
        case BUILDING_ROAD:
        case BUILDING_LOW_BRIDGE:
        case BUILDING_SHIP_BRIDGE:
            return '=';
        case BUILDING_HIGHWAY:
            return '%';
        case BUILDING_WALL:
        case BUILDING_PALISADE:
            return '#';
        case BUILDING_FOUNTAIN:
            return 'F';
        case BUILDING_WELL:
            return 'W';
        case BUILDING_RESERVOIR:
            return 'R';
        case BUILDING_LATRINES:
            return 'L';
        case BUILDING_PREFECTURE:
            return 'P';
        case BUILDING_ENGINEERS_POST:
            return 'E';
        case BUILDING_MARKET:
            return 'M';
        case BUILDING_GRANARY:
            return 'G';
        case BUILDING_WAREHOUSE:
        case BUILDING_WAREHOUSE_SPACE:
            return 'S';
        case BUILDING_ROADBLOCK:
        case BUILDING_GATEHOUSE:
        case BUILDING_PALISADE_GATE:
        case BUILDING_ROOFED_GARDEN_WALL_GATE:
        case BUILDING_HEDGE_GATE_DARK:
        case BUILDING_HEDGE_GATE_LIGHT:
        case BUILDING_PANELLED_GARDEN_GATE:
        case BUILDING_LOOPED_GARDEN_GATE:
            return 'O';
        case BUILDING_SMALL_TEMPLE_CERES:
        case BUILDING_SMALL_TEMPLE_NEPTUNE:
        case BUILDING_SMALL_TEMPLE_MERCURY:
        case BUILDING_SMALL_TEMPLE_MARS:
        case BUILDING_SMALL_TEMPLE_VENUS:
        case BUILDING_LARGE_TEMPLE_CERES:
        case BUILDING_LARGE_TEMPLE_NEPTUNE:
        case BUILDING_LARGE_TEMPLE_MERCURY:
        case BUILDING_LARGE_TEMPLE_MARS:
        case BUILDING_LARGE_TEMPLE_VENUS:
        case BUILDING_GRAND_TEMPLE_CERES:
        case BUILDING_GRAND_TEMPLE_NEPTUNE:
        case BUILDING_GRAND_TEMPLE_MERCURY:
        case BUILDING_GRAND_TEMPLE_MARS:
        case BUILDING_GRAND_TEMPLE_VENUS:
        case BUILDING_SHRINE_CERES:
        case BUILDING_SHRINE_NEPTUNE:
        case BUILDING_SHRINE_MERCURY:
        case BUILDING_SHRINE_MARS:
        case BUILDING_SHRINE_VENUS:
        case BUILDING_ORACLE:
        case BUILDING_LARARIUM:
        case BUILDING_PANTHEON:
            return 'T';
        case BUILDING_THEATER:
        case BUILDING_AMPHITHEATER:
        case BUILDING_COLOSSEUM:
        case BUILDING_HIPPODROME:
        case BUILDING_ARENA:
        case BUILDING_TAVERN:
        case BUILDING_ACTOR_COLONY:
        case BUILDING_GLADIATOR_SCHOOL:
        case BUILDING_LION_HOUSE:
        case BUILDING_CHARIOT_MAKER:
            return 'A';
        case BUILDING_SCHOOL:
        case BUILDING_LIBRARY:
        case BUILDING_ACADEMY:
            return 'U';
        case BUILDING_DOCTOR:
        case BUILDING_HOSPITAL:
        case BUILDING_BATHHOUSE:
        case BUILDING_BARBER:
            return 'D';
        case BUILDING_WHEAT_FARM:
        case BUILDING_VEGETABLE_FARM:
        case BUILDING_FRUIT_FARM:
        case BUILDING_OLIVE_FARM:
        case BUILDING_VINES_FARM:
        case BUILDING_PIG_FARM:
            return 'Y';
        case BUILDING_MARBLE_QUARRY:
        case BUILDING_IRON_MINE:
        case BUILDING_TIMBER_YARD:
        case BUILDING_CLAY_PIT:
        case BUILDING_GOLD_MINE:
        case BUILDING_SAND_PIT:
        case BUILDING_STONE_QUARRY:
        case BUILDING_WINE_WORKSHOP:
        case BUILDING_OIL_WORKSHOP:
        case BUILDING_WEAPONS_WORKSHOP:
        case BUILDING_FURNITURE_WORKSHOP:
        case BUILDING_POTTERY_WORKSHOP:
        case BUILDING_CONCRETE_MAKER:
        case BUILDING_BRICKWORKS:
            return 'I';
        case BUILDING_FORUM:
        case BUILDING_SENATE:
        case BUILDING_GOVERNORS_HOUSE:
        case BUILDING_GOVERNORS_VILLA:
        case BUILDING_GOVERNORS_PALACE:
        case BUILDING_CITY_MINT:
            return 'V';
        case BUILDING_DOCK:
        case BUILDING_WHARF:
        case BUILDING_SHIPYARD:
        case BUILDING_LIGHTHOUSE:
            return 'K';
        case BUILDING_WORKCAMP:
        case BUILDING_ARCHITECT_GUILD:
        case BUILDING_DEPOT:
        case BUILDING_CARAVANSERAI:
        case BUILDING_HIGHWAY_STATION:
        case BUILDING_MISSION_POST:
            return 'C';
        case BUILDING_FORT_LEGIONARIES:
        case BUILDING_FORT_JAVELIN:
        case BUILDING_FORT_MOUNTED:
        case BUILDING_FORT_AUXILIA_INFANTRY:
        case BUILDING_FORT_ARCHERS:
        case BUILDING_FORT_GROUND:
        case BUILDING_BARRACKS:
        case BUILDING_MILITARY_ACADEMY:
        case BUILDING_TOWER:
        case BUILDING_WATCHTOWER:
        case BUILDING_MESS_HALL:
        case BUILDING_ARMOURY:
            return 'X';
        case BUILDING_NATIVE_HUT:
        case BUILDING_NATIVE_MEETING:
        case BUILDING_NATIVE_CROPS:
        case BUILDING_NATIVE_HUT_ALT:
        case BUILDING_NATIVE_WATCHTOWER:
        case BUILDING_NATIVE_MONUMENT:
        case BUILDING_NATIVE_DECORATION:
        case BUILDING_NATIVE_WELL:
        case BUILDING_NATIVE_LARGE_HUT_ALT:
        case BUILDING_NATIVE_HUT_ALT_2:
        case BUILDING_NATIVE_LARGE_HUT_ALT_2:
        case BUILDING_NATIVE_PALISADE:
            return 'N';
        case BUILDING_BURNING_RUIN:
            return '!';
        default:
            return 'Z'; // gardens, plazas, statues, trees, paths and anything else
    }
}

char agent_tile_char(int grid_offset)
{
    int building_id = map_building_at(grid_offset);
    if (building_id) {
        building *b = building_get(building_id);
        if (b->state == BUILDING_STATE_IN_USE || b->state == BUILDING_STATE_MOTHBALLED ||
            b->state == BUILDING_STATE_CREATED) {
            return building_char(b->type);
        }
    }
    int terrain = map_terrain_get(grid_offset);
    if (terrain & TERRAIN_HIGHWAY) {
        return '%';
    }
    if (terrain & TERRAIN_ROAD) {
        return '=';
    }
    if (terrain & TERRAIN_AQUEDUCT) {
        return 'a';
    }
    if (terrain & TERRAIN_WALL_OR_GATEHOUSE) {
        return '#';
    }
    if (terrain & TERRAIN_RUBBLE) {
        return 'x';
    }
    if (terrain & TERRAIN_SHALLOW_WATER) {
        return ';';
    }
    if (terrain & TERRAIN_WATER) {
        return '~';
    }
    if (terrain & TERRAIN_ACCESS_RAMP) {
        return '/';
    }
    if (terrain & TERRAIN_ELEVATION) {
        return '_';
    }
    if (terrain & TERRAIN_ROCK) {
        return '^';
    }
    if (terrain & (TERRAIN_TREE | TERRAIN_SHRUB)) {
        return 't';
    }
    if (terrain & TERRAIN_MARSHLAND) {
        return '*';
    }
    if (terrain & TERRAIN_BEACH) {
        return ':';
    }
    if (terrain & TERRAIN_GARDEN) {
        return 'Z';
    }
    if (terrain & TERRAIN_MEADOW) {
        return ',';
    }
    return '.';
}

static char desirability_char(int grid_offset)
{
    int value = map_desirability_get(grid_offset);
    if (value < 0) {
        return value <= -10 ? '=' : '-';
    }
    if (value >= 100) {
        return '+';
    }
    return (char) ('0' + value / 10);
}

static char water_char(int grid_offset)
{
    int terrain = map_terrain_get(grid_offset);
    int fountain = terrain & TERRAIN_FOUNTAIN_RANGE;
    int reservoir = terrain & TERRAIN_RESERVOIR_RANGE;
    if (fountain && reservoir) {
        return 'b';
    }
    if (fountain) {
        return 'f';
    }
    if (reservoir) {
        return 'r';
    }
    return '.';
}

static void write_legend(json_writer *w, map_layer layer)
{
    switch (layer) {
        case LAYER_TERRAIN:
            json_string(w, "legend",
                "terrain: .=clear ,=meadow t=trees ^=rock ~=water ;=shallow water _=cliff /=ramp *=marsh :=beach "
                "==road %=highway a=aqueduct #=wall x=rubble | buildings: H=house F=fountain W=well R=reservoir "
                "L=latrines P=prefecture E=engineers post M=market G=granary S=warehouse O=roadblock/gate "
                "T=temple A=entertainment U=education D=health Y=farm I=industry/workshop V=government K=dock/wharf "
                "C=logistics X=military N=native !=fire Z=decoration/garden/plaza/other");
            break;
        case LAYER_DESIRABILITY:
            json_string(w, "legend", "desirability in tens: 0=0..9 1=10..19 ... 9=90..99 +=100+ -=-1..-9 ==-10 or less");
            break;
        case LAYER_WATER:
            json_string(w, "legend", "f=fountain range r=reservoir range b=both .=none (wells are per house)");
            break;
    }
}

void agent_observe_map(json_writer *w, const char *args)
{
    int width = map_grid_width();
    int height = map_grid_height();
    int x = 0, y = 0, w_tiles = width, h_tiles = height;
    char layer_names[MAX_LAYERS][16];
    int num_words = 0;
    int num_layers = 0;
    map_layer layers[MAX_LAYERS];

    // Optional "x y w h" followed by layer names
    int consumed = 0;
    if (args && sscanf(args, "%d %d %d %d%n", &x, &y, &w_tiles, &h_tiles, &consumed) == 4) {
        args += consumed;
    } else {
        x = 0;
        y = 0;
        w_tiles = width;
        h_tiles = height;
    }
    while (args && num_words < MAX_LAYERS &&
        sscanf(args, "%15s%n", layer_names[num_words], &consumed) == 1) {
        args += consumed;
        num_words++;
    }
    for (int i = 0; i < num_words; i++) {
        int found = 0;
        for (int l = 0; l < (int) (sizeof(LAYER_NAMES) / sizeof(LAYER_NAMES[0])); l++) {
            if (strcmp(layer_names[i], LAYER_NAMES[l]) == 0) {
                layers[num_layers++] = (map_layer) l;
                found = 1;
            }
        }
        if (!found) {
            write_error(w, "unknown layer; use terrain, desirability, water");
            return;
        }
    }
    if (!num_layers) {
        layers[num_layers++] = LAYER_TERRAIN;
    }

    if (x < 0 || y < 0 || w_tiles < 1 || h_tiles < 1 || x + w_tiles > width || y + h_tiles > height) {
        write_error(w, "region outside the map");
        json_int(w, "map_width", width);
        json_int(w, "map_height", height);
        return;
    }

    json_bool(w, "ok", 1);
    json_int(w, "map_width", width);
    json_int(w, "map_height", height);
    json_int(w, "x", x);
    json_int(w, "y", y);
    json_int(w, "w", w_tiles);
    json_int(w, "h", h_tiles);
    map_point entry = scenario_map_entry();
    map_point exit = scenario_map_exit();
    json_object_start(w, "entry");
    json_int(w, "x", entry.x);
    json_int(w, "y", entry.y);
    json_object_end(w);
    json_object_start(w, "exit");
    json_int(w, "x", exit.x);
    json_int(w, "y", exit.y);
    json_object_end(w);

    char row[GRID_SIZE + 1];
    json_array_start(w, "layers");
    for (int l = 0; l < num_layers; l++) {
        json_object_start(w, 0);
        json_string(w, "name", LAYER_NAMES[layers[l]]);
        write_legend(w, layers[l]);
        json_array_start(w, "rows");
        for (int yy = y; yy < y + h_tiles; yy++) {
            for (int xx = x; xx < x + w_tiles; xx++) {
                int grid_offset = map_grid_offset(xx, yy);
                char c;
                switch (layers[l]) {
                    case LAYER_DESIRABILITY:
                        c = desirability_char(grid_offset);
                        break;
                    case LAYER_WATER:
                        c = water_char(grid_offset);
                        break;
                    default:
                        c = agent_tile_char(grid_offset);
                        break;
                }
                row[xx - x] = c;
            }
            row[w_tiles] = 0;
            json_string(w, 0, row);
        }
        json_array_end(w);
        json_object_end(w);
    }
    json_array_end(w);
}

static int is_listable(const building *b)
{
    // CREATED = placed this tick, becomes IN_USE on the next building state update
    if (b->state != BUILDING_STATE_IN_USE && b->state != BUILDING_STATE_MOTHBALLED &&
        b->state != BUILDING_STATE_CREATED) {
        return 0;
    }
    if (b->prev_part_building_id || b->type == BUILDING_WAREHOUSE_SPACE) {
        return 0; // only list the main part of multi-part buildings
    }
    if (building_properties_for_type(b->type)->shared) {
        return 0; // one building record for all tiles of e.g. aqueducts, placed at 0,0: not a real position
    }
    return 1;
}

static void write_stock(json_writer *w, building *b)
{
    int amounts[RESOURCE_MAX] = { 0 };
    if (b->type == BUILDING_WAREHOUSE) {
        // Goods are stored in the 8 warehouse spaces, in cartloads
        building *space = b;
        for (int i = 0; i < 8; i++) {
            space = building_next(space);
            if (space->id <= 0) {
                break;
            }
            int r = space->subtype.warehouse_resource_id;
            if (r > RESOURCE_NONE && r < RESOURCE_MAX) {
                amounts[r] += space->resources[r];
            }
        }
    } else {
        for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
            amounts[r] = b->resources[r];
        }
    }
    int any = 0;
    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        if (amounts[r] > 0) {
            if (!any) {
                json_object_start(w, "stock");
                any = 1;
            }
            json_int(w, agent_resource_name(r), amounts[r]);
        }
    }
    if (any) {
        json_object_end(w);
    }
}

void agent_observe_buildings(json_writer *w, const char *args)
{
    char filter[48] = "";
    if (args) {
        sscanf(args, "%47s", filter);
    }
    json_bool(w, "ok", 1);
    json_array_start(w, "buildings");
    for (int id = 1; id < building_count(); id++) {
        building *b = building_get(id);
        if (!is_listable(b)) {
            continue;
        }
        const char *name = agent_building_type_name(b->type);
        if (*filter) {
            if (strcmp(filter, name) != 0) {
                continue;
            }
        } else if (is_house(b->type)) {
            continue;
        }
        json_object_start(w, 0);
        json_int(w, "id", b->id);
        json_string(w, "type", name);
        json_int(w, "x", b->x);
        json_int(w, "y", b->y);
        json_int(w, "size", b->type == BUILDING_WAREHOUSE ? 3 : b->size);
        int laborers = model_get_building(b->type)->laborers;
        if (laborers) {
            json_int(w, "workers", b->num_workers);
            json_int(w, "workers_needed", laborers);
        }
        json_bool(w, "road_access", b->has_road_access);
        if (b->state == BUILDING_STATE_MOTHBALLED) {
            json_bool(w, "mothballed", 1);
        }
        if (!b->fire_proof) {
            json_int(w, "fire_risk", b->fire_risk);
            json_int(w, "damage_risk", b->damage_risk);
        }
        if (b->has_problem) {
            json_bool(w, "has_problem", 1);
        }
        if (b->has_plague) {
            json_bool(w, "plague", 1);
        }
        if (building_monument_is_unfinished_monument(b)) {
            // Monuments (incl. oracles, large temples, colosseum...) are built in phases from materials that
            // work camp workers deliver from warehouses; they don't work until finished
            json_object_start(w, "construction");
            json_int(w, "phase", b->monument.phase);
            json_int(w, "phases", building_monument_phases(b->type));
            json_bool(w, "halted", building_monument_is_construction_halted(b));
            json_object_start(w, "materials_still_needed");
            for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
                if (b->resources[r] > 0) {
                    json_int(w, agent_resource_name(r), b->resources[r]);
                }
            }
            json_object_end(w);
            json_int(w, "workcamps", building_count_active(BUILDING_WORKCAMP));
            json_object_end(w);
        } else {
            write_stock(w, b);
        }
        json_object_end(w);
    }
    json_array_end(w);
}

static const uint8_t *evolve_text(const building *b)
{
    int id = b->data.house.evolve_text_id;
    if (id > 106) {
        return translation_for(id); // Augustus custom translation key
    }
    return lang_get_string(127, 40 + id);
}

void agent_observe_houses(json_writer *w, const char *args)
{
    int x = 0, y = 0, w_tiles = map_grid_width(), h_tiles = map_grid_height();
    if (args && *args && sscanf(args, "%d %d %d %d", &x, &y, &w_tiles, &h_tiles) != 4) {
        write_error(w, "usage: houses [x y w h]");
        return;
    }
    int total = 0;
    json_bool(w, "ok", 1);
    json_array_start(w, "houses");
    for (int id = 1; id < building_count(); id++) {
        building *b = building_get(id);
        if (!is_listable(b) || !is_house(b->type)) {
            continue;
        }
        if (b->x < x || b->y < y || b->x >= x + w_tiles || b->y >= y + h_tiles) {
            continue;
        }
        total++;
        json_object_start(w, 0);
        json_int(w, "id", b->id);
        json_string(w, "level", agent_building_type_name(b->type));
        json_int(w, "x", b->x);
        json_int(w, "y", b->y);
        json_int(w, "size", b->size);
        json_int(w, "population", b->house_population);
        json_int(w, "room", b->house_population_room);
        json_int(w, "desirability", b->desirability);
        json_int(w, "happiness", b->sentiment.house_happiness);
        json_bool(w, "road_access", b->has_road_access);
        json_bool(w, "fountain", b->has_water_access);
        json_bool(w, "well", b->has_well_access);
        json_int(w, "foods", b->data.house.num_foods);
        write_stock(w, b);

        json_object_start(w, "access");
        json_int(w, "entertainment", b->data.house.entertainment);
        json_int(w, "education", b->data.house.education);
        json_int(w, "health", b->data.house.health);
        json_int(w, "gods", b->data.house.num_gods);
        json_int(w, "school", b->data.house.school);
        json_int(w, "library", b->data.house.library);
        json_int(w, "academy", b->data.house.academy);
        json_int(w, "barber", b->data.house.barber);
        json_int(w, "clinic", b->data.house.clinic);
        json_int(w, "bathhouse", b->data.house.bathhouse);
        json_int(w, "hospital", b->data.house.hospital);
        json_int(w, "tax_collector", b->house_tax_coverage);
        json_object_end(w);

        if (b->has_plague) {
            json_bool(w, "plague", 1);
        }
        // The game only computes this when the house info window opens, so compute it fresh
        int worst = building_house_determine_worst_desirability_building_type(b);
        building_house_determine_evolve_text(b, worst);
        json_int(w, "evolve_id", b->data.house.evolve_text_id);
        if (b->data.house.evolve_text_id == 62) {
            // "A nearby building (" + building + ") " + rest of the sentence, as the house window shows it
            char start[256], rest[512], full[800];
            encoding_to_utf8(lang_get_string(127, 102), start, sizeof(start), 0);
            encoding_to_utf8(lang_get_string(127, 103), rest, sizeof(rest), 0);
            snprintf(full, sizeof(full), "%s%s) %s", start, agent_building_type_name(worst), rest);
            json_string(w, "evolve", full);
            json_string(w, "worst_desirability_building", agent_building_type_name(worst));
        } else {
            json_game_string(w, "evolve", evolve_text(b));
        }
        json_object_end(w);
    }
    json_array_end(w);
    json_int(w, "count", total);
}

void agent_observe_buildable(json_writer *w)
{
    json_bool(w, "ok", 1);
    json_array_start(w, "buildable");
    for (int type = BUILDING_NONE + 1; type < BUILDING_TYPE_MAX; type++) {
        if (is_menu_group(type) || (is_house(type) && type != BUILDING_HOUSE_VACANT_LOT) ||
            type == BUILDING_RESERVOIR || type == BUILDING_CLEAR_LAND || type == BUILDING_REPAIR_LAND ||
            !building_menu_is_enabled(type)) {
            continue; // BUILDING_RESERVOIR is placed through the draggable reservoir; clearing has its own command
        }
        const char *name = type == BUILDING_HOUSE_VACANT_LOT ? "housing" : agent_building_type_name(type);
        if (!name || strncmp(name, "type_", 5) == 0) {
            continue;
        }
        // Use the name that `build` resolves back to this type (e.g. paths share a first alias with trees)
        char alias[48];
        if (type != BUILDING_HOUSE_VACANT_LOT && agent_building_type_from_name(name) != type) {
            int found = 0;
            for (int i = 1; get_alias(type, i, alias, sizeof(alias)); i++) {
                if (agent_building_type_from_name(alias) == type) {
                    name = alias;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                continue;
            }
        }
        json_object_start(w, 0);
        json_string(w, "type", name);
        json_int(w, "cost", model_get_building(type)->cost);
        json_int(w, "size", type == BUILDING_WAREHOUSE ? 3 : building_properties_for_type(type)->size);
        int laborers = model_get_building(type)->laborers;
        if (laborers) {
            json_int(w, "workers", laborers);
        }
        json_object_end(w);
    }
    json_array_end(w);
}
