# Delta de código: nuestro fork vs `gow3_pc` — estado y hoja de ruta

> **Tipo:** ingeniería inversa de solo lectura sobre dos repositorios (nuestro fork y `FelipePrado0/gow3_pc`).
> No se ejecutó el juego, no se descargó contenido de Sony/Sucker Punch y no se copió código: las acciones
> propuestas se describen por comportamiento y deben re-implementarse en nuestro árbol con su estilo y nombres.
> **Fecha:** 2026-10-19 · **Máquina de referencia:** RTX 3050 Laptop 4 GB, i5-13420H, 16 GB, Windows 11.
> **Complementa a:** `docs/analysis/gow3-playability-recon.md` (matriz de jugabilidad GOW3 / release oficial / nuestro fork).
> **Licencias:** `gow3_pc` (FelipePrado0) y nuestro fork parten de `bloodborne_pc` + shadPS4, GPL-2.0(-or-later).

---

## 1. Resumen ejecutivo

1. Nuestro `port/` nació como copia de `gow3_pc` en `9a6d17c` (2026-10-12) y sincronizó el core en
   `2a0eac6` (2026-10-13). GOW3 siguió avanzando hasta `41e9473` (2026-10-19, release 2.5.0): ~100
   commits después de nuestra última sincronización.
2. **La mayoría de esos commits ya están en nuestro árbol, portados por nombre a `TLG_*`** — varios de
   nuestros comentarios citan literalmente `gow3_pc #331`, `#348`, `#386`. Verificado uno a uno con
   `archivo:línea` (§3). No estamos "por detrás": somos un fork hermano que sigue el core de cerca.
3. Quedan **tres huecos pequeños y accionables** (§4, P0): guardia del recompilador ante shader vacío,
   orden correcto de backups de saves cuando dos caen en el mismo segundo, y cola Vulkan con
   `eSparseBinding` para `bindSparse`.
4. Los huecos de jugabilidad que sí importan (presupuesto de VRAM, `stale_readback`, preflight de
   pagefile) ya están analizados con prioridad en `gow3-playability-recon.md`; aquí no se repiten.
5. Features de UX de GOW3 **no portables tal cual** (menú de pausa, iconos de botones, parches de
   FPS/resolución): se proponen equivalentes propios, con coste y verificación (§4, P1-P2).
6. Descartados con motivo (§5): QTE automático, ultrawide/parches de resolución de GOW3, cheats de
   GoW III, extras de distribución.
7. Nuestro fork tiene módulos que GOW3 no tiene (runtime GR2/inFAMOUS, ratón y teclado, view hook,
   regiones, perf-trace, launcher Qt con presets por GPU). El rumbo futuro no es "alcanzar a GOW3",
   sino **cherry-pick selectivo por comportamiento** + seguir evolucionando lo propio (§6, §7).

---

## 2. Método (reproducible)

| Elemento | Valor |
|---|---|
| Copia GOW3 | `C:\Users\luisd\proyectos\gow3-lab` — `FelipePrado0/gow3_pc`, `main` @ `41e9473` (historia completa: 435 commits) |
| Nuestro repo | `origin` = `HayatoG/iss_pc` @ `fb7e452` (historia completa: 477 commits); árbol de trabajo `bootstrap/windows-g0` |
| Ancla de copia | `9a6d17c` (2026-10-12) — ver `port/UPSTREAM.md` |
| Ancla de sincronización | `2a0eac6` (2026-10-13) — merge #215 de GOW3 |
| Rango comparado | `git -C gow3-lab log --no-merges 2a0eac6..main` (~100 commits, hasta `41e9473`) |
| Verificación | por cada commit relevante: `git show <sha> --stat` y `git show <sha> -- <archivo>`; después `Select-String` del identificador o comentario equivalente en nuestro árbol |

Los clones eran superficiales (2 y 1 commits); se desprofundizaron con `git fetch --unshallow --tags`
(solo lectura, sin tocar ramas de trabajo).

---

## 3. Lo que GOW3 cambió tras el sync y **ya está** en nuestro fork

| Commit GOW3 | Cambio (comportamiento) | Evidencia en nuestro árbol |
|---|---|---|
| `a101edb` (#331) | Los readbacks que escriben memoria guest a través de la vista de respaldo invalidan las texturas encima (`NoteBackingWrite` / `ApplyBackingWrites`) | `port/gpu/shadps4/video_core/texture_cache/texture_cache.cpp:672,681`, `texture_cache.h:135-137`, `port/gpu/shim/tlggpu.cpp:154`; toggle `BackingInvalidate` en `tlg_toggles.h:91` (cita `gow3_pc #331`) |
| `b646683` (#348) | Caída de FPS con poderes/efectos: vistas parciales reutilizan la imagen cacheada en vez de crear y subir una nueva | `PartialHeightViews` en `tlg_toggles.h:88`, `texture_cache.cpp:204,839`; `ReportImageCreates` en `texture_cache.cpp:227,271`; `needs_native` en `vk_rasterizer.cpp:2825` |
| `88c32d9`-precedentes (#386) | Esperas de readback exactas (solo copias que comparten bytes) | `ExactReadbackWaits` en `tlg_toggles.h:94` (cita `gow3_pc #386`) |
| `081df6f` | Captura de frame: volcar la copia del ring, no la dirección guest (que puede ser 0 y crashear) | `vk_rasterizer.cpp:2681-2687` (comentario `tlg:` y `constant_ring->Data(ring->offset)`) |
| `4df3c7e` | `prepare.py` acepta ELF plano (dump GoldHEN), no solo SELF | `port/scripts/prepare.py:52` (`\x7fELF` → `parse_elf`) |
| `7468c19` | Stream buffer de Vulkan seleccionable; `cached` (RAM del sistema) por defecto en portátiles NVIDIA | `port/gpu/shadps4/video_core/buffer_cache/buffer.cpp:45` (`TLG_STREAM_MEMORY`), expuesto en `port/launcher/advanced_table.py:110` |

Además, nuestro `tlg_toggles.h` tiene bits que GOW3 **no** tiene hoy (DeferredReadback `1<<58`,
SceneMipBias `1<<57`, PipelinedCompute `1<<56`, PipelinedCommands `1<<63`, `VariantReuse` `1<<62` de
`i3-pc`), o sea que en el core vamos por delante en varias líneas de trabajo.

---

## 4. Huecos reales y plan de mejora

### P0 — pequeños, verificables ya (antes de la sesión manual G0)

> Nota de integración: durante este estudio apareció además un fault fatal del hilo de vista
> (`evidence/20261019-104920/105508`) que no venía de GOW3; ya está blindado
> (`docs/analysis/view-thread-fault-fix.md`).

**P0-A · Guardia del recompilador ante shader vacío** (GOW3 `368d899`) — **aplicado**
- Estado nuestro: `port/gpu/shadps4/shader_recompiler/recompiler.cpp:67` leía `code[0]` **sin**
  comprobar `code.empty()`; un shader vacío es un crash potencial.
- Hecho: guardia con `UNREACHABLE_MSG` (mismo comportamiento que GOW3) en `recompiler.cpp:64-68`;
  el TU compila con las banderas reales del build.
- Verificación en vivo: el run del usuario (defensa, sin cambio visual esperado).

**P0-B · Orden de backups de saves con colisión de segundo** (GOW3 `88c32d9`) — **aplicado**
- Estado nuestro: `save_backup.py:23` ordenaba solo por nombre; con varios backups en el mismo
  segundo (o al reutilizarse un `-N` tras la poda) la poda y `restore` podían quedarse con el
  antiguo.
- Hecho: `_backup_order` por `(timestamp, secuencia)` + numeración tras el mayor ya usado
  (`save_backup.py`); tests nuevos `test_same_second_orders_by_suffix` y `test_same_second_keeps_newest`.
- Verificación: `test_save_backup.py` 9/9 OK y `test_savemanager.py` 32/32 OK (2026-10-19).

**P0-C · Cola Vulkan con `eSparseBinding` para `bindSparse`** (GOW3 `195c7ab`) — **aplazado a post-G0**
- Estado nuestro: `buffer_cache.cpp:1642` hace `GetGraphicsQueue().bindSparse(...)`.
- Motivo del aplazo: es un fix de RADV (AMD); en NVIDIA no cambia el baseline y tocar la creación
  del device justo antes de la sesión manual añade riesgo sin beneficio medible. Queda como
  paridad para el preset RX 580, después del Gate G0.

### P1 — jugabilidad/UX, con equivalente propio

**P1-D · "Salir al escritorio" desde nuestro menú** (inspirado en `a553618`, no portable)
- GOW3 hookea el intérprete del menú de pausa del juego (firma de CUSA01623 en `menu_hook.h`); eso es
  RE por juego y no se puede copiar.
- Nuestro overlay (tecla `Insert`) no ofrece salir (grep sin resultados en `tlg_overlay.cpp`).
- Acción: añadir un ítem "Salir al escritorio" a la pestaña Game del overlay que pida al launcher un
  cierre limpio (nuestro runtime ya cierra ordenadamente y el launcher muestra el estado del run).
- Verificación: salir desde overlay → el run queda como cierre limpio (sin `Fault:`, sin cachés sucias).

**P1-E · Parches de FPS de motor y resolución: infraestructura sí, XMLs no**
- Lo que ya hay: `port/scripts/graphics_options.py:13-18` (resoluciones y `ENGINE_FPS = 60/120/240`),
  `port/run.py:193` aplica `applied_engine_fps(names)` y exporta `TLG_ENGINE_FPS`; `patches.py:123`
  reserva `EXCLUSIVE_PREFIX = 'Resolution Patch'`.
- Lo que falta: **el XML del parche**. `port/patches/` solo envía `iss_builtin_intro.xml`, así que un
  "Frame Rate Patch - 120 FPS" resuelve a 60 y una "Resolution Patch" no aplica: la opción existe pero
  no hace nada hasta que haya XML auditado.
- Acción: (1) buscar el parche comunitario de CUSA00223 en la base de parches de shadPS4, (2) auditarlo
  contra nuestro `mod_patches.py` (title ID + versión + segmentos del eboot) y enviarlo como opt-in,
  o bien (3) autorarlo con RE del limitador del eboot. Si nada de eso, ocultar la opción hasta que
  exista para no prometer lo que no ocurre.
- Valor: el salto 30→60 FPS es la mejora jugable más grande que queda, pero en 4 GB debe medirse con
  p95/VRAM antes de recomendarla.

**P1-F · VRAM: presupuesto automático + preflight de pagefile**
- Ya cubierto con prioridad en `gow3-playability-recon.md` (M1, M3); no se repite aquí. Nuestro
  launcher fuerza `ISS_GC_BUDGET_MB=3500` (`port/launcher/infamous_run.py:221,689`); GOW3 adapta el
  presupuesto al driver. Es el punto donde nuestro fork decide su viabilidad en 4 GB.

### P2 — pospuestos, post-G0

- **P2-G · Iconos de botones en el juego** (`e25254e`, `9155cdc`, `66a33a2`): GOW3 sustituye el atlas
  de glifos PS4 del juego por los de tu mando/teclado. Nosotros mostramos la guía en el overlay
  (`tlg_overlay.cpp`) y en `docs/controls.md`; reescribir el atlas es RE/arte por juego. Posponer:
  los prompts en pantalla seguirán siendo de PS4.
- **P2-H · Diagramas de mando en el menú** (`4ccf29d`): cubierto por `docs/controls.md`; no urge.
- **P2-I · Discord Rich Presence** (`96bb3c3`): no lo tenemos; cosmético. Descartable salvo petición.
- **P2-J · Ajustes del launcher en la carpeta del port** (`86be81d`): nuestro launcher Qt ya guarda
  presets y ajustes en la carpeta del port; sin acción.

---

## 5. Descartados con motivo (anti-cargo-cult)

| Cambio GOW3 | Por qué no |
|---|---|
| QTE automático (`41e9473`, `qte_hook.h`, rama `qte_automatico`) | Las firmas son del eboot CUSA01623 y el problema (QTE de rotación de stick) no existe igual en inFAMOUS; portarlo sería RE desde cero con otro objetivo |
| Ultrawide y parches de resolución (`f5b1589`, `e1cdd00`, `a190b61`, `970fdc2`) | Tocan direcciones y tamaños del arena de vídeo de otro juego; nuestros parches los escribiríamos desde el eboot de inFAMOUS (ver P1-E) |
| Cheats XML de GoW III, set de parches comunitario de GOW3 | Otra build; nuestro `infamous_patches.py` ya carga XMLs de mods con scope CUSA00223 |
| Fuente Cinzel, Steam shortcut, badges Trendshift, idioma polaco, filtro de aspecto de launcher | Distribución/cosmética sin efecto en jugabilidad |
| Fixes exclusivos de RADV/Linux (p. ej. parte de `195c7ab`) | La cola sparse sí se porta (P0-C) por corrección, pero el bug solo se reproducía en RADV |

---

## 6. Fortalezas propias que GOW3 no tiene (no perderlas)

- **Runtime del juego:** `runtime_gr2*.c` (motor GR2: audio 3D, font, json, np), `runtime_infamous.c`,
  `runtime_tlg.c`; hooks de actor/daño/orbes/vista (`view_hook.h`, `tlg_region.h`, `tlg_view.h`).
- **Input:** `mouse_stick.h`, `mouse_touch.h` (giro con ratón, touchpad sintetizado), remapeo
  teclado/ratón (`TLG_KEY_MAP`), asistente de sensor/grafiti.
- **Diagnóstico:** `tlg_perf_trace`, reporte de FREEZE, `tlg_perf_stats` (HUD F10).
- **Launcher Qt:** presets por GPU (`presets_apolar.py`, `presets_rx580.py`, `presets_amd.py`,
  `presets_view.py`), `savemanager`, `update_install`, `backports_table`, `precompile`, `padsettings`.
- **Core GPU con toggles más ricos:** bits vivos que GOW3 aún no tiene (DeferredReadback, SceneMipBias,
  PipelinedCompute/Commands, VariantReuse, SamplerSetMemo).

El flujo correcto es mantener estos módulos como la "vía inFAMOUS" y usar GOW3 solo como fuente de
fixes de core y de ideas de UX.

---

## 7. Cómo seguir mejorando en el futuro (rutina de sincronización)

1. **Ancla:** al integrar cambios de GOW3, actualizar `port/UPSTREAM.md` con el SHA exacto (hoy
   `2a0eac6`). Sin ancla, la próxima comparación se vuelve a hacer a mano.
2. **Delta:** `git -C C:\Users\luisd\proyectos\gow3-lab fetch --tags` y después
   `git log --no-merges <ancla>..main`; filtrar por `src/`, `gpu/shadps4/video_core/`, `gpu/shim/`,
   `scripts/` y `launcher/`.
3. **Clasificar** cada commit con la tabla de §3 como plantilla: ¿ya está? ¿portable por
   comportamiento? ¿firma de otro eboot (descartar)?
4. **Portar** un cambio = una variable, con build CLANG64 y 3 repeticiones de la misma escena antes de
   declarar mejora (AGENTS.md §5).
5. **Automatizar (post-G0):** un `tools/gow3-delta.ps1` que liste el rango del ancla y busque
   identificadores ya presentes ahorra la mitad de este trabajo; hasta entonces, este documento es la
   referencia.

---

## 8. Verificación de este documento

- Lista de commits: `git -C gow3-lab log --no-merges --date=short --pretty='%h %ad %s' 2a0eac6..main`.
- Cada fila de §3 se comprobó con `Select-String` sobre nuestro árbol; los números de línea pueden
  moverse con las ediciones en curso (p. ej. el agente de caché de pipelines está tocando
  `vk_pipeline_cache.*` ahora mismo).
- No se modificó código con este estudio; las acciones P0/P1 son propuestas, no hechos consumados.
