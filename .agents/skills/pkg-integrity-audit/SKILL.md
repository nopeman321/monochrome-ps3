---
name: pkg-integrity-audit
description: Auditoría de integridad criptográfica y estructural de paquetes PKG de PlayStation 3 (alineación de 16 bytes, descifrado SHA-1, QA Digest y validación para PS3HEN/CFW/RPCS3).
---

# PS3 PKG Integrity & Container Audit

Esta skill proporciona los métodos de inspección, verificación y auditoría forense para paquetes `.pkg` de PlayStation 3 generados para la arquitectura Cell / GameOS.

---

## 1. Requisitos Críticos del Formato PKG de PS3

Para que un archivo `.pkg` sea aceptado por el instalador oficial de GameOS, PS3HEN o RPCS3 sin arrojar error `80029564` o `Corrupted Data`:
1. **Magic Header**: Los primeros 4 bytes deben ser `0x7F 0x50 0x4B 0x47` (`\x7fPKG`).
2. **QA Digest y Cifrado SHA-1**:
   - Los datos y nombres de archivos se cifran con un generador de flujo pseudoaleatorio basado en SHA-1 utilizando el QA Digest del encabezado.
   - **Regla Inflexible**: Los offsets de los nombres de archivo (`name_off`) deben estar alineados a bloques de **16 bytes**.
3. **Estructura Interna Mínima**:
   - `PARAM.SFO`: Parámetros del sistema con Title ID coincidente con el Content ID del PKG.
   - `USRDIR/EBOOT.BIN`: Ejecutable seguro firmado como SELF (NPDRM Free).
   - `ICON0.PNG`: Icono de 320x176 píxeles (PNG indexado o RGBA estándar).

---

## 2. Ejecución de la Auditoría Automatizada

Ejecuta el script de verificación contra el paquete compilado:

```bash
python tools/verify_pkg.py MCRM00001.pkg
```

### Interpretación de la Salida

- **Magic y Platform**:
  - `Magic: b'\x7fPKG'`, `Platform: 1` (PS3).
- **Entradas Desencriptadas**:
  - Verifica que cada entrada reporte `name_off`, `name_sz`, `f_off`, `f_sz` consistentes dentro de los límites del payload.
- **Resultado Esperado**:
  ```text
  [SUCCESS] All RPCS3 PKG checks passed 100%!
  ```

---

## 3. Resolución de Errores Comunes de Auditoría

| Código / Error | Causa | Solución |
| :--- | :--- | :--- |
| `AssertionError: name exceeds bounds` | Offset corrupto en tabla de entradas o desfase por falta de relleno (padding). | Regenerar con `tools/pkg_builder.py` que fuerza alineación a 16 bytes. |
| `RPCS3: Failed to decrypt PKG` | QA Digest inválido o clave de producto no correspondiente al tipo Debug/Retail. | Asegurar que el Content ID siga el patrón `UP0001-TITLEID_00-...` y use claves FREE NPDRM. |
| `PS3 Error 80029564` | Paquete dañado durante la transferencia FTP o fallo de hash MD5. | Transferir siempre en modo **Binary** (nunca ASCII) y verificar hash localmente antes de enviar. |
