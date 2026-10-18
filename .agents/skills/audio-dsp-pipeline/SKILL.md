---
name: audio-dsp-pipeline
description: Arquitectura, decodificación FLAC/PCM, resampleo de alta fidelidad (44.1kHz a 48kHz) y gestión de buffers de audio en tiempo real para PlayStation 3 y plataformas embebidas.
---

# Audio DSP & Real-Time Streaming Pipeline

Esta skill detalla los requerimientos, algoritmos y procedimientos para el procesamiento de audio digital, streaming sin pérdida (Lossless FLAC) y sincronización de reproducción en PlayStation 3 y clientes nativos.

---

## 1. Requisitos de Hardware de Audio en PS3 (GameOS)

El subsistema de audio de PlayStation 3 (`sysAudioPortOpen`) impone restricciones estrictas:
- **Frecuencia de Muestreo Obligatoria**: **48,000 Hz (48 kHz)**. La PS3 no admite 44.1 kHz de forma nativa en su puerto de salida LPCM estándar.
- **Formato de Canales**: 2 canales (Stereo L/R interleaved).
- **Formato de Muestra**: PCM flotante de 32 bits (`float` normalizado entre `-1.0f` y `+1.0f`) o enteros con signo de 16 bits (`int16_t` Big-Endian).
- **Bloque de Salida**: Típicamente bloques fijos de 256 o 512 muestras por canal por interrupción de hardware.

---

## 2. Resampleo de Audio: 44.1 kHz -> 48.0 kHz

Dado que la mayor parte del catálogo musical (Tidal / CD Hi-Res) se encuentra a 44,100 Hz, es imperativo realizar un resampleo de alta calidad sin introducir aliasing ni artefactos audibles.

### Algoritmo de Resampleo por Interpolación Lineal / Polifásica
Relación de paso:
$$\text{ratio} = \frac{44100}{48000} = 0.91875$$

```c
// Resampleo estéreo interleaved de 44.1kHz a 48kHz
void resample_stereo_44k_to_48k(const float* in, size_t in_frames,
                                float* out, size_t out_frames) {
    double step = (double)in_frames / (double)out_frames;
    double src_pos = 0.0;

    for (size_t i = 0; i < out_frames; i++) {
        size_t idx = (size_t)src_pos;
        float frac = (float)(src_pos - idx);

        if (idx + 1 < in_frames) {
            // Canal Izquierdo
            out[i * 2 + 0] = in[idx * 2 + 0] * (1.0f - frac) + in[(idx + 1) * 2 + 0] * frac;
            // Canal Derecho
            out[i * 2 + 1] = in[idx * 2 + 1] * (1.0f - frac) + in[(idx + 1) * 2 + 1] * frac;
        } else {
            out[i * 2 + 0] = in[idx * 2 + 0];
            out[i * 2 + 1] = in[idx * 2 + 1];
        }

        src_pos += step;
    }
}
```

---

## 3. Decodificación de FLAC por Bloques

Para evitar saturar la memoria RAM de la PS3 almacenando la pista completa descomprimida:
- Utilizar `dr_flac` o decodificador basado en streams con lecturas paginadas de **4 KB a 16 KB**.
- Decodificar lotes de 1024 a 4096 muestras por iteración directamente hacia el ring buffer.
- Monitorear el nivel de llenado del buffer: si cae por debajo del 25%, aumentar la prioridad del hilo de red/decodificación para evitar el chasquido (*underrun*).

---

## 4. Control de Ganancia y Atenuación de Volumen

Aplicar multiplicación escalar o AltiVec con curva logarítmica para control perceptual suave de volumen:
```c
static inline float apply_volume(float sample, float volume_0_to_1) {
    // Curva cuadrática aproximada a escala logarítmica
    float gain = volume_0_to_1 * volume_0_to_1;
    return sample * gain;
}
```
