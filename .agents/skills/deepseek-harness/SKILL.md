---
name: deepseek-harness
description: DeepSeek autonomous testing harness, unit test execution, regression suites, endpoint validation, and benchmarking for Monochrome.
---

# DeepSeek Autonomous Test Harness & Quality Assurance

Esta skill detalla los procedimientos de verificación, auditoría y pruebas automatizadas empleadas por el arnés **DeepSeek Harness** en Monochrome.

---

## 1. Suite de Verificación de Endpoints Locales

El arnés ejecuta periódicamente o tras cada cambio de código las siguientes pruebas de regresión:

1. **Prueba de Servidor Estático**:
   - `GET http://127.0.0.1:7890/index.html` -> Debe responder `200 OK`, longitud > 10 KB, `Content-Type: text/html`.
2. **Prueba de Búsqueda y Metadatos**:
   - `GET http://127.0.0.1:7890/api/search?q=Daft+Punk` -> Debe responder `200 OK`, JSON válido con lista de `tracks > 0`.
3. **Prueba de Stream de Audio Hi-Res**:
   - `GET http://127.0.0.1:7890/api/track/155102871061270528` con header `Range: bytes=0-1023` -> Debe responder `206 Partial Content`, `Content-Type: audio/flac`.

---

## 2. Suite de Verificación de Compilación y Firma

1. **Cliente PC**:
   - Ejecuta `build_pc.bat` y comprueba que `bin/monochrome_pc.exe` y `bin/monochrome_c99_core.exe` se generen sin errores ni warnings críticos.
2. **Homebrew PS3**:
   - Ejecuta `build_homebrew.bat` y comprueba:
     - Generación del ELF de PowerPC64.
     - Firma válida del archivo SELF (`SCE0` magic en `pkg_build/USRDIR/EBOOT.BIN`).
     - Construcción de `MCRM00001.pkg` (tamaño ~259 KB).
     - Validación de alineación de 16 bytes en la estructura interna del PKG.

---

## 3. Comandos de Diagnóstico Rápido

```powershell
# Ejecución de verificación integral con reporte
python -c "
import urllib.request, json
endpoints = [
  ('Static UI', 'http://127.0.0.1:7890/index.html'),
  ('Search API', 'http://127.0.0.1:7890/api/search?q=Daft+Punk'),
  ('FLAC Stream', 'http://127.0.0.1:7890/api/track/155102871061270528')
]
for name, url in endpoints:
    try:
        r = urllib.request.urlopen(url, timeout=10)
        print(f'[PASS] {name}: HTTP {r.status}')
    except Exception as e:
        print(f'[FAIL] {name}: {e}')
"
```
