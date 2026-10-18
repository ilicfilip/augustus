#include "agent.h"

#include "agent/act.h"
#include "agent/campaign.h"
#include "agent/json.h"
#include "agent/layout.h"
#include "agent/logistics.h"
#include "agent/observe.h"
#include "agent/ratings.h"
#include "agent/rome.h"
#include "agent/settings.h"
#include "agent/trade.h"
#include "city/finance.h"
#include "city/view.h"
#include "city/labor.h"
#include "city/population.h"
#include "city/ratings.h"
#include "city/data_private.h"
#include "city/resource.h"
#include "city/victory.h"
#include "core/dir.h"
#include "core/log.h"
#include "core/time.h"
#include "game/file.h"
#include "game/settings.h"
#include "game/state.h"
#include "game/system.h"
#include "game/tick.h"
#include "game/time.h"
#include "graphics/screenshot.h"
#include "graphics/window.h"
#include "scenario/criteria.h"
#include "scenario/property.h"
#include "window/city.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

int agent_init(int port)
{
    log_error("Agent server is not supported on Windows", 0, 0);
    return 0;
}

int agent_is_enabled(void)
{
    return 0;
}

unsigned long long agent_time_offset(void)
{
    return 0;
}

void agent_poll(void)
{
}

void agent_shutdown(void)
{
}

#else

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define TICKS_PER_DAY 50
#define MAX_STEP_DAYS 3840 // 20 years
#define INPUT_BUFFER_SIZE 4096
#define OUTPUT_BUFFER_SIZE (1024 * 1024)

static struct {
    int listen_fd;
    int client_fd;
    char input[INPUT_BUFFER_SIZE];
    int input_len;
    char output[OUTPUT_BUFFER_SIZE];
    time_millis time_offset;
    int reply_deferred; // set by a command that answers later (play)
    struct {
        int active;
        int days;
        int start_ticks;
        int target_ticks;
    } play;
} data = { -1, -1 };

unsigned long long agent_time_offset(void)
{
    return data.time_offset;
}

static int set_non_blocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

int agent_init(int port)
{
    data.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (data.listen_fd < 0) {
        log_error("Agent: unable to create socket", strerror(errno), 0);
        return 0;
    }
    int reuse = 1;
    setsockopt(data.listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t) port);
    if (bind(data.listen_fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
        listen(data.listen_fd, 1) != 0 || !set_non_blocking(data.listen_fd)) {
        log_error("Agent: unable to listen", strerror(errno), port);
        close(data.listen_fd);
        data.listen_fd = -1;
        return 0;
    }
    log_info("Agent: listening on 127.0.0.1 port", 0, port);
    return 1;
}

int agent_is_enabled(void)
{
    return data.listen_fd >= 0;
}

static void close_client(void)
{
    if (data.play.active) {
        game_state_pause();
        data.play.active = 0;
    }
    if (data.client_fd >= 0) {
        close(data.client_fd);
        data.client_fd = -1;
        data.input_len = 0;
        log_info("Agent: client disconnected", 0, 0);
    }
}

static void send_output(void)
{
    size_t len = strlen(data.output);
    data.output[len++] = '\n';
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(data.client_fd, data.output + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t) n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        } else {
            close_client();
            return;
        }
    }
}

static int is_city_window(void)
{
    switch (window_get_id()) {
        case WINDOW_CITY:
        case WINDOW_CITY_MILITARY:
        case WINDOW_SLIDING_SIDEBAR:
        case WINDOW_OVERLAY_MENU:
        case WINDOW_MILITARY_MENU:
        case WINDOW_BUILD_MENU:
            return 1;
        default:
            return 0;
    }
}

/** City data can be read here: the city screens, and the victory dialog (the city is still loaded behind it) */
static int can_read_city(void)
{
    return is_city_window() || window_get_id() == WINDOW_VICTORY_DIALOG;
}

static const char *window_name(void)
{
    switch (window_get_id()) {
        case WINDOW_LOGO: return "logo";
        case WINDOW_MAIN_MENU: return "main_menu";
        case WINDOW_CITY: return "city";
        case WINDOW_MESSAGE_DIALOG: return "message_dialog";
        case WINDOW_MESSAGE_LIST: return "message_list";
        case WINDOW_BUILDING_INFO: return "building_info";
        case WINDOW_ADVISORS: return "advisors";
        case WINDOW_MISSION_END: return "mission_end";
        case WINDOW_MISSION_BRIEFING: return "mission_briefing";
        case WINDOW_MISSION_SELECTION: return "mission_selection";
        case WINDOW_VICTORY_DIALOG: return "victory_dialog";
        case WINDOW_VIDEO: return "video";
        case WINDOW_INTERMEZZO: return "intermezzo";
        default: return is_city_window() ? "city" : "other";
    }
}

static void write_window(json_writer *w)
{
    json_object_start(w, "window");
    json_int(w, "id", window_get_id());
    json_string(w, "name", window_name());
    json_object_end(w);
}

static void write_error(json_writer *w, const char *message)
{
    json_bool(w, "ok", 0);
    json_string(w, "error", message);
}

static void write_date(json_writer *w)
{
    json_object_start(w, "date");
    json_int(w, "year", game_time_year());
    json_int(w, "month", game_time_month());
    json_int(w, "day", game_time_day());
    json_object_end(w);
}

static void write_criterion(json_writer *w, const char *name, int enabled, int target)
{
    if (enabled) {
        json_int(w, name, target);
    }
}

static void cmd_ping(json_writer *w)
{
    json_bool(w, "ok", 1);
    json_string(w, "version", system_version());
    write_window(w);
    json_bool(w, "in_city", is_city_window());
    json_bool(w, "paused", game_state_is_paused());
}

static void cmd_city(json_writer *w)
{
    if (!can_read_city()) {
        write_error(w, "no city on screen");
        write_window(w);
        return;
    }
    json_bool(w, "ok", 1);
    json_game_string(w, "scenario", scenario_name());
    write_date(w);
    json_bool(w, "paused", game_state_is_paused());
    static const char *DIFFICULTIES[] = { "very_easy", "easy", "normal", "hard", "very_hard" };
    int difficulty = setting_difficulty();
    json_string(w, "difficulty", difficulty >= 0 && difficulty <= 4 ? DIFFICULTIES[difficulty] : "unknown");
    int victory = city_victory_state();
    json_string(w, "victory", victory == VICTORY_STATE_WON ? "won" : victory == VICTORY_STATE_LOST ? "lost" : "none");

    // As opening an advisor does; otherwise these only refresh at year end
    city_finance_estimate_taxes();
    city_finance_estimate_wages();
    json_object_start(w, "finance");
    json_int(w, "treasury", city_finance_treasury());
    json_int(w, "tax_percentage", city_finance_tax_percentage());
    json_int(w, "estimated_tax_income", city_finance_estimated_tax_income());
    json_int(w, "estimated_wages", city_finance_estimated_wages());
    json_int(w, "wages", city_labor_wages());
    json_int(w, "wages_rome", city_labor_wages_rome());
    json_object_end(w);

    json_object_start(w, "population");
    json_int(w, "total", city_population());
    json_int(w, "working_age", city_population_people_of_working_age());
    json_int(w, "retired", city_population_retired_people());
    json_object_end(w);

    json_object_start(w, "labour");
    json_int(w, "workers_short", city_labor_workers_needed());
    json_int(w, "workers_employed", city_labor_workers_employed());
    json_int(w, "unemployed", city_labor_workers_unemployed());
    json_int(w, "unemployment_percentage", city_labor_unemployment_percentage());
    json_object_end(w);
    agent_settings_write_labour_categories(w);

    json_object_start(w, "food");
    json_int(w, "stored", city_resource_food_stored());
    json_int(w, "needed_per_month", city_resource_food_needed());
    json_int(w, "produced_last_month", city_data.resource.food_produced_last_month);
    json_int(w, "eaten_last_month", city_data.resource.food_consumed_last_month);
    json_string(w, "units", "food units, 100 = 1 cartload; percentage_produced = produced/eaten last month and "
        "swings with harvests (farms deliver in batches) — compare produced vs eaten over a few months instead");
    json_int(w, "supply_months", city_resource_food_supply_months());
    json_int(w, "types_eaten", city_resource_food_types_eaten());
    json_int(w, "percentage_produced", city_resource_food_percentage_produced());
    json_object_end(w);

    json_object_start(w, "ratings");
    json_int(w, "culture", city_rating_culture());
    json_int(w, "prosperity", city_rating_prosperity());
    json_int(w, "peace", city_rating_peace());
    json_int(w, "favor", city_rating_favor());
    json_object_end(w);

    json_object_start(w, "win_criteria");
    write_criterion(w, "population", scenario_criteria_population_enabled(), scenario_criteria_population());
    write_criterion(w, "culture", scenario_criteria_culture_enabled(), scenario_criteria_culture());
    write_criterion(w, "prosperity", scenario_criteria_prosperity_enabled(), scenario_criteria_prosperity());
    write_criterion(w, "peace", scenario_criteria_peace_enabled(), scenario_criteria_peace());
    write_criterion(w, "favor", scenario_criteria_favor_enabled(), scenario_criteria_favor());
    write_criterion(w, "time_limit_years", scenario_criteria_time_limit_enabled(),
        scenario_criteria_time_limit_years());
    write_criterion(w, "survival_years", scenario_criteria_survival_enabled(), scenario_criteria_survival_years());
    json_object_end(w);
}

static int total_game_ticks(void)
{
    return ((game_time_year() * 12 + game_time_month()) * 16 + game_time_day()) * TICKS_PER_DAY + game_time_tick();
}

static int is_valid_speed(int speed)
{
    return (speed >= 10 && speed <= 100 && speed % 10 == 0) || speed == 200 || speed == 300 || speed == 500;
}

/** play <days> [speed%]: run the game in real time, like a player watching it, then pause and answer */
static void cmd_play(json_writer *w, const char *args)
{
    int days = 1, speed = 100;
    if (args && *args && sscanf(args, "%d %d", &days, &speed) < 1) {
        days = 0;
    }
    if (days < 1 || days > MAX_STEP_DAYS) {
        write_error(w, "usage: play <days 1-3840> [speed 10-100 (steps of 10), 200, 300 or 500]");
        return;
    }
    if (!is_valid_speed(speed)) {
        write_error(w, "speed must be 10-100 in steps of 10, or 200, 300, 500");
        return;
    }
    if (!is_city_window()) {
        write_error(w, "no city on screen");
        write_window(w);
        return;
    }
    setting_set_game_speed(speed);
    data.play.active = 1;
    data.play.days = days;
    data.play.start_ticks = total_game_ticks();
    data.play.target_ticks = data.play.start_ticks + days * TICKS_PER_DAY;
    data.reply_deferred = 1;
    game_state_unpause();
}

static void cmd_step(json_writer *w, const char *args)
{
    int days = args && *args ? atoi(args) : 1;
    if (days < 1 || days > MAX_STEP_DAYS) {
        write_error(w, "days must be between 1 and 3840");
        return;
    }
    if (!is_city_window()) {
        write_error(w, "no city on screen");
        write_window(w);
        return;
    }
    game_state_pause();
    int total_ticks = days * TICKS_PER_DAY;
    int ticks = 0;
    const char *stopped_by = 0;
    time_millis now = time_get_millis();
    while (ticks < total_ticks) {
        // One millisecond per tick: code like house evolution skips buildings already processed "this
        // millisecond", so running many ticks at the same clock value would skip work
        time_set_millis(++now);
        data.time_offset++;
        game_tick_run();
        ticks++;
        if (!is_city_window()) {
            // A popup or another screen appeared: let the caller decide what to do
            stopped_by = "window_changed";
            break;
        }
    }
    window_invalidate();
    json_bool(w, "ok", 1);
    json_int(w, "days_requested", days);
    json_int(w, "ticks_run", ticks);
    if (stopped_by) {
        json_string(w, "stopped_by", stopped_by);
        write_window(w);
    }
    write_date(w);
}

static void cmd_load(json_writer *w, const char *name)
{
    if (!name || !*name) {
        write_error(w, "usage: load <savegame name, e.g. Engineer.svx>");
        return;
    }
    // An absolute path loads that file directly (used by tests); otherwise look in the savegame directory
    const char *filename = name[0] == '/' ? name : dir_get_file_at_location(name, PATH_LOCATION_SAVEGAME);
    if (!filename) {
        write_error(w, "savegame not found");
        return;
    }
    int result = game_file_load_saved_game(filename);
    if (result != FILE_LOAD_SUCCESS) {
        write_error(w, "unable to load savegame");
        json_int(w, "result", result);
        return;
    }
    // Coordinates always refer to the same tiles; multi-tile placement depends on the view orientation
    city_view_reset_orientation();
    city_victory_reset(); // the victory state isn't part of the save; don't carry it over from the last city
    window_city_show();
    game_state_pause();
    json_bool(w, "ok", 1);
    json_game_string(w, "scenario", scenario_name());
    write_date(w);
}

static void cmd_close(json_writer *w)
{
    if (is_city_window()) {
        write_error(w, "nothing to close");
        return;
    }
    // Message popups and in-game panels a watching player may have opened (advisors, building info, …)
    switch (window_get_id()) {
        case WINDOW_MESSAGE_DIALOG:
        case WINDOW_MESSAGE_LIST:
        case WINDOW_BUILDING_INFO:
        case WINDOW_ADVISORS:
        case WINDOW_LABOR_PRIORITY:
        case WINDOW_SET_SALARY:
        case WINDOW_DONATE_TO_CITY:
        case WINDOW_GIFT_TO_EMPEROR:
        case WINDOW_TRADE_PRICES:
        case WINDOW_RESOURCE_SETTINGS:
        case WINDOW_HOLD_FESTIVAL:
        case WINDOW_EMPIRE:
        case WINDOW_TRADE_OPENED:
        case WINDOW_TRADE_LEDGER:
        case WINDOW_OVERLAY_MENU:
            break;
        default:
            write_error(w, "this window can't be closed by the agent yet");
            write_window(w);
            return;
    }
    window_city_show();
    json_bool(w, "ok", 1);
    write_window(w);
}

static void cmd_screenshot(json_writer *w, const char *args)
{
    screenshot_type type = SCREENSHOT_FULL_CITY;
    if (args && strcmp(args, "minimap") == 0) {
        type = SCREENSHOT_MINIMAP;
    } else if (args && strcmp(args, "display") == 0) {
        type = SCREENSHOT_DISPLAY;
    } else if (args && *args && strcmp(args, "city") != 0) {
        write_error(w, "usage: screenshot [city|minimap|display]");
        return;
    }
    if (!is_city_window()) {
        write_error(w, "no city on screen");
        write_window(w);
        return;
    }
    graphics_save_screenshot(type);
    json_bool(w, "ok", 1);
    // The game names the file after the current time; the newest PNG in this directory is it
    char directory[1024];
    const char *location = dir_append_location("", PATH_LOCATION_SCREENSHOT);
    if (location[0] == '/') {
        snprintf(directory, sizeof(directory), "%s", location);
    } else {
        // Relative to the game data directory, which is the working directory
        char cwd[768];
        snprintf(directory, sizeof(directory), "%s/%s", getcwd(cwd, sizeof(cwd)) ? cwd : ".", location);
    }
    json_string(w, "directory", directory);
}

static void cmd_pause(json_writer *w, int pause)
{
    if (pause) {
        game_state_pause();
    } else {
        game_state_unpause();
    }
    json_bool(w, "ok", 1);
    json_bool(w, "paused", game_state_is_paused());
}

static void cmd_help(json_writer *w)
{
    json_bool(w, "ok", 1);
    json_array_start(w, "commands");
    json_string(w, 0, "ping");
    json_string(w, 0, "city");
    json_string(w, 0, "step [days=1]");
    json_string(w, 0, "play <days> [speed%] (real time, answers when done)");
    json_string(w, 0, "map [x y w h] [terrain|desirability|water ...]");
    json_string(w, 0, "buildings [type]");
    json_string(w, 0, "houses [x y w h]");
    json_string(w, 0, "buildable");
    json_string(w, 0, "risks [min percent]");
    json_string(w, 0, "build <type> <x> <y> [x2 y2]");
    json_string(w, 0, "clear <x> <y> [x2 y2]");
    json_string(w, 0, "undo");
    json_string(w, 0, "sites <type> <x> <y> [radius]");
    json_string(w, 0, "set tax <0-25> | set wages <0-100> | set priority <category> <0-9>");
    json_string(w, 0, "storage <id> [<resource> <accept|get|refuse|keep> [4-32] | permission <name> <on|off>]");
    json_string(w, 0, "market <id> [<resource> <on|off>]");
    json_string(w, 0, "roadblock <id> [<walker> <on|off>]");
    json_string(w, 0, "message");
    json_string(w, 0, "briefing");
    json_string(w, 0, "victory <accept|continue_2|continue_5>");
    json_string(w, 0, "next_mission [choice]");
    json_string(w, 0, "trade [<resource> <import|export|stockpile|mothball> <on|off> | <import_up_to|export_over> <0-100>]");
    json_string(w, 0, "open_route <city id>");
    json_string(w, 0, "depot <id> [<resource> <source id> <destination id> [always|never|source_over N|dest_under N]]");
    json_string(w, 0, "storage_report");
    json_string(w, 0, "supply <market/temple/tavern id> [<resource>]");
    json_string(w, 0, "walker_preview <type> <x> <y>");
    json_string(w, 0, "layout_review [x y w h]");
    json_string(w, 0, "ratings");
    json_string(w, 0, "finance");
    json_string(w, 0, "rome");
    json_string(w, 0, "salary <rank 0-10>");
    json_string(w, 0, "gift <modest|generous|lavish>");
    json_string(w, 0, "dispatch <request id>");
    json_string(w, 0, "festival [<god> <small|large|grand>]");
    json_string(w, 0, "save <agent_name>");
    json_string(w, 0, "load <savegame>");
    json_string(w, 0, "screenshot [city|minimap|display]");
    json_string(w, 0, "close");
    json_string(w, 0, "pause");
    json_string(w, 0, "resume");
    json_string(w, 0, "help");
    json_array_end(w);
}

static void execute(char *line)
{
    json_writer w;
    json_init(&w, data.output, OUTPUT_BUFFER_SIZE - 1); // leave room for the newline

    while (*line == ' ' || *line == '\t') {
        line++;
    }
    char *args = line;
    while (*args && *args != ' ' && *args != '\t') {
        args++;
    }
    if (*args) {
        *args++ = 0;
    }

    json_object_start(&w, 0);
    json_string(&w, "command", line);
    if (strcmp(line, "ping") == 0) {
        cmd_ping(&w);
    } else if (strcmp(line, "city") == 0) {
        cmd_city(&w);
    } else if (strcmp(line, "play") == 0) {
        cmd_play(&w, args);
    } else if (strcmp(line, "step") == 0) {
        cmd_step(&w, args);
    } else if (strcmp(line, "risks") == 0) {
        if (!can_read_city()) {
            write_error(&w, "no city on screen");
            write_window(&w);
        } else {
            agent_observe_risks(&w, args);
        }
    } else if (strcmp(line, "buildable") == 0) {
        if (!can_read_city()) {
            write_error(&w, "no city on screen");
            write_window(&w);
        } else {
            agent_observe_buildable(&w);
        }
    } else if (strcmp(line, "map") == 0 || strcmp(line, "buildings") == 0 || strcmp(line, "houses") == 0) {
        if (!can_read_city()) {
            write_error(&w, "no city on screen");
            write_window(&w);
        } else if (line[0] == 'm') {
            agent_observe_map(&w, args);
        } else if (line[0] == 'b') {
            agent_observe_buildings(&w, args);
        } else {
            agent_observe_houses(&w, args);
        }
    } else if (strcmp(line, "sites") == 0) {
        if (!can_read_city()) {
            write_error(&w, "no city on screen");
            write_window(&w);
        } else {
            agent_act_sites(&w, args);
        }
    } else if (strcmp(line, "build") == 0 || strcmp(line, "clear") == 0 || strcmp(line, "undo") == 0) {
        if (!is_city_window()) {
            write_error(&w, "no city on screen");
            write_window(&w);
        } else if (line[0] == 'b') {
            agent_act_build(&w, args);
        } else if (line[0] == 'c') {
            agent_act_clear(&w, args);
        } else {
            agent_act_undo(&w);
        }
    } else if (strcmp(line, "set") == 0 || strcmp(line, "storage") == 0 || strcmp(line, "market") == 0 ||
        strcmp(line, "roadblock") == 0 || strcmp(line, "save") == 0) {
        // Saving also works on the victory dialog: after accepting, the city is gone (videos, mission end)
        if (!is_city_window() && !(strcmp(line, "save") == 0 && window_get_id() == WINDOW_VICTORY_DIALOG)) {
            write_error(&w, "no city on screen");
            write_window(&w);
        } else if (strcmp(line, "set") == 0) {
            agent_settings_set(&w, args);
        } else if (strcmp(line, "storage") == 0) {
            agent_settings_storage(&w, args);
        } else if (strcmp(line, "market") == 0) {
            agent_settings_market(&w, args);
        } else if (strcmp(line, "roadblock") == 0) {
            agent_settings_roadblock(&w, args);
        } else {
            agent_settings_save(&w, args);
        }
    } else if (strcmp(line, "rome") == 0 || strcmp(line, "salary") == 0 || strcmp(line, "gift") == 0 ||
        strcmp(line, "dispatch") == 0 || strcmp(line, "festival") == 0) {
        if (!is_city_window()) {
            write_error(&w, "no city on screen");
            write_window(&w);
        } else if (strcmp(line, "rome") == 0) {
            agent_rome_status(&w);
        } else if (strcmp(line, "salary") == 0) {
            agent_rome_salary(&w, args);
        } else if (strcmp(line, "gift") == 0) {
            agent_rome_gift(&w, args);
        } else if (strcmp(line, "festival") == 0) {
            agent_rome_festival(&w, args);
        } else {
            agent_rome_dispatch(&w, args);
        }
    } else if (strcmp(line, "walker_preview") == 0 || strcmp(line, "layout_review") == 0) {
        if (!can_read_city()) {
            write_error(&w, "no city on screen");
            write_window(&w);
        } else if (line[0] == 'w') {
            agent_layout_walker_preview(&w, args);
        } else {
            agent_layout_review(&w, args);
        }
    } else if (strcmp(line, "ratings") == 0 || strcmp(line, "finance") == 0) {
        if (!can_read_city()) {
            write_error(&w, "no city on screen");
            write_window(&w);
        } else if (line[0] == 'r') {
            agent_ratings(&w);
        } else {
            agent_ratings_finance(&w);
        }
    } else if (strcmp(line, "depot") == 0 || strcmp(line, "storage_report") == 0 || strcmp(line, "supply") == 0) {
        if (strcmp(line, "depot") == 0 ? !is_city_window() : !can_read_city()) {
            write_error(&w, "no city on screen");
            write_window(&w);
        } else if (line[0] == 'd') {
            agent_logistics_depot(&w, args);
        } else if (strcmp(line, "supply") == 0) {
            agent_logistics_supply(&w, args);
        } else {
            agent_logistics_storage_report(&w);
        }
    } else if (strcmp(line, "trade") == 0 || strcmp(line, "open_route") == 0) {
        if (!is_city_window()) {
            write_error(&w, "no city on screen");
            write_window(&w);
        } else if (line[0] == 't') {
            agent_trade(&w, args);
        } else {
            agent_trade_open_route(&w, args);
        }
    } else if (strcmp(line, "victory") == 0) {
        agent_campaign_victory(&w, args);
    } else if (strcmp(line, "next_mission") == 0) {
        agent_campaign_next_mission(&w, args);
    } else if (strcmp(line, "test_win") == 0) {
        agent_campaign_test_win(&w);
    } else if (strcmp(line, "briefing") == 0) {
        agent_settings_briefing(&w);
    } else if (strcmp(line, "message") == 0) {
        agent_settings_message(&w);
    } else if (strcmp(line, "load") == 0) {
        cmd_load(&w, args);
    } else if (strcmp(line, "screenshot") == 0) {
        cmd_screenshot(&w, args);
    } else if (strcmp(line, "close") == 0) {
        cmd_close(&w);
    } else if (strcmp(line, "pause") == 0) {
        cmd_pause(&w, 1);
    } else if (strcmp(line, "resume") == 0) {
        cmd_pause(&w, 0);
    } else if (strcmp(line, "help") == 0) {
        cmd_help(&w);
    } else {
        write_error(&w, "unknown command, try: help");
    }
    json_object_end(&w);

    if (data.reply_deferred) {
        data.reply_deferred = 0;
        return; // answered when the command finishes
    }
    if (w.overflow) {
        json_init(&w, data.output, OUTPUT_BUFFER_SIZE - 1);
        json_object_start(&w, 0);
        write_error(&w, "response too large");
        json_object_end(&w);
    }
    send_output();
}

static void finish_play(const char *stopped_by)
{
    game_state_pause();
    data.play.active = 0;
    json_writer w;
    json_init(&w, data.output, OUTPUT_BUFFER_SIZE - 1);
    json_object_start(&w, 0);
    json_string(&w, "command", "play");
    json_bool(&w, "ok", 1);
    json_int(&w, "days_requested", data.play.days);
    json_int(&w, "ticks_run", total_game_ticks() - data.play.start_ticks);
    if (stopped_by) {
        json_string(&w, "stopped_by", stopped_by);
        write_window(&w);
    }
    write_date(&w);
    json_object_end(&w);
    if (data.client_fd >= 0) {
        send_output();
    }
}

static void accept_client(void)
{
    int fd = accept(data.listen_fd, 0, 0);
    if (fd < 0) {
        return;
    }
    if (data.client_fd >= 0) {
        // Only one controller at a time
        const char *busy = "{\"ok\":false,\"error\":\"another agent is connected\"}\n";
        send(fd, busy, strlen(busy), 0);
        close(fd);
        return;
    }
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    set_non_blocking(fd);
    data.client_fd = fd;
    data.input_len = 0;
    // The agent decides when time moves
    game_state_pause();
    log_info("Agent: client connected, game paused", 0, 0);
}

static void process_input(void);

static void read_client(void)
{
    ssize_t n = recv(data.client_fd, data.input + data.input_len, INPUT_BUFFER_SIZE - 1 - data.input_len, 0);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        close_client();
        return;
    }
    if (n < 0) {
        return;
    }
    data.input_len += (int) n;
    data.input[data.input_len] = 0;
    process_input();
}

/** Executes complete command lines; stops while a deferred command (play) is running */
static void process_input(void)
{
    char *start = data.input;
    char *newline;
    while (data.client_fd >= 0 && !data.play.active && (newline = strchr(start, '\n')) != 0) {
        *newline = 0;
        if (newline > start && newline[-1] == '\r') {
            newline[-1] = 0;
        }
        if (*start) {
            execute(start);
        }
        start = newline + 1;
    }
    if (data.client_fd < 0) {
        return;
    }
    data.input_len -= (int) (start - data.input);
    memmove(data.input, start, (size_t) data.input_len);
    data.input[data.input_len] = 0;
    if (data.input_len >= INPUT_BUFFER_SIZE - 1) {
        // Line too long: drop it
        data.input_len = 0;
    }
}

void agent_poll(void)
{
    if (data.listen_fd < 0) {
        return;
    }
    accept_client();
    if (data.play.active) {
        // Real-time play: the normal game loop advances time; answer once done or interrupted
        if (!is_city_window()) {
            finish_play("window_changed");
        } else if (total_game_ticks() >= data.play.target_ticks) {
            finish_play(0);
        } else {
            return;
        }
        process_input(); // commands that arrived while playing
        return;
    }
    if (data.client_fd >= 0) {
        read_client();
    }
}

void agent_shutdown(void)
{
    close_client();
    if (data.listen_fd >= 0) {
        close(data.listen_fd);
        data.listen_fd = -1;
    }
}

#endif
