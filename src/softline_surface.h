#ifndef SOFTLINE_SURFACE_H
#define SOFTLINE_SURFACE_H
#include <stddef.h>
/* Native terminal output state: cursor, geometry and bounded parser state.
 * Transcript cells are owned exclusively by the terminal. */
typedef struct sl_surface sl_surface_t;
sl_surface_t *sl_surface_create_validator(int fd);
sl_surface_t *sl_surface_create_native(int fd, int width, int height, int row,
                                       int col, int history_rows);
int sl_surface_is_native(const sl_surface_t *surface);
/* Apply the observed cursor delta to the producer; exclude prompt reflow
 * when the observed cursor belongs to an editor frame. */
void sl_surface_native_prompt_reflow(sl_surface_t *surface, int extra_rows);
/* Physical row change of the retained ASCII line under native width reflow. */
int sl_surface_native_reflow_rows(const sl_surface_t *surface, int width);
/* Count only viewport shifts, never transcript cells or glyph history. */
int sl_surface_native_history_rows(const sl_surface_t *surface);
int sl_surface_native_history_growth(const sl_surface_t *surface, int rows);
void sl_surface_native_history_shift(sl_surface_t *surface, int rows);
void sl_surface_native_position(const sl_surface_t *surface, int *row,
                                int *col);
int sl_surface_native_exit_advances(const sl_surface_t *surface);
int sl_surface_native_write(sl_surface_t *surface, const char *bytes,
                            size_t length, int prompt_row, int prompt_col);
int sl_surface_native_finish(sl_surface_t *surface, int prompt_row);
void sl_surface_destroy(sl_surface_t *surface);
/* Physical resize updates geometry and restores reserved margins. Prompt
 * growth reserves needed rows; a full-height live producer keeps its cursor. */
int sl_surface_resize(sl_surface_t *surface, int width, int height);
void sl_surface_geometry(const sl_surface_t *surface, int *width, int *height,
                         int *terminal_rows);
int sl_surface_native_resize_pending(const sl_surface_t *surface);
/* Validate without rendering. accepted reports the consumed prefix. */
int sl_surface_validate(sl_surface_t *surface, const char *bytes, size_t length,
                        size_t *accepted);
int sl_surface_complete(const sl_surface_t *surface);
void sl_surface_reset_partial(sl_surface_t *surface);
void sl_surface_set_osc8(sl_surface_t *surface, int enabled);
int sl_surface_hyperlink_active(const sl_surface_t *surface);
int sl_surface_matches(const sl_surface_t *surface, int width, int height);
/* While the hardware cursor belongs to the producer, writes need no cursor
 * command. Observe its position before moving away; frame commands release it.
 * Reports record endpoints; Unicode/tab glyph layout is never reconstructed.
 */
int sl_surface_native_cursor_live(const sl_surface_t *surface);
void sl_surface_native_observe(sl_surface_t *surface, int width, int rows,
                               int row, int col);
/* Record the terminal endpoint; never attribute a report to Unicode glyphs. */
void sl_surface_native_observe_write(sl_surface_t *surface, int row, int col);
void sl_surface_native_release_cursor(sl_surface_t *surface);
/* Reassert existing output margins without changing producer/parser state.
 * Homes the hardware cursor; the input owner must restore its cursor. */
int sl_surface_native_restore_region(sl_surface_t *surface);
#endif
