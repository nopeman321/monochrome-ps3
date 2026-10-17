# MANUAL MAESTRO DE DESARROLLO HOMEBREW PARA PLAYSTATION 3
### Arquitectura Cell B.E., GameOS LV2, Criptografía, Formato PKG y Ecosistema PS3HEN / CFW
**Autor / Compilador:** Senior Cell B.E. & GameOS Systems Engineer  
**Versión de Referencia:** 2.0 (2026 Edition)  
**Compatibilidad:** PS3HEN (v3.0.0+), Custom Firmware (Evilnat / Rebug 4.80 - 4.91), RPCS3 (v0.0.35+)

---

## ÍNDICE GENERAL
1. [Arquitectura del Hardware Cell B.E. y Modelo de Memoria](#1-arquitectura-del-hardware-cell-be-y-modelo-de-memoria)
2. [Jerarquía de Privilegios y Kernel GameOS (LV0 - LV3)](#2-jerarquía-de-privilegios-y-kernel-gameos-lv0---lv3)
3. [Convención de Llamadas LV2 y Tabla Maestra de Syscalls](#3-convención-de-llamadas-lv2-y-tabla-maestra-de-syscalls)
4. [Ecosistema Custom Firmware y PS3HEN (Syscall 8 / Cobra)](#4-ecosistema-custom-firmware-y-ps3hen-syscall-8--cobra)
5. [Formatos de Ejecutables: ELF64, Descriptores OPD y SELF (SCE0)](#5-formatos-de-ejecutables-elf64-descriptores-opd-y-self-sce0)
6. [Especificación y Criptografía del Contenedor Sony PKG](#6-especificación-y-criptografía-del-contenedor-sony-pkg)
7. [Subflujo Gráfico: RSX (Reality Synthesizer), VRAM y Framebuffers](#7-subflujo-gráfico-rsx-reality-synthesizer-vram-y-framebuffers)
8. [Subflujo de Audio: sys_audio_out y LPCM DAC DMA](#8-subflujo-de-audio-sys_audio_out-y-lpcm-dac-dma)
9. [Pila de Red: sys_net, Sockets BSD y TLS Embebido](#9-pila-de-red-sys_net-sockets-bsd-y-tls-embebido)
10. [Estructura del Sistema de Archivos y Metadatos PARAM.SFO](#10-estructura-del-sistema-de-archivos-y-metadatos-paramsfo)
11. [Matriz de Comandos SCETool y Flujo de Compilación Moderno](#11-matriz-de-comandos-scetool-y-flujo-de-compilación-moderno)

---

## 1. Arquitectura del Hardware Cell B.E. y Modelo de Memoria

### 1.1 PPE (Power Processing Element)
El procesador principal es un núcleo PowerPC de 64 bits con soporte de dos hilos simultáneos (SMT - Simultaneous Multithreading).
- **Endianness:** Big-Endian estricto. Todas las estructuras multiocteto se almacenan con el byte más significativo en la dirección más baja (`>`).
- **Juego de Instrucciones:** PowerPC 970 / Cell PPU ISA v2.02 con extensiones vectoriales VMX (AltiVec).
- **Llamadas al Sistema:** Instrucción `sc` (Syscall) con preservación estricta de registros de acuerdo a la ABI PowerPC64 v1.

### 1.2 SPE / SPU (Synergistic Processing Elements)
- Cuenta con 8 procesadores vectoriales SIMD de 128 bits:
  - 6 SPUs disponibles para juegos y aplicaciones de usuario.
  - 1 SPU reservado por GameOS para servicios de seguridad y kernel.
  - 1 SPU deshabilitado de fábrica para maximizar rendimiento de oblea en manufactura.
- **Local Store (LS):** Cada SPU tiene 256 KB de memoria estática ultrarrápida. Las SPUs no acceden directamente a la memoria principal (XDR); todo el intercambio de datos se realiza obligatoriamente mediante transferencias DMA iniciadas por el MFC (Memory Flow Controller).

### 1.3 Mapa de Memoria Física
| Banco de Memoria | Tamaño | Ancho de Banda | Latencia / Tipo | Propósito |
| :--- | :--- | :--- | :--- | :--- |
| **XDR DRAM** | 256 MB | 25.6 GB/s @ 3.2 GHz | Principal (Rambus) | Código PPE, SPU stacks, buffers dinámicos, audio |
| **GDDR3 VRAM** | 256 MB | 20.8 GB/s @ 650 MHz | GPU (RSX) | Framebuffers, texturas, shaders, command buffers |

### 1.4 Barreras de Memoria de Hardware (PPU)
Debido a la ejecución fuera de orden y la coexistencia de hilos PPU/SPU:
- `sync`: Barrera pesada. Detiene el pipeline hasta que todas las cargas y almacenes previos se hayan completado en el bus del procesador.
- `lwsync` (Lightweight Sync): Garantiza orden entre cargas y almacenes entre sí sin vaciar el pipeline de instrucciones. Esencial en búferes circulares de audio lock-free.
- `eieio` (Enforce In-order Execution of I/O): Asegura el orden estricto de escrituras hacia dispositivos de memoria mapeada (MMIO / RSX).

---

## 2. Jerarquía de Privilegios y Kernel GameOS (LV0 - LV3)

El software de seguridad de PlayStation 3 está estructurado en 4 niveles de aislamiento por hardware:

```
┌─────────────────────────────────────────────────────────────┐
│ NIVEL 3 (LV3): Modo Usuario (Game, Homebrew, VSH/XMB)       │
├─────────────────────────────────────────────────────────────┤
│ NIVEL 2 (LV2): Kernel GameOS (Procesos, Hilos, FS, Red, RSX)│
├─────────────────────────────────────────────────────────────┤
│ NIVEL 1 (LV1): Hypervisor (Aislamiento de hardware, LPARs)  │
├─────────────────────────────────────────────────────────────┤
│ NIVEL 0 (LV0): Boot ROM y Criptografía Base (Aislado)        │
└─────────────────────────────────────────────────────────────┘
```

1. **LV0:** Cargador base grabado en silicio. Desencripta y verifica la firma de LV1.
2. **LV1 (Hypervisor):** Gestiona las particiones lógicas (LPAR). Aísla el kernel LV2 y controla el acceso a controladores de hardware protegidos.
3. **LV2 (GameOS Kernel):** El sistema operativo principal. Provee la tabla de llamadas al sistema (syscalls) para hilos, gestión de memoria virtual (`sys_memory`), sistema de archivos (`sys_fs`), red (`sys_net`), audio (`sys_audio`) y acceso al chip gráfico (`sys_rsx`).
4. **LV3 (User Mode):** Espacio donde se ejecutan los juegos comerciales, el VSH (Virtual Shell / menú XMB) y las aplicaciones Homebrew.

---

## 3. Convención de Llamadas LV2 y Tabla Maestra de Syscalls

### 3.1 Convención de Registro para Syscalls PPU (ABI PowerPC64)
Para invocar una llamada al sistema en GameOS:
- **`r11`:** Número / código de la syscall (Syscall ID).
- **`r3`:** Argumento 1 / Retorno de la syscall (0 = `CELL_OK`, negativo = código de error).
- **`r4` - `r10`:** Argumentos 2 a 8.
- **`r1`:** Puntero de Pila (Stack Pointer), debe estar alineado a 16 bytes.
- **`r2`:** Puntero a la Tabla de Contenidos (TOC Pointer). **No debe ser destruido por la syscall.**

```assembly
/* Ejemplo de invocación en ensamblador inline PowerPC64 */
li   %r11, 403           /* SYSCALL_TTY_WRITE */
mr   %r3, %r4            /* r3 = canal/descriptor */
mr   %r4, %r5            /* r4 = puntero al buffer */
mr   %r5, %r6            /* r5 = longitud en bytes */
mr   %r6, %r7            /* r6 = puntero a bytes escritos */
sc                       /* Trap al kernel LV2 */
```

### 3.2 Tabla Maestra de Syscalls LV2
| ID | Nombre Simbólico | Parámetros Principales | Descripción |
| :---: | :--- | :--- | :--- |
| **1** | `sys_process_getpid` | `()` | Obtiene el Process ID del proceso actual |
| **3** | `sys_process_exit` | `(s32 status)` | Termina limpiamente el proceso |
| **19** | `sys_process_kill` | `(sys_pid_t pid)` | Fuerza la detención de un proceso |
| **41** | `sys_ppu_thread_exit` | `(u64 val)` | Termina el hilo PPU en ejecución |
| **43** | `sys_ppu_thread_yield` | `()` | Cede el tiempo de CPU a otro hilo |
| **44** | `sys_ppu_thread_join` | `(sys_ppu_thread_t id, u64 *val)` | Espera la terminación de un hilo |
| **47** | `sys_ppu_thread_set_priority` | `(sys_ppu_thread_t id, s32 prio)` | Ajusta prioridad de hilo (-1024 a 3071) |
| **52** | `sys_ppu_thread_create` | `(id*, entry, arg, prio, stack, flags, name)` | Crea un nuevo hilo PPU |
| **100** | `sys_mutex_create` | `(mutex_id*, attr*)` | Inicializa un mutex de exclusión mutua |
| **102** | `sys_mutex_lock` | `(mutex_id, timeout)` | Adquiere el bloqueo del mutex |
| **104** | `sys_mutex_unlock` | `(mutex_id)` | Libera el bloqueo del mutex |
| **141** | `sys_timer_usleep` | `(usecond_t sleep_time)` | Pausa el hilo en microsegundos |
| **142** | `sys_timer_sleep` | `(second_t sleep_time)` | Pausa el hilo en segundos |
| **145** | `sys_time_get_current_time` | `(sec*, nsec*)` | Devuelve la hora UTC del reloj en tiempo real |
| **147** | `sys_time_get_timebase_frequency` | `()` | Frecuencia del contador base (~79.8 MHz) |
| **169** | `sys_spu_initialize` | `(max_usable, max_raw)` | Inicializa el subsistema SPU |
| **170** | `sys_spu_thread_group_create`| `(group_id*, name, num, prio, attr*)` | Crea un grupo de hilos SPU |
| **324** | `sys_memory_container_create`| `(container_id*, yield_size)` | Reserva un contenedor de memoria física |
| **330** | `sys_mmapper_allocate_address`| `(size, flags, alignment, alloc_addr*)`| Reserva rango en el mapa de memoria virtual |
| **348** | `sys_memory_allocate` | `(size, flags, alloc_addr*)` | Reserva un bloque de memoria física XDR |
| **349** | `sys_memory_free` | `(start_addr)` | Libera un bloque de memoria física |
| **392** | `sys_game_process_exitspawn` | `(path, argv, envp, data, size, prio, flags)` | Lanza otro juego o homebrew y finaliza |
| **402** | `sys_tty_read` | `(ch, buf, len, read_len*)` | Lee desde la consola TTY de depuración |
| **403** | `sys_tty_write` | `(ch, buf, len, written_len*)` | Escribe caracteres al log TTY de GameOS |
| **461** | `sys_prx_get_module_id_by_address` | `(addr)` | Resuelve el módulo PRX en una dirección |
| **480** | `sys_prx_load_module` | `(path, flags, opt*)` | Carga un módulo PRX dinámico en memoria |
| **481** | `sys_prx_start_module` | `(id, args, argp, result*, flags, opt*)` | Ejecuta la rutina de inicio de un PRX |
| **600** | `sys_storage_open` | `(device_id, unk, fd*, flags)` | Abre un dispositivo de bloque de almacenamiento |
| **602** | `sys_storage_read` | `(fd, unk, start_sector, sectors, buf*, ...)` | Lee sectores crudos de HDD/USB/BD |
| **666** | `sys_rsx_device_open` | `(device_id*, unk)` | Inicializa el hardware acelerador RSX |
| **668** | `sys_rsx_memory_allocate` | `(mem_id*, type, size, flags, ...)` | Reserva memoria VRAM para la GPU |
| **670** | `sys_rsx_context_allocate` | `(context_id*, device_id, ...)` | Inicializa el contexto de renderizado RSX |
| **672** | `sys_rsx_context_iomap` | `(context_id, bus_addr, gpu_addr, size, flags)`| Mapea memoria XDR/VRAM al bus RSX |

---

## 4. Ecosistema Custom Firmware y PS3HEN (Syscall 8 / Cobra)

Tanto **CFW (Evilnat, Rebug)** como **PS3HEN (Homebrew Enabler)** incorporan el motor **Cobra Payload**, el cual extiende el kernel de GameOS con llamadas propietarias para desbloquear el sistema.

### 4.1 La Puerta de Entrada: Syscall 8 (Cobra Interface)
La syscall 8 es una pasarela polimórfica que recibe un sub-código de operación en `r3` y parámetros auxiliares:

```c
/* Definiciones fundamentales de Syscall 8 (Cobra/MAMBA/HEN) */
#define SYSCALL8_OPCODE_GET_VERSION             0x7000 /* Retorna versión de Cobra (ej. 0x0850 = 8.5) */
#define SYSCALL8_OPCODE_GET_MAMBA               0x7002 /* Retorna si el payload es MAMBA */
#define SYSCALL8_OPCODE_POKE_LV2                0x7003 /* Escribe memoria en el espacio de kernel LV2 */
#define SYSCALL8_OPCODE_POKE_LV1                0x7005 /* Escribe memoria en el espacio de Hypervisor LV1 */
#define SYSCALL8_OPCODE_MAP_PATHS               0x7964 /* Remapea una ruta (ej. redirigir /dev_bdvd) */
#define SYSCALL8_OPCODE_LOAD_VSH_PLUGIN         0x1EE7 /* Carga un plugin SPRX en el proceso VSH */
#define SYSCALL8_OPCODE_UNLOAD_VSH_PLUGIN       0x3EE7 /* Descarga un plugin SPRX del VSH */
#define SYSCALL8_OPCODE_PS3MAPI                 0x7777 /* API de control de procesos y memoria remota */
#define SYSCALL8_OPCODE_DISABLE_COBRA           0x0000 /* Deshabilita temporalmente Syscall 8 para sigilo */
```

### 4.2 Diferencias Clave entre CFW y PS3HEN
| Característica | Custom Firmware (CFW) | PS3HEN (Homebrew Enabler) |
| :--- | :--- | :--- |
| **Consolas Soportadas** | Modelos FAT y Slim compatibles (hasta 2.50 de fábrica) | **100% de consolas** (FAT, Slim y SuperSlim 3000/4000) |
| **Modo de Instalación** | Grabación en memoria Flash NOR/NAND persistente | En memoria RAM tras cada encendido mediante exploit WebKit |
| **Acceso a LV1 / LV2** | Completo y permanente desde el arranque | Parcial/Hookeado dinámicamente en RAM |
| **Ejecución de EBOOTs** | Cualquier formato (Debug, Retail, Free NPDRM, FSELF) | **Estricto:** Free NPDRM SELF o firmado para 4.80+ |
| **Partición `/dev_blind`** | Montable con lectura/escritura nativa | Montable tras parchear permisos de kernel |

---

## 5. Formatos de Ejecutables: ELF64, Descriptores OPD y SELF (SCE0)

### 5.1 ELF64 y la Estructura OPD (Official Procedure Descriptor)
En la especificación de 64 bits de PowerPC (ABI v1), los punteros a función no apuntan al código de máquina, sino a una estructura descriptora de 24 bytes (o 16 bytes reducida):

```c
struct opd32 {
    uint32_t func_address;  /* Dirección absoluta de la primera instrucción */
    uint32_t toc_address;   /* Dirección base de la Tabla de Contenidos (TOC/r2) */
    uint32_t env_address;   /* Contexto de entorno (opcional, habitualmente 0) */
};
```

> [!CRITICAL]
> Si el descriptor OPD en la sección `.opd` contiene punteros absolutos de 64 bits pero el cargador de GameOS o RPCS3 lee campos de 32 bits, el registro `r2` (TOC) se desalineará, provocando un fallo inmediato (`Illegal Instruction` o `Memory Fault`).

### 5.2 Estructura del Contenedor SELF (Signed ELF)
Todo ejecutable que corre en el PPU de PS3 debe estar encapsulado en un archivo `EBOOT.BIN` o `.self`:

```
┌────────────────────────────────────────────────────────┐
│ Encabezado SELF (Magic: "SCE\0", Versión, Atributos)   │
├────────────────────────────────────────────────────────┤
│ Metadatos de Secciones (Compresión, Cifrado, Tamaños)  │
├────────────────────────────────────────────────────────┤
│ Encabezado NPDRM (Content-ID, Categoría, DRM Flag)     │
├────────────────────────────────────────────────────────┤
│ Secciones ELF Cifradas / Comprimidas (zlib / NPDRM)    │
├────────────────────────────────────────────────────────┤
│ Firmas Digitales ECDSA y Certificados de Integridad    │
└────────────────────────────────────────────────────────┘
```

- **Tipo FREE NPDRM:** Es el estándar universal para Homebrew. Utiliza una licencia nula (`KLicensee = 0`) y no requiere archivos de activación `.rap` en el disco duro.

---

## 6. Especificación y Criptografía del Contenedor Sony PKG

### 6.1 Encabezado del Paquete (0x80 bytes, Big Endian)
| Offset | Tamaño | Campo | Valor Típico / Homebrew |
| :---: | :---: | :--- | :--- |
| `0x00` | 4 B | `pkg_magic` | `0x7F504B47` (`\x7fPKG`) |
| `0x04` | 2 B | `pkg_type` | `0x0000` (Debug) o `0x0001` (Retail) |
| `0x06` | 2 B | `pkg_platform` | `0x0001` (PS3) |
| `0x08` | 4 B | `meta_offset` | `0x00000080` (offset a metadatos) |
| `0x0C` | 4 B | `meta_count` | `0x00000000` (0 en paquetes simples) |
| `0x10` | 4 B | `meta_size` | `0x00000080` |
| `0x14` | 4 B | `item_count` | Número total de archivos y directorios |
| `0x18` | 8 B | `total_pkg_size` | Tamaño final del `.pkg` incluyendo pie |
| `0x20` | 8 B | `data_offset` | Offset de inicio del payload (`0x00000080`) |
| `0x28` | 8 B | `payload_size` | Longitud total del payload en bytes |
| `0x30` | 48 B | `content_id` | 36 bytes ASCII + 12 bytes padding null |
| `0x60` | 16 B | `qa_digest` | SHA-1 truncado derivado del Content-ID |
| `0x70` | 16 B | `klicensee` | Clave de licencia (16 ceros en Free NPDRM) |

### 6.2 Cifrado Keystream SHA-1 Sony Debug (Crucial para RPCS3)
Para paquetes Homebrew Debug (`pkg_type == 0x0000`), el payload completo **debe cifrarse** con el keystream stream cipher de Sony:

$$\text{Bloque Input}_i = \Big[ \text{qa}[0],\, \text{qa}[0],\, \text{qa}[1],\, \text{qa}[1],\, 0,\, 0,\, 0,\, i \Big] \quad (8 \text{ enteros Big-Endian de 64 bits = 64 bytes})$$
$$\text{Keystream}_i = \text{SHA1}(\text{Bloque Input}_i)[0..15]$$
$$\text{Ciphertext}[16i \dots 16i+15] = \text{Plaintext}[16i \dots 16i+15] \oplus \text{Keystream}_i$$

> [!WARNING]
> Si el payload se escribe en texto plano sin aplicar este XOR, RPCS3 intentará descifrarlo en su rutina `package_reader::decrypt()`, corrompiendo la tabla de nombres de archivo y abortando con `PKG decryption failed!`.

### 6.3 Formato de Entradas `PKGEntry` (32 bytes por entrada)
```c
struct PKGEntry {
    uint32_t name_offset;  /* Offset relativo a data_offset */
    uint32_t name_size;    /* Longitud de la cadena incluyendo \0 */
    uint64_t file_offset;  /* Offset relativo a data_offset (0 si es directorio) */
    uint64_t file_size;    /* Tamaño en bytes (0 si es directorio) */
    uint32_t type;         /* Tipo | 0x80000000 (FLAG_OVERWRITE) */
    uint32_t pad;          /* Relleno estricto de ceros */
};
```
- **Tipos de entrada válidos:**
  - `0x80000001`: Ejecutable NPDRM SELF (`EBOOT.BIN`).
  - `0x80000003`: Archivo regular de datos (PNG, SFO, certificados).
  - `0x80000004`: Carpeta / Directorio contenedor.

---

## 7. Subflujo Gráfico: RSX (Reality Synthesizer), VRAM y Framebuffers

El procesador gráfico de PS3 es el **NVIDIA RSX** (arquitectura basada en NV40/G70 a 550 MHz).

### 7.1 Geometría Estándar de Video
- **Resolución HD:** 1280×720 píxeles @ 60 Hz progresivo.
- **Formato de Color:** ARGB8888 (32 bits por píxel = 4 bytes).
- **Pitch / Stride:** $1280 \times 4 = 5120$ bytes por línea de exploración.
- **Tamaño de Framebuffer:** $1280 \times 720 \times 4 = 3,686,400$ bytes (~3.51 MB).

### 7.2 Double-Buffering y Sincronización Vertical (VSync)
Para evitar el parpadeo y desgarro de pantalla (*screen tearing*):
1. Se reservan 2 framebuffers en memoria de video: `Buffer 0` y `Buffer 1`.
2. La CPU (PPU) dibuja en el búfer trasero (*back buffer*).
3. Se invoca el intercambio de página (*flip*) sincronizado con la señal de retorno vertical del display (VBLANK).

---

## 8. Subflujo de Audio: sys_audio_out y LPCM DAC DMA

El procesador de sonido de GameOS gestiona el hardware de salida mediante canales DMA hacia el chip HDMI/Óptico.

### 8.1 Configuración Estándar Homebrew
```c
/* Estructura de inicialización de sys_audio_out */
typedef struct {
    uint32_t num_channels;  /* 2 canales (estéreo) */
    uint32_t sample_rate;   /* 48000 Hz (estándar nativo de PS3) */
    uint32_t format;        /* AUDIO_OUT_FORMAT_16BIT_BE */
} sys_audio_out_config_t;
```

### 8.2 Parámetros del Bloque de Reproducción
- **Tamaño de Bloque:** 256 muestras por frame.
- **Bytes por Muestra Estéreo:** 2 canales × 2 bytes (16 bits) = 4 bytes por frame.
- **Bytes por Bloque:** $256 \times 4 = 1,024$ bytes.
- **Frecuencia de Bloques:** A 48 kHz, se consumen exactamente $\frac{48000}{256} = 187.5$ bloques de 1024 bytes por segundo.

---

## 9. Pila de Red: sys_net, Sockets BSD y TLS Embebido

A diferencia de los sistemas operativos estándar de PC, GameOS no inicializa sockets automáticamente:

### 9.1 Inicialización de Memoria de Red
Antes de llamar a cualquier función de sockets, se debe reservar un bloque de memoria para los buffers del kernel:
```c
#define SYS_NET_MEMORY_SIZE (512 * 1024) /* 512 KB para pila TCP/IP */
static uint8_t net_memory_pool[SYS_NET_MEMORY_SIZE] __attribute__((aligned(64)));

sys_net_initialize_parameter_t param;
param.memory = (uint32_t)(uintptr_t)net_memory_pool;
param.memory_size = SYS_NET_MEMORY_SIZE;
param.flags = 0;
sys_net_initialize_network_ex(&param);
```

### 9.2 Seguridad HTTPS / TLS
GameOS no expone una API nativa abierta para certificados de terceros en homebrew. Se utiliza un motor TLS ligero embebido (como mbedTLS o wolfSSL) cargando el archivo de certificados raíz `cacert.pem` ubicado en `USRDIR/certs/`.

---

## 10. Estructura del Sistema de Archivos y Metadatos PARAM.SFO

### 10.1 Jerarquía Estándar de Instalación
```
/dev_hdd0/game/MCRM00001/
├── ICON0.PNG             (Icono XMB 320x176 o 500x500 PNG)
├── PIC1.PNG              (Fondo de pantalla XMB 1920x1080 PNG)
├── PARAM.SFO             (Metadatos de la aplicación de sistema)
└── USRDIR/
    ├── EBOOT.BIN         (Ejecutable Free NPDRM firmado por SCETool)
    └── certs/
        └── cacert.pem    (Bundle de certificados SSL/TLS)
```

### 10.2 Parámetros Clave en PARAM.SFO
| Clave | Tipo | Valor Recomendado | Significado |
| :--- | :---: | :--- | :--- |
| `TITLE` | String | `Monochrome PS3` | Nombre visible en el carrusel de XMB |
| `TITLE_ID` | String | `MCRM00001` | Código de producto de 9 caracteres alfanuméricos |
| `CATEGORY` | String | `CB` o `HG` | `CB` = Homebrew / `HG` = Harddisk Game |
| `APP_VER` | String | `01.00` | Versión del paquete instalable |
| `PS3_SYSTEM_VER` | String | `04.8000` | Versión mínima de firmware requerida |

---

## 11. Matriz de Comandos SCETool y Flujo de Compilación Moderno

### 11.1 Firma Universal Free NPDRM con SCETool
Para generar un `EBOOT.BIN` que inicie tanto en RPCS3 como en consolas físicas con PS3HEN o CFW sin necesidad de claves `.rap`:

```cmd
tools\scetool\scetool.exe ^
  -0 SELF ^
  -1 TRUE ^
  -s TRUE ^
  -2 04 ^
  -3 1010000001000003 ^
  -4 01000002 ^
  -5 NPDRM ^
  -A 0001000000000000 ^
  -6 0003004100000000 ^
  -b FREE ^
  -f UP0001-MCRM00001_00-MONOCHROMEPS3001 ^
  -c EXEC ^
  -g EBOOT.BIN ^
  -e build\EBOOT.ELF ^
  pkg_build\USRDIR\EBOOT.BIN
```

### 11.2 Desglose de Parámetros de Seguridad SCETool
- `-0 SELF`: Formato de salida firmado.
- `-1 TRUE`: Habilita compresión zlib de las secciones de código.
- `-s TRUE`: Habilita firmas criptográficas de integridad.
- `-2 04`: Versión de clave de aplicación para firmware 4.xx.
- `-3 1010000001000003`: Tipo de aplicación GameOS (Ejecutable de usuario).
- `-5 NPDRM`: Tipo de cifrado Sony Network Platform DRM.
- `-b FREE`: Bandera que omite la verificación de licencia comercial `.rap`.
- `-f <Content-ID>`: Identificador único del paquete de 36 caracteres.
- `-c EXEC`: Tipo de contenido (ejecutable).
