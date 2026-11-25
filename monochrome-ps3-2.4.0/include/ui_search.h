#ifndef UI_SEARCH_H
#define UI_SEARCH_H

#include <stdint.h>
#include <stdbool.h>
#include "monochrome.h"
#include "app_state.h"

void ui_search_init(void);
void ui_search_update(void);
void ui_search_render(void);
void ui_search_handle_input(InputAction act);
bool ui_search_is_keyboard_open(void);
void ui_search_start_initial(const char *query);

#endif /* UI_SEARCH_H */
