#include "agent.h"

#include "agent/json.h"
#include "agent/observe.h"
#include "city/finance.h"
#include "city/labor.h"
#include "city/population.h"
#include "city/ratings.h"
#include "city/resource.h"
#include "core/dir.h"
#include "core/log.h"
#include "game/file.h"
#include "game/state.h"
#include "game/system.h"
#include "game/tick.h"
#include "game/time.h"
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
} data = { -1, -1 };

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

static const char *window_name(void)
{
    switch (window_get_id()) {
        case WINDOW_LOGO: return "logo";
        case WINDOW_MAIN_MENU: return "main_menu";
        case WINDOW_CITY: return "city";
        case WINDOW_MESSAGE_DIALOG: return "message_dialog";
        case WINDOW_MESSAGE_LIST: return "message_list";
        case WINDOW_BUILDING_INFO: return "building_info";
        case WINDOW_MISSION_END: return "mission_end";
        case WINDOW_MISSION_BRIEFING: return "mission_briefing";
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
    if (!is_city_window()) {
        write_error(w, "no city on screen");
        write_window(w);
        return;
    }
    json_bool(w, "ok", 1);
    json_game_string(w, "scenario", scenario_name());
    write_date(w);
    json_bool(w, "paused", game_state_is_paused());

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

    json_object_start(w, "food");
    json_int(w, "stored", city_resource_food_stored());
    json_int(w, "needed_per_month", city_resource_food_needed());
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
    while (ticks < total_ticks) {
        game_tick_run();
        game_file_write_mission_saved_game();
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
    const char *filename = dir_get_file_at_location(name, PATH_LOCATION_SAVEGAME);
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
    if (window_get_id() != WINDOW_MESSAGE_DIALOG && window_get_id() != WINDOW_MESSAGE_LIST &&
        window_get_id() != WINDOW_BUILDING_INFO) {
        write_error(w, "this window can't be closed by the agent yet");
        write_window(w);
        return;
    }
    window_city_show();
    json_bool(w, "ok", 1);
    write_window(w);
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
    json_string(w, 0, "map [x y w h] [terrain|desirability|water ...]");
    json_string(w, 0, "buildings [type]");
    json_string(w, 0, "houses [x y w h]");
    json_string(w, 0, "load <savegame>");
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
    } else if (strcmp(line, "step") == 0) {
        cmd_step(&w, args);
    } else if (strcmp(line, "map") == 0 || strcmp(line, "buildings") == 0 || strcmp(line, "houses") == 0) {
        if (!is_city_window()) {
            write_error(&w, "no city on screen");
            write_window(&w);
        } else if (line[0] == 'm') {
            agent_observe_map(&w, args);
        } else if (line[0] == 'b') {
            agent_observe_buildings(&w, args);
        } else {
            agent_observe_houses(&w, args);
        }
    } else if (strcmp(line, "load") == 0) {
        cmd_load(&w, args);
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

    if (w.overflow) {
        json_init(&w, data.output, OUTPUT_BUFFER_SIZE - 1);
        json_object_start(&w, 0);
        write_error(&w, "response too large");
        json_object_end(&w);
    }
    send_output();
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

    char *start = data.input;
    char *newline;
    while (data.client_fd >= 0 && (newline = strchr(start, '\n')) != 0) {
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
