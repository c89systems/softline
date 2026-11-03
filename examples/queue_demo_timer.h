#ifndef SOFTLINE_QUEUE_DEMO_TIMER_H
#define SOFTLINE_QUEUE_DEMO_TIMER_H
/* Application-owned periodic descriptor; Softline only watches its readiness.
 * The caller removes its watch and closes the descriptor at teardown. */
int demo_timer_create(unsigned int interval_ms);
int demo_timer_drain(int fd);
#endif
