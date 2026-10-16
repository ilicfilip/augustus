#include "trade.h"

#include "agent/observe.h"
#include "building/menu.h"
#include "city/buildings.h"
#include "city/constants.h"
#include "city/finance.h"
#include "city/resource.h"
#include "city/trade.h"
#include "core/encoding.h"
#include "empire/city.h"
#include "empire/object.h"
#include "empire/trade_prices.h"
#include "empire/trade_route.h"
#include "empire/type.h"
#include "game/resource.h"
#include "scenario/map.h"

#include <stdio.h>
#include <string.h>

#define MAX_THRESHOLD 100

static void write_error(json_writer *w, const char *message)
{
    json_bool(w, "ok", 0);
    json_string(w, "error", message);
}

static const char *status_name(resource_trade_status status)
{
    switch (status) {
        case TRADE_STATUS_IMPORT: return "import";
        case TRADE_STATUS_EXPORT: return "export";
        case TRADE_STATUS_IMPORT_EXPORT: return "import_export";
        default: return "none";
    }
}

static int is_trade_partner(const empire_city *city)
{
    return city && city->in_use && (city->type == EMPIRE_CITY_TRADE || city->type == EMPIRE_CITY_FUTURE_TRADE);
}

/** Stock in cartloads, as traders count it (warehouses, plus granaries for food) */
static int stock(resource_type r)
{
    return city_resource_get_total_amount(r, 0);
}

static void write_resources(json_writer *w)
{
    json_array_start(w, "resources");
    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        if (!resource_is_storable(r)) {
            continue;
        }
        json_object_start(w, 0);
        json_string(w, "resource", agent_resource_name(r));
        json_int(w, "stock", stock(r));
        json_bool(w, "can_produce", empire_can_produce_resource(r));
        json_string(w, "status", status_name(city_resource_trade_status(r)));
        json_int(w, "import_up_to", city_resource_import_over(r));
        json_int(w, "export_over", city_resource_export_over(r));
        json_bool(w, "stockpiled", city_resource_is_stockpiled(r));
        json_bool(w, "industry_mothballed", city_resource_is_mothballed(r));
        json_bool(w, "importable_now", empire_can_import_resource(r));
        json_bool(w, "importable_if_route_opened", empire_can_import_resource_potentially(r));
        json_bool(w, "exportable_now", empire_can_export_resource(r));
        json_bool(w, "exportable_if_route_opened", empire_can_export_resource_potentially(r));
        json_object_start(w, "prices");
        json_int(w, "import_land", trade_price_buy(r, 1));
        json_int(w, "import_sea", trade_price_buy(r, 0));
        json_int(w, "export_land", trade_price_sell(r, 1));
        json_int(w, "export_sea", trade_price_sell(r, 0));
        json_object_end(w);
        json_object_end(w);
    }
    json_array_end(w);
}

static void write_quotas(json_writer *w, const char *key, const empire_city *city, int buying)
{
    json_array_start(w, key);
    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        if (buying ? !city->buys_resource[r] : !city->sells_resource[r]) {
            continue;
        }
        json_object_start(w, 0);
        json_string(w, "resource", agent_resource_name(r));
        json_int(w, "per_year", trade_route_limit(city->route_id, r, buying));
        json_int(w, "traded_this_year", trade_route_traded(city->route_id, r, buying));
        json_object_end(w);
    }
    json_array_end(w);
}

static void write_routes(json_writer *w)
{
    json_array_start(w, "partners");
    for (int id = 1; id < empire_city_get_array_size(); id++) { // 0 is the "no city" entry
        empire_city *city = empire_city_get(id);
        if (!is_trade_partner(city)) {
            continue;
        }
        json_object_start(w, 0);
        json_int(w, "city_id", id);
        json_game_string(w, "name", empire_city_get_name(city));
        json_bool(w, "open", city->is_open);
        json_bool(w, "available", city->type == EMPIRE_CITY_TRADE); // future partners can't be opened yet
        json_string(w, "route", city->is_sea_trade ? "sea" : "land");
        if (!city->is_open) {
            json_int(w, "cost_to_open", city->cost_to_open);
            full_empire_object *full = empire_object_get_full(city->empire_object_id);
            json_object_start(w, "goods_to_open");
            for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
                if (full && full->route_resource_cost[r]) {
                    json_int(w, agent_resource_name(r), full->route_resource_cost[r]);
                }
            }
            json_object_end(w);
        }
        write_quotas(w, "sells_to_us", city, 0);
        write_quotas(w, "buys_from_us", city, 1);
        json_object_end(w);
    }
    json_array_end(w);
}

static void write_overview(json_writer *w)
{
    json_bool(w, "ok", 1);
    write_resources(w);
    write_routes(w);
    json_object_start(w, "conditions");
    json_bool(w, "working_dock", city_buildings_has_working_dock());
    json_bool(w, "river_entry", scenario_map_has_river_entry());
    json_bool(w, "land_trade_disrupted", city_trade_has_land_trade_problems());
    json_bool(w, "sea_trade_disrupted", city_trade_has_sea_trade_problems());
    json_object_end(w);
}

static int parse_on_off(const char *value)
{
    if (strcmp(value, "on") == 0) {
        return 1;
    }
    if (strcmp(value, "off") == 0) {
        return 0;
    }
    return -1;
}

static void write_resource_settings(json_writer *w, resource_type r)
{
    json_bool(w, "ok", 1);
    json_string(w, "resource", agent_resource_name(r));
    json_string(w, "status", status_name(city_resource_trade_status(r)));
    json_int(w, "import_up_to", city_resource_import_over(r));
    json_int(w, "export_over", city_resource_export_over(r));
    json_bool(w, "stockpiled", city_resource_is_stockpiled(r));
    json_bool(w, "industry_mothballed", city_resource_is_mothballed(r));
}

void agent_trade(json_writer *w, const char *args)
{
    char resource_name[32] = "", setting[32] = "", value[16] = "";
    int n = args ? sscanf(args, "%31s %31s %15s", resource_name, setting, value) : 0;
    if (n <= 0) {
        write_overview(w);
        return;
    }
    resource_type r = agent_resource_from_name(resource_name);
    if (r == RESOURCE_NONE || !resource_is_storable(r)) {
        write_error(w, "unknown or non-tradeable resource");
        return;
    }
    if (n != 3) {
        write_error(w, "usage: trade <resource> <import|export|stockpile|mothball> <on|off> | "
            "trade <resource> <import_up_to|export_over> <0-100>");
        return;
    }
    if (strcmp(setting, "import_up_to") == 0 || strcmp(setting, "export_over") == 0) {
        int amount;
        if (sscanf(value, "%d", &amount) != 1 || amount < 0 || amount > MAX_THRESHOLD) {
            write_error(w, "amount must be 0-100 cartloads (0 = no limit)");
            return;
        }
        if (setting[0] == 'i') {
            city_resource_change_import_over(r, amount - city_resource_import_over(r));
        } else {
            city_resource_change_export_over(r, amount - city_resource_export_over(r));
        }
        write_resource_settings(w, r);
        return;
    }
    int on = parse_on_off(value);
    if (on < 0) {
        write_error(w, "value must be on or off");
        return;
    }
    if (strcmp(setting, "import") == 0 || strcmp(setting, "export") == 0) {
        resource_trade_status flag = setting[0] == 'i' ? TRADE_STATUS_IMPORT : TRADE_STATUS_EXPORT;
        int is_on = (city_resource_trade_status(r) & flag) != 0;
        if (on && !is_on) {
            int possible = flag == TRADE_STATUS_IMPORT ? empire_can_import_resource(r) : empire_can_export_resource(r);
            if (!possible) {
                write_error(w, flag == TRADE_STATUS_IMPORT ? "no open trade route sells this resource" :
                    "no open trade route buys this resource");
                return;
            }
        }
        if (on != is_on) {
            city_resource_cycle_trade_status(r, flag); // toggles the flag (and un-stockpiles on export)
        }
    } else if (strcmp(setting, "stockpile") == 0) {
        if (city_resource_is_stockpiled(r) != on) {
            city_resource_toggle_stockpiled(r);
        }
    } else if (strcmp(setting, "mothball") == 0) {
        if (city_resource_is_mothballed(r) != on) {
            city_resource_toggle_mothballed(r);
        }
    } else {
        write_error(w, "setting must be import, export, stockpile, mothball, import_up_to or export_over");
        return;
    }
    write_resource_settings(w, r);
}

void agent_trade_open_route(json_writer *w, const char *args)
{
    int id;
    if (!args || sscanf(args, "%d", &id) != 1 || id < 1 || id >= empire_city_get_array_size()) {
        write_error(w, "usage: open_route <city id> (see trade)");
        return;
    }
    empire_city *city = empire_city_get(id);
    if (!is_trade_partner(city) || city->type != EMPIRE_CITY_TRADE) {
        write_error(w, "not a trade partner that can be opened now");
        return;
    }
    if (city->is_open) {
        write_error(w, "route is already open");
        return;
    }
    if (city_finance_treasury() < (int) city->cost_to_open) {
        write_error(w, "not enough money in the treasury");
        json_int(w, "cost_to_open", city->cost_to_open);
        return;
    }
    // Same goods check as the empire screen (window/empire.c button_open_trade_by_route)
    full_empire_object *full = empire_object_get_full(city->empire_object_id);
    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        if (!full || !full->route_resource_cost[r]) {
            continue;
        }
        int stored = city_resource_count_warehouses_amount(r);
        if (resource_is_food(r)) {
            stored += city_resource_count_food_on_granaries(r) / RESOURCE_ONE_LOAD;
        }
        if (stored < full->route_resource_cost[r]) {
            write_error(w, "not enough goods in storage to open this route");
            json_string(w, "missing", agent_resource_name(r));
            json_int(w, "needed", full->route_resource_cost[r]);
            json_int(w, "stored", stored);
            return;
        }
    }
    int treasury_before = city_finance_treasury();
    empire_city_open_trade(id, 1);
    building_menu_update();
    json_bool(w, "ok", 1);
    json_game_string(w, "opened", empire_city_get_name(city));
    json_int(w, "cost", treasury_before - city_finance_treasury());
}
