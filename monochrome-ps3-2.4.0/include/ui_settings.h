#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include "monochrome.h"
#include "app_state.h"

void ui_settings_init(void);
void ui_settings_render(void);
void ui_settings_handle_input(InputAction act);

#endif /* UI_SETTINGS_H */
