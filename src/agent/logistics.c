#include "logistics.h"

#include "agent/observe.h"
#include "building/building.h"
#include "building/granary.h"
#include "building/industry.h"
#include "building/properties.h"
#include "building/storage.h"
#include "building/warehouse.h"
#include "core/calc.h"
#include "figure/action.h"
#include "figure/figure.h"
#include "game/resource.h"
#include "map/point.h"

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
