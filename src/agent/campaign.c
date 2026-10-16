#include "campaign.h"

#include "city/emperor.h"
#include "city/mission.h"
#include "city/victory.h"
#include "city/view.h"
#include "core/calc.h"
#include "game/campaign.h"
#include "game/settings.h"
#include "game/state.h"
#include "game/undo.h"
#include "graphics/window.h"
#include "scenario/event/controller.h"
#include "scenario/property.h"
#include "scenario/scenario.h"
#include "sound/music.h"
#include "sound/speech.h"
#include "window/city.h"
#include "window/main_menu.h"
#include "window/mission_selection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_error(json_writer *w, const char *message)
{
    json_bool(w, "ok", 0);
    json_string(w, "error", message);
}

void agent_campaign_victory(json_writer *w, const char *args)
{
    if (window_get_id() != WINDOW_VICTORY_DIALOG) {
        write_error(w, "no victory dialog is open");
        return;
    }
    char choice[16] = "";
    if (args) {
        sscanf(args, "%15s", choice);
    }
    // As window/victory_dialog.c button_accept / button_continue_governing
    if (strcmp(choice, "accept") == 0) {
        window_city_show();
        json_bool(w, "ok", 1);
        json_string(w, "next", "advance time a little: the victory screens follow, then use next_mission");
    } else if (strcmp(choice, "continue_2") == 0 || strcmp(choice, "continue_5") == 0) {
        int months = choice[9] == '2' ? 24 : 60;
        city_victory_continue_governing(months);
        window_city_show();
        city_victory_reset();
        sound_music_update(1);
        json_bool(w, "ok", 1);
        json_int(w, "months_to_govern", months);
    } else {
        write_error(w, "usage: victory <accept|continue_2|continue_5>");
    }
}

static int is_after_victory_window(void)
{
    switch (window_get_id()) {
        case WINDOW_VIDEO:
        case WINDOW_INTERMEZZO:
        case WINDOW_MISSION_END:
            return 1;
        default:
            return 0;
    }
}

/** As window/mission_end.c advance_to_next_mission(); returns 0 when the campaign is complete */
static int advance_campaign(void)
{
    sound_music_stop();
    sound_speech_stop();
    const campaign_mission_info *mission_info = game_campaign_get_current_mission(scenario_campaign_mission());
    if (mission_info && mission_info->next_rank != CAMPAIGN_NO_RANK) {
        scenario_set_campaign_rank(mission_info->next_rank);
    }
    scenario_save_campaign_player_name();
    city_victory_stop_governing();
    game_undo_disable();
    game_state_reset_overlay();

    mission_info = game_campaign_advance_mission(scenario_campaign_mission());
    if (!mission_info) {
        window_main_menu_show(1);
        setting_clear_personal_savings();
        scenario_settings_init();
        scenario_set_campaign_rank(2);
        return 0;
    }
    scenario_set_campaign_mission(mission_info->first_scenario);
    int personal_savings = calc_bound(city_emperor_personal_savings(), 0, mission_info->max_personal_savings);
    setting_set_personal_savings_for_mission(game_campaign_is_custom() ? 0 : scenario_campaign_rank(),
        personal_savings);
    return 1;
}

static void write_choices(json_writer *w, const campaign_mission_info *mission)
{
    json_array_start(w, "choices");
    for (int i = 0; i < mission->total_scenarios; i++) {
        const campaign_scenario *scenario = game_campaign_get_scenario(mission->first_scenario + i);
        json_object_start(w, 0);
        json_int(w, "choice", i + 1);
        json_game_string(w, "name", scenario ? scenario->name : 0);
        json_game_string(w, "description", scenario ? scenario->description : 0);
        json_object_end(w);
    }
    json_array_end(w);
}

void agent_campaign_next_mission(json_writer *w, const char *args)
{
    int choice = args && *args ? atoi(args) : 0;
    if (city_victory_state() == VICTORY_STATE_WON && is_after_victory_window()) {
        if (!advance_campaign()) {
            json_bool(w, "ok", 1);
            json_bool(w, "campaign_complete", 1);
            return;
        }
    } else if (window_get_id() != WINDOW_MISSION_SELECTION) {
        write_error(w, "only after a won mission (victory screens) or on the mission selection screen");
        return;
    }
    const campaign_mission_info *mission = game_campaign_get_current_mission(scenario_campaign_mission());
    if (!mission) {
        write_error(w, "no campaign mission");
        return;
    }
    if (mission->total_scenarios > 1) {
        if (choice < 1 || choice > mission->total_scenarios) {
            // Show the selection screen to anyone watching, and let the agent pick
            if (window_get_id() != WINDOW_MISSION_SELECTION) {
                window_mission_selection_show();
            }
            json_bool(w, "ok", 1);
            json_bool(w, "choice_needed", 1);
            json_game_string(w, "mission", mission->title);
            write_choices(w, mission);
            return;
        }
        const campaign_scenario *scenario = game_campaign_get_scenario(mission->first_scenario + choice - 1);
        scenario_set_campaign_mission(scenario->id);
    }
    // As window/mission_briefing.c load_scenario_file() + button_start_mission()
    if (!game_campaign_load_scenario(scenario_campaign_mission())) {
        write_error(w, "loading the mission failed");
        return;
    }
    sound_music_stop();
    sound_speech_stop();
    sound_music_update(1);
    window_city_show();
    city_mission_reset_save_start();
    scenario_events_process_all();
    city_view_reset_orientation();
    city_victory_reset(); // the previous mission's "won" would otherwise linger until the next tick
    game_state_pause();
    json_bool(w, "ok", 1);
    json_int(w, "campaign_mission", scenario_campaign_mission());
    json_game_string(w, "scenario", scenario_name());
    json_string(w, "next", "read the briefing, then play");
}

void agent_campaign_test_win(json_writer *w)
{
    const char *test_mode = getenv("AUGUSTUS_AGENT_TEST_MODE");
    if (!test_mode || strcmp(test_mode, "1") != 0) {
        write_error(w, "unknown command, try: help");
        return;
    }
    city_victory_force_win();
    json_bool(w, "ok", 1);
    json_string(w, "next", "step a day: the victory dialog opens");
}
