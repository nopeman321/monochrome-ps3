---
name: ps3-builder
description: Compila, firma ejecutables y empaqueta aplicaciones Homebrew para PlayStation 3 (Cell Broadband Engine) utilizando el toolchain abierto PSL1GHT v2 (ppu-gcc, make_fself, sfo_builder, pkg_builder).
---

# PlayStation 3 Homebrew Builder (PSL1GHT v2)

Esta skill proporciona el procedimiento paso a paso para compilar, verificar y empaquetar aplicaciones Homebrew para PlayStation 3 utilizando el toolchain abierto **PSL1GHT v2**, garantizando la compatibilidad con PS3HEN, Custom Firmware (Evilnat, Rebug) y el emulador RPCS3.

## Requisitos de Entorno

El toolchain requiere las siguientes variables de entorno configuradas:
- `PS3DEV`: Ruta base del toolchain PSL1GHT (e.g. `/opt/ps3dev` o `C:/ps3dev`).
- `PSL1GHT`: Ruta de los SDK headers y bibliotecas base (`$PS3DEV/ppu` o `$PS3DEV/lib/psl1ght`).
- `PATH`: Debe incluir `$PS3DEV/bin` y `$PS3DEV/ppu/bin` (para acceder a `ppu-gcc`, `spu-gcc`, `make_fself`, `make_package_npdrm`).

---

## Flujo Automatizado de Compilación y Empaquetado

Para ejecutar el pipeline completo de limpieza, compilación, verificación de binarios y empaquetado en un solo paso, ejecuta el script auxiliar:

```bash
bash scripts/build_and_pkg.sh
```

El script realiza de manera automática las siguientes fases:
1. **Limpieza**: Ejecuta `make clean` para purgar objetos y binarios previos.
2. **Compilación PPU**: Ejecuta `make` invocando `ppu-gcc` con las directivas para Cell (`-mcpu=cell -mtune=cell`).
3. **Verificación ELF**: Comprueba que el archivo binario `.elf` resultante exista, no esté vacío y contenga la firma ELF válida de 64 bits para PowerPC.
4. **Firmado SELF (`EBOOT.BIN`)**: Genera y firma el ejecutable seguro NPDRM/FREE (`EBOOT.BIN`) en `pkg_build/USRDIR/` mediante `make_fself` o `scetool`.
5. **Generación `PARAM.SFO`**: Valida o construye el archivo de parámetros del sistema con el Title ID (`MCRM00001`), versión y permisos requeridos por GameOS.
6. **Empaquetado PKG**: Empaqueta los directorios `USRDIR`, `ICON0.PNG`, `PIC1.PNG` y `PARAM.SFO` en un paquete `.pkg` listo para instalar.

---

## Comandos Manuales y Targets de Makefile

Si necesitas depurar o ejecutar fases individuales:

### 1. Limpieza y Compilación de Binarios
```bash
# Limpiar compilaciones anteriores
make clean

# Compilar el binario ELF
make all
```

### 2. Firma del Ejecutable Seguro (SELF)
```bash
# Mediante PSL1GHT fself
make_fself build/monochrome.elf pkg_build/USRDIR/EBOOT.BIN

# O mediante SCETool
scetool -0 SELF -1 TRUE -s TRUE -2 04 -3 1010000001000003 -4 01000002 \
        -5 NPDRM -A 0001000000000000 -6 0003004100000000 -b FREE \
        -f UP0001-MCRM00001_00-MONOCHROMEPS3001 -c EXEC -g EBOOT.BIN \
        -e build/monochrome.elf pkg_build/USRDIR/EBOOT.BIN
```

### 3. Generación de Metadatos PARAM.SFO
```bash
python3 tools/sfo_builder.py pkg_build/PARAM.SFO
```

### 4. Empaquetado Final del Archivo PKG
```bash
python3 tools/pkg_builder.py pkg_build MCRM00001.pkg
```

---

## Verificación de Integridad

Antes de transferir a una consola PlayStation 3 física o a RPCS3, valida los siguientes puntos:
- **EBOOT.BIN**: Comprueba que el ejecutable esté ubicado en `USRDIR/EBOOT.BIN` y empiece por el encabezado `SCE\0` (o `0x53 0x43 0x45 0x00`).
- **PARAM.SFO**: Debe iniciar con la firma `\0PSF` y contener las claves estándar `TITLE`, `TITLE_ID` y `CATEGORY=HG` (Harddisk Game) o `1P`.
- **Alineación de Archivos en PKG**: Asegúrate de que los nombres de archivo y streams dentro del paquete mantengan la alineación de 16 bytes requerida por el descifrador de GameOS.
