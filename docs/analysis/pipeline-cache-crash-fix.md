# Caché de pipelines del driver (BP08): persistencia a prueba de crashes

**Fecha:** 2026-10-19
**Alcance:** `port/gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp` y
`vk_pipeline_cache.h` (BP08), más este documento. No se tocó nada más.
**Estado:** parche aplicado y compilado (`bash build.sh` exit 0, MSYS2 CLANG64). **No se ejecutó
el juego**: la validación en ejecución la hace el usuario (sección 6). GPL-2.0.

---

## Resumen ejecutivo

1. **Los dos crashes citados no vienen de la caché de pipelines.** El fallo medido es idéntico en
   ambos (`read access to 0000000800da08c0 ... tlg-probe.exe+0x361b0`) y ocurre en
   `port/gpu/shim/tlg_view.cpp:26`, en el hilo de vista, leyendo la tabla LOD del eboot en
   `0x0800da08c0`; ver sección 1. Uno de los runs que crasheó lo hizo **sin ningún `.vkc` en
   disco** ("none saved for this GPU/driver"), así que el blob queda descartado como causa.
   Este parche **no arregla** ese fallo; lo que hace es blindar la persistencia del blob.
2. **El código anterior ya escribía `tmp` + `rename` y validaba la cabecera, pero tenía huecos:**
   el temporal era un nombre fijo, no había `fsync` antes del rename, no había tope de tamaño al
   leer (un `file_size` corrupto iba directo a un `std::vector`), un archivo dañado se conservaba
   y se releía entero en cada arranque, no había ninguna prueba de que el archivo se hubiera
   escrito completo, y los `.tmp` huérfanos nunca se limpiaban. Detalle en sección 2.
3. **Ahora la carga valida tamaño mínimo/máximo, magia/versión, tamaño declarado vs tamaño real,
   XXH3 del payload y una marca de escritura completa (`<blob>.clean`)**. Cualquier fallo mueve el
   archivo a `<blob>.stale` y la sesión arranca en fresco, sin abortar nada.
4. **La escritura es atómica y durable:** temporal con nombre único, `Commit()` (fflush +
   `_commit`/`fsync`), y `MoveFileExW(..., REPLACE_EXISTING | WRITE_THROUGH)` en Windows. Un
   `taskkill /F`, un kill -9 o un corte de luz dejan el archivo anterior o ninguno, jamás la
   mitad del nuevo.
5. **Desviación consciente y documentada (sección 4):** la marca se escribe también tras cada
   guardado con estado `Running`; una marca `Running` con hash válido **sí se carga** por defecto.
   El pedido literal (“sin cierre limpio no se carga”) está disponible con
   `TLG_DRIVER_CACHE_STRICT=1`, pero aplicarlo por defecto desactivaría BP08 en este port, porque
   el cierre de ventana usa `std::_Exit(0)` y el destructor no corre nunca (más detalle abajo).

---

## 1. Reproducción y análisis de los crashes reportados

Evidencia directa:

| Run | Blob al arrancar | Resultado |
|---|---|---|
| `evidence/20261019-104920-motion-test-fresh-180s` | **ninguno** (`none saved for this GPU/driver`) | fault a ~4.7 s de "Starting linked module 0", `early_exit`, código 3 |
| `evidence/20261019-105508-motion-test-2-300s` | presente (de un run anterior) | fault a ~4.7 s, idéntico |

Cadena del fallo (misma en ambos):

```
stderr: Fault: read access to 0000000800da08c0 at tlg-probe.exe+0x361b0 (thread 19512)
        #0 tlg-probe.exe+0x368a5
result.json: "log_issues": ["Fault: read access to 0000000800da08c0 ..."], status early_exit
```

- El offset `tlg-probe.exe+0x361b0` desensambla a `movss 0xda08c0(%rcx),%xmm3` con
  `rcx = 0x0800000000` (base de la imagen del juego). Es la **primera fila de la tabla LOD del
  eboot** (`TLG_VIEW_LOD_TABLE = 0x0800da08c0`), leída por `ViewHook::Put` en
  `port/gpu/shim/tlg_view.cpp:26`, llamado desde `Apply` (línea 36) en el hilo de vista
  desconectado.
- `Rasterizer::ReadMemory` (`vk_rasterizer.cpp:3865-3889`) solo es fatal cuando `!IsMapped`: esa
  lectura cae en una página sin acceso/pin en el rastreador de GPU, y `probe.c` convierte el
  fallo en `TerminateProcess(..., 3)`. `probe.c:1317-1320` protege la imagen con
  `PAGE_NOACCESS` y luego aplica los permisos por segmento: la ventana es real.
- **Contraevidencia de la caché:** el run `20261019-104920` crasheó **sin blob**; y los runs que
  sí cargaron un blob (`20261019-101754-fps-hud-smoke`, 45 s; `20261019-102015-g0-auto-720p-b3500`,
  960 s, 11,157,026 bytes cargados) **no fallaron**. Los runs BP08=0 (`105829`, `110706`,
  `111234`) tampoco. Conclusión: el `.vkc` no es la causa de ese fault.

Por tanto, este parche **no cambia el comportamiento de render** ni pretende arreglar ese crash;
elimina la clase de fallos que sí podría causar el archivo de caché (archivo a medias, dañado o
ajeno) y deja evidencia de cada rechazo.

## 2. Causa raíz de lo que sí arregla el parche (código anterior)

1. **Temporal con nombre fijo** (`<file>.tmp`): dos procesos (o un crash entre escrituras)
   podían pisarse el temporal; un `taskkill` lo dejaba huérfano para siempre.
2. **Sin durabilidad real**: `std::filesystem::rename` sin `fsync` ni `MOVEFILE_WRITE_THROUGH`;
   el rename podía “completarse” y perderse con un corte de luz.
3. **Sin tope de tamaño en la lectura**: `std::vector<u8> file(std::filesystem::file_size(path))`
   con el valor que devolviera el FS; un archivo gigante o corrupto intentaba asignarse entero
   (hasta reventar por OOM) antes de validar nada.
4. **Sin prueba de escritura completa**: cualquier `.vkc` (truncado, copiado, alterado) entraba al
   mismo camino de validación; si fallaba, se imprimía “ignored (…kept)” y el archivo **se
   conservaba**, así que cada arranque repetía la lectura y el parseo del driver.
5. **`.tmp` huérfanos** de writers muertos nunca se limpiaban.

## 3. Formato en disco y reglas de validación

```
<blob>            = DriverCacheHeader (24 B) + payload
DriverCacheHeader = { u32 magic "IVCK" (0x4B435649); u32 version = 1;
                      u64 payload_size; u64 payload_hash = XXH3-64(payload) }
payload           = VkPipelineCacheHeaderVersionOne (cabecera del driver) + datos

<blob>.clean      = DriverCacheMarker (32 B)      # marca de escritura completa
DriverCacheMarker = { u32 magic "TLCM" (0x4D434C54); u32 version = 1;
                      u32 state (0 = Running, 1 = Clean); u32 reserved = 0;
                      u64 payload_size; u64 payload_hash }
```

**Al cargar (`LoadDriverCacheBlob`, `vk_pipeline_cache.cpp:584`)**, en orden:

1. Limpieza best-effort de temporales `<blob>.*.tmp` huérfanos.
2. Sin archivo → “none saved for this GPU/driver” (igual que antes).
3. Tamaño mínimo `24 + sizeof(VkPipelineCacheHeaderVersionOne)`; máximo 4 GiB fijo
   (`DriverCacheMaxLoadBytes`; no depende de `TLG_DRIVER_CACHE_MAX_MB`, para que bajar ese tope
   no invalide un archivo ya guardado).
4. Marca `.clean` (32 B exactos, magia/versión/reservado):
   - Ausente y sin `TLG_DRIVER_CACHE_TRUST_DIRTY=1` → fresco.
   - Inválida → fresco.
   - `payload_size` de la marca ≠ tamaño real del blob − 24 → fresco.
5. Estado `Running` + `TLG_DRIVER_CACHE_STRICT=1` → fresco.
6. Cabecera del contenedor: magia, versión, `payload_size == file_size − 24`, XXH3 del payload.
7. Si hay marca: su `payload_hash`/`payload_size` deben coincidir con los de la cabecera.
8. Cabecera Vulkan: `headerVersion`, `vendorID`, `deviceID`, `pipelineCacheUUID` del dispositivo
   actual.
9. Cualquier rechazo mueve el blob a `<blob>.stale` (una copia por clave, la anterior se
   reemplaza), borra su `.clean` y escribe
   `GPU: driver pipeline cache corrupt; starting fresh (moved to …)`. La sesión sigue sin caché
   (recompila como cuando no hay archivo) y **nunca falla**.

**Al guardar (`SaveDriverCacheUnchecked`, `vk_pipeline_cache.cpp:688`)**:

1. Temporal único `<blob>.<ticks>.<seq>.tmp` en el mismo directorio.
2. Escritura de cabecera + payload, `Commit()` (fflush + `_commit` en Windows / `fsync` en POSIX).
3. `DriverCacheRename` = `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` (o `rename` en POSIX).
   Si algo falla, se borra el temporal y el destino queda intacto.
4. Tras el rename, se escribe/actualiza `<blob>.clean` con el hash y tamaño del blob y el estado
   (`Running` en guardados periódicos, `Clean` en el cierre ordenado del destructor). Un fallo al
   escribir la marca solo se avisa: el blob queda sin marca y la próxima sesión arrancará fresca,
   que es la opción segura.
5. Se conservan `TLG_DRIVER_CACHE_MAX_MB`, `TLG_DRIVER_CACHE_SAVE_S`, el hash “sin cambios no se
   reescribe” y el arranque en fresco si no hay caché válida. `TLG_BP08=0/1` no cambia.

## 4. Semántica de la marca y desviación consciente del pedido literal

- Lo pedido (“la marca se escribe **solo** en cierre ordenado; sin marca limpia el blob se
  ignora/renombra”) está implementado como **modo estricto**: `TLG_DRIVER_CACHE_STRICT=1`.
  Con esa variable, un blob con marca `Running` o sin marca se rechaza igual que uno corrupto.
- **Por defecto** se carga una marca con hash coincidente aunque el estado sea `Running`. Razón
  medida en el código de este port: el cierre de ventana termina en
  `runtime_report(); TlgPerf::Stop(); std::_Exit(0)` (`port/gpu/shim/tlggpu.cpp`), `probe.c` usa
  `_exit`/`TerminateProcess` en los demás caminos y `fail("entry unexpectedly returned")` si la
  entrada retorna; **el destructor de `PipelineCache` no corre en una sesión normal**. Si la
  marca `Clean` fuera el único criterio, BP08 no cargaría jamás ninguna caché y la función
  quedaría desactivada de facto.
- La marca es, en la práctica, un **registro de commit**: prueba que el blob se escribió entero,
  con durabilidad, y con ese contenido exacto (hash). El estado `Clean`/`Running` solo distingue
  *cómo terminó la sesión*, no la integridad del archivo. Por eso el valor por defecto es seguro
  frente a kills y a la vez útil.
- Alternativa futura, fuera del alcance de este parche: escribir la marca `Clean` desde el camino
  de salida del shim (`tlggpu.cpp`), antes del `std::_Exit(0)`. Con eso, `STRICT=1` podría ser el
  comportamiento por defecto sin perder BP08.
- `TLG_DRIVER_CACHE_TRUST_DIRTY=1` acepta blobs **sin marca** (para las cachés ya existentes de
  la versión anterior), siempre con hash y tamaños validados. `probe.c` aliasea `ISS_* → TLG_*`,
  así que ambos nombres funcionan.

## 5. Cómo se validó sin jugar

- **Compilación completa**: `bash build.sh` (MSYS2 CLANG64) → exit 0; `tlg-probe.exe`,
  `tlg-gpu-capabilities.exe`, `tlg-play.exe` relinkeados. La TU modificada compila sin warnings
  nuevos (los 2 warnings del build están en `tlg_overlay.cpp`, preexistentes).
- **Revisión estática** del flujo completo: rutas de rechazo (todo rechazo pasa por
  `RejectDriverCacheBlob`), rutas de escritura (todo pasa por `DriverCacheWriteFile`), caminos del
  destructor y del hilo saver (serializados por `StopDriverCacheSaver` antes del guardado final).
- **No se ejecutó el juego** (restricción explícita), así que la validación en ejecución es la
  sección 6.

## 6. Qué probar en ejecución real (A/B, sin jugar partida)

Preparar: `$dir = <ruta>\cache\driver-CUSA00223`; el archivo y la marca se reconocen por
`vk_pipeline_cache_*_*.vkc` y `*.vkc.clean`. Sugerido `ISS_DRIVER_CACHE_SAVE_S=10` para no
esperar 60 s entre guardados.

1. **Guardado + kill (la clase de fallo que se quiere cubrir).** Lanzar con BP08=1; esperar en
   stdout `tlg: driver pipeline cache saved (…)`; matar con `taskkill /F /IM tlg-probe.exe`.
   Comprobar: existe `*.vkc`, existe `*.vkc.clean`, **no** hay `.stale` ni `.tmp`. Relanzar:
   debe aparecer `GPU: driver pipeline cache loaded (… bytes, no clean shutdown recorded, hash
   verified)` y el run continúa normal.
2. **Blob corrupto.** Con el juego cerrado, copiar el `.vkc` a `backup`, alterar un byte del
   payload (`fsutil`/hex editor) o `truncate -s -1`. Relanzar: `GPU: driver pipeline cache
   corrupt; starting fresh (moved to …vkc.stale)` y aparece el `.stale`. Restaurar el backup si
   se quiere volver atrás.
3. **Marca ausente (semántica literal).** Borrar `*.vkc.clean`. Relanzar: `has no clean shutdown
   marker; starting fresh`. Con `TLG_DRIVER_CACHE_TRUST_DIRTY=1` en su lugar: `loaded (… no
   marker, trusted …)`.
4. **Modo estricto.** Tras un kill (`Running` en la marca), relanzar con
   `TLG_DRIVER_CACHE_STRICT=1`: `last session ended without an orderly shutdown (strict);
   starting fresh`.
5. **Camino feliz / sin caché.** Borrar `driver-CUSA00223` entera: `none saved for this
   GPU/driver`; el juego recompila como siempre; con el primer guardado reaparecen `.vkc` y
   `.clean`.
6. **Cierre normal de ventana.** Esperar a que el juego cierre por la X: la marca quedará
   `Running` (por el `std::_Exit(0)` del shim, no es un fallo del parche); con `STRICT=1` la
   siguiente sesión arrancará fresca.

## 7. Riesgos y límites

- **Una recompilación la primera vez**: la caché existente no tiene marca, así que por defecto se
  moverá a `.stale` en el primer arranque tras el parche. Con `TLG_DRIVER_CACHE_TRUST_DIRTY=1` se
  conserva (una vez; a partir del primer guardado ya hay marca).
- **`STRICT=1` deja BP08 casi inútil en este port** (sección 4) hasta que el shim escriba la
  marca en la salida; es una variable de diagnóstico, no el modo recomendado.
- `.stale` conserva solo la última copia rechazada; es evidencia, no un histórico.
- La limpieza de `.tmp` asume un proceso por directorio de datos (el flujo normal del port); con
  dos instancias simultáneas podría borrar un temporal vivo de la otra.
- El tope de 4 GiB al cargar es fijo; no se deriva de `TLG_DRIVER_CACHE_MAX_MB` (a propósito).
- **No arregla el fault de `tlg_view.cpp:26` / tabla LOD** descrito en la sección 1; ese fallo
  sigue vivo y es independiente del blob.
