# PlayStation 3 & Cell Broadband Engine Architectural Rules (PSL1GHT v2)

Este documento define las restricciones de arquitectura, directrices de ingeniería y estándares de desarrollo para el entorno PlayStation 3 (Cell Broadband Engine / GameOS / PS3HEN / CFW) utilizando el SDK abierto **PSL1GHT v2**.

---

## 1. Toolchain y SDK Exclusivo: PSL1GHT v2

- **Compiladores Obligatorios**:
  - `ppu-gcc` (GNU C/C++ Compiler para el PowerPC Processing Unit - PPU de 64 bits con extensiones AltiVec/VMX).
  - `spu-gcc` (GNU C/C++ Compiler para los Synergistic Processing Units - SPU de 128 bits SIMD).
- **Entorno del SDK**:
  - Toda compilación debe basarse en el toolchain abierto de PSL1GHT v2 alojado bajo las rutas `$PS3DEV` y `$PSL1GHT`.
  - Librerías base permitidas: `librt.a`, `liblv2.a`, `libnet.a`, `libio.a`, `libaudio.a`, `libgcm_sys.a`, `libsysutil.a`, `libtiny3d.a`, `libmbedtls.a`, y las dependencias compiladas en `$PS3DEV/portlibs/ppu/`.
- **Prohibición de SDK Propietario**:
  - **Queda estrictamente prohibido** el uso de encabezados, llamadas o símbolos propietarios del SDK oficial de Sony SCE / SN Systems (e.g. `cellSysmodule*`, `cellGcm*`, `cellPad*`, `libsysmodule.a`).
  - Emplear exclusivamente las interfaces abiertas de PSL1GHT: `<io/pad.h>`, `<sysutil/sysutil.h>`, `<audio/audio.h>`, `<sys/spu.h>`, `<sys/dma.h>`, etc.
- **Flags de Compilación Recomendados**:
  - C99 / GNU99: `-O2 -Wall -Wextra --std=gnu99 -mcpu=cell -mtune=cell -fno-strict-aliasing`

---

## 2. Orden de Bytes (Endianness): Big-Endian Estricto

La arquitectura Cell B.E. opera de manera nativa en **Big-Endian** tanto en el núcleo PPU (PowerPC 970 modificado) como en las unidades SPU.

- **Serialización e I/O de Archivos**:
  - Todo dato binario exportado o importado debe considerar el orden Big-Endian.
  - Al procesar formatos Little-Endian (e.g., encabezados RIFF WAV, metadatos externos x86/ARM), se debe aplicar byte-swapping explícito mediante macros o intrínsecos de GCC:
    ```c
    #define SWAP16(x) __builtin_bswap16(x)
    #define SWAP32(x) __builtin_bswap32(x)
    #define SWAP64(x) __builtin_bswap64(x)
    ```
- **Red y Sockets**:
  - El formato de red (Network Byte Order) coincide con el formato nativo Big-Endian del Cell.
  - Se debe utilizar siempre `htons`, `htonl`, `ntohs`, `ntohl` para garantizar portabilidad y legibilidad de código, aunque resulten operaciones transparentes (no-op) en el PPU.
- **Intercambio de Punteros y Direcciones entre PPU y SPU**:
  - El PPU utiliza direcciones efectivas de 64 bits (`uint64_t` / `ea64_t`).
  - El SPU Local Store utiliza direcciones locales de 32 bits (`uint32_t` para LS).
  - Nunca truncar punteros de PPU a 32 bits sin comprobación de rango en el espacio de memoria efectiva.
  - Toda estructura transmitida entre PPU y SPU debe ser declarada como `packed` o con padding explícito para evitar divergencias de alineación entre compiladores PPU y SPU.

---

## 3. Presupuesto de Memoria y Alineación DMA (256 MB XDR + 256 MB GDDR3)

La PlayStation 3 posee dos dominios de memoria física independientes:
1. **256 MB XDR RAM (Main Memory)**:
   - Compartida entre el kernel de GameOS (LV2/LV1 reservan ~48–50 MB) y el espacio de usuario PPU/SPU.
   - Presupuesto máximo seguro para la aplicación homebrew: **~200 MB**.
2. **256 MB GDDR3 VRAM (RSX)**:
   - Dedicada exclusivamente al sintetizador gráfico RSX (Reality Synthesizer) para framebuffers, texturas y vertex buffers.
   - La transferencia entre XDR y VRAM requiere buffers mapeados por GCM o DMA.

### Alineación Crítica de 128 Bytes para DMA (MFC)
- El **Memory Flow Controller (MFC)** de los SPUs transfiere datos mediante líneas de caché de **128 bytes**.
- **Regla Inflexible**: Toda transferencia DMA (MFC `get` / `put`) debe tener:
  1. Dirección de memoria efectiva (EA en PPU) alineada a **128 bytes**.
  2. Dirección de Local Store (LS en SPU) alineada a **128 bytes**.
  3. Tamaño de transferencia múltiplo de **128 bytes** (para máxima velocidad y ancho de banda) o mínimo 16 bytes.
  4. Tamaño máximo por comando DMA individual: **16 KB**.
- **Directivas de Reserva de Memoria**:
  ```c
  // Reserva estática
  uint8_t dma_buffer[16384] __attribute__((aligned(128)));

  // Reserva dinámica en heap
  void* dma_ptr = memalign(128, transfer_size);
  ```
- **Límite de Local Store (SPU)**:
  - Cada SPU dispone de exactamente **256 KB** de memoria local SRAM (Local Store) compartida para código ejecutable, pila (stack), variables globales y buffers de DMA. Monitorear rigurosamente el tamaño del `.text` y `.data` de los binarios SPU.

---

## 4. Comunicación PPU - SPU: Mailboxes y MFC DMA

- **Canales de Notificación y Control (Mailboxes & Signals)**:
  - **Mailbox PPU -> SPU** (`spu_read_in_mbox`): Transmisión de comandos breves de 32 bits (inicio de tarea, pausa, códigos de control).
  - **Mailbox SPU -> PPU** (`spu_write_out_mbox`): Notificación de finalización de lote, estado de error o requerimiento de atención.
  - **Signal Registers**: Utilizar los registros de señales SPU de 32 bits para señalización asíncrona PPU->SPU sin saturar el canal de mailbox.
- **Transferencia Masiva de Datos (MFC DMA)**:
  - Para cualquier intercambio de datos superior a 4 bytes, utilizar comandos de DMA emitidos desde el SPU:
    - `mfc_get()`: Lee desde la memoria principal (PPU EA) hacia el Local Store (SPU LS).
    - `mfc_put()`: Escribe desde el Local Store (SPU LS) hacia la memoria principal (PPU EA).
  - Emplear **doble buffer** (*double-buffering*) con etiquetas DMA distintas (`tag_group`) para solapar la transferencia por bus con el procesamiento SIMD vectorial de las unidades SPU.
  - Esperar la confirmación de la transferencia mediante lectura de máscara de tags (`mfc_read_tag_status_all` o `mfc_read_tag_status_any`) antes de consumir o sobreescribir los buffers.
- **Prohibición de Busy-Waiting Bloqueante**:
  - Evitar bucles cerrados `while(1)` sin rendimiento de CPU; usar eventos de sincronización del sistema (`sys_event_queue`, semáforos de LV2 o barreras) para minimizar consumo de energía y latencia en el bus EIB (Element Interconnect Bus).
