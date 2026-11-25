#ifndef UI_LIBRARY_H
#define UI_LIBRARY_H

#include <stdint.h>
#include <stdbool.h>
#include "monochrome.h"
#include "app_state.h"

void ui_library_init(void);
void ui_library_render(void);
void ui_library_handle_input(InputAction act);

#endif /* UI_LIBRARY_H */
