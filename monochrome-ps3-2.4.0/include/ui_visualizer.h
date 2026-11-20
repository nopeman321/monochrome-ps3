#ifndef UI_VISUALIZER_H
#define UI_VISUALIZER_H

#include <stdbool.h>
#include "monochrome.h"

#ifndef ACTION_TYPEDEF
#define ACTION_TYPEDEF
typedef InputAction Action;
#endif

void ui_visualizer_init(void);
void ui_visualizer_render(void);
bool ui_visualizer_handle_input(Action act);
bool ui_visualizer_is_fullscreen(void);
void ui_visualizer_set_fullscreen(bool fullscreen);

#endif /* UI_VISUALIZER_H */
