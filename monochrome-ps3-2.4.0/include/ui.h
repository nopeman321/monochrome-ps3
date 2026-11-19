#ifndef MONOCHROME_UI_H
#define MONOCHROME_UI_H

#include "monochrome.h"

void ui_init(void);
void ui_render_splash(void);
AppState ui_handle_input(AppState current, InputAction act);
void ui_render(AppState state, long frame);
const char *ui_get_state_name(AppState state);
bool ui_should_exit(void);

#endif /* MONOCHROME_UI_H */
