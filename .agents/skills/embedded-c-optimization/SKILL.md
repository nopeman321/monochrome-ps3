---
name: embedded-c-optimization
description: Optimización de rendimiento, gestión estricta de memoria (heap/stack), vectorización SIMD (AltiVec/SPU) y diseño de bajo consumo para C99 en el procesador Cell Broadband Engine.
---

# Embedded C Optimization for Cell Broadband Engine

Esta skill establece las técnicas de optimización y patrones de arquitectura de software en C para maximizar el rendimiento en el procesador **Cell Broadband Engine** (PPU de 64 bits con AltiVec y SPUs de 128 bits), manteniendo el consumo de memoria dentro de los límites estrictos de la PlayStation 3.

---

## 1. Gestión de Memoria y Prevención de Fragmentación

En sistemas embebidos como la PS3 (con 256 MB de RAM física y sin memoria virtual swap):
- **Cero `malloc`/`free` en el Loop Principal**: Toda reserva dinámica debe realizarse durante la fase de inicialización. En el bucle de procesamiento de audio o renderizado gráfico, la reserva dinámica produce fragmentación en el heap y retrasos imprevistos.
- **Ring Buffers y Memory Pools**:
  - Para streaming de audio o red, utilizar buffers circulares estáticos:
    ```c
    #define RING_BUFFER_SIZE (512 * 1024) // 512 KB
    typedef struct {
        uint8_t buffer[RING_BUFFER_SIZE] __attribute__((aligned(128)));
        volatile uint32_t head;
        volatile uint32_t tail;
    } AudioRingBuffer;
    ```
- **Alineación de Estructuras Críticas**:
  - Alinear siempre las estructuras pasadas por DMA o accedidas en bucles intensivos a **16 bytes** (vector SIMD) o **128 bytes** (línea de caché del Cell).

---

## 2. Vectorización SIMD (AltiVec / VMX en PPU)

El núcleo PPU cuenta con la unidad vectorial AltiVec (registros de 128 bits). Puede procesar 4 floats o cuatro enteros de 32 bits por ciclo de instrucción:

```c
#include <altivec.h>

// Suma vectorial de 4 elementos de 32 bits a la vez
void vector_add_4f(const float* a, const float* b, float* out, size_t count) {
    size_t i;
    for (i = 0; i + 4 <= count; i += 4) {
        vector float va = vec_ld(0, a + i);
        vector float vb = vec_ld(0, b + i);
        vector float vres = vec_add(va, vb);
        vec_st(vres, 0, out + i);
    }
    // Procesar remanente escalar si count no es múltiplo de 4
    for (; i < count; i++) {
        out[i] = a[i] + b[i];
    }
}
```

---

## 3. Predicción de Ramificaciones y Optimización de Bucles

- **Hints de Branching**:
  Utiliza intrínsecos de compilador para indicar al PPU las ramas más probables:
  ```c
  #define likely(x)    __builtin_expect(!!(x), 1)
  #define unlikely(x)  __builtin_expect(!!(x), 0)

  if (unlikely(packet_error)) {
      handle_packet_error();
  }
  ```
- **Loop Unrolling (Desenrollado de Bucles)**:
  Para bucles con conteo fijo de iteraciones (e.g. filtros FIR, convoluciones de audio), aplicar desenrollado de 4 u 8 pasos para saturar los pipelines de ejecución superescalar.

---

## 4. Manipulación de Datos Big-Endian Eficiente

Al desempaquetar paquetes de red o decodificar flujos de audio:
- Evitar conversiones byte a byte en bucles. Usar cargas directas de 32/64 bits con byte swapping en hardware:
  ```c
  static inline uint32_t read_u32_be(const uint8_t* ptr) {
      uint32_t val;
      memcpy(&val, ptr, sizeof(val));
      return val; // Nativo en PPU (Big-Endian)
  }

  static inline uint32_t read_u32_le(const uint8_t* ptr) {
      uint32_t val;
      memcpy(&val, ptr, sizeof(val));
      return __builtin_bswap32(val); // Invertir si la fuente es Little-Endian
  }
  ```
