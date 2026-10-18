---
name: modern-web-client
description: Guía de arquitectura, desarrollo y depuración para la interfaz web HTML5/ES6/CSS3 (Web Audio API, streaming, maquetación adaptativa) de Monochrome Player.
---

# Modern Web Client Architecture for Monochrome

Esta skill proporciona las directrices para el desarrollo, depuración y mantenimiento de la interfaz web de Monochrome (`web/index.html`, `web/styles.css`, `web/app.js`), coordinada con el daemon local (`tools/desktop_app.py`).

---

## 1. Estructura y Tecnologías

- **Vanilla Modern JavaScript (ES6+)**: Cero dependencias pesadas (sin React ni bundlers obligatorios) para garantizar tiempos de carga instantáneos y bajo consumo de CPU.
- **Web Audio API**: Para reproducción de flujos de audio de alta resolución con control de volumen por nodo de ganancia (`GainNode`), visualizador de espectro FFT (`AnalyserNode`) y decodificación fluida.
- **CSS3 Moderno**: CSS Grid, Flexbox y variables CSS (`--bg-color`, `--accent-color`) con soporte para temas oscuros y modos compactos de reproducción.

---

## 2. Puntos Clave de Implementación

### Inicialización Segura del AudioContext
Los navegadores modernos bloquean el `AudioContext` antes del primer gesto del usuario:
```javascript
let audioCtx = null;

function ensureAudioContext() {
    if (!audioCtx) {
        audioCtx = new (window.AudioContext || window.webkitAudioContext)({
            latencyHint: 'playback',
            sampleRate: 48000
        });
    }
    if (audioCtx.state === 'suspended') {
        audioCtx.resume();
    }
    return audioCtx;
}

document.getElementById('play-btn').addEventListener('click', () => {
    ensureAudioContext();
    // Iniciar o reanudar reproducción
});
```

### Manejo de API REST y Fallback Offline
- El cliente debe consultar las APIs de Tidal / Monochrome a través del proxy local en `http://localhost:7890/api/` o endpoints remotos.
- En caso de desconexión, mostrar el catálogo en caché local (`tools/offline_catalog.json`) sin bloquear la interfaz.

---

## 3. Pruebas y Validación

Para levantar y verificar el cliente web localmente:
```cmd
run_web.bat
```
O directamente con el servidor de desarrollo en Python:
```bash
python tools/desktop_app.py
```
Abre en el navegador: `http://localhost:7890`
