#define _DEFAULT_SOURCE
#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum scenario {
  LOCAL,
  FAST,
  ZERO,
  CUSTOM,
  RECONFIGURE,
  SNAPSHOT,
  SNAPSHOT_FAST,
  PENDING,
  PENDING_FAST,
  REJECTED,
  HELD,
  EDITING,
  INDEPENDENT,
  SPLIT_ESCAPE,
  VISIBLE,
  VISIBLE_DRAIN,
  VISIBLE_RESET,
  VISIBLE_SNAPSHOT,
  VISIBLE_PENDING,
  VISIBLE_MIXED
};
struct host {
  int events, scenario, revision, extra;
  sl_queue_request_id_t request;
};
static double milliseconds(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}
static int request(void *context, const sl_queue_request_t *value) {
  struct host *host = context;
  host->request = value->request_id;
  return SL_OK;
}
static int pending_scenario(int scenario) {
  return (scenario >= PENDING && scenario <= REJECTED) ||
         scenario == VISIBLE_PENDING;
}
static int snapshot_scenario(int scenario) {
  return scenario == SNAPSHOT || scenario == SNAPSHOT_FAST ||
         scenario == VISIBLE_SNAPSHOT || scenario == VISIBLE_MIXED;
}
static int snapshot(sl_t *sl, struct host *host, int present) {
  sl_queue_entry_t entries[2];
  sl_queue_change_t change;
  sl_queue_request_t sync;
  sl_queue_request_id_t request_id = 0;
  sl_queue_result_t result;
  int status;
  char revision[32];
  memset(entries, 0, sizeof(entries));
  entries[0].id = "entry";
  entries[0].revision = "item1";
  entries[0].text = "queued-marker";
  entries[0].mode = SL_PROMPT_QUEUE_MODE_STEER;
  entries[1] = entries[0];
  entries[1].id = "second";
  entries[1].text = "second-marker";
  memset(&change, 0, sizeof(change));
  snprintf(revision, sizeof(revision), "queue%d", ++host->revision);
  change.kind = SL_QUEUE_SNAPSHOT;
  change.revision = revision;
  change.entries = present ? entries : NULL;
  change.count = change.total_count = present ? 1 + host->extra : 0;
  if (host->revision > 1) {
    memset(&sync, 0, sizeof(sync));
    sync.operation = SL_QUEUE_SYNCHRONIZE;
    status = sl_queue_backend_request(sl, &sync, &request_id);
    if (status != SL_OK)
      return status;
    change.request_id = request_id;
  }
  status = sl_queue_backend_update(sl, &change);
  if (status != SL_OK || !request_id)
    return status;
  memset(&result, 0, sizeof(result));
  result.request_id = request_id;
  result.status = SL_QUEUE_ACCEPTED;
  return sl_queue_backend_result(sl, &result);
}
static int command(sl_t *sl, const sl_watch_event_t *event, void *context) {
  struct host *host = context;
  char key;
  int status = SL_OK;
  if (read(event->fd, &key, 1) != 1)
    return SL_ERROR_IO;
  if (key == 'S') {
    if (snapshot_scenario(host->scenario))
      status = snapshot(sl, host, 1);
    else if (pending_scenario(host->scenario)) {
      sl_queue_request_t offer;
      sl_queue_backend_info_t info;
      memset(&offer, 0, sizeof(offer));
      offer.operation = SL_QUEUE_OFFER;
      offer.text = "pending-marker";
      status = sl_queue_backend_request(sl, &offer, &host->request);
      if (sl_queue_backend_info(sl, &info) != SL_OK || info.pending_count != 1)
        return SL_ERROR;
    } else {
      status = sl_prompt_queue_append(sl, "queued-marker",
                                      host->scenario == HELD, NULL);
    }
    if (!pending_scenario(host->scenario))
      if (sl_prompt_queue_count(sl) != 1) {
        fprintf(stderr, "queue count after start: %lu (status %d)\n",
                (unsigned long)sl_prompt_queue_count(sl), status);
        return SL_ERROR;
      }
    if (status == SL_OK && write(host->events, "R", 1) != 1)
      status = SL_ERROR_IO;
  } else if (key == 'C') {
    if (host->scenario == SNAPSHOT_FAST)
      status = snapshot(sl, host, 0);
    else if (host->scenario == PENDING_FAST || host->scenario == REJECTED) {
      sl_queue_result_t result;
      memset(&result, 0, sizeof(result));
      result.request_id = host->request;
      result.status =
          host->scenario == REJECTED ? SL_QUEUE_REJECTED : SL_QUEUE_ACCEPTED;
      result.reason = host->scenario == REJECTED ? "rejected-marker" : NULL;
      if (host->scenario == PENDING_FAST) {
        result.item_id = "accepted-item";
        result.item_revision = "item1";
      }
      status = sl_queue_backend_result(sl, &result);
    } else {
      char *text = NULL;
      status = sl_prompt_queue_take(sl, 0, &text);
      if (status == SL_OK && (!text || strcmp(text, "queued-marker")))
        status = SL_ERROR;
      sl_free_string(sl, text);
    }
  } else if (key == 'U')
    status = snapshot(sl, host, 1);
  else if (key == 'Z')
    status = sl->set_prompt_queue_display_delay(sl, 0);
  else if (key == 'L') {
    if (host->scenario == VISIBLE_SNAPSHOT) {
      host->extra = 1;
      status = snapshot(sl, host, 1);
    } else if (host->scenario == VISIBLE_PENDING ||
               host->scenario == VISIBLE_MIXED) {
      sl_queue_request_t offer;
      sl_queue_backend_info_t info;
      memset(&offer, 0, sizeof(offer));
      offer.operation = SL_QUEUE_OFFER;
      offer.text = "second-marker";
      status = sl_queue_backend_request(sl, &offer, &host->request);
      if (sl_queue_backend_info(sl, &info) != SL_OK ||
          info.pending_count != (host->scenario == VISIBLE_PENDING ? 2 : 1))
        return SL_ERROR;
    } else {
      int index = (int)sl_prompt_queue_count(sl);
      status = sl_prompt_queue_append(sl, "second-marker", 0, NULL);
      if (status == SL_OK)
        status =
            sl_prompt_queue_set_mode(sl, index, SL_PROMPT_QUEUE_MODE_STEER);
      if (status == SL_OK && host->scenario == VISIBLE_DRAIN) {
        char *text = NULL;
        status = sl_prompt_queue_take(sl, 0, &text);
        sl_free_string(sl, text);
      }
    }
  } else if (key == 'X')
    status = sl_cancel(sl);
  else
    status = SL_ERROR;
  if (status != SL_OK)
    fprintf(stderr, "delay command %c failed: %d (%s)\n", key, status,
            sl_last_error(sl) ? sl_last_error(sl) : "no diagnostic");
  return status;
}
static void child(int slave, int commands, int events, int scenario,
                  int native) {
  sl_config_t config;
  sl_queue_backend_t backend;
  sl_watch_id_t watch;
  struct host host;
  sl_t *sl;
  char *line;
  memset(&host, 0, sizeof(host));
  host.scenario = scenario;
  host.events = events;
  memset(&backend, 0, sizeof(backend));
  backend.request = request;
  backend.capabilities =
      (1UL << SL_QUEUE_OFFER) | (1UL << SL_QUEUE_SYNCHRONIZE);
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  config.prompt_queue = 1;
  config.prompt_theme = SL_PROMPT_THEME_PLAIN;
  if (scenario == ZERO)
    config.prompt_queue_display_delay_ms = 0;
  if (scenario == CUSTOM)
    config.prompt_queue_display_delay_ms = 120;
  if ((scenario >= SNAPSHOT && scenario <= REJECTED) ||
      scenario >= VISIBLE_SNAPSHOT) {
    config.queue_backend = &backend;
    config.queue_backend_context = &host;
  }
  sl = sl_create_with_config(&config);
  if (!sl || (config.queue_backend && snapshot(sl, &host, 0) != SL_OK) ||
      sl_watch_add(sl, commands, SL_WATCH_READ, command, &host, &watch) !=
          SL_OK ||
      (native && sl_output_stream_begin(sl) != SL_OK))
    _exit(2);
  line = sl_readline(sl, "delay> ");
  if (line || sl_last_readline_status(sl) != SL_READLINE_CANCELLED) {
    fprintf(stderr, "delay child readline status %d: %s\n",
            sl_last_readline_status(sl),
            sl_last_error(sl) ? sl_last_error(sl) : "no diagnostic");
    _exit(3);
  }
  if (native && sl_output_stream_end(sl) != SL_OK)
    _exit(4);
  sl_destroy(sl);
  _exit(0);
}
#define REQUIRE(test)                                                          \
  do {                                                                         \
    if (!(test))                                                               \
      goto cleanup;                                                            \
  } while (0)
static int run(int scenario, int native) {
  int master = -1, slave = -1, commands[2] = {-1, -1}, events[2] = {-1, -1};
  int failed = 1, status, sent = 0, changed = 0, finished = 0, updates = 0;
  pid_t pid = -1;
  char *output = calloc(1, 65536);
  size_t used = 0, scanned = 0;
  double deadline = milliseconds() + 3000, started = 0, first = -1, second = -1;
  double appended = -1;
  struct winsize size = {24, 100, 0, 0};
  const char *marker = snapshot_scenario(scenario)  ? "S 1. queued-marker"
                       : pending_scenario(scenario) ? "P 1 pending"
                                                    : "Q 1. queued-marker";
  const char *second_marker =
      scenario == VISIBLE_PENDING ? "P 2 pending"
      : scenario == VISIBLE_MIXED ? "P 1 pending"
      : scenario == VISIBLE_DRAIN || scenario == VISIBLE_RESET
          ? "S 1. second-marker"
          : "S 2. second-marker";
  REQUIRE(output && openpty(&master, &slave, NULL, NULL, &size) == 0);
  REQUIRE(pipe(commands) == 0 && pipe(events) == 0);
  pid = fork();
  REQUIRE(pid >= 0);
  if (!pid) {
    close(master);
    close(commands[1]);
    close(events[0]);
    child(slave, commands[0], events[1], scenario, native);
  }
  close(slave);
  slave = -1;
  close(commands[0]);
  commands[0] = -1;
  close(events[1]);
  events[1] = -1;
  while (milliseconds() < deadline) {
    struct pollfd fds[2];
    double elapsed;
    pid_t exited;
    fds[0].fd = master;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = events[0];
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    REQUIRE(poll(fds, 2, 10) >= 0);
    if (fds[0].revents & POLLIN) {
      const char *query;
      ssize_t count = read(master, output + used, 65535 - used);
      if (count > 0)
        used += (size_t)count;
      REQUIRE(used < 65535);
      output[used] = 0;
      while ((query = strstr(output + scanned, "\033[6n")) != NULL) {
        REQUIRE(write(master, "\033[1;1R", 6) == 6);
        scanned = (size_t)(query - output) + 4;
      }
    }
    if (!sent && strstr(output, "delay> ")) {
      REQUIRE(write(commands[1], "S", 1) == 1);
      sent = 1;
    }
    if (fds[1].revents & POLLIN) {
      char event;
      REQUIRE(read(events[0], &event, 1) == 1 && event == 'R');
      started = milliseconds();
    }
    elapsed = milliseconds() - started;
    if (started && first < 0 && strstr(output, marker))
      first = elapsed;
    if (started && second < 0 && strstr(output, second_marker))
      second = elapsed;
    if (started && !finished) {
      if (scenario >= VISIBLE) {
        if (second >= 0) {
          REQUIRE(first >= 450 && appended >= first);
          REQUIRE(second - appended < 800);
          if (scenario == VISIBLE_RESET)
            REQUIRE(second - appended >= 450);
          else
            REQUIRE(second - appended < 250);
          finished = 1;
        }
        if (first >= 0 && !changed) {
          REQUIRE(write(commands[1], scenario == VISIBLE_RESET ? "C" : "L",
                        1) == 1);
          changed = 1;
          if (scenario != VISIBLE_RESET)
            appended = elapsed;
        } else if (scenario == VISIBLE_RESET && changed == 1 &&
                   elapsed - first >= 100) {
          REQUIRE(write(commands[1], "L", 1) == 1);
          changed = 2;
          appended = elapsed;
        }
      } else if (scenario == HELD && strstr(output, "approval"))
        finished = 1;
      else if ((scenario == EDITING || scenario == SPLIT_ESCAPE) &&
               strstr(output, "Q editing"))
        finished = 1;
      else if (scenario == REJECTED && strstr(output, "1 rejected"))
        finished = 1;
      else if (scenario == FAST || scenario == SNAPSHOT_FAST ||
               scenario == PENDING_FAST) {
        REQUIRE(first < 0);
        if (elapsed >= 700)
          finished = 1;
      } else if (scenario == INDEPENDENT) {
        if (second >= 0) {
          REQUIRE(first >= 450 && first < 700 && second >= 700);
          finished = 1;
        }
      } else if (first >= 0) {
        REQUIRE(first < 800);
        if (scenario == ZERO)
          REQUIRE(first < 250);
        else if (scenario == CUSTOM)
          REQUIRE(first >= 90);
        else if (scenario == RECONFIGURE)
          REQUIRE(first >= 90 && first < 350);
        else
          REQUIRE(first >= 450);
        finished = 1;
      }
      if (!changed && elapsed >= 100 &&
          (scenario == FAST || scenario == SNAPSHOT_FAST ||
           scenario == PENDING_FAST || scenario == REJECTED ||
           scenario == RECONFIGURE || scenario == EDITING)) {
        REQUIRE(scenario == EDITING
                    ? write(master, "\033e", 2) == 2
                    : write(commands[1], scenario == RECONFIGURE ? "Z" : "C",
                            1) == 1);
        changed = 1;
      }
      if (!changed && scenario == INDEPENDENT && elapsed >= 250) {
        REQUIRE(write(commands[1], "L", 1) == 1);
        changed = 1;
      }
      if (scenario == SPLIT_ESCAPE) {
        if (!changed && elapsed >= 450) {
          REQUIRE(write(master, "\033", 1) == 1);
          changed = 1;
        } else if (changed == 1 && elapsed >= 520) {
          REQUIRE(write(master, "e", 1) == 1);
          changed = 2;
        }
      }
      if (scenario == SNAPSHOT && first < 0 && elapsed >= (updates + 1) * 100) {
        REQUIRE(write(commands[1], "U", 1) == 1);
        ++updates;
      }
      if (finished) {
        if (scenario == HELD)
          REQUIRE(elapsed < 350);
        if (scenario == EDITING || scenario == REJECTED)
          REQUIRE(elapsed < 350);
        if (scenario == SPLIT_ESCAPE)
          REQUIRE(elapsed >= 500 && elapsed < 900);
        REQUIRE(write(commands[1], "X", 1) == 1);
      }
    }
    exited = waitpid(pid, &status, WNOHANG);
    if (exited == pid) {
      pid = -1;
      REQUIRE(finished && WIFEXITED(status) && WEXITSTATUS(status) == 0);
      failed = 0;
      break;
    }
  }
cleanup:
  if (failed)
    fprintf(stderr, "queue delay scenario=%d native=%d first=%.1f\n%s\n",
            scenario, native, first, output ? output : "");
  if (pid > 0) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
  }
  if (master >= 0)
    close(master);
  if (slave >= 0)
    close(slave);
  if (commands[0] >= 0)
    close(commands[0]);
  if (commands[1] >= 0)
    close(commands[1]);
  if (events[0] >= 0)
    close(events[0]);
  if (events[1] >= 0)
    close(events[1]);
  free(output);
  return failed;
}
static int immediate_delivery(void) {
  int master, slave, failed;
  sl_config_t config;
  sl_prompt_source_t source;
  sl_t *sl;
  char *line;
  double started;
  if (openpty(&master, &slave, NULL, NULL, NULL) != 0)
    return 1;
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  config.prompt_queue = 1;
  sl = sl_create_with_config(&config);
  if (!sl || sl_prompt_queue_append(sl, "deliver-now", 0, NULL) != SL_OK)
    return 1;
  started = milliseconds();
  line = sl_next_prompt(sl, "> ", &source);
  failed = !line || strcmp(line, "deliver-now") ||
           source != SL_PROMPT_SOURCE_QUEUED || milliseconds() - started >= 250;
  sl_free_string(sl, line);
  sl_destroy(sl);
  close(master);
  close(slave);
  return failed;
}

int main(void) {
  int scenario, native;
  sl_config_t config;
  sl_t *sl;
  sl_config_init(&config);
  if (config.prompt_queue_display_delay_ms != 500)
    return 1;
  config.prompt_queue_display_delay_ms = -1;
  if (sl_create_with_config(&config))
    return 1;
  sl = sl_create();
  if (!sl || sl_set_prompt_queue_display_delay(NULL, 0) != SL_ERROR_INVALID ||
      sl_set_prompt_queue_display_delay(sl, -1) != SL_ERROR_INVALID ||
      sl->set_prompt_queue_display_delay(sl, 0) != SL_OK)
    return 1;
  sl_destroy(sl);
  if (immediate_delivery())
    return 1;
  for (native = 0; native < 2; ++native)
    for (scenario = LOCAL; scenario <= VISIBLE_MIXED; ++scenario)
      if (run(scenario, native))
        return 1;
  return 0;
}
