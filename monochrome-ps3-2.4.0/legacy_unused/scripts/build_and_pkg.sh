#!/usr/bin/env bash
# ==============================================================================
# PS3 Homebrew Build & Packaging Script for PSL1GHT v2 Toolchain
# Target: PlayStation 3 (Cell Broadband Engine / GameOS / CFW / PS3HEN)
# ==============================================================================

set -euo pipefail

# Resaltado de terminal
COLOR_RESET="\033[0m"
COLOR_GREEN="\033[1;32m"
COLOR_YELLOW="\033[1;33m"
COLOR_CYAN="\033[1;36m"
COLOR_RED="\033[1;31m"

log_info()    { echo -e "${COLOR_CYAN}[INFO]${COLOR_RESET} $1"; }
log_success() { echo -e "${COLOR_GREEN}[OK]${COLOR_RESET} $1"; }
log_warn()    { echo -e "${COLOR_YELLOW}[WARN]${COLOR_RESET} $1"; }
log_error()   { echo -e "${COLOR_RED}[ERROR]${COLOR_RESET} $1" >&2; }

# Determinar directorio raíz del proyecto
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

echo "=========================================================="
echo "  PlayStation 3 Homebrew Build & Packaging Pipeline"
echo "  Toolchain: PSL1GHT v2 (ppu-gcc / make_fself / PKG)"
echo "=========================================================="

# 1. Comprobación del entorno PSL1GHT v2
log_info "Verificando variables de entorno del SDK..."
if [ -n "${PS3DEV:-}" ]; then
    log_info "PS3DEV detectado: ${PS3DEV}"
else
    log_warn "PS3DEV no está definido en el entorno actual. Se usará el PATH del sistema o configuración local."
fi

if [ -n "${PSL1GHT:-}" ]; then
    log_info "PSL1GHT detectado: ${PSL1GHT}"
else
    log_warn "PSL1GHT no está definido. Si usas WSL o toolchain emulado/local, asegúrate de exportarlo."
fi

# 2. Limpieza y Compilación: make clean && make
log_info "Ejecutando 'make clean'..."
make clean || {
    log_warn "No se pudo ejecutar 'make clean' limpiamente; continuando..."
}

log_info "Compilando proyecto con 'make'..."
make all || make || {
    log_error "Fallo en la fase de compilación ('make'). Abortando."
    exit 1
}
log_success "Fase de compilación finalizada con éxito."

# 3. Verificación del ejecutable ELF
log_info "Localizando y verificando binario .elf..."
ELF_FILE=""
CANDIDATE_ELFS=(
    "${PROJECT_ROOT}/build/monochrome.elf"
    "${PROJECT_ROOT}/build/EBOOT.ELF"
    "${PROJECT_ROOT}/build/app.elf"
    "${PROJECT_ROOT}/monochrome.elf"
)

for f in "${CANDIDATE_ELFS[@]}"; do
    if [ -f "$f" ]; then
        ELF_FILE="$f"
        break
    fi
done

if [ -z "${ELF_FILE}" ]; then
    ELF_FILE="$(find "${PROJECT_ROOT}/build" -maxdepth 2 -name "*.elf" 2>/dev/null | head -n 1 || true)"
fi

if [ -z "${ELF_FILE}" ] || [ ! -f "${ELF_FILE}" ]; then
    log_error "No se encontró ningún binario .elf generado en build/."
    exit 1
fi

ELF_SIZE=$(stat -c%s "${ELF_FILE}" 2>/dev/null || stat -f%z "${ELF_FILE}" 2>/dev/null || wc -c < "${ELF_FILE}")
if [ "${ELF_SIZE}" -le 0 ]; then
    log_error "El archivo ELF está vacío (${ELF_FILE})."
    exit 1
fi

# Validar cabecera ELF (0x7F 'E' 'L' 'F')
ELF_MAGIC="$(head -c 4 "${ELF_FILE}" 2>/dev/null | tr -d '\0' || true)"
if [[ "$(head -c 4 "${ELF_FILE}" | od -An -tx1 | tr -d ' ')" =~ ^7f454c46 ]]; then
    log_success "ELF válido verificado: ${ELF_FILE} (${ELF_SIZE} bytes, Magic: 0x7F 'ELF')"
else
    log_warn "Cabecera ELF no estándar en: ${ELF_FILE}, procediendo con precaución..."
fi

# 4. Verificación y/o Generación del SELF (EBOOT.BIN)
log_info "Comprobando y firmando ejecutable seguro SELF (EBOOT.BIN)..."
PKG_BUILD_DIR="${PROJECT_ROOT}/pkg_build"
USRDIR="${PKG_BUILD_DIR}/USRDIR"
EBOOT_FILE="${USRDIR}/EBOOT.BIN"
mkdir -p "${USRDIR}"

if make -n eboot >/dev/null 2>&1; then
    log_info "Ejecutando regla 'make eboot'..."
    make eboot
elif [ ! -f "${EBOOT_FILE}" ] || [ "${ELF_FILE}" -nt "${EBOOT_FILE}" ]; then
    if command -v make_fself >/dev/null 2>&1; then
        log_info "Generando EBOOT.BIN mediante make_fself..."
        make_fself "${ELF_FILE}" "${EBOOT_FILE}"
    elif command -v make_self >/dev/null 2>&1; then
        log_info "Generando EBOOT.BIN mediante make_self..."
        make_self "${ELF_FILE}" "${EBOOT_FILE}"
    elif [ -f "${PROJECT_ROOT}/tools/scetool/scetool.exe" ] || command -v scetool >/dev/null 2>&1; then
        SCETOOL_BIN="${PROJECT_ROOT}/tools/scetool/scetool.exe"
        [ -x "${SCETOOL_BIN}" ] || SCETOOL_BIN="scetool"
        log_info "Generando EBOOT.BIN firmado con SCETool..."
        "${SCETOOL_BIN}" -0 SELF -1 TRUE -s TRUE -2 04 -3 1010000001000003 -4 01000002 -5 NPDRM \
            -A 0001000000000000 -6 0003004100000000 -b FREE -f UP0001-MCRM00001_00-MONOCHROMEPS3001 \
            -c EXEC -g EBOOT.BIN -e "${ELF_FILE}" "${EBOOT_FILE}"
    elif [ -f "${PROJECT_ROOT}/tools/make_fself_stub.py" ]; then
        log_info "Utilizando make_fself_stub.py..."
        python3 "${PROJECT_ROOT}/tools/make_fself_stub.py" "${ELF_FILE}" "${EBOOT_FILE}" || python "${PROJECT_ROOT}/tools/make_fself_stub.py" "${ELF_FILE}" "${EBOOT_FILE}"
    fi
fi

if [ ! -f "${EBOOT_FILE}" ]; then
    log_error "No se pudo generar ni localizar EBOOT.BIN en ${EBOOT_FILE}."
    exit 1
fi

EBOOT_SIZE=$(stat -c%s "${EBOOT_FILE}" 2>/dev/null || stat -f%z "${EBOOT_FILE}" 2>/dev/null || wc -c < "${EBOOT_FILE}")
if [ "${EBOOT_SIZE}" -le 0 ]; then
    log_error "El archivo EBOOT.BIN está vacío."
    exit 1
fi
log_success "SELF verificado: ${EBOOT_FILE} (${EBOOT_SIZE} bytes)"

# 5. Generación y Verificación de PARAM.SFO
log_info "Comprobando y generando PARAM.SFO..."
SFO_FILE="${PKG_BUILD_DIR}/PARAM.SFO"

if make -n sfo >/dev/null 2>&1; then
    make sfo
elif [ -f "${PROJECT_ROOT}/tools/sfo_builder.py" ]; then
    python3 "${PROJECT_ROOT}/tools/sfo_builder.py" "${SFO_FILE}" 2>/dev/null || \
    python "${PROJECT_ROOT}/tools/sfo_builder.py" "${SFO_FILE}"
fi

if [ ! -f "${SFO_FILE}" ]; then
    log_error "No se pudo encontrar ni generar PARAM.SFO en ${SFO_FILE}."
    exit 1
fi

# Validar firma SFO: 0x00 0x50 0x53 0x46 ("\0PSF")
SFO_MAGIC="$(head -c 4 "${SFO_FILE}" | od -An -tx1 | tr -d ' ')"
if [[ "${SFO_MAGIC}" =~ ^00505346 ]]; then
    log_success "PARAM.SFO válido verificado (Magic: \\0PSF)"
else
    log_warn "PARAM.SFO presente pero con magic no convencional: ${SFO_MAGIC}"
fi

# 6. Generación de Recursos/Assets si existen generadores
if [ -f "${PROJECT_ROOT}/tools/generate_assets.py" ]; then
    log_info "Actualizando assets gráficos (ICON0.PNG / PIC1.PNG)..."
    python3 "${PROJECT_ROOT}/tools/generate_assets.py" 2>/dev/null || \
    python "${PROJECT_ROOT}/tools/generate_assets.py" 2>/dev/null || true
fi

# 7. Empaquetado del paquete instalable .pkg
log_info "Empaquetando archivo PKG instalable..."
PKG_OUTPUT="${PROJECT_ROOT}/MCRM00001.pkg"

if make -n pkg >/dev/null 2>&1; then
    make pkg
elif [ -f "${PROJECT_ROOT}/tools/pkg_builder.py" ]; then
    python3 "${PROJECT_ROOT}/tools/pkg_builder.py" "${PKG_BUILD_DIR}" "${PKG_OUTPUT}" || \
    python "${PROJECT_ROOT}/tools/pkg_builder.py" "${PKG_BUILD_DIR}" "${PKG_OUTPUT}"
fi

# Localizar paquete PKG generado
if [ ! -f "${PKG_OUTPUT}" ]; then
    PKG_OUTPUT="$(find "${PROJECT_ROOT}" -maxdepth 2 -name "*.pkg" 2>/dev/null | head -n 1 || true)"
fi

if [ -z "${PKG_OUTPUT}" ] || [ ! -f "${PKG_OUTPUT}" ]; then
    log_error "No se pudo generar el archivo .pkg de instalación."
    exit 1
fi

PKG_SIZE=$(stat -c%s "${PKG_OUTPUT}" 2>/dev/null || stat -f%z "${PKG_OUTPUT}" 2>/dev/null || wc -c < "${PKG_OUTPUT}")
log_success "Paquete PKG generado con éxito: ${PKG_OUTPUT} (${PKG_SIZE} bytes)"

# 8. Resumen Final
echo "=========================================================="
echo -e "${COLOR_GREEN}  PROCESO DE COMPILACIÓN Y EMPAQUETADO COMPLETADO EXITOSAMENTE${COLOR_RESET}"
echo "=========================================================="
echo "  - ELF Binario: ${ELF_FILE}"
echo "  - SELF/EBOOT:  ${EBOOT_FILE}"
echo "  - Metadata:    ${SFO_FILE}"
echo "  - Paquete PKG: ${PKG_OUTPUT}"
echo "  Listo para instalación en PS3HEN / CFW o emulación en RPCS3."
echo "=========================================================="
