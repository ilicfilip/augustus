#include "settings.h"

#include "agent/observe.h"
#include "building/building.h"
#include "building/distribution.h"
#include "building/roadblock.h"
#include "building/storage.h"
#include "city/finance.h"
#include "city/labor.h"
#include "core/dir.h"
#include "core/encoding.h"
#include "core/lang.h"
#include "game/campaign.h"
#include "game/file.h"
#include "game/resource.h"
#include "scenario/custom_messages.h"
#include "scenario/property.h"
#include "window/message_dialog.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_TAX 25
#define MAX_WAGES 100
#define MAX_TEXT 8192

// Same order as the labor_category enum in city/labor.c, without LABOR_CATEGORY_NONE. The game's labour
// data and priority functions are 0-based (index = enum value - 1), as in the labour advisor.
static const char *LABOUR_CATEGORIES[] = {
    "industry_commerce", "food_production", "engineering", "water", "prefectures", "military",
    "entertainment", "health_education", "governance_religion"
};
#define NUM_LABOUR_CATEGORIES 9

static const struct {
    const char *name;
    building_storage_permission_states permission;
} STORAGE_PERMISSIONS[] = {
    { "market", BUILDING_STORAGE_PERMISSION_MARKET },
    { "traders", BUILDING_STORAGE_PERMISSION_TRADERS },
    { "dock", BUILDING_STORAGE_PERMISSION_DOCK },
    { "quartermaster", BUILDING_STORAGE_PERMISSION_QUARTERMASTER },
    { "worker", BUILDING_STORAGE_PERMISSION_WORKER },
    { "barkeep", BUILDING_STORAGE_PERMISSION_BARKEEP },
    { "caravanserai", BUILDING_STORAGE_PERMISSION_CARAVANSERAI },
    { "lighthouse", BUILDING_STORAGE_PERMISSION_LIGHTHOUSE },
    { "armoury", BUILDING_STORAGE_PERMISSION_ARMOURY },
    { "workcamp", BUILDING_STORAGE_PERMISSION_WORKCAMP },
    { "natives", BUILDING_STORAGE_PERMISSION_NATIVES },
    { "caesar", BUILDING_STORAGE_PERMISSION_CAESAR },
};

static const char *ROADBLOCK_PERMISSIONS[] = {
    0, "maintenance", "priest", "market", "entertainer", "education", "medicine", "tax_collector",
    "labor_seeker", "missionary", "watchman"
};
#define NUM_ROADBLOCK_PERMISSIONS 11

static void write_error(json_writer *w, const char *message)
{
    json_bool(w, "ok", 0);
    json_string(w, "error", message);
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

static building *get_building(int id)
{
    if (id <= 0 || id >= building_count()) {
        return 0;
    }
    building *b = building_get(id);
    if (b->state != BUILDING_STATE_IN_USE && b->state != BUILDING_STATE_MOTHBALLED &&
        b->state != BUILDING_STATE_CREATED) {
        return 0;
    }
    return b;
}

void agent_settings_write_labour_categories(json_writer *w)
{
    json_array_start(w, "labour_categories");
    for (int i = 0; i < NUM_LABOUR_CATEGORIES; i++) {
        const labor_category_data *data = city_labor_category(i);
        json_object_start(w, 0);
        json_string(w, "name", LABOUR_CATEGORIES[i]);
        json_int(w, "workers_needed", data->workers_needed);
        json_int(w, "workers_allocated", data->workers_allocated);
        json_int(w, "priority", data->priority);
        json_object_end(w);
    }
    json_array_end(w);
}

void agent_settings_set(json_writer *w, const char *args)
{
    char setting[32], category[32];
    int value;
    if (args && sscanf(args, "priority %31s %d", category, &value) == 2) {
        for (int i = 0; i < NUM_LABOUR_CATEGORIES; i++) {
            if (strcmp(LABOUR_CATEGORIES[i], category) == 0) {
                int max = city_labor_max_selectable_priority(i);
                if (value < 0 || value > max) {
                    write_error(w, "priority must be 0 (none) to the number of prioritised categories + 1");
                    json_int(w, "max", max);
                    return;
                }
                city_labor_set_priority(i, value);
                city_labor_allocate_workers();
                json_bool(w, "ok", 1);
                agent_settings_write_labour_categories(w);
                return;
            }
        }
        write_error(w, "unknown labour category");
        return;
    }
    if (!args || sscanf(args, "%31s %d", setting, &value) != 2) {
        write_error(w, "usage: set tax <0-25> | set wages <0-100> | set priority <category> <0-9>");
        return;
    }
    if (strcmp(setting, "tax") == 0) {
        if (value < 0 || value > MAX_TAX) {
            write_error(w, "tax must be 0-25");
            return;
        }
        city_finance_set_tax_percentage(value);
        city_finance_estimate_taxes();
    } else if (strcmp(setting, "wages") == 0) {
        if (value < 0 || value > MAX_WAGES) {
            write_error(w, "wages must be 0-100");
            return;
        }
        city_labor_change_wages(value - city_labor_wages());
        city_finance_estimate_wages();
    } else {
        write_error(w, "unknown setting; use tax, wages or priority");
        return;
    }
    json_bool(w, "ok", 1);
    json_int(w, "tax_percentage", city_finance_tax_percentage());
    json_int(w, "wages", city_labor_wages());
    json_int(w, "wages_rome", city_labor_wages_rome());
}

static void write_storage(json_writer *w, building *b)
{
    const building_storage *s = building_storage_get(b->storage_id);
    static const char *STATE_NAMES[] = { "refuse", "accept", "get", "keep" };
    json_object_start(w, "orders");
    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        const resource_storage_entry *entry = &s->resource_state[r];
        if (entry->state == BUILDING_STORAGE_STATE_NOT_ACCEPTING) {
            continue;
        }
        char value[24];
        snprintf(value, sizeof(value), "%s %d", STATE_NAMES[entry->state], entry->quantity);
        json_string(w, agent_resource_name(r), value);
    }
    json_object_end(w);
    json_object_start(w, "permissions");
    for (unsigned int i = 0; i < sizeof(STORAGE_PERMISSIONS) / sizeof(STORAGE_PERMISSIONS[0]); i++) {
        json_bool(w, STORAGE_PERMISSIONS[i].name, building_storage_get_permission(STORAGE_PERMISSIONS[i].permission, b));
    }
    json_object_end(w);
}

void agent_settings_storage(json_writer *w, const char *args)
{
    int id = 0, quantity = BUILDING_STORAGE_QUANTITY_MAX;
    char what[32] = "", value[32] = "";
    int n = args ? sscanf(args, "%d %31s %31s %d", &id, what, value, &quantity) : 0;
    building *b = get_building(id);
    if (n < 1 || !b || (b->type != BUILDING_WAREHOUSE && b->type != BUILDING_GRANARY)) {
        write_error(w, "usage: storage <warehouse/granary id> [<resource> <accept|get|refuse|keep> [4-32]] "
            "| [permission <name> <on|off>]");
        return;
    }
    if (n == 1) {
        json_bool(w, "ok", 1);
        write_storage(w, b);
        return;
    }
    if (strcmp(what, "permission") == 0) {
        char name[32], on_off[8];
        if (sscanf(args, "%*d permission %31s %7s", name, on_off) != 2 || parse_on_off(on_off) < 0) {
            write_error(w, "usage: storage <id> permission <name> <on|off>");
            return;
        }
        for (unsigned int i = 0; i < sizeof(STORAGE_PERMISSIONS) / sizeof(STORAGE_PERMISSIONS[0]); i++) {
            if (strcmp(STORAGE_PERMISSIONS[i].name, name) == 0) {
                // The game stores restrictions: its "enable" sets the bit that *blocks* the permission
                building_storage_set_permission(STORAGE_PERMISSIONS[i].permission, b, !parse_on_off(on_off));
                json_bool(w, "ok", 1);
                write_storage(w, b);
                return;
            }
        }
        write_error(w, "unknown permission");
        return;
    }
    resource_type r = agent_resource_from_name(what);
    if (r == RESOURCE_NONE) {
        write_error(w, "unknown resource");
        return;
    }
    building_storage_state state;
    if (strcmp(value, "accept") == 0) {
        state = BUILDING_STORAGE_STATE_ACCEPTING;
    } else if (strcmp(value, "get") == 0) {
        state = BUILDING_STORAGE_STATE_GETTING;
    } else if (strcmp(value, "refuse") == 0) {
        state = BUILDING_STORAGE_STATE_NOT_ACCEPTING;
    } else if (strcmp(value, "keep") == 0) {
        state = BUILDING_STORAGE_STATE_MAINTAINING;
    } else {
        write_error(w, "state must be accept, get, refuse or keep");
        return;
    }
    if (quantity < 4 || quantity > 32 || quantity % 4) {
        write_error(w, "quantity must be 4, 8, ... 32 (cartloads)");
        return;
    }
    if (b->type == BUILDING_GRANARY && !resource_is_food(r)) {
        write_error(w, "granaries only store food");
        return;
    }
    building_storage data = *building_storage_get(b->storage_id);
    data.resource_state[r].state = state;
    data.resource_state[r].quantity = (building_storage_quantity) quantity;
    building_storage_set_data(b->storage_id, data);
    json_bool(w, "ok", 1);
    write_storage(w, b);
}

void agent_settings_market(json_writer *w, const char *args)
{
    int id = 0;
    char resource[32] = "", on_off[8] = "";
    int n = args ? sscanf(args, "%d %31s %7s", &id, resource, on_off) : 0;
    building *b = get_building(id);
    if (n < 1 || !b || !building_type_is_distributor(b->type)) {
        write_error(w, "usage: market <market id> [<resource> <on|off>]");
        return;
    }
    if (n == 3) {
        resource_type r = agent_resource_from_name(resource);
        int on = parse_on_off(on_off);
        if (r == RESOURCE_NONE || on < 0 || !building_distribution_resource_is_handled(r, b->type)) {
            write_error(w, "unknown resource for this building, or value not on/off");
            return;
        }
        if (building_distribution_is_good_accepted(b, r) != on) {
            building_distribution_toggle_good_accepted(b, r);
        }
    } else if (n != 1) {
        write_error(w, "usage: market <market id> [<resource> <on|off>]");
        return;
    }
    json_bool(w, "ok", 1);
    json_object_start(w, "fetches");
    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        if (building_distribution_resource_is_handled(r, b->type)) {
            json_bool(w, agent_resource_name(r), building_distribution_is_good_accepted(b, r));
        }
    }
    json_object_end(w);
}

void agent_settings_roadblock(json_writer *w, const char *args)
{
    int id = 0;
    char walker[32] = "", on_off[8] = "";
    int n = args ? sscanf(args, "%d %31s %7s", &id, walker, on_off) : 0;
    building *b = get_building(id);
    if (n < 1 || !b || !building_type_is_roadblock(b->type)) {
        write_error(w, "usage: roadblock <roadblock/gate id> [<walker> <on|off>]");
        return;
    }
    if (n == 3) {
        int on = parse_on_off(on_off);
        int found = 0;
        for (int p = 1; p < NUM_ROADBLOCK_PERMISSIONS; p++) {
            if (strcmp(ROADBLOCK_PERMISSIONS[p], walker) == 0) {
                found = 1;
                if (on >= 0 && (building_roadblock_get_permission(p, b) != 0) != on) {
                    building_roadblock_set_permission(p, b); // toggles
                }
            }
        }
        if (!found || on < 0) {
            write_error(w, "unknown walker type, or value not on/off");
            return;
        }
    } else if (n != 1) {
        write_error(w, "usage: roadblock <roadblock/gate id> [<walker> <on|off>]");
        return;
    }
    json_bool(w, "ok", 1);
    json_object_start(w, "lets_through");
    for (int p = 1; p < NUM_ROADBLOCK_PERMISSIONS; p++) {
        json_bool(w, ROADBLOCK_PERMISSIONS[p], building_roadblock_get_permission(p, b) != 0);
    }
    json_object_end(w);
}

/** Converts the game's rich text (@P paragraph, @L line break, @G image, @<n> link) to plain UTF-8 */
static void rich_text_to_plain(const uint8_t *text, char *out, int size)
{
    static char utf8[MAX_TEXT];
    encoding_to_utf8(text, utf8, sizeof(utf8), 0);
    int len = 0;
    for (const char *p = utf8; *p && len < size - 2; p++) {
        if (*p != '@') {
            out[len++] = *p;
            continue;
        }
        p++;
        if (*p == 'P') {
            out[len++] = '\n';
            out[len++] = '\n';
        } else if (*p == 'L') {
            out[len++] = '\n';
        } else if (*p == 'G' || *p == 'H') {
            // image / heading marker: skip it and its number
        } else if (!*p) {
            break;
        } else if (!(*p >= '0' && *p <= '9')) {
            continue;
        }
        while (p[1] >= '0' && p[1] <= '9') {
            p++;
        }
    }
    out[len] = 0;
}

void agent_settings_message(json_writer *w)
{
    int year, month, param1, param2;
    const lang_message *msg = window_message_dialog_current(&year, &month, &param1, &param2);
    if (!msg) {
        write_error(w, "no message is open");
        return;
    }
    static const char *TYPES[] = {
        "general", "disaster", "imperial", "emigration", "tutorial", "trade_change", "price_change",
        "invasion", "building_completion", "custom", "route_price_change", "rank_change"
    };
    static char text[MAX_TEXT];
    json_bool(w, "ok", 1);
    if (msg->message_type >= 0 && msg->message_type < (int) (sizeof(TYPES) / sizeof(TYPES[0]))) {
        json_string(w, "type", TYPES[msg->message_type]);
    }
    json_bool(w, "urgent", msg->urgent);
    json_int(w, "year", year);
    json_int(w, "month", month);
    // Shown next to the text by the game, e.g. amounts and resources for requests
    json_int(w, "param1", param1);
    json_int(w, "param2", param2);
    if (msg->title.text) {
        rich_text_to_plain(msg->title.text, text, sizeof(text));
        json_string(w, "title", text);
    }
    if (msg->subtitle.text) {
        rich_text_to_plain(msg->subtitle.text, text, sizeof(text));
        json_string(w, "subtitle", text);
    }
    if (msg->content.text) {
        rich_text_to_plain(msg->content.text, text, sizeof(text));
        json_string(w, "text", text);
    }
}

void agent_settings_briefing(json_writer *w)
{
    // As window/mission_briefing.c get_briefing_texts()
    const uint8_t *title = 0, *subtitle = 0, *content = 0;
    if (game_campaign_is_original()) {
        const lang_message *msg = lang_get_message(200 + scenario_campaign_mission());
        title = msg->title.text;
        subtitle = msg->subtitle.text;
        content = msg->content.text;
    } else if (scenario_intro_message()) {
        custom_message_t *message = custom_messages_get(scenario_intro_message());
        title = custom_messages_get_title(message);
        subtitle = custom_messages_get_subtitle(message);
        content = custom_messages_get_text(message);
    }
    if (!content || !*content) {
        write_error(w, "this scenario has no briefing text");
        return;
    }
    static char text[MAX_TEXT];
    json_bool(w, "ok", 1);
    json_bool(w, "original_campaign", game_campaign_is_original());
    json_int(w, "campaign_mission", scenario_campaign_mission());
    if (title && *title) {
        rich_text_to_plain(title, text, sizeof(text));
        json_string(w, "title", text);
    }
    if (subtitle && *subtitle) {
        rich_text_to_plain(subtitle, text, sizeof(text));
        json_string(w, "subtitle", text);
    }
    rich_text_to_plain(content, text, sizeof(text));
    json_string(w, "text", text);
}

void agent_settings_save(json_writer *w, const char *args)
{
    char name[256];
    if (!args || sscanf(args, "%255s", name) != 1) {
        write_error(w, "usage: save <agent_name> (saved as agent_name.svx) or save </absolute/path.svx>");
        return;
    }
    char filename[512];
    if (name[0] == '/') {
        snprintf(filename, sizeof(filename), "%s", name);
    } else {
        // Never overwrite the player's own saves
        if (strncmp(name, "agent_", 6) != 0 || strchr(name, '/')) {
            write_error(w, "save names must start with agent_ (the player's saves are never overwritten)");
            return;
        }
        if (!strstr(name, ".svx")) {
            strncat(name, ".svx", sizeof(name) - strlen(name) - 1);
        }
        snprintf(filename, sizeof(filename), "%s", dir_append_location(name, PATH_LOCATION_SAVEGAME));
    }
    if (!game_file_write_saved_game(filename)) {
        write_error(w, "saving failed");
        return;
    }
    json_bool(w, "ok", 1);
    if (filename[0] == '/') {
        json_string(w, "file", filename);
    } else {
        char cwd[768], path[1280];
        snprintf(path, sizeof(path), "%s/%s", getcwd(cwd, sizeof(cwd)) ? cwd : ".", filename);
        json_string(w, "file", path);
    }
}
