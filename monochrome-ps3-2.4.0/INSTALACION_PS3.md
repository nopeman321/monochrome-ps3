# Instalación en PS3 real (PS3HEN / CFW)

> **Importante:** el archivo `MCRM00001.pkg` de la versión 1.0.0 **no se instala** en hardware
> real porque se generó con un contenedor PKG no estándar. Usa **la instalación por carpeta**
> (método A), que es la vía fiable, o el PKG corregido a partir de la v1.0.1 (método B).

---

## Método A — Instalación por carpeta (recomendado, funciona siempre)

En PS3HEN el XMB **escanea** `/dev_hdd0/game/<TITLE_ID>/` y muestra el icono si hay un
`PARAM.SFO` válido y un `USRDIR/EBOOT.BIN` firmado. No pasa por el instalador de PKG, así que
no hay validación de paquete que pueda fallar.

### Qué necesitas
- `dist/MCRM00001/` (o `dist/Monochrome-PS3-folder.zip`, ya con esa carpeta dentro)
- HEN activo en la consola
- Conexión de red (para FTP) o un pendrive USB en FAT32

### Estructura que debe quedar en la consola
```
/dev_hdd0/game/MCRM00001/
├── PARAM.SFO
├── ICON0.PNG
├── PIC1.PNG
└── USRDIR/
    ├── EBOOT.BIN
    └── certs/cacert.pem
```

### A1. Copia por FTP (lo más rápido)
1. En la PS3: enciende y ejecuta **★ Enable HEN** (columna Juego). Espera a volver al XMB.
2. Activa la red: **Ajustes → Configuración de red → Activar conexión a Internet**.
3. Apunta la IP: **Ajustes → Configuración de red → Estado de la conexión**
   (o en multiMAN: Settings → △ → Advanced Settings → PS3 IP).
4. En el PC abre **FileZilla** y conéctate:
   - Host: `IP_DE_LA_PS3`
   - Puerto: `21`
   - Protocolo: **FTP** (no SFTP)
   - Usuario: `anonymous`, contraseña: vacía
5. Ve a `/dev_hdd0/game/` y sube la carpeta `MCRM00001` completa.
   - Si HEN no trae webMAN: inicia **multiMAN → Settings → FTP service → Activate (No Timeout)**,
     o usa **irisMAN**, que también trae servidor FTP integrado.

### A2. Copia por USB (sin red)
1. Formatea el pendrive en **FAT32** (unidad de asignación 32 KB).
2. Copia la carpeta `MCRM00001` en la **raíz** del pendrive.
3. En la PS3 con HEN: abre **multiMAN** → file manager (mmOS) → `dev_usb000` →
   copia `MCRM00001` → pégala en `dev_hdd0/game/`.

### A3. Refrescar el XMB y arrancar
1. Reinicia la consola y vuelve a pulsar **★ Enable HEN** (al activarse refresca la columna Juego).
   - Alternativa sin reiniciar: abre el navegador de la PS3 y visita `http://IP_DE_LA_PS3/refresh.ps3`.
2. En la columna **Juego** aparecerá **Monochrome Music**. Lánzalo con ✕.
3. Si el icono no aparece:
   - Comprueba que la carpeta se llama exactamente `MCRM00001`.
   - Comprueba que `PARAM.SFO` está en la raíz de esa carpeta y `USRDIR/EBOOT.BIN` existe.
   - Como último recurso: **Modo a prueba de fallos → Reconstruir base de datos** (hace que VSH
     vuelva a escanear `/dev_hdd0/game/`).

---

## Método B — Instalación del PKG (desde v1.0.1)

A partir de la v1.0.1 el `MCRM00001.pkg` se genera con el formato **PKG3 de PS3 en variante
debug/no finalizada**, que es la que acepta el Package Manager de HEN/CFW.

1. Copia `MCRM00001.pkg` a la **raíz** de un USB en FAT32.
2. En la PS3 con HEN: **Juego → Administrador de paquetes → Instalar paquete de archivos**.
3. Selecciona la unidad USB y el `MCRM00001.pkg`.

Si devuelve **80029564** (paquete corrupto), usa el Método A.
Si devuelve **80010017** al arrancar, HEN no está activo en ese arranque: apaga, enciende y
vuelve a pulsar **★ Enable HEN**.

---

## Verificar el PKG en el PC antes de copiarlo

```cmd
python tools\pkg3.py verify MCRM00001.pkg
```
Debe mostrar `pkg_revision : 0x0000 (debug/no finalizado)`, `pkg_type : 0x0001 (PS3)`,
`metadata_offset: 0xC0 count=5 size=0x80`, `data_offset : 0x140` y
`Estructura y digests: OK`.

---

## Notas

- HEN **se pierde en cada apagado**: hay que volver a activarlo tras encender.
- No uses Factory Service Mode ni CCAPI con HEN.
- El `EBOOT.BIN` es un SELF **NPDRM Free** firmado con SCETool: es el formato homebrew
  estándar y HEN lo acepta sin licencias `.rap`.
- Referencias: [PS3HEN](https://github.com/PS3Xploit/PS3HEN),
  [PARAM.SFO (psdevwiki)](https://www.psdevwiki.com/ps3/PARAM.SFO),
  [PS3HEN en ConsoleMods](https://consolemods.org/wiki/PS3:PS3HEN),
  [FTP con multiMAN + FileZilla](https://wololo.net/2014/06/30/ps3-how-to-ftp-transfer-using-multiman-and-filezilla-by-raphaelligh/).
