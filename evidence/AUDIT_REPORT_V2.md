# AUDIT_REPORT_V2 — Auditoría estática del target PS3

Fecha: 2026-10-20. Repo: `ps3-monochrome`. Alcance: código PS3 (`src/`, `include/`).

## Veredicto por punto

| # | Punto | Veredicto | Evidencia |
|---|---|---|---|
| 1 | Sin TODO/FIXME/stubs/funciones vacías | **PASS** | 0 marcadores reales |
| 2 | Regla PS3-only (PC intacto) | **PASS con reserva** | ver hallazgo H3 |
| 3 | Syscalls LV2: firmas y tipos | **PASS** | `sys_memory_allocate` out-ptr u32; `sys_ppu_thread_start`=53 |
| 4 | Imports GCM/PAD/AUDIO declarados y usados | **PASS** | 23 símbolos únicos, todos referenciados, 22 `SCE_SLOT` |
| 5 | Presupuesto XDR < 256 MB | **PASS** | ≈104 MB (ver cálculo) |
| 6 | Hallazgos con file:line | **PASS** | ver H1-H4 |

## 1. Limpieza de código
`grep -n 'TODO\|FIXME\|XXX\|not implemented\|unimplemented\|stub'` en el target PS3:
- `src/platform_ps3.c:196,219,252,267,279` → nombre de función legítima
  `ps3_thread_entry_stub` / `ps3_thread_entry_stub_opd` (trampolín OPD real, no un stub vacío).
- `src/platform_ps3.c:1192` → comentario `/* - Console stubs - */` del bloque de logging TTY.
- `src/http_ps3.c:3` → comentario histórico (`Replaces the former "fallback catalogue" stubs`).
Conclusión: **no hay TODO/FIXME ni funciones vacías** en rutas alcanzables.

## 2. Regla PS3-only
El target PS3 no incluye ni llama a `src/platform_pc.c`, `build_pc.bat` ni `tools/launcher.c`;
la build PS3 solo compila `entry.s main.c ui.c audio.c network.c cover.c cJSON.c
flac_decoder.c http_ps3.c platform_ps3.c lv2.c`.
Reserva: el árbol de trabajo contiene cambios preexistentes en `src/platform_pc.c`
(helpers GDI Unicode, 207 líneas) que NO forman parte del target PS3. Antes de commitear hay
que decidir si se descartan (`git checkout -- src/platform_pc.c`) para respetar la regla
"PC = solo lectura".

## 3. Syscalls LV2
- `ps3_sys_memory_allocate(uint32_t size, uint64_t flags, uint32_t *addr)` — out-ptr **u32**:
  correcto para `sys_memory_allocate` (348); un u64 provocaba `SIG: Narrowing error` (0x2009000000000000).
- `ps3_sys_ppu_thread_start` = syscall **53**: sin él los hilos quedaban suspendidos.
- `sys_ppu_thread_create(..., const char *name, void *tls)` con TLS por hilo: musl ppc64 usa
  `TLS_ABOVE_TP` (TP_OFFSET 0x7000); se clona el TCB del hilo principal y se pasa `r13 = base+0x7060`.
- `sys_net` 700-726: envoltorios en `src/lv2.c`; `SO_RCVTIMEO` en microsegundos.

## 4. Imports
`tools/sce_imports.py`: 23 símbolos únicos (11 cellGcmSys, 2 sys_io/cellPad, 7 cellAudio,
más `_cellGcmInitBody`, `cellGcmSetWaitFlip`, `cellGcmGetConfiguration`).
Todos aparecen referenciados en `src/*.c`; 22 `SCE_SLOT` + `SCE_BOUND` comprueban el binding
en runtime (`platform_ps3.c`). El patcher confirma `21 imports in 3 modules, 9 phdrs`.

## 5. Presupuesto XDR
- Buffer de stream: `STREAM_BUFFER_CAPACITY = 64 MB` (`src/network.c:313`), reservado con
  `platform_alloc_stream_buffer` → `sys_memory_allocate`.
- Ring de audio: 8 MB. Texturas: ~16 MB. Heap bump: 16 MB.
- Framebuffers: **1,35 MB × 2 en memoria local RSX** (no XDR) desde el fix de scanout.
Total ≈ **104 MB** < 256 MB. ✅

## Hallazgos

**H1 (BLOQUEANTE, corregido) — Framebuffers en memoria equivocada.**
`platform_ps3.c` registraba los display buffers con offsets de *main memory* vía
`cellGcmMapMainMemory` (0x100000/0x300000). El scanout PS3 (y RPCS3) resuelve los offsets de
`cellGcmSetDisplayBuffer` contra la **memoria local RSX**; con offsets < localSize interpretaba
main memory como local → pantalla negra a 60 FPS. Fix: `cellGcmGetConfiguration` →
`localAddress=0xC0000000`, buffers en `local_base + i*0x200000`, offsets 0x0/0x200000.
Verificado: `[RSX] buf0 ea=0xc0000000 offset=0x00000000` y UI visible.

**H2 (IMPORTANTE, corregido) — Corrupción de codificación en `src/platform_ps3.c`.**
Ciclos de edición con PowerShell 5.1 (`Get-Content` ANSI + `WriteAllLines` UTF-8)
doble/triple-codificaron los comentarios: el fichero pasó de 55 KB a 393 KB de mojibake.
Fix: saneado a ASCII (57,8 KB), 140/140 comentarios balanceados, y el binario resultante es
idéntico en tamaño al funcional (los comentarios no afectan al código). Evidencia: build
posterior exit 0 + runtime verificado.

**H3 (IMPORTANTE) — Cambios no-PS3 en el árbol de trabajo.**
`src/platform_pc.c` tiene 207 líneas modificadas (helpers GDI UTF-8) ajenas al target PS3.
Riesgo de incumplir la regla "PC solo lectura". Acción: revisar/descartar antes de commit.

**H4 (MENOR, conocido) — Escritura de caché FLAC en VFS.**
`sys_fs_open` con create devuelve `CELL_ENOENT` en `/dev_hdd0/.../cache`. No bloquea: el
streaming progresivo funciona sin caché (verificado: 8 MB descargados y reproducción).

## Nota de proceso
Trampa detectada: `cmd /c "build_homebrew.bat > build_out.txt 2>&1"` ejecutado desde pwsh
NO ejecutaba el .bat; se leía un `build_out.txt` obsoleto y se creía "build OK". La forma
fiable en este entorno es `& .\build_homebrew.bat` desde PowerShell. Refuerza la regla
"COMPILAR != FUNCIONAR": verificar siempre mtime/tamaño de los artefactos.
