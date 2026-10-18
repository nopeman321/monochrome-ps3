---
name: desktop-launcher
description: Windows PC standalone desktop player, Win32 launcher compilation, Edge/Chromium App Mode integration, and local streaming proxy daemon.
---

# Monochrome Windows Desktop Launcher & Proxy Architecture

Esta skill detalla la arquitectura de ejecución del cliente de escritorio para Windows, compuesto por un lanzador nativo Win32 en C y un daemon local de streaming en Python.

---

## 1. Arquitectura de Dos Capas

```
[Usuario hace clic en Monochrome.bat o monochrome_pc.exe]
                       │
                       ▼
        [bin/monochrome_pc.exe] (Nativo Win32 C99)
        - Detecta la raíz del repositorio de forma dinámica.
        - Lanza el proceso en segundo plano oculto (SW_HIDE, CREATE_NO_WINDOW).
                       │
                       ▼
         [tools/desktop_app.py] (Local Daemon & Proxy)
        - Inicia el servidor HTTP multihilo en 127.0.0.1:7890.
        - Enruta /api/search, /api/track y /api/proxy/mi.
        - Abre la ventana de escritorio nativa en modo app (Edge/Chrome).
                       │
                       ▼
    [Ventana de Aplicación Dedicada (800x500)]
    - Carga http://127.0.0.1:7890
    - Sin barra de direcciones, sin pestañas, UI pura de escritorio.
```

---

## 2. Compilación del Lanzador Nativo

Para compilar el lanzador nativo C99 y el core de audio:
```cmd
build_pc.bat
```
Genera:
- `bin/monochrome_pc.exe` (Lanzador silencioso Win32, < 150 KB).
- `bin/monochrome_c99_core.exe` (Core ejecutable de pruebas en C99).

---

## 3. Parámetros del Servidor y Proxy Local

- **Puerto**: `7890` (con reutilización de socket `SO_REUSEADDR`).
- **Proxy de Búsqueda**: `/api/search?q=...` (Inyecta User-Agent de navegador para evitar bloqueos 403 de Cloudflare).
- **Proxy de Audio**: `/api/track/<id>` (Transfiere streams FLAC con soporte de rangos `206 Partial Content` y MIME type `audio/flac`).
- **Proxy de Carátulas**: `/api/proxy/mi/<id>` (Descarga y cachea imágenes en JPEG).
- **Resolución por Defecto**: `800x500` (Modo compacto mínimo con sidebar responsivo colapsable).
