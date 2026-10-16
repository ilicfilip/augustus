#include "ratings.h"

#include "building/count.h"
#include "city/culture.h"
#include "city/data_private.h"
#include "city/emperor.h"
#include "city/finance.h"
#include "city/labor.h"
#include "city/ratings.h"
#include "core/calc.h"

#include <stdio.h>

/*
 * Mirrors city/ratings.c update_culture_rating(): each part's points by city-wide coverage.
 * Steps: coverage > 30, > 50, > 70, > 85, >= 100.
 */
static const struct {
    const char *name;
    int points[5]; // at the five steps
} CULTURE_PARTS[] = {
    { "theater", { 3, 8, 12, 18, 25 } },
    { "religion", { 3, 9, 14, 22, 30 } },
    { "school", { 1, 4, 6, 10, 15 } },
    { "academy", { 1, 2, 4, 7, 10 } },
    { "library", { 2, 4, 8, 14, 20 } },
};
static const int CULTURE_STEP_MIN_COVERAGE[] = { 31, 51, 71, 86, 100 };

static int culture_step(int coverage)
{
    int step = -1;
    for (int i = 0; i < 5; i++) {
        if (coverage >= CULTURE_STEP_MIN_COVERAGE[i]) {
            step = i;
        }
    }
    return step;
}

static void write_culture(json_writer *w)
{
    int coverage[5] = {
        city_culture_coverage_theater(),
        city_data.culture.religion_coverage,
        city_culture_coverage_school(),
        city_culture_coverage_academy(),
        city_culture_coverage_library(),
    };
    json_object_start(w, "culture");
    json_int(w, "rating", city_rating_culture());
    json_array_start(w, "parts");
    for (int i = 0; i < 5; i++) {
        int step = culture_step(coverage[i]);
        json_object_start(w, 0);
        json_string(w, "part", CULTURE_PARTS[i].name);
        json_int(w, "coverage_percent", coverage[i]);
        json_int(w, "points", step >= 0 ? CULTURE_PARTS[i].points[step] : 0);
        json_int(w, "max_points", CULTURE_PARTS[i].points[4]);
        if (step < 4) {
            json_int(w, "next_step_at_coverage", CULTURE_STEP_MIN_COVERAGE[step + 1]);
            json_int(w, "points_at_next_step", CULTURE_PARTS[i].points[step + 1]);
        }
        json_object_end(w);
    }
    json_array_end(w);
    int monuments = building_count_active(BUILDING_GRAND_TEMPLE_CERES) + building_count_active(BUILDING_GRAND_TEMPLE_NEPTUNE) +
        building_count_active(BUILDING_GRAND_TEMPLE_MERCURY) + building_count_active(BUILDING_GRAND_TEMPLE_MARS) +
        building_count_active(BUILDING_GRAND_TEMPLE_VENUS) + building_count_active(BUILDING_PANTHEON) +
        building_count_active(BUILDING_COLOSSEUM) + building_count_active(BUILDING_HIPPODROME);
    json_int(w, "monuments_counted", monuments);
    json_string(w, "how", "coverage is city-wide (people served / population), not by location; updated yearly");
    json_object_end(w);
}

/** Mirrors city/ratings.c update_prosperity_rating(): the yearly change factors, as they stand now */
static void write_prosperity(json_writer *w)
{
    json_object_start(w, "prosperity");
    json_int(w, "rating", city_rating_prosperity());
    json_int(w, "cap_from_housing", city_data.ratings.prosperity_max);
    json_string(w, "cap_note", "can't exceed the average prosperity value of your houses: evolve houses to raise it");
    json_object_start(w, "yearly_factors_now");
    int unemployment = city_data.labor.unemployment_percentage;
    json_int(w, "unemployment", unemployment < 5 ? 1 : (unemployment >= 15 ? -1 : 0));
    int made_money = city_data.finance.this_year.expenses.construction + city_data.finance.treasury >
        city_data.ratings.prosperity_treasury_last_year;
    json_int(w, "made_money_this_year", made_money ? 5 : -1);
    json_int(w, "food_variety", city_data.resource.food_types_eaten >= 2 ? 1 : 0);
    int wages = city_labor_wages();
    json_int(w, "wages_vs_rome", wages >= city_labor_wages_rome() + 2 ? 1 : (wages < city_labor_wages_rome() ? -1 : 0));
    int population = city_data.population.population;
    json_int(w, "poor_housing", calc_percentage(city_data.population.people_in_tents_shacks, population) > 30 ? -1 : 0);
    json_int(w, "rich_housing", calc_percentage(city_data.population.people_in_villas_palaces, population) > 10 ? 1 : 0);
    json_int(w, "hippodrome", city_data.entertainment.hippodrome_shows > 0 ? 1 : 0);
    json_object_end(w);
    json_int(w, "percent_in_tents_shacks", calc_percentage(city_data.population.people_in_tents_shacks, population));
    json_int(w, "percent_in_villas_palaces", calc_percentage(city_data.population.people_in_villas_palaces, population));
    json_object_end(w);
}

static void write_peace(json_writer *w)
{
    json_object_start(w, "peace");
    json_int(w, "rating", city_rating_peace());
    json_int(w, "calm_years_in_a_row", city_data.ratings.peace_years_of_peace);
    json_int(w, "next_yearly_gain_if_calm", city_data.ratings.peace_years_of_peace < 2 ? 2 : 5);
    json_object_start(w, "this_year_so_far");
    json_int(w, "criminals", city_data.ratings.peace_num_criminals);
    json_int(w, "rioters", city_data.ratings.peace_num_rioters);
    json_int(w, "buildings_destroyed", city_data.ratings.peace_destroyed_buildings);
    json_object_end(w);
    json_string(w, "how", "yearly: +2 (first 2 calm years) or +5; -1 any criminals, -5 any rioters, -1 per building "
        "destroyed by enemies/rioters (fire and collapse don't count); a riot or such a loss resets the calm-year "
        "count. Keep every house happy (no squalor, jobs, wages) so there are no criminals or rioters");
    json_object_end(w);
}

static void write_favor(json_writer *w)
{
    json_object_start(w, "favor");
    json_int(w, "rating", city_rating_favor());
    json_int(w, "salary_penalty", city_data.ratings.favor_salary_penalty);
    json_int(w, "milestone_penalty", city_data.ratings.favor_milestone_penalty);
    json_int(w, "ignored_request_penalty", city_data.ratings.favor_ignored_request_penalty);
    json_int(w, "months_since_gift", city_emperor_months_since_gift());
    json_string(w, "how", "yearly -2; salary above rank costs, below rank +1; requests met raise it, missed lower it; "
        "gifts +3/+5/+10 but a gift within 12 months of the last one is worth less");
    json_object_end(w);
}

void agent_ratings(json_writer *w)
{
    json_bool(w, "ok", 1);
    write_culture(w);
    write_prosperity(w);
    write_peace(w);
    write_favor(w);
}

static void write_overview(json_writer *w, const char *key, const finance_overview *o)
{
    json_object_start(w, key);
    json_object_start(w, "income");
    json_int(w, "taxes", o->income.taxes);
    json_int(w, "exports", o->income.exports);
    json_int(w, "donated", o->income.donated);
    json_int(w, "total", o->income.total);
    json_object_end(w);
    json_object_start(w, "expenses");
    json_int(w, "imports", o->expenses.imports);
    json_int(w, "wages", o->expenses.wages);
    json_int(w, "construction", o->expenses.construction);
    json_int(w, "interest", o->expenses.interest);
    json_int(w, "salary", o->expenses.salary);
    json_int(w, "sundries", o->expenses.sundries);
    json_int(w, "tribute", o->expenses.tribute);
    json_int(w, "levies", o->expenses.levies);
    json_int(w, "total", o->expenses.total);
    json_object_end(w);
    json_int(w, "net", o->net_in_out);
    json_object_end(w);
}

void agent_ratings_finance(json_writer *w)
{
    json_bool(w, "ok", 1);
    json_int(w, "treasury", city_finance_treasury());
    write_overview(w, "this_year", city_finance_overview_this_year());
    write_overview(w, "last_year", city_finance_overview_last_year());
    json_string(w, "note", "this_year is so far in the current year; sundries include festivals and gifts");
}
