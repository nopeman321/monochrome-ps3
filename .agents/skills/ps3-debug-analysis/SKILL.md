---
name: ps3-debug-analysis
description: Diagnostica, depura y analiza errores en tiempo de ejecución, registros de RPCS3, volcados TTY, fallos de syscalls LV2 y excepciones de arquitectura en aplicaciones Homebrew de PS3.
---

# PS3 Homebrew Debugging & Log Analysis

Esta skill proporciona procedimientos sistemáticos para depurar fallos, congelamientos (hangs) y excepciones en aplicaciones Homebrew para PlayStation 3 tanto en el emulador RPCS3 como en hardware físico (PS3HEN / CFW con debugger ProDG o webMAN MOD).

## Fuentes Principales de Diagnóstico

1. **RPCS3 TTY / Standard Out**:
   - Salida del syscall `sys_tty_write` (descriptor de archivo 1 o 2).
   - En RPCS3: Ver en ventana Log o inspeccionar `emulator/rpcs3/RPCS3.log`.
2. **Registro de Syscalls LV2**:
   - Comprueba valores de retorno en funciones del sistema GameOS. Los códigos de error en LV2 son siempre números negativos o valores hexadecimales `0x8001xxxx` (e.g. `CELL_OK = 0`, `CELL_EINVAL = 0x80010002`).
3. **PPU Thread Exceptions**:
   - `Trap (0x700)`: Desreferencia de puntero nulo, alineación errónea de memoria o división por cero.
   - `DSI (Data Storage Interrupt)`: Acceso a dirección fuera de rango mapeado en memoria efectiva (XDR).

---

## Procedimiento de Depuración con RPCS3

### 1. Ejecución con Captura de Logs
Utiliza el script de configuración y ejecución de RPCS3:

```cmd
run_rpcs3.bat
```
O directamente vía Python para capturar stdout/stderr:
```bash
python tools/debug_rpcs3.py
```

### 2. Patrones Comunes de Fallos y Resolución

| Síntoma en Log | Causa Raíz | Solución |
| :--- | :--- | :--- |
| `F {PPU[0x...]} MEM: Access violation reading location 0x0` | Puntero nulo en PPU o estructura no inicializada. | Revisar llamadas a `malloc` / inicializaciones antes de uso. |
| `E {PPU[0x...]} 'sys_audio_port_open' failed with 0x80010005` | El puerto de audio de GameOS ya estaba abierto o parámetros inválidos. | Comprobar que `audioPortClose` se invoque antes o verificar frecuencia (48kHz estricta). |
| `E {RSX} FIFO queue stalled` | Buffer de comandos de RSX sobrellenado o desalineado. | Alinear punteros de vértices y llamadas GCM a múltiplos de 16/128 bytes. |
| `U {PPU[0x...]} 'sys_net_socket' failed with 0x80010016` | La pila de red (`netInit`) no fue inicializada antes de crear sockets. | Asegurar que `netInitialize()` o `netInit()` se ejecute al arranque de la app. |
| `E {MFC} DMA unaligned transfer (LS: 0x..., EA: 0x...)` | Puntero o tamaño de transferencia DMA no alineado a 16 o 128 bytes. | Utilizar `memalign(128, size)` y asegurar que la longitud sea múltiplo de 128 bytes. |

---

## Técnicas de Instrumentación en Código C (PSL1GHT)

### Inyección de Mensajes TTY
Utiliza la macro estándar o escritura directa por consola:
```c
#include <stdio.h>
#include <unistd.h>

#define PS3_LOG(fmt, ...) \
    do { \
        printf("[PS3_APP][%s:%d] " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__); \
        fflush(stdout); \
    } while(0)
```

### Inspección de Código de Error LV2
```c
int32_t ret = sysAudioPortOpen(&params, &port_num);
if (ret != 0) {
    PS3_LOG("sysAudioPortOpen FAILED: 0x%08X", (unsigned int)ret);
}
```
