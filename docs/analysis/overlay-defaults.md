# Overlay de rendimiento activo por defecto

**Fecha:** 2026-10-19  
**Alcance:** `port/gpu/shim/tlg_settings.h`, `ControlsTab()` de `port/gpu/shim/tlg_overlay.cpp` y `port/tests/test_perf_overlay.cpp`.

## Qué cambió

| Ajuste | Antes | Ahora |
|---|---|---|
| `show_fps` | `false` | `true` |
| `overlay_corner` | `1` (arriba a la derecha) | `0` (arriba a la izquierda) |
| `overlay_scale` | `100` | `115` |
| `overlay_opacity` | `50` | `35` |
| `overlay_items` | `DefaultItems` | sin cambio: FPS, frametime, GPU y VRAM |

## Por qué

- El HUD aparece al arrancar, como el `show_fps=1` del port de God of War 3; el baseline del Gate G0 se lee sin pulsar nada.
- 115 % de tamaño y 35 % de fondo mantienen el texto legible sin tapar la imagen; la esquina superior izquierda deja libre el centro de la acción.
- Los elementos por defecto (FPS, frametime, GPU y VRAM) son los que pide el harness para diagnosticar los 4 GB de VRAM.

## Cómo se alterna

- **F10**: muestra u oculta el HUD de métricas en cualquier momento.
- **Insert** o **R3+L2**: abre o cierra el menú del port. En la pestaña **Overlay** se activa o desactiva el HUD y se eligen elementos, tamaño, esquina, opacidad y disposición.
- La pestaña **Controls** vuelve a mostrar solo el mapeo de teclado y ratón; la guía del mando Xbox 360 vive en `docs/controls.md`.

## Configuración

Los valores siguen siendo configurables en `tlg.ini` con `show_fps`, `overlay_fps`, `overlay_frametime`, `overlay_gpu`, `overlay_vram`, `overlay_scale`, `overlay_corner`, `overlay_opacity` y `overlay_layout`. `TLG_SHOW_FPS` sigue sobrescribiendo `show_fps` al arrancar. Un `tlg.ini` antiguo con `show_fps=0` u otros valores manda sobre estos defaults: ajústalo o bórralo para recuperar el comportamiento nuevo.

El launcher Qt y el lanzador `infamous_run.py` también arrancan ahora con `show_fps=True` (`gfxlogic.SETTING_DEFAULTS` y `preset_share.SETTING_DEFAULTS` se actualizaron para seguir coincidiendo), así que la vía gráfica no vuelve a escribir `show_fps=0` al lanzar.
