#ifndef UI_HOME_H
#define UI_HOME_H

#include <stdint.h>
#include <stdbool.h>

#include "monochrome.h"
#include "app_state.h"
#include "gfx_engine.h"
#include "ui_theme.h"
#include "ui_components.h"

void ui_home_init(void);
void ui_home_render(void);
void ui_home_handle_input(InputAction act);

#endif /* UI_HOME_H */
