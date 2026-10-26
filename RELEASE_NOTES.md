# Monochrome PS3 — Release Notes

## 🚀 v2.3.0 — Versión Final Oficial para PlayStation 3

**Title ID**: `MONO00024`  
**Display Version**: `v2.3.0`  
**Package Format**: Free NPDRM CEX PKG (Signed)  
**Target Hardware**: Sony PlayStation 3 (Super Slim / Slim / FAT) con PS3HEN v3.x o CFW Evilnat 4.88-4.92+  

---

### 🎵 Novedades y Mejoras Clave

1. **Streaming y Reproducción Lossless FLAC**:
   - Streaming continuo y fluido desde `tracks.monochrome.st`.
   - Pipeline de audio en tiempo real con hilo DMA a prioridad 100 de GameOS.
   - Socket buffer TCP de 512 KB y llamadas no bloqueantes para eliminar microcortes o lags en la reproducción de pistas Hi-Res.
   - Prebuffering ampliado a 4 MB para máxima estabilidad de red.

2. **Calibración y Ajuste de Pantalla (Screen Adjust)**:
   - Ajuste ultra-fino de posición píxel a píxel (paso 1px).
   - Ajuste ultra-fino de escala global (paso 0.005 con L2/R2).
   - Ajuste fino del tamaño de fuente (paso 0.02 con L1/R1).
   - Persistencia automática en disco de los valores elegidos (`screen_config.txt`), aplicándose automáticamente desde el primer frame de inicio.

3. **Interfaz y Arte Minimalista Metalheart**:
   - Splash screen de inicio con banner ASCII y especificaciones del motor LPCM 48kHz.
   - Fondo técnico con marcas y cuadrículas tenues sobre negro puro (`0x00000000`).
   - Esquema de colores sobrio: líneas blancas puras, texto de alto contraste y detalles en carmesí web Monochrome (`#C83838`).

4. **Integración Total con el XMB**:
   - Icono oficial `ICON0.PNG` adaptado a la relación de aspecto nativa (320×176).
   - Fondo panorámico oscuro `PIC1.PNG` (1920×1080) asegurando que el texto blanco del XMB mantenga total legibilidad.
