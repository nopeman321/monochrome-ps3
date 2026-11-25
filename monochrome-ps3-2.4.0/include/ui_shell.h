#ifndef UI_SHELL_H
#define UI_SHELL_H

#include <stdint.h>
#include <stdbool.h>
#include "monochrome.h"

/* Render completo del shell: fondo + top bar + left rail + área central limpia */
void shell_render(int sw, int sh, long frame);

/* Manejo de entrada para navegación por zonas (L1/R1/D-Pad/△/□) */
void shell_handle_input(InputAction act);

/* Obtener rectángulo del área central disponible para la vista activa */
void shell_get_main_area(int *x0, int *y0, int *x1, int *y1);

/* Obtener el ancho actual del Left Rail (con animación) */
int shell_rail_width(void);

/* Estado de expansión del rail */
bool shell_rail_is_expanded(void);

#endif /* UI_SHELL_H */
