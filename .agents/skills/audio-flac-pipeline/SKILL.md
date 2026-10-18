---
name: audio-flac-pipeline
description: Lossless FLAC audio decoding, 48kHz LPCM resampling, byte-range streaming, and Tidal Hi-Res streaming architecture.
---

# Lossless FLAC Audio Pipeline & Resampling Guide

Este documento define la arquitectura y los procedimientos para el flujo de audio de **Monochrome** en todas sus plataformas objetivo (PS3 GameOS, PC Win32, Web).

---

## 1. Pipeline de Audio Unificado

```
[Upstream Tidal / Monochrome API]
            │
    (HTTP FLAC Stream)
            ▼
   [HTTP Stream Buffer] ── (Soporte de Range: bytes=X-Y para scrubbing inmediato)
            │
            ▼
     [dr_flac Decoder] ── (Decodificación sin pérdidas 16-bit / 24-bit Hi-Res)
            │
            ▼
  [Audio Resampler Engine] ── (Conversión a 48,000 Hz estéreo LPCM)
            │
    ┌───────┴────────┐
    ▼                ▼
[PS3 audioPort]  [Win32 WaveOut / Audio API]
 (GameOS 48kHz)       (DirectSound / WASAPI)
```

---

## 2. Especificación de Hardware de Audio en PS3

- **Formato Nativo Obligatorio**: GameOS (`audioPortOpen`) requiere estrictamente:
  - **Frecuencia de Muestreo**: `48,000 Hz` (`AUDIO_PORT_SAMPLING_RATE_48000`).
  - **Canales**: Estéreo de 2 canales (`AUDIO_PORT_CHANNEL_2`).
  - **Formato**: LPCM con muestras en punto flotante (`float32` de 32 bits normalizado entre `[-1.0, 1.0]`) o enteros de 16 bits según el puerto.
  - **Tamaño de Bloque**: 256 muestras por canal por bloque (1024 bytes por bloque estéreo).
  - **Número de Bloques en Anillo**: Mínimo 8 bloques para prevenir underflows en el buffer.

---

## 3. Resampling y Manejo de Frecuencias Nativas

La mayoría del catálogo de Tidal se distribuye en:
- `44.1 kHz` (Estándar Red Book CD de 16 bits).
- `88.2 kHz / 96 kHz` (Hi-Res Masters de 24 bits).
- `192 kHz` (Ultra Hi-Res).

### Reglas de Resampling:
1. **Ratio de Conversión**:
   $$\text{ratio} = \frac{48000.0}{\text{sampleRate}}$$
2. **Algoritmo**:
   - En plataformas limitadas (PPU/SPU de PS3): Utilizar interpolación lineal optimizada o sinc cúbico de bajo costo para no saturar los ciclos de CPU.
   - En PC / Web: Utilizar el motor nativo del navegador o resampling de alta fidelidad.
3. **Evitar Clipping**:
   - Al convertir de 24-bit a 16-bit o float, aplicar soft-clipping o normalización para prevenir distorsión armónica.

---

## 4. Streaming por Rangos HTTP (`Range: bytes`)

Para permitir que el usuario adelante o retroceda la pista en cualquier punto:
- La petición HTTP debe enviar la cabecera `Range: bytes=START-END`.
- El servidor responde con `HTTP 206 Partial Content` y `Content-Range: bytes START-END/TOTAL`.
- El decodificador `dr_flac` se inicializa mediante callbacks personalizados de I/O que solicitan bloques bajo demanda.
