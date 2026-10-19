# Fault del hilo de vista (tabla LOD) — causa y blindaje

> **Fecha:** 2026-10-19 · **Alcance:** `port/gpu/shim/tlg_view.cpp` (draw distance). No se ejecutó el juego;
> la validación en vivo queda para el run del usuario.
> **Evidencia del fallo:** `evidence/20261019-104920-motion-test-fresh-180s` y
> `evidence/20261019-105508-motion-test-2-300s` (`run.log:102`, `stderr.log`): el run muere a los ~5 s.

## 1. Síntoma

```
Fault: read access to 0000000800da08c0 at tlg-probe.exe+0x361b0 (thread 4728)
  rcx=0000000800000000 rdx=000000003f800000 ...
```

- `0x0800da08c0` = base de imagen guest (`0x800000000`) + `0xda08c0` = **primera fila de la tabla LOD**
  (`TLG_VIEW_LOD_TABLE`, `port/src/view_hook.h:17`).
- `rdx = 0x3f800000` = `1.0f`, el valor de esa fila (`tlg_view_lod_original[0]`).
- El fault es la **lectura de `Put()`** (`tlg_view.cpp:26`), en el hilo de vista creado por
  `tlg_view_attach` (`tlg_view.cpp:67`), ejecutado en el proceso del probe.

La tabla estaba mapeada al adjuntar el hook (el log imprime `Draw distance: ready` justo después de
leerla en la firma, `run.log:71-72`); minutos/segundos después esa página estaba inaccesible para el
hilo de vista y, como el hilo no tenía punto de recuperación, el fault terminó el proceso.

La causa exacta de la ventana de inaccesibilidad queda **abierta**: el tracker de páginas de la GPU
(`Core::Signals::DispatchAccessViolation`, vía `tlggpu_handle_fault`) no reclamó el fault, y una
página de la imagen del juego protegida transitoriamente es la hipótesis principal. Lo que este
parche garantiza es que, sea cual sea el origen, el hilo de vista **no puede matar el run**.

## 2. Cambio

`tlg_view.cpp`: nuevo `ApplySafely()` (patrón ya usado en `vk_frame_record.cpp:75` y
`vk_pipeline_cache.cpp:1086`):

- `sigsetjmp(recover, 0)`; con `runtime_fault_recover = &recover` se llama `Apply()` y se limpia el
  puntero si termina.
- Si una lectura/escritura de `Apply()` faulta, el VEH del probe (`probe.c:227`) reanuda el hilo en
  `tlg_longjmp` y `sigsetjmp` devuelve distinto de cero: la pasada se descarta.
- El bucle reintenta cada 100 ms; un fallo transitorio se recupera solo. Se imprime un aviso la
  primera vez de cada episodio y nada más (`Draw distance: the game's LOD page was unreadable; the
  next tick retries`).

El `Apply()` inicial de `tlg_view_attach` no se toca: corre antes del arranque del juego, después de
que la firma ya leyó esa memoria con éxito.

## 3. Verificación

- Compilación del TU con las banderas reales del build: `ninja CMakeFiles/tlggpu.dir/shim/tlg_view.cpp.obj`
  → OK. La compilación completa quedó pendiente del cierre del agente de input (`runtime_pad.c`).
- En vivo (run del usuario): buscar el aviso en el log; ausencia de `Fault: read access to
  ...da08c0`. Si el aviso aparece y no se repite, el episodio fue transitorio y el draw distance
  siguió aplicándose.

## 4. Relación con otros hallazgos

- `docs/analysis/bc1d-auto-fix.md` atribuye el mismo fault al bug de texturas BC 1D en NVIDIA. Ambas
  cosas conviven: el BC 1D automático (`vk_instance.cpp`) elimina la causa de corrupción de texturas
  que pudo dejar el juego en un estado raro, y este parche elimina el camino fatal del hilo de vista
  pase lo que pase con las páginas.
- `docs/analysis/pipeline-cache-crash-fix.md` concluyó, con el mismo par de runs, que el `.vkc` no era
  la causa; esta nota cierra el cabo suelto que aquel informe dejó abierto.
