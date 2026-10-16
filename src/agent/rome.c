#include "rome.h"

#include "agent/observe.h"
#include "city/constants.h"
#include "city/emperor.h"
#include "city/festival.h"
#include "city/gods.h"
#include "city/finance.h"
#include "city/ratings.h"
#include "city/victory.h"
#include "core/lang.h"
#include "game/resource.h"
#include "scenario/request.h"

#include <stdio.h>
#include <string.h>

#define MAX_RANK 10

static const char *GIFT_NAMES[] = { "modest", "generous", "lavish" };

static void write_error(json_writer *w, const char *message)
{
    json_bool(w, "ok", 0);
    json_string(w, "error", message);
}

static void write_rank(json_writer *w, const char *key, int rank)
{
    json_object_start(w, key);
    json_int(w, "rank", rank);
    json_game_string(w, "title", lang_get_string(32, rank)); // e.g. "Engineer"
    json_int(w, "salary", city_emperor_salary_for_rank(rank));
    json_object_end(w);
}

static const char *request_resource_name(int resource)
{
    if (resource == RESOURCE_DENARII) {
        return "denarii";
    }
    if (resource == RESOURCE_TROOPS) {
        return "troops";
    }
    return agent_resource_name(resource);
}

static const char *request_state_name(scenario_request_state state)
{
    switch (state) {
        case REQUEST_STATE_NORMAL: return "open";
        case REQUEST_STATE_OVERDUE: return "overdue";
        case REQUEST_STATE_DISPATCHED: return "dispatched";
        case REQUEST_STATE_DISPATCHED_LATE: return "dispatched_late";
        case REQUEST_STATE_IGNORED: return "ignored";
        case REQUEST_STATE_RECEIVED: return "received";
        default: return "unknown";
    }
}

void agent_rome_status(json_writer *w)
{
    json_bool(w, "ok", 1);
    json_int(w, "favor", city_rating_favor());
    write_rank(w, "player_rank", city_emperor_rank());
    write_rank(w, "salary_rank", city_emperor_salary_rank());
    json_int(w, "personal_savings", city_emperor_personal_savings());
    json_int(w, "months_since_gift", city_emperor_months_since_gift());

    city_emperor_calculate_gift_costs();
    json_array_start(w, "gifts");
    for (int size = 0; size < 3; size++) {
        json_object_start(w, 0);
        json_string(w, "size", GIFT_NAMES[size]);
        json_int(w, "cost", city_emperor_get_gift(size)->cost);
        json_bool(w, "affordable", city_emperor_can_send_gift(size));
        json_object_end(w);
    }
    json_array_end(w);

    json_array_start(w, "requests");
    for (unsigned int id = 0; id < scenario_request_count_total(); id++) {
        const scenario_request *request = scenario_request_get(id);
        if (!request || !request->visible) {
            continue;
        }
        json_object_start(w, 0);
        json_int(w, "id", request->id);
        json_string(w, "resource", request_resource_name(request->resource));
        json_int(w, "amount", request->amount.requested);
        json_string(w, "state", request_state_name(request->state));
        json_int(w, "months_left", request->months_to_comply);
        json_int(w, "favor", request->favor);
        json_bool(w, "can_dispatch", scenario_request_can_comply(id));
        json_object_end(w);
    }
    json_array_end(w);
}

void agent_rome_salary(json_writer *w, const char *args)
{
    int rank;
    if (!args || sscanf(args, "%d", &rank) != 1 || rank < 0 || rank > MAX_RANK) {
        write_error(w, "usage: salary <rank 0-10> (see rome for titles and amounts)");
        return;
    }
    if (city_victory_has_won()) {
        write_error(w, "the mission is won; salary can't be changed");
        return;
    }
    // As the set-salary window does
    city_emperor_set_salary_rank(rank);
    city_finance_update_salary();
    city_ratings_update_favor_explanation();
    json_bool(w, "ok", 1);
    write_rank(w, "salary_rank", city_emperor_salary_rank());
    write_rank(w, "player_rank", city_emperor_rank());
    json_bool(w, "above_rank", city_emperor_salary_rank() > city_emperor_rank());
}

void agent_rome_gift(json_writer *w, const char *args)
{
    char name[16] = "";
    int size = -1;
    if (args) {
        sscanf(args, "%15s", name);
    }
    for (int i = 0; i < 3; i++) {
        if (strcmp(name, GIFT_NAMES[i]) == 0) {
            size = i;
        }
    }
    if (size < 0) {
        write_error(w, "usage: gift <modest|generous|lavish>");
        return;
    }
    city_emperor_calculate_gift_costs();
    if (!city_emperor_can_send_gift(size)) {
        write_error(w, "not enough personal savings for this gift");
        json_int(w, "cost", city_emperor_get_gift(size)->cost);
        json_int(w, "personal_savings", city_emperor_personal_savings());
        return;
    }
    int favor_before = city_rating_favor();
    int savings_before = city_emperor_personal_savings();
    city_emperor_set_gift_size(size);
    city_emperor_send_gift();
    json_bool(w, "ok", 1);
    json_int(w, "cost", savings_before - city_emperor_personal_savings());
    json_int(w, "personal_savings", city_emperor_personal_savings());
    json_int(w, "favor_change", city_rating_favor() - favor_before);
}

void agent_rome_dispatch(json_writer *w, const char *args)
{
    int id;
    if (!args || sscanf(args, "%d", &id) != 1 || id < 0 || id >= (int) scenario_request_count_total()) {
        write_error(w, "usage: dispatch <request id> (see rome)");
        return;
    }
    if (!scenario_request_can_comply(id)) {
        write_error(w, "can't dispatch: not open, not visible, or not enough in storage/treasury");
        return;
    }
    int treasury_before = city_finance_treasury();
    scenario_request_dispatch(id);
    const scenario_request *request = scenario_request_get(id);
    json_bool(w, "ok", 1);
    json_string(w, "state", request_state_name(request->state));
    json_int(w, "treasury_change", city_finance_treasury() - treasury_before);
}

static const char *GOD_NAMES[] = { "ceres", "neptune", "mercury", "mars", "venus" };
static const char *FESTIVAL_SIZES[] = { 0, "small", "large", "grand" };

static void write_festival_status(json_writer *w)
{
    city_festival_calculate_costs();
    json_array_start(w, "gods");
    for (int god = GOD_CERES; god <= GOD_VENUS; god++) {
        json_object_start(w, 0);
        json_string(w, "god", GOD_NAMES[god]);
        json_int(w, "happiness", city_god_happiness(god));
        json_int(w, "wrath_bolts", city_god_wrath_bolts(god));
        json_int(w, "happy_bolts", city_god_happy_bolts(god));
        json_int(w, "months_since_festival", city_god_months_since_festival(god));
        json_object_end(w);
    }
    json_array_end(w);
    json_object_start(w, "festival_costs");
    json_int(w, "small", city_festival_small_cost());
    json_int(w, "large", city_festival_large_cost());
    json_int(w, "grand", city_festival_grand_cost());
    json_int(w, "grand_wine", city_festival_grand_wine());
    json_bool(w, "enough_wine_for_grand", !city_festival_out_of_wine());
    json_object_end(w);
    json_bool(w, "festival_planned", city_festival_is_planned());
    json_int(w, "months_since_last_festival", city_festival_months_since_last());
}

void agent_rome_festival(json_writer *w, const char *args)
{
    char god_name[16] = "", size_name[16] = "";
    int n = args ? sscanf(args, "%15s %15s", god_name, size_name) : 0;
    if (n <= 0) {
        json_bool(w, "ok", 1);
        write_festival_status(w);
        return;
    }
    int god = -1, size = 0;
    for (int i = GOD_CERES; i <= GOD_VENUS; i++) {
        if (strcmp(god_name, GOD_NAMES[i]) == 0) {
            god = i;
        }
    }
    for (int i = FESTIVAL_SMALL; i <= FESTIVAL_GRAND; i++) {
        if (strcmp(size_name, FESTIVAL_SIZES[i]) == 0) {
            size = i;
        }
    }
    if (n != 2 || god < 0 || !size) {
        write_error(w, "usage: festival [<ceres|neptune|mercury|mars|venus> <small|large|grand>]");
        return;
    }
    if (city_festival_is_planned()) {
        write_error(w, "a festival is already planned");
        return;
    }
    if (city_finance_out_of_money()) {
        write_error(w, "the city is out of money");
        return;
    }
    // As the hold-festival window does
    city_festival_calculate_costs();
    city_festival_select_god(god);
    if (!city_festival_select_size(size)) {
        write_error(w, "not enough wine in warehouses for a grand festival");
        return;
    }
    int treasury_before = city_finance_treasury();
    city_festival_schedule();
    json_bool(w, "ok", 1);
    json_int(w, "cost", treasury_before - city_finance_treasury());
    write_festival_status(w);
}
