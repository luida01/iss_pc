# Gate G0 — baseline en RTX 3050 Laptop 4 GB

**Fecha:** 2026-10-19  
**Build:** `fb7e45238c06+dirty`, compilado el 2026-10-19 09:20:04  
**Juego:** CUSA00223 01.07 (dump propio, vista hardlink base 1.00 + patch 1.07)  
**Equipo:** i5-13420H, 16 GB RAM, RTX 3050 Laptop 4 GB, Windows 11, driver NVIDIA 617.42

## Configuración medida

| Parámetro | Valor |
|---|---|
| Resolución interna | `720p` (`render_resolution` en copia de `tlg.ini`) |
| Presupuesto GC | `ISS_GC_BUDGET_MB=3500` (default del launcher) |
| Frame stats | `TLG_FRAME_STATS=1` |
| Entorno | `TLG_DATA_DIR=port`, `ISS_USER_DIR=userdata` |
| Flags heredados | `TLG_ASYNC_SHADERS=1`, `TLG_DEFERRED_READBACK=1`, `TLG_STALE_READBACK=1`, `TLG_READBACK_LINEAR=1`, `TLG_UPSCALER=none` |
| Vblank / engine | 60 Hz / 60 FPS |

Comando:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -Command "& '.\tools\run-baseline.ps1' `
  -DurationSec 120 -Resolution 720p -Tag 'source-720p-b3500-r2' `
  -ExtraEnv @{ ISS_GC_BUDGET_MB = '3500' }"
```

## Resultados

### Run corto de 120 s (válido)

Evidencia: `evidence/20261019-092448-source-720p-b3500-r2/`

| Métrica | Valor |
|---|---|
| Estado | `completed` (120.0 s) |
| FPS mediana | **32.8** |
| FPS p95 (ventanas de 5 s) | **59.2** |
| Frame pacing mediano | 33.08 ms |
| Frame pacing p99 | 67.3 ms |
| VRAM máxima | **3835 / 4096 MB** |
| VRAM mediana | 3754 MB |
| RAM máxima (working set) | **7490 MB** |
| RAM privada máxima | 8592 MB |
| GPU utilización mediana | 77 % (p95 84 %, máx 89 %) |
| GPU temperatura máxima | 70 °C |
| GPU potencia máxima | 33.1 W |
| CPU mediana / máxima | 21.4 % / 27.4 % |
| Muestras | 120 (1 s) |

Escena: título + intro (texto “Seven years ago… Conduits… DUP” y cinemática del vehículo). El juego renderiza ~2000 draws/frame y alcanza estado de gameplay/cinemática; no se observó OOM ni cierre durante 120 s.

### Run largo de 15 min (cerrado por el usuario a los 436.8 s)

Evidencia: `evidence/20261019-092725-source-720p-b3500-15m/`

| Métrica | Valor |
|---|---|
| Estado | `early_exit` (el usuario cerró el juego) |
| Duración medida | **436.8 s** de 900 s solicitados |
| FPS mediana | **35.1** |
| FPS p95 (ventanas de 5 s) | **55.4** |
| Frame pacing mediano | 32.63 ms |
| Frame pacing p99 | 66.82 ms |
| VRAM máxima | **3891 / 4096 MB** |
| RAM máxima (working set) | **7338 MB** |
| RAM privada máxima | 8452 MB |
| GPU utilización mediana | 62 % (máx 73 °C, 33.1 W) |
| CPU mediana / máxima | 17.5 % / 31.6 % |
| Muestras | 436 (1 s) |
| `log_issues` | `[]`; sin `Out of device memory`, OOM ni fault |

La salida fue ordenada (`stop_reason=game_exit`, `forced_stop=false`); no fue un crash del port.
La sesión estuvo la mayor parte del tiempo en título/intro/cinemática, no en gameplay sostenido.
Además, las capturas de ese run se tomaron de la pantalla completa y muestran una ventana de
navegador en primer plano, así que **no sirven como evidencia visual del juego**. El run no cumple
el requisito de 15–30 min de juego real del Gate G0.

## Observaciones del log

- `GPU [Render] buffer_cache.cpp:1483 UploadInParts: Uploading 2459 MB in parts of 256 MB`.
- `GPU [Render] buffer_cache.cpp:1660 SubmitPendingArenaBinds: Sparse bind ... 2640 MB resident in 6 allocations`.
- `Texture cache: memory pressure, 4631 of 1922 MiB (critical 1922): 0 images evicted`.
- La contabilidad de la caché de texturas está muy por encima del presupuesto, pero la VRAM real se mantiene en 3.5–3.9 GB y no hubo `Out of device memory` en 120 s.
- El presupuesto de 3500 MB colapsa los umbrales del GC; la caché no evicta nada. Es el principal riesgo para sesiones largas y para zonas nuevas.

## Hallazgos de harness

- El primer intento (`evidence/20261019-092209-source-720p-b3500`) abortó a los ~20 s por un lock de Windows al leer `stdout.log` mientras el probe heredaba el handle del launcher.
- `tools/run-baseline.ps1` se corrigió para leer con `FileShare.ReadWrite` y reintentos; un fallo de merge ya no impide escribir `result.json`.
- El sampler funciona por separado (`measure-run.ps1`); el CSV del primer intento quedó solo con cabecera por el aborto.

## Veredicto provisional

**Viable en 4 GB para sesiones cortas a 720p**, con muy poco margen de VRAM (261 MB en el pico) y una caché de texturas que reporta 2.4× su presupuesto. El run de 436.8 s no muestra crash ni OOM, pero el usuario lo cerró y no llegó a gameplay sostenido; además las capturas no enfocaban la ventana del juego. No se puede declarar Gate G0 hasta completar 15–30 min de juego real con capturas de la ventana y confirmar que no hay OOM al cargar zonas nuevas.

## Próximos experimentos

1. Completar el run de 15 min con `-AutoPlayScenario intro-g0` y capturas de la ventana del juego; revisar VRAM máxima, OOM y transición intro → gameplay.
2. A/B de presupuesto: `ISS_GC_BUDGET_MB=2300` y `2000` con la misma escena.
3. A/B de resolución: 720p vs 1080p/native, misma ruta.
4. A/B de MSAA (`ISS_NO_MSAA=1`) y readbacks realmente desactivados (`TLG_STALE_READBACK=0`, `TLG_DEFERRED_READBACK=0`).
5. Instrumentar bytes residentes de buffers, evicciones/OOM y tamaño real de asignación de imagen.
