#ifndef UI_QUEUE_H
#define UI_QUEUE_H

#include <stdbool.h>
#include "monochrome.h"

#ifndef ACTION_TYPEDEF
#define ACTION_TYPEDEF
typedef InputAction Action;
#endif

void ui_queue_init(void);
void ui_queue_render(void);
bool ui_queue_handle_input(Action act);
bool ui_queue_is_open(void);
void ui_queue_set_open(bool open);

#endif /* UI_QUEUE_H */
