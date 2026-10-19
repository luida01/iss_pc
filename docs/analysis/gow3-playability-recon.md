# Recon de jugabilidad — `gow3_pc` y release oficial `iss_pc` → mejoras para nuestro fork

> **Tipo:** análisis de solo lectura (recon). **No** se modificó código, **no** se ejecutó ningún juego y **no** se descargó nada de Sony/Sucker Punch.
> **Fecha:** 2026-10-19 · **Autor:** agente `recon` · **Máquina de referencia:** RTX 3050 Laptop 4 GB, i5-13420H, 16 GB RAM, Windows 11.
> **Licencias:** `gow3_pc` es GPL-2.0-or-later; `iss_pc`/`bloodborne_pc`/shadPS4, GPL-2.0 (ver §7). Este documento es análisis propio; no reproduce código fuente de terceros.

---

## 1. Resumen ejecutivo

1. Nuestro `port/` **ya es un fork de `gow3_pc`** (base `9a6d17c`, 2026-10-12, ver `port/UPSTREAM.md`), así que la mayoría de mejoras de jugabilidad de GOW3 (touchpad sintetizado, record/replay de pad, asistente de sensor, backups de saves, "Apply and restart" con reversión, reporte de FREEZE) **ya están presentes**. El trabajo real es cerrar un puñado de diferencias concretas y adoptar dos políticas del release oficial.
2. Diferencias verificadas con mayor impacto para nuestra máquina de 4 GB:
   - **Presupuesto de VRAM:** GOW3 solo lo fuerza por variable de entorno para pruebas (`GOW3_GC_BUDGET_MB`); en integradas usa el presupuesto del driver. Nuestro launcher lo fija siempre en 3500 MB (`port/launcher/infamous_run.py:689`), y nuestro run llegó a 3835 MB de pico (`AGENTS.md` §8) → candidato a modo `auto` y preset 4 GB.
   - **`stale_readback`:** GOW3 lo trae **encendido** por defecto (`gpu/shim/gow3_settings.h:92`, `gow3.ini:11`); el nuestro lo trae **apagado** (`port/gpu/shim/tlg_settings.h:100`) → A/B pendiente.
   - **`object_motion`:** GOW3 lo trae encendido en código (`gow3_settings.h:37`) restringido a shaders con pose/huesos, con coste medido (≈49 vs 77–78 FPS en su doc interno); el nuestro lo apaga (`tlg_settings.h:49`, `ISS_OBJECT_MOTION=0`). Atarlo al upscaler es la política a copiar.
   - **Persistencia de pipelines:** GOW3 guarda binarios por serial + warmup en segundo plano; el nuestro apaga `TLG_PIPELINE_BINARIES` por un crash del driver NVIDIA 595 y conserva warmup/BP08. Falta revalidar y documentar el gating.
3. El release oficial (`iss_pc v2026.10.19`) aporta dos cosas que no tenemos: una **tabla local de "problemas conocidos" por hardware** (`_internal/qt/data/known_issues.json`; con entradas específicas para **4 GB de VRAM** y para **NVIDIA RTX 30**) y el consejo explícito de **BP09 para RX 580** (que nosotros ya tenemos como BP09 apagado por defecto).
4. `BC_TINY` **no existe** en `gow3_pc` ni aparece en el paquete oficial; es una pieza que solo está en nuestro fork (`TLG_BC_TINY`, `port/gpu/shadps4/video_core/amdgpu/tlg_bc_tiny.h:12`). No hay nada que extraer ahí; sí conviene documentarlo como divergencia.
5. Se proponen **10 mejoras priorizadas** (§4): 3 de memoria/estabilidad (P0), 4 de rendimiento/entrada/config (P1) y 3 de UX/proceso (P2). Todas con evidencia `archivo:línea` y verificación mínima en §6.

---

## 2. Fuentes, versiones y método

| Fuente | Versión / commit | Ruta local | Uso en este recon |
|---|---|---|---|
| `FelipePrado0/gow3_pc` (fuente) | `main` @ `41e9473` | `C:\Users\luisd\proyectos\gow3-lab` | Código de referencia: input, GPU, launcher, estabilidad |
| `gow3_pc` publicado | `gow3-windows` (dist del usuario) | `C:\Users\luisd\Documents\Codex\2026-10-17\...\outputs\gow3-windows` | `gow3.ini`, `user/last_run.log` (run en **la misma** RTX 3050 4 GB), parches XML |
| `iss_pc` oficial | `v2026.10.19-d3995fe` | `infamous-port/dist/upstream/iss_pc-v2026.10.19/infamous-windows-x86_64` | Comportamiento jugador de referencia: `README-player.md`, `_internal/qt/data/known_issues.json` |
| Nuestro fork | base `9a6d17c` + trabajo local | `infamous-port/port` | Estado actual: `src/runtime_pad.c`, `gpu/shim/*`, `launcher/*`, `gpu/shadps4/*` |

Método: lectura dirigida de código y logs; cada afirmación lleva `archivo:línea` de la versión citada. Los números de línea pueden desplazarse en commits posteriores. No se copió código; las propuestas son descripciones de comportamiento.

---

## 3. Matriz de paridad

Leyenda: **sí** = presente/verificado · **no** = ausente · **parcial** = presente con diferencias · **n/v** = no verificado en esta pasada · **n/a** = no aplica al otro juego (los juegos son distintos: GoW III vs inFAMOUS Second Son).

### 3.1 Entrada, cámara, touchpad y prompts

| Capacidad | `gow3_pc` @41e9473 | `iss_pc` oficial v2026.10.19 | Nuestro fork (`port/`) | Evidencia clave |
|---|---|---|---|---|
| Clic de touchpad sintetizado alrededor del botón (50/50/100 ms) | sí | documentado (View/Select clic y mantener) | sí | GOW3 `src/runtime_pad.c:87`; oficial `README-player.md:120`; nuestro `port/src/runtime_pad.c:104` |
| Dos dedos / ambas puntas del touchpad | n/v | sí (View+stick; View+R1) | parcial (hay `touchpad_left`+`touchpad_right` y fusión "two") | oficial `README-player.md:120`; nuestro `port/src/runtime_pad.c:132-147` |
| Ratón como cámara | sí (`mouse_stick`) | sí (mover gira la cámara) | sí (`mouse_stick.h`, `mouse_touch.h`) | oficial `README-player.md:121`; nuestro `port/src/runtime_pad.c:13-14` |
| "Aim" con L2 y dos estilos (velocidad vs radio) + sensibilidad 10–200 | sí | L2 = apuntar (botón derecho) | cámara con ratón sí; el "aim" estilo GOW3 es específico de GoW III | GOW3 `src/runtime_pad.c:614-651`, `gpu/shim/gow3_settings.cpp:130-133`; nuestro `port/src/runtime_pad.c:13` |
| Prompts adaptativos ratón↔mando (`mouse_prompt_distance`, `mouse_lock`) | sí | n/v | **no** (no existen esas claves) | GOW3 `gpu/shim/gow3_settings.cpp:126-129`; búsqueda sin resultados en `port/launcher` → mejora **M6** |
| Giroscopio/acelerómetro | **stub** (identidad, sin sensores) | real en DS4/DualSense + modo sensor L3+R3 + teclas T/G/Y | **real + simulado** con asistente de grafiti (más avanzado) | GOW3 `src/runtime_pad.c:458,809`; oficial `README-player.md:57,60,119,122`; nuestro `port/src/runtime_pad.c:322-345,459-461` |
| Prompts de QTE con stick rotatorio para teclado/ratón | sí | n/a (sin QTE de ese tipo) | **no** (y no aplica a inFAMOUS; ver §5) | GOW3 `src/runtime_pad.c:307-316,353-355,380-381` |
| Entrada scripted (archivo/replay) | sí | n/v | sí | GOW3 `src/runtime_pad.c:506-548`; nuestro `port/src/runtime_pad.c:815-862` |
| Deadzone de stick configurable | n/v | n/v | sí (`ISS_STICK_DEADZONE`, por defecto 10 %) | `port/launcher/infamous_run.py:240,694` |

### 3.2 Memoria, VRAM, texturas y rendimiento

| Capacidad | `gow3_pc` @41e9473 | `iss_pc` oficial v2026.10.19 | Nuestro fork (`port/`) | Evidencia clave |
|---|---|---|---|---|
| Regla del colector 70 % / 85 % / 95 % del presupuesto | sí | n/v | sí (misma regla) | GOW3 `gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1631-1633`; nuestro `.../texture_cache.cpp:1845-1847` |
| Presupuesto forzado por env | solo para pruebas (`GOW3_GC_BUDGET_MB`) | `ISS_GC_BUDGET_MB` (issues #72/#75 citados por el propio código) | **siempre** desde el launcher (3500 MB) | GOW3 `texture_cache.cpp:1623-1627`; oficial `known_issues.json:28-29`; nuestro `port/launcher/infamous_run.py:689` → mejora **M1** |
| Presupuesto del driver en integradas | sí | n/v | sí, mejorado: `min(forzado, driver)` + `ApplyResidencyHeadroom` | GOW3 `texture_cache.cpp:1628-1634`; nuestro `texture_cache.cpp:1835-1851` |
| Readback diferido por defecto | **on** (64 copias / 64 MiB) | n/v | **on** | GOW3 `gpu/shim/gow3_settings.h:91`, log `user/last_run.log:40`; nuestro `port/launcher/infamous_run.py:243,693` |
| `stale_readback` por defecto | **on** | n/v | **off** | GOW3 `gow3_settings.h:92`, `gow3.ini:11`; nuestro `port/gpu/shim/tlg_settings.h:100` → mejora **M2** |
| `object_motion` (vectores por objeto) | default de código **on**, limitado a shaders con pose/huesos; apagado en el run de referencia | n/v | **off** (runtime y launcher) | GOW3 `gow3_settings.h:37`, `docs/motion_vectors.md:7-16`, log `user/last_run.log:42`; nuestro `tlg_settings.h:49`, `infamous_run.py:224` → mejora **M4** |
| "1D como 2D" (BP09) | n/v | BP09 documentado para RX 580 | sí, apagado por defecto | oficial `README-player.md:128`, `known_issues.json:9`; nuestro `port/launcher/backports_table.py:172` |
| BC minúsculas (`BC_TINY`) | **no** (sin coincidencias) | no aparece en `README-player.md` ni en los scripts del paquete | sí, `auto` en AMD propietario | nuestro `port/gpu/shadps4/video_core/amdgpu/tlg_bc_tiny.h:2-12`, `backports_table.py:216` |
| Persistencia de pipelines (binarios por serial + warmup) | sí: `VK_KHR_pipeline_binary`, carpeta `pipeline-binaries/`, warmup on-demand/segundo plano | n/v | binarios **off** (crash driver 595); warmup paralelo sí; BP08 (caché clásica) on | GOW3 `vk_instance.cpp:244`, `vk_graphics_pipeline.cpp:435`, `vk_presenter.cpp:330-345`, log `user/last_run.log:44`; nuestro `vk_instance.cpp:279-280`, `vk_presenter.cpp:211`, `infamous_run.py:220`, `backports_table.py:168`, `vk_pipeline_serialization.cpp:546` → mejora **M5** |
| Multisampling de invitado y vida de memoria | sí (log: `guest multisampling on`; `budget=7.45 GB, live=5.65 GB`) | consejo 4 GB (bajar resolución, presupuesto < VRAM) | n/v en esta pasada | GOW3 `user/last_run.log:19,1960`; oficial `known_issues.json:24-28` |

### 3.3 Estabilidad, caches, launcher y configuración

| Capacidad | `gow3_pc` @41e9473 | `iss_pc` oficial v2026.10.19 | Nuestro fork (`port/`) | Evidencia clave |
|---|---|---|---|---|
| Backups de saves antes de cada arranque (keep 10, restaurar) | sí | n/v | sí | GOW3 `scripts/save_backup.py:13,62-76`, `run.py:333-338`; nuestro `port/launcher/savemanager.py` + `infamous_run.py:802` |
| Revertir opciones si el reinicio no llega al juego | sí ("the last restart never reached the game; previous options restored") | n/v | sí (`restart_unconfirmed`) | GOW3 `run.py:196-198`, `gpu/shim/gow3_overlay.cpp:660-666`, `scripts/graphics_options.py:157-160`; nuestro `port/gpu/shim/tlg_settings.h:101-103` |
| Contador de cambios que exigen reinicio + aviso "progreso sin guardar" | sí, con botón deshabilitado sin comando de reinicio | n/v | parcial (se cuentan cambios, no se muestra el aviso/botón) | GOW3 `gow3_overlay.cpp:655-667`; nuestro `port/gpu/shim/tlg_overlay.cpp:686` → mejora **M9** |
| Reporte de cuelgue (FREEZE, 10 s por defecto) | n/v | sí (`ISS_FREEZE_REPORT_SECONDS`) | sí (`TLG_FREEZE_REPORT_SECONDS`, 10 s; informe por hilos) | oficial `README-player.md:131`; nuestro `port/src/probe.c:1348-1350`, `runtime_freeze.c:394,427` |
| Preflight de memoria de Windows (página, error 1450) | sí: calcula el commit necesario y avisa antes de arrancar | consejos por hardware en el launcher | solo mensaje al fallar | GOW3 `scripts/guest_memory.py:5-18,57-66`; nuestro `port/src/runtime_memory.c:283` → mejora **M3** |
| Mods: solapado de archivos + parches XML por mod | sí | n/v | sí (cheats por `APP_VER`, mods.json) | GOW3 `scripts/mods.py:1-31,53-55`, `run.py:187-195`; nuestro `infamous_run.py:34-71,283` + `mod_patches` |
| Resoluciones extra aportadas por mods visibles en el menú | sí (`GOW3_EXTRA_RESOLUTIONS`) | n/v | **no encontrado** | GOW3 `run.py:187-195`; búsqueda sin resultados en `port/launcher` → mejora **M7** |
| HUD/FPS: opciones de overlay (ítems, escala, esquina, opacidad, disposición) | sí (ítems por defecto = FPS+FrameTime+Upscaler) | F10 contador de FPS | sí en lo esencial (Fps+FrameTime+GPU+VRAM; esquina; `show_fps` on) | GOW3 `gow3_settings.h:46`, `gow3_settings.cpp:40-53`; oficial `README-player.md:114`; nuestro `tlg_perf_stats.h:31`, `infamous_run.py:236`, `backports_table.py:14` |
| Tabla local de "problemas conocidos" por hardware | n/v | sí (por vendor/GPU/VRAM/driver) | **no** | oficial `known_issues.json:1-41` → mejora **M10** |
| Cheats/QoL específicos de GoW III (orbes, doble salto, barra de vida enemiga, auto-QTE) | sí | n/a | n/a | GOW3 `gow3_settings.cpp:136-151` — ver §5 |

---

## 4. Las 10 mejoras priorizadas

### M1 — [P0] Presupuesto de VRAM `auto` + preset "4 GB"

- **Qué:** que `vram_budget_mb` acepte un modo `auto` (presupuesto del driver − reserva, p. ej. 12.5 %) y añadir un preset explícito para 4 GB; hoy el launcher siempre fuerza 3500.
- **Evidencia GOW3:** el colector solo usa presupuesto fijo cuando hay `GOW3_GC_BUDGET_MB` (pruebas); si no, en integradas compara con el presupuesto **actual** del driver (`texture_cache.cpp:1623-1634`). El run de referencia en la misma 3050 llegó a `budget=7.45 GB` de memoria de sistema (`user/last_run.log:1960`) mientras nuestro run tocó 3835 MB de VRAM (`AGENTS.md` §8).
- **Estado nuestro:** `ISS_GC_BUDGET_MB=3500` fijo (`infamous_run.py:221,689`, `DEFAULT_SETTINGS['vram_budget_mb']=3500` en `:239`); el motor ya hace `min(forzado, driver)` en integradas y aplica `ApplyResidencyHeadroom` (`texture_cache.cpp:1835-1851`), así que el modo `auto` encaja sin tocar la regla del colector.
- **Acción:** `vram_budget_mb: 'auto'` → `GetDeviceMemoryBudgetNow()` − reserva cuando `CanReportMemoryUsage()` (la plomería ya existe); exponer 3000/3300/3500/4000 en la UI y un preset "4 GB" que combine esto con 720p y `ISS_READBACKS=2` (preciso, como está) o `0` (rápido) documentado.
- **Verificación:** run de 15 min con `TLG_FRAME_STATS=1`; comparar mediana/p95, VRAM máx y frecuencia de GC agresiva en el log; el criterio del oficial es "presupuesto por debajo de la memoria de la placa" (`known_issues.json:28`).

### M2 — [P0] A/B de `stale_readback` y posible default `on`

- **Qué:** probar `stale_readback=1` (lecturas de CPU usan la copia del frame anterior) en escenas CPU-bound; si no hay artefactos, proponerlo por defecto.
- **Evidencia GOW3:** default de código `true` (`gow3_settings.h:92`) y el `gow3.ini` distribuido lo trae `stale_readback=1` (`gow3.ini:11`); el readback diferido va "64 copias, 64 MiB" con lecturas del frame anterior (`user/last_run.log:40`).
- **Estado nuestro:** `stale_readback{false}` (`tlg_settings.h:100`), pero ya es conmutable desde el menú (cuenta para "necesita reinicio", `tlg_overlay.cpp:686`).
- **Acción:** A/B ×3 en la misma escena (protocolo `AGENTS.md` §5); si no rompe partículas/smoke, `stale_readback` on por defecto con nota en el preset.
- **Verificación:** comparar frametime y artefactos con `TLG_STALE_READBACK=0` vs `1` (`backports_table.py` fila A/B `stale_readback`); revisar partículas de humo y UI leída por CPU.

### M3 — [P0] Preflight de memoria de Windows (error 1450) en el launcher

- **Qué:** calcular antes de lanzar si el commit de memoria del invitado (sección pagefile-backed + span flexible) cabe en la RAM física + archivo de paginación, y avisar en español con pasos concretos; hoy solo hay mensaje al fallar.
- **Evidencia GOW3:** `guest_memory.py` modela el pool (`DMEM_RETAIL_MB=5056`, `+2048` cuando el fix de texturas/resolución está activo, span flexible de 1 GiB) y avisa: "más que los X MiB de RAM; el juego puede cerrarse al arrancar (error 1450): … asegure que el archivo de paginación de Windows pueda crecer, o elija menor resolución" (`guest_memory.py:5-18,57-66`).
- **Estado nuestro:** el runtime solo imprime `"is the page file large enough?"` cuando `VirtualAlloc2` falla (`port/src/runtime_memory.c:283`); el launcher no preflight-ea.
- **Acción:** portar la idea (sin copiar código): conocer el tamaño de la sección (constante del runtime) + span flexible; comparar con `os.sysconf`/API Windows; mostrar aviso bloqueante no fatal en la UI.
- **Verificación:** probar en la 3050 con pagefile "system managed" y con uno fijo pequeño; el aviso debe aparecer antes del crash.

### M4 — [P1] `object_motion` atado al upscaler (y coherencia de inicializadores)

- **Qué:** adoptar la política de GOW3: vectores de objeto activos solo cuando aportan (shaders con buffer de pose/huesos, 640–16384 B), documentando el costo medido; y revisar la incoherencia de inicializadores de nuestro shim.
- **Evidencia GOW3:** `object_motion{true}` (`gow3_settings.h:37`); su doc interno mide 49 FPS con el pase general vs 77–78 sin él y describe la restricción por shader y el presupuesto de 4 194 304 posiciones/frame (`docs/motion_vectors.md:7-16,44`); en el run publicado apareció apagado ("enable in menu or `GOW3_OBJECT_MOTION=1`", `user/last_run.log:42`).
- **Estado nuestro:** `object_motion{false}` (`tlg_settings.h:49`) y `ISS_OBJECT_MOTION=0` en `DEFAULT_ENV` (`infamous_run.py:224`); el upscaler también está off (`:222`), así que hoy no hay a quién alimentar. Ojo: `startup_object_motion = true` (`tlg_settings.h:137`) no coincide con el default `false` del runtime — verificar que el flag de "necesita reinicio" no dé falsos positivos.
- **Acción:** condicionar `object_motion` a `upscaler != off` (o exponerlo en el preset "Calidad"); corregir el inicializador si procede.
- **Verificación:** A/B con FSR1 encendido en escena con movimiento; medir p95 y comprobar ghosting/estabilidad de imagen.

### M5 — [P1] Pipeline binaries: revalidar y desacoplar persistencia

- **Qué:** mantener la caché clásica (BP08) verificada, reintentar `TLG_PIPELINE_BINARIES` tras cada driver nuevo y documentar el gating por driver en lugar de apagado global silencioso.
- **Evidencia GOW3:** usa `VK_KHR_pipeline_binary` (`vk_instance.cpp:244`), guarda por serial en `pipeline-binaries/` (`vk_graphics_pipeline.cpp:435`) y tiene warmup on-demand/segundo plano (`vk_presenter.cpp:330-345`); su run de referencia dice "on demand, no bulk warm-up" (`user/last_run.log:44`).
- **Estado nuestro:** `TLG_PIPELINE_BINARIES` off por crash en NVIDIA 595 (`vk_instance.cpp:279-280`, `ISS_PIPELINE_BINARIES=0` en `infamous_run.py:220`; trampa en `AGENTS.md` §9); warmup paralelo presente (`TLG_PARALLEL_WARMUP`, `vk_pipeline_serialization.cpp:546,1101-1104,1281`) y BP08 "Driver pipeline cache" on (`backports_table.py:168`). Shaders/pipelines viven junto a los saves (`TLG_GPU_USER_DIR`, `infamous_run.py:719-721`).
- **Acción:** (a) confirmar en logs del baseline que la persistencia clásica está activa; (b) A/B `ISS_PIPELINE_BINARIES=1` solo tras actualizar driver, con revert rápido si crashea; (c) dejar el gating (p. ej. "off para 595.x, auto para el resto") escrito en la UI del launcher.
- **Verificación:** segunda sesión seguida debe no recompilar: comparar tiempo hasta el primer frame jugable y ausencia de stutter, como sugiere el propio oficial para Ampere (`known_issues.json:18-19`).

### M6 — [P1] Prompts adaptativos ratón↔mando (`mouse_prompt_distance`, `mouse_lock`)

- **Qué:** que los avisos de botones en pantalla cambien de glifos de mando a teclas/ratón según el último dispositivo usado, con distancia configurable; y opciones de captura del ratón.
- **Evidencia GOW3:** `mouse_prompt_distance` (16–200 px, por defecto 48) y `mouse_lock`, parseados y persistidos (`gow3_settings.cpp:126-129`).
- **Estado nuestro:** hay glifos por mando (`pad_glyphs`, `infamous_run.py:636,707-709`) y ajustes de ratón ricos (sensibilidad, suavizado, aceleración, deadzone: `padsettings.py:109`, `qt/controls.py:14`), pero **no** existe ninguna clave de distancia/auto-cambio de prompts (búsqueda sin resultados en `port/launcher`).
- **Acción:** añadir la clave (nombre propuesto: `mouse_prompt_distance`, espejo del GOW3) + documentarla en `docs/controls.md`; es puramente UX y barato.
- **Verificación:** alternar mando/ratón en el menú y comprobar que los glifos cambian tras mover el ratón N px.

### M7 — [P1] Resoluciones y parches aportados por mods, visibles en el menú

- **Qué:** que un mod pueda traer sus XML de parche (p. ej. "Resolution Patch — 21:9") y que esas resoluciones aparezcan en la lista de render del juego y del menú.
- **Evidencia GOW3:** `run.py` recolecta los nombres de resolución de los XML de los mods habilitados y los pasa como `GOW3_EXTRA_RESOLUTIONS` (`run.py:187-195`); los mods pueden llevar `patches/` aplicados solo mientras están activos (`mods.py:1-6`).
- **Estado nuestro:** selección de cheats por `APP_VER` (`infamous_run.py:58-65,283`) y mods externos; **no** se encontró el flujo "mod → resolución extra" (`extra_resolutions`/`resolution_names`/`ultrawide` sin coincidencias en `port/launcher`).
- **Acción:** evaluar utilidad real en inFAMOUS (¿hay demanda de ultrawide?) antes de portar; si se hace, integrarlo con `mod_patches.external_*` existente.
- **Verificación:** mod de prueba con un XML de resolución; comprobar que aparece en el menú y que renderiza el tamaño correcto. **Nota 4 GB:** en GOW3 los perfiles altos inflan la arena de vídeo; vigilar VRAM.

### M8 — [P1] Record/replay en el protocolo QA del Gate G0

- **Qué:** usar ya-basado en código existente: grabar 2–3 recorridos de escena (F9 inicio/fin) y reutilizarlos para A/B, regresiones y evidencia repetible.
- **Evidencia GOW3:** mismo mecanismo (`GOW3_PAD_FILE/RECORD/REPLAY`, `runtime_pad.c:506-548`) — confirma que el enfoque es estable en este linaje.
- **Estado nuestro:** implementado completo (`TLG_PAD_FILE/RECORD/REPLAY`, `port/src/runtime_pad.c:815-862`) incluyendo tokens de sensor (`tilt_right`, `tilt_left`, `shake`, `:851-853`), que permiten automatizar también la escena de grafiti/sensor.
- **Acción:** no es código nuevo: incorporarlo al harness (`docs/qa-harness.md`) y usar los mismos recorridos en las 3 repeticiones que exige `AGENTS.md` §5.
- **Verificación:** reproducir una escena grabada dos veces y comparar métricas (deben ser del mismo tramo).

### M9 — [P2] Menú: avisos de reinicio y opciones del HUD que faltan

- **Qué:** (a) mostrar cuántos cambios exigen reinicio con aviso de pérdida de progreso y botón deshabilitado si no hay comando de reinicio; (b) revisar opciones del overlay (escala 50–300 %, esquina 0–3, opacidad 0–100, disposición) frente a las nuestras.
- **Evidencia GOW3:** "(N) changes need a restart (unsaved progress is lost)" + `BeginDisabled(!GOW3_RESTART_COMMAND)` (`gow3_overlay.cpp:655-667`); opciones `overlay_scale/corner/opacity/layout` (`gow3_settings.cpp:40-53`) e ítems por defecto = FPS+FrameTime+Upscaler (`gow3_settings.h:46`).
- **Estado nuestro:** contamos cambios para reinicio (`tlg_overlay.cpp:686`) y tenemos `overlay_corner` en las claves del menú (`backports_table.py:14`); `show_fps` ya arranca on (`infamous_run.py:236`); los ítems por defecto son Fps+FrameTime+GPU+VRAM (`tlg_perf_stats.h:31`) — razonable en 4 GB, pero conviene revisar si faltan los avisos del punto (a).
- **Acción:** verificar el UI actual y añadir lo que falte; bajo costo.
- **Verificación:** cambiar `stale_readback` en el menú y comprobar el contador + el aviso.

### M10 — [P2] Tabla local de "problemas conocidos" por hardware en nuestro launcher

- **Qué:** adoptar el patrón del oficial: una tabla local (nada se descarga) que, según vendor/GPU/VRAM/driver, muestre consejos en la pantalla de Soporte; las condiciones se evalúan con lo que ya reporta el probe.
- **Evidencia oficial:** `known_issues.json` con match por `os/vendor/gpu(regex)/driver/vram_gb_max`; entradas: **4 GB de VRAM** ("la memoria de textura puede estourar; baje la resolución y mantenga el presupuesto de VRAM por debajo de la memoria de la placa", issues #72/#75, `:22-30`), **NVIDIA RTX 30** ("luces y humo pueden parpadear; use el perfil Recomendado y juegue dos sesiones seguidas: la primera recompila shaders", `:13-21`), **RX 580/Polaris** (BP09, `:4-12`).
- **Estado nuestro:** no existe; nuestro hardware objetivo (RTX 3050 4 GB) cae exactamente en dos de las entradas.
- **Acción:** copiar el esquema (JSON local + pantalla), redactado en es/en para nuestro launcher Qt; enlazar con los presets (`preset_share.py`, `presets_*`).
- **Verificación:** en la 3050 debe aparecer el aviso de 4 GB y el de RTX 30; en una AMD, el de BP09.

---

## 5. Descartado / no aplica (anti-cargo-cult)

- **Prompts de QTE con stick rotatorio (`GOW3_STICK_PROMPTS`):** específico de los quick-time-events de GoW III; el oficial no documenta ese tipo de QTE en inFAMOUS, y nuestro asistente de sensor/grafiti ya cubre las escenas de movimiento (`port/src/runtime_pad.c:322-345`). No portar.
- **Cheats de GoW III** (orbes rojos/verdes/azules/oro, doble salto, "very hard", barra de vida enemiga, auto-QTE, multiplicadores de daño): `gow3_settings.cpp:136-151`; son del juego, no del port. No aplica.
- **Parches XML de resolución/FPS de GOW3:** sus firmas (`Pattern/At`) apuntan al `eboot` de GoW III (`scripts/patches.py:1-7,47-49`); no son portables a inFAMOUS. Lo reutilizable es el *flujo* (firmas + parches en memoria + evitar `pattern`/`mask`), que ya está en nuestro linaje. El "arreglo de corrupción de texturas" de GOW3 (arena en `0x90000000`, DMEM ≥ 2048) es del juego, no una técnica pendiente en inFAMOUS.
- **Touchpad GOW3→clic:** ya heredado; no hay nada nuevo que traer (`CLICK_LEAD/HOLD/TAIL`, `runtime_pad.c:104`).
- **`GOW3_GC_BUDGET_MB` como el único camino:** no copiar el "forzado solo para pruebas" sin más; nuestro `ApplyResidencyHeadroom` + `min` con el driver es mejor base; lo que falta es el modo `auto` en el launcher (M1).

---

## 6. Reproducción y verificación mínima

Todo es solo lectura/ejecución del propio dump del usuario; no descargar nada.

```powershell
# A. Comprobar las fuentes citadas (GOW3 @41e9473)
git -C C:\Users\luisd\proyectos\gow3-lab log -1 --format=%H   # -> 41e9473...
git -C C:\Users\luisd\proyectos\gow3-lab grep -n "CLICK_LEAD_US"        -- src/runtime_pad.c
git -C C:\Users\luisd\proyectos\gow3-lab grep -n "stale_readback"       -- gpu/shim/gow3_settings.h
git -C C:\Users\luisd\proyectos\gow3-lab grep -n "GOW3_GC_BUDGET_MB"    -- gpu/shadps4/video_core/texture_cache/texture_cache.cpp
git -C C:\Users\luisd\proyectos\gow3-lab grep -n "prompt_distance"      -- gpu/shim/gow3_settings.cpp
git -C C:\Users\luisd\proyectos\gow3-lab grep -n "EXTRA_RESOLUTIONS"    -- run.py

# B. Log de referencia del run GOW3 en la misma 3050
Select-String -Path "C:\Users\luisd\Documents\Codex\2026-10-17\editar-para-aclarar-lo-de-nativamente\outputs\gow3-windows\user\last_run.log" `
  -Pattern "deferred image readback|Object motion|on demand|multisampling|budget="

# C. Probar GOW3 con las variables en discusión (dist local; no toca el dump de inFAMOUS)
cd C:\Users\luisd\Documents\Codex\2026-10-17\editar-para-aclarar-lo-de-nativamente\outputs\gow3-windows
$env:GOW3_OBJECT_MOTION='1'; $env:GOW3_GC_BUDGET_MB='3500'; python run.py

# D. Nuestro fork — A/B de los puntos M1/M2/M5 (log en port\out\logs\run-*.log)
$env:TLG_GAME_DIR = 'C:\Users\luisd\Documents\Codex\2026-09-29\trabajemos-en-esto-quiero-crear-una\work\games\CUSA00223-view'
$env:TLG_DATA_DIR = 'C:\Users\luisd\proyectos\infamous-port\port'
$env:ISS_USER_DIR = 'C:\Users\luisd\proyectos\infamous-port\userdata'
$env:TLG_STALE_READBACK='1'      # A/B de M2 (0 vs 1, 3 repeticiones)
$env:ISS_GC_BUDGET_MB='3300'     # M1: comparar 3300/3500 con el modo por defecto
$env:ISS_PIPELINE_BINARIES='0'   # M5: mantener off en el baseline (crash 595)
python C:\Users\luisd\proyectos\infamous-port\port\run.py

# E. Grabación/reproducción de escena para A/B repetible (M8)
#    F9 inicia y termina la grabación; después, en otra sesión:
$env:TLG_PAD_FILE  = 'C:\ruta\guion.txt'      # con el token: replay
$env:TLG_PAD_REPLAY= 'C:\ruta\grabacion.txt'  # líneas: ms buttons LX LY RX RY
```

Criterios de aceptación por experimento (los del proyecto): mediana y p95 de frametime, VRAM máxima y frecuencia de GC agresiva en el log; 3 repeticiones o evidencia equivalente (`AGENTS.md` §5). Para M2/M4 revisar además artefactos visuales (humo/partículas) en captura.

---

## 7. Atribución y licencias

- `FelipePrado0/gow3_pc` — **GPL-2.0-or-later**; se usó como referencia de lectura. Cualquier adaptación de comportamiento a nuestro fork debe mantener `LICENSE` y atribución a FelipePrado0, shadPS4 y bloodborne_pc según corresponda.
- `HayatoG/iss_pc` (nuestro upstream), release `v2026.10.19-d3995fe` — **GPL-2.0**; los consejos citados (`README-player.md`, `known_issues.json`) se toman solo como especificación de comportamiento a reimplementar, no como código.
- Nuestro fork ya declara la cadena en `port/UPSTREAM.md` (base `gow3_pc` @ `9a6d17c`) y `AGENTS.md` §6; este documento no cambia licencias.
- **No se copió código** de los proyectos citados: todas las propuestas son descripciones de comportamiento con referencias `archivo:línea` para verificación independiente.
