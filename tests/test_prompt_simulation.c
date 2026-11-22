#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#include <errno.h>
#include <softline/softline.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "simulation line %d: %s\n", __LINE__, #c);               \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static const char text[] = "e\314\201x";
typedef struct {
  double start, first, full;
  int steered, master, send_manual;
} probe_t;

static double milliseconds(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}

static void observe(sl_t *sl, void *userdata) {
  probe_t *probe = userdata;
  const char *draft = sl_buffer(sl);
  if (!probe->first && !strcmp(draft, "e\314\201"))
    probe->first = milliseconds();
  if (!probe->full && !strcmp(draft, text))
    probe->full = milliseconds();
  if (probe->send_manual == 1)
    probe->send_manual = write(probe->master, "manual\r", 7) == 7 ? 0 : -1;
}

static int admit(sl_t *sl, const char *draft, sl_prompt_queue_mode_t mode,
                 sl_prompt_queue_policy_t *policy, void *userdata) {
  probe_t *probe = userdata;
  (void)sl;
  if (!strcmp(draft, "steering"))
    probe->steered += mode == SL_PROMPT_QUEUE_MODE_STEER;
  policy->enqueue = mode == SL_PROMPT_QUEUE_MODE_STEER;
  policy->executable = 1;
  return SL_OK;
}

static int simulate(int master, int slave) {
  sl_config_t config;
  sl_prompt_simulation_options_t options = {40, 30, 50,
                                            SL_PROMPT_QUEUE_MODE_QUEUED};
  sl_t *sl;
  char *line;
  probe_t probe = {0};
  sl_prompt_source_t source;
  probe.master = master;
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  config.prompt_queue = 1;
  sl = sl_create_with_config(&config);
  CHECK(sl);
  CHECK(sl_set_idle_callback(sl, observe, &probe) == SL_OK);
  CHECK(sl_set_prompt_queue_policy(sl, admit, &probe) == SL_OK);
  CHECK(sl_set_prompt_queue_profile(sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) ==
        SL_OK);
  CHECK(sl_set_prompt_queue_delivery(sl, SL_PROMPT_QUEUE_DELIVERY_AUTO) ==
        SL_OK);
  CHECK(sl_set_buffer(sl, "preserve this") == SL_OK &&
        sl_simulate_prompt(sl, text, &options) == SL_ERROR_NOT_READY &&
        !strcmp(sl_buffer(sl), "preserve this") &&
        sl_set_buffer(sl, "") == SL_OK);
  probe.start = milliseconds();
  CHECK(sl->simulate_prompt(sl, text, &options) == SL_OK &&
        sl_simulate_prompt(sl, text, &options) == SL_ERROR_NOT_READY);
  line = sl->readline(sl, "> ");
  CHECK(line && !strcmp(line, text) && probe.first >= probe.start + 35 &&
        probe.full >= probe.first + 25 && milliseconds() >= probe.full + 45);
  sl_free_string(sl, line);
  memset(&options, 0, sizeof(options));
  options.mode = SL_PROMPT_QUEUE_MODE_STEER;
  CHECK(sl_simulate_prompt(sl, "steering", &options) == SL_OK);
  line = sl_next_prompt(sl, "> ", &source);
  CHECK(line && !strcmp(line, "steering") && probe.steered == 1 &&
        source == SL_PROMPT_SOURCE_PROMOTED);
  sl_free_string(sl, line);
  options.start_delay_ms = 10000;
  options.mode = SL_PROMPT_QUEUE_MODE_QUEUED;
  CHECK(sl_simulate_prompt(sl, "cancel when typing", &options) == SL_OK);
  probe.send_manual = 1;
  line = sl->readline(sl, "> ");
  CHECK(!probe.send_manual && line && !strcmp(line, "manual"));
  sl_free_string(sl, line);
  CHECK(sl_cancel_prompt_simulation(sl) == SL_OK);
  sl_destroy(sl);
  return 0;
}

int main(void) {
  int master, slave, status;
  struct winsize size = {24, 80, 0, 0};
  char bytes[4096];
  ssize_t n;
  pid_t process;
  CHECK(!openpty(&master, &slave, NULL, NULL, &size));
  process = fork();
  CHECK(process >= 0);
  if (!process)
    _exit(simulate(master, slave));
  close(slave);
  /* Keep the terminal writable through every render and mode transition. */
  do {
    n = read(master, bytes, sizeof(bytes));
  } while (n > 0 || (n < 0 && errno == EINTR));
  CHECK(n == 0 || errno == EIO);
  CHECK(waitpid(process, &status, 0) == process);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  close(master);
  return 0;
}
