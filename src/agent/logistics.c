#include "logistics.h"

#include "agent/observe.h"
#include "building/building.h"
#include "building/distribution.h"
#include "building/granary.h"
#include "building/industry.h"
#include "building/market.h"
#include "building/properties.h"
#include "building/storage.h"
#include "building/warehouse.h"
#include "city/resource.h"
#include "core/calc.h"
#include "core/config.h"
#include "figure/action.h"
#include "figure/figure.h"
#include "game/resource.h"
#include "map/data.h"
#include "map/grid.h"
#include "map/point.h"
#include "map/road_access.h"
#include "map/routing.h"
#include "scenario/property.h"

#include <stdio.h>
#include <string.h>


static void write_error(json_writer *w, const char *message)
{
    json_bool(w, "ok", 0);
    json_string(w, "error", message);
}

static int is_storage(const building *b)
{
    return b && b->state == BUILDING_STATE_IN_USE && (b->type == BUILDING_WAREHOUSE || b->type == BUILDING_GRANARY);
}

static int staffing_percent(const building *b)
{
    int needed = model_get_building(b->type)->laborers;
    return needed ? calc_percentage(b->num_workers, needed) : 100;
}

static const char *CONDITIONS[] = { "never", "always", "source_over", "dest_under" };

static void write_order(json_writer *w, building *depot)
{
    const order *o = &depot->data.depot.current_order;
    json_object_start(w, "order");
    json_string(w, "resource", o->resource_type ? agent_resource_name(o->resource_type) : "");
    json_int(w, "source_id", o->src_storage_id);
    json_int(w, "destination_id", o->dst_storage_id);
    json_string(w, "condition", CONDITIONS[o->condition.condition_type]);
    if (o->condition.condition_type == ORDER_CONDITION_SOURCE_HAS_MORE_THAN ||
        o->condition.condition_type == ORDER_CONDITION_DESTINATION_HAS_LESS_THAN) {
        json_int(w, "threshold", o->condition.threshold);
    }
    json_object_end(w);
    int carts = 0;
    for (int i = 0; i < 3; i++) {
        if (depot->data.distribution.cartpusher_ids[i]) {
            carts++;
        }
    }
    json_int(w, "carts", carts);
    json_int(w, "staffing_percent", staffing_percent(depot));
}

void agent_logistics_depot(json_writer *w, const char *args)
{
    int id = 0, src = 0, dst = 0, threshold = 0;
    char resource_name[32] = "", condition[16] = "always";
    int n = args ? sscanf(args, "%d %31s %d %d %15s %d", &id, resource_name, &src, &dst, condition, &threshold) : 0;
    building *depot = id > 0 && id < building_count() ? building_get(id) : 0;
    if (n < 1 || !depot || depot->type != BUILDING_DEPOT || depot->state != BUILDING_STATE_IN_USE) {
        write_error(w, "usage: depot <cart depot id> [<resource> <source storage id> <destination storage id> "
            "[always|never|source_over N|dest_under N]]");
        return;
    }
    if (n > 1) {
        resource_type r = agent_resource_from_name(resource_name);
        building *source = src > 0 && src < building_count() ? building_get(src) : 0;
        building *destination = dst > 0 && dst < building_count() ? building_get(dst) : 0;
        if (n < 4 || r == RESOURCE_NONE || !resource_is_storable(r)) {
            write_error(w, "need a storable resource, a source and a destination storage id");
            return;
        }
        if (!is_storage(source) || !is_storage(destination) || src == dst) {
            write_error(w, "source and destination must be two different warehouses/granaries in use");
            return;
        }
        if (!resource_is_food(r) && (source->type == BUILDING_GRANARY || destination->type == BUILDING_GRANARY)) {
            write_error(w, "granaries only hold food");
            return;
        }
        int condition_type = -1;
        for (int i = 0; i < 4; i++) {
            if (strcmp(condition, CONDITIONS[i]) == 0) {
                condition_type = i;
            }
        }
        int needs_threshold = condition_type == ORDER_CONDITION_SOURCE_HAS_MORE_THAN ||
            condition_type == ORDER_CONDITION_DESTINATION_HAS_LESS_THAN;
        if (condition_type < 0 || (needs_threshold && (n < 6 || threshold < 0 || threshold > 32))) {
            write_error(w, "condition: always, never, source_over N or dest_under N (N in cartloads, 0-32)");
            return;
        }
        // As the depot window does; carts already under way re-check their order and reroute
        order *o = &depot->data.depot.current_order;
        o->resource_type = r;
        o->src_storage_id = src;
        o->dst_storage_id = dst;
        o->condition.condition_type = condition_type;
        o->condition.threshold = needs_threshold ? threshold : 0;
    }
    json_bool(w, "ok", 1);
    write_order(w, depot);
}

static void write_storage_building(json_writer *w, building *b)
{
    json_object_start(w, 0);
    json_int(w, "id", b->id);
    json_string(w, "type", agent_building_type_name(b->type));
    json_int(w, "x", b->x);
    json_int(w, "y", b->y);
    json_int(w, "staffing_percent", staffing_percent(b));
    json_bool(w, "road_access", b->has_road_access);
    int stored = 0;
    json_object_start(w, "contents");
    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        int amount = b->type == BUILDING_WAREHOUSE ? building_warehouse_get_amount(b, r) :
            (resource_is_food(r) ? building_granary_get_amount(b, r) : 0);
        if (amount > 0) {
            json_int(w, agent_resource_name(r), amount);
            stored += amount;
        }
    }
    json_object_end(w);
    json_int(w, "stored", stored);
    json_int(w, "free_space", b->type == BUILDING_WAREHOUSE ? building_warehouse_get_free_space_amount(b) :
        building_granary_get_free_space_amount(b));
    json_array_start(w, "accepts");
    const building_storage *s = building_storage_get(b->storage_id);
    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        if (s && s->resource_state[r].state != BUILDING_STORAGE_STATE_NOT_ACCEPTING &&
            (b->type == BUILDING_WAREHOUSE || resource_is_food(r))) {
            json_string(w, 0, agent_resource_name(r));
        }
    }
    json_array_end(w);
    json_object_end(w);
}

static int is_producer(building_type type)
{
    return building_is_farm(type) || building_is_raw_resource_producer(type) || building_is_workshop(type);
}

/**
 * The producer's cart pusher is waiting at the building because no storage would take its load. The game marks
 * that in min_max_seen (1 = nobody accepts, 2 = only understaffed storages would; used for its tooltip) and
 * keeps retrying, resetting wait_ticks each time (figuretype/cartpusher.c determine_cartpusher_destination).
 */
static figure *waiting_cart_pusher(const building *b)
{
    if (!b->figure_id) {
        return 0;
    }
    figure *f = figure_get(b->figure_id);
    if (f->state != FIGURE_STATE_ALIVE || f->type != FIGURE_CART_PUSHER) {
        return 0;
    }
    if ((f->action_state == FIGURE_ACTION_20_CARTPUSHER_INITIAL && f->min_max_seen) ||
        f->action_state == FIGURE_ACTION_245_CARTPUSHER_WAITING_FOR_DESTINATION) {
        return f;
    }
    return 0;
}

void agent_logistics_storage_report(json_writer *w)
{
    json_bool(w, "ok", 1);
    json_array_start(w, "storage");
    for (building *b = building_first_of_type(BUILDING_WAREHOUSE); b; b = b->next_of_type) {
        if (is_storage(b)) {
            write_storage_building(w, b);
        }
    }
    for (building *b = building_first_of_type(BUILDING_GRANARY); b; b = b->next_of_type) {
        if (is_storage(b)) {
            write_storage_building(w, b);
        }
    }
    json_array_end(w);

    // Workplaces whose labour seeker passes no house: no workers however many are unemployed (unless global labour)
    json_array_start(w, "no_labour_access");
    for (int id = 1; id < building_count(); id++) {
        building *b = building_get(id);
        if (b->state != BUILDING_STATE_IN_USE || building_main(b) != b || !model_get_building(b->type)->laborers ||
            b->type == BUILDING_FOUNTAIN || b->type == BUILDING_LATRINES) {
            continue;
        }
        if (b->houses_covered <= 0 && b->num_workers <= 0) {
            json_object_start(w, 0);
            json_int(w, "id", b->id);
            json_string(w, "type", agent_building_type_name(b->type));
            json_int(w, "x", b->x);
            json_int(w, "y", b->y);
            json_object_end(w);
        }
    }
    json_array_end(w);

    json_array_start(w, "stuck_producers");
    for (int id = 1; id < building_count(); id++) {
        building *b = building_get(id);
        if (b->state != BUILDING_STATE_IN_USE || !is_producer(b->type)) {
            continue;
        }
        figure *f = waiting_cart_pusher(b);
        if (!f) {
            continue;
        }
        resource_type r = b->output_resource_id;
        int understaffed = 0;
        map_point dst;
        int warehouse = building_warehouse_for_storing(0, b->x, b->y, r, b->road_network_id, &understaffed, &dst);
        int granary = resource_is_food(r) ?
            building_granary_for_storing(b->x, b->y, r, b->road_network_id, 0, &understaffed, &dst) : 0;
        json_object_start(w, 0);
        json_int(w, "id", b->id);
        json_string(w, "type", agent_building_type_name(b->type));
        json_int(w, "x", b->x);
        json_int(w, "y", b->y);
        json_string(w, "waiting_with", agent_resource_name(r));
        if (warehouse || granary) {
            json_string(w, "reason", "a storage accepts it now; the cart should leave shortly");
        } else if (understaffed) {
            json_string(w, "reason", "the only storages that would accept it are understaffed");
            json_int(w, "understaffed_storages", understaffed);
        } else {
            json_string(w, "reason", "no warehouse/granary on its road network accepts it with free space");
        }
        json_object_end(w);
    }
    json_array_end(w);
}

static int stored_amount(building *b, resource_type r)
{
    if (b->type == BUILDING_WAREHOUSE) {
        return resource_is_food(r) ? 0 : building_warehouse_get_available_amount(b, r);
    }
    return resource_is_food(r) ? b->resources[r] : 0;
}

/**
 * Why a storage can't supply this distributor, mirroring building/distribution.c is_invalid_destination() and
 * get_resource_storages(); 0 if it can.
 */
static const char *supply_blocker(building *b, building *dist, int permission, int max_distance, int distance)
{
    if (b->state != BUILDING_STATE_IN_USE) {
        return "not in use";
    }
    if (!b->has_road_access) {
        return "storage has no road access";
    }
    if (b->road_network_id != dist->road_network_id) {
        return "on a different road network (no road connects them)";
    }
    if (b->distance_from_entry <= 0) {
        return "storage road isn't connected to the city entry road";
    }
    if (!building_storage_get_permission(permission, b)) {
        return "storage forbids this walker (storage permissions; storage <id> permission <walker> on)";
    }
    if (distance >= max_distance) {
        return "out of range (40 tiles from the building edge)";
    }
    return 0;
}

/** Tiles the buyer walks one way, between the two road access tiles (as figure/route.c routes it); -1 if none */
static int road_trip(building *from, building *to)
{
    map_point a, b;
    int to_road = to->type == BUILDING_GRANARY ? map_has_road_access_granary(to->x, to->y, &b) :
        map_has_road_access_warehouse(to->x, to->y, &b);
    if (!map_has_road_access(from->x, from->y, building_properties_for_type(from->type)->size, &a) || !to_road) {
        return -1;
    }
    if (a.x == b.x && a.y == b.y) {
        return 0;
    }
    if (!map_routing_citizen_can_travel_over_road_garden(a.x, a.y, b.x, b.y, 4)) {
        return -1;
    }
    return map_routing_distance(map_grid_offset(b.x, b.y)) - 1;
}

static figure *market_supplier(const building *b)
{
    if (!b->figure_id2) {
        return 0;
    }
    figure *f = figure_get(b->figure_id2);
    return f->state == FIGURE_STATE_ALIVE && f->type == FIGURE_MARKET_SUPPLIER ? f : 0;
}

void agent_logistics_supply(json_writer *w, const char *args)
{
    int id = 0;
    char resource[32] = "";
    int n = args ? sscanf(args, "%d %31s", &id, resource) : 0;
    building *b = n >= 1 && id > 0 && id < building_count() ? building_get(id) : 0;
    if (!b || b->state != BUILDING_STATE_IN_USE || !building_type_is_distributor(b->type)) {
        write_error(w, "usage: supply <market/temple/tavern/... id> [<resource>]");
        return;
    }
    resource_type only = n == 2 ? agent_resource_from_name(resource) : RESOURCE_NONE;
    if (n == 2 && (only == RESOURCE_NONE || !building_distribution_resource_is_handled(only, b->type))) {
        write_error(w, "this building doesn't fetch that resource");
        return;
    }
    int max_distance = b->type == BUILDING_MARKET && config_get(CONFIG_GP_CH_MARKET_RANGE) ?
        MARKET_MAX_DISTANCE : map_data.width;
    int permission = building_storage_get_permission_from_building_type(b->type);
    int size = building_properties_for_type(b->type)->size;

    json_bool(w, "ok", 1);
    json_int(w, "id", b->id);
    json_string(w, "type", agent_building_type_name(b->type));
    json_int(w, "staffing_percent", staffing_percent(b));
    json_bool(w, "road_access", b->has_road_access);
    if (b->num_workers <= 0) {
        json_string(w, "problem", "no workers: it sends no buyer at all");
    } else if (!b->has_road_access) {
        json_string(w, "problem", "no road access: it sends no buyer");
    }
    if (b->type == BUILDING_MARKET) {
        figure *f = market_supplier(b);
        json_object_start(w, "buyer");
        json_bool(w, "out", f != 0);
        if (f) {
            json_string(w, "fetching", agent_resource_name(f->collecting_item_id));
            json_int(w, "destination_id", f->destination_building_id);
            json_int(w, "x", f->x);
            json_int(w, "y", f->y);
        }
        json_string(w, "how", "one buyer, one good per trip: a missing good first, then the lowest stock; "
            "food only from granaries, goods only from warehouses");
        json_object_end(w);
    }
    json_array_start(w, "resources");
    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        if (!building_distribution_resource_is_handled(r, b->type) || (only && r != only)) {
            continue;
        }
        json_object_start(w, 0);
        json_string(w, "resource", agent_resource_name(r));
        json_int(w, "stock_units", b->resources[r]); // 100 = one cartload
        int fetching = building_distribution_is_good_accepted(b, r);
        json_bool(w, "fetching", fetching);
        if (!fetching) {
            json_string(w, "problem", "switched off for this building (market <id> <resource> on)");
        } else if (resource_is_food(r) && b->type == BUILDING_MARKET && scenario_property_rome_supplies_wheat()) {
            json_string(w, "problem", "Rome supplies food in this mission; markets don't fetch it");
        } else if (!resource_is_food(r) && city_resource_is_stockpiled(r)) {
            json_string(w, "problem", "stockpiled in the city (trade advisor): warehouses hand none out");
        } else if (b->type == BUILDING_MARKET && !resource_is_food(r) && b->accepted_goods[r] <= 1) {
            json_string(w, "note", "no house served by this market has asked for it lately, so it isn't fetched");
        }
        json_array_start(w, "sources");
        int usable = 0, held_anywhere = 0;
        building_type storage_type = resource_is_food(r) ? BUILDING_GRANARY : BUILDING_WAREHOUSE;
        for (building *s = building_first_of_type(storage_type); s; s = s->next_of_type) {
            int amount = stored_amount(s, r);
            if (amount <= 0) {
                continue;
            }
            held_anywhere = 1;
            int distance = building_dist(b->x, b->y, size, size, s);
            const char *blocker = supply_blocker(s, b, permission, max_distance, distance);
            usable += !blocker;
            json_object_start(w, 0);
            json_int(w, "id", s->id);
            json_string(w, "type", agent_building_type_name(s->type));
            json_int(w, "x", s->x);
            json_int(w, "y", s->y);
            json_int(w, "cartloads", amount);
            json_int(w, "distance", distance);
            json_bool(w, "usable", !blocker);
            if (blocker) {
                json_string(w, "why_not", blocker);
            } else {
                int trip = road_trip(b, s);
                json_int(w, "road_trip_tiles", trip);
                if (trip >= 40 && trip > 3 * distance) {
                    json_string(w, "slow", "the buyer's road route is far longer than the straight distance (the "
                        "range check uses the straight one), so each trip takes weeks and the houses eat the load "
                        "before the next one: link them with a short road or move the market/storage");
                }
            }
            json_object_end(w);
        }
        json_array_end(w);
        if (!held_anywhere) {
            json_string(w, "verdict", resource_is_food(r) ? "no granary holds any" : "no warehouse holds any");
        } else if (!usable) {
            json_string(w, "verdict", "stores hold some, but none this building may use (see why_not)");
        } else {
            json_string(w, "verdict", "a usable store holds it; the buyer will fetch it when it's the most needed good");
        }
        json_object_end(w);
    }
    json_array_end(w);
}
