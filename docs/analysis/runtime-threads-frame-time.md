# Los hilos del invitado: dónde esperan y por qué el frame no avanza (2026-10-19)

Port nativo de inFAMOUS Second Son (CUSA00223), rama `bootstrap/windows-g0`.
Alcance de este informe: `port/src/runtime_thread.c`, `runtime_mutex.c`, `runtime_sema.c`,
`runtime_rwlock.c`, `runtime.h` y el banco `port/tests/test_guest_threads.c`.

Síntoma que se investigó (medido por el usuario el 2026-10-19, RTX 3050 4 GB, 720p, escena
SAFETY TRUST): el frame tarda 100-200 ms (5-12 FPS), la GPU está al 13-22 % (ociosa), el
proceso gasta ~4,6 núcleos lógicos (~460 ms de CPU por frame) y el pipe de dibujado del port
no es el cuello (stage A espera el 2 %; el grabador está ocupado el 32 % y gira en vacío el
67 %).

## 1. Respuesta corta

**El invitado no está esperando: está trabajando.** En la sesión de 100 s con el mapa de
esperas del port (`evidence/20261019-200829-wait-stats2/`), el hilo `main` del invitado —el
que lleva el frame— pasó **875,4 ms de 100 s bloqueado (8,8 %)**; el 91 % del tiempo estaba
**ejecutando código del juego**. Los cinco `JobWorker` estaban bloqueados ~4.840 ms cada uno
(48 % del tiempo, parked en **una sola** variable de condición), pero bloqueado no es
ocupado: es tiempo de espera por trabajo, con la CPU libre.

La prueba más fuerte son los dos *freeze reports* (`evidence/20261019-165749-720p-fsr3/` y
`evidence/20261019-140541-g0-manual-paint4/`), donde el proceso tenía **91 hilos** (18 del
invitado + 73 del port y del driver):

```
guest 'main' (tid 12768): running (not in a marked wait)
    now at guest+0x53b342, ...
  frames: 1817 flips submitted by the game (+0 since the last shown frame), 1817 shown,
          1817 presented (+0), 4809 vblank ticks (+0); page faults being handled now: 0
          (57368 handled, +36)
```

El hilo principal del invitado estaba **corriendo código del juego** mientras el juego no
mandaba ni un flip nuevo, el port no esperaba en ninguna parte (todos sus hilos: "no marked
wait") y la GPU estaba ociosa. En el otro freeze el mismo hilo estaba
`waiting on GPU thread (command) for 10797 ms (inside GPU page fault handler for 10812 ms)`:
el write-fault de la GPU (dominio del agente de GPU) es la otra forma que tiene el mismo
síntoma.

**Conclusión:** el cuello **está en el juego** (o en el camino de write-fault de la GPU, que
es del agente de GPU), no en la sincronización del port. Lo que faltaba era poder medirlo
por ventana y por hilo, y eso es lo que añade este informe (§2).

## 2. El instrumento: la contabilidad de esperas del invitado (lo que faltaba)

Antes de este trabajo el port medía las esperas de los hilos **del host** y las del
invitado sólo de forma agregada y acumulada (el "wait stats" antiguo: el número total de
esperas y la espera más larga *de toda la sesión*, por hilo, sin deltas y sin CPU). Con eso
no se puede razonar: en una sesión de 30 min los totales no dicen qué pasó en los últimos
5 s, y no dicen cuánto CPU usa cada hilo.

Ahora (`port/src/runtime_thread.c`, `runtime_mutex.c`, `runtime_sema.c`, `runtime.h`):

| Qué | Dónde | Detalle |
|---|---|---|
| Espera por **tipo** | `RuntimeWait.kind_count[]` / `kind_ns[]` | cond, mutex, sema, rwlock, join, sleep, event, file, **gpu**, other. El texto de la espera se mapea a un tipo por la **punta del texto** (los textos son estáticos), con una caché por hilo: la búsqueda cuesta ~2 ns |
| **Latencia de despertar** | `RuntimeWait.wake_ns/wake_max_ns/wake_hist[]` | del *post* (signal/broadcast) al instante en que el despierto vuelve a correr. El quewake pone el sello (`signal_ns`+`signal_seq` en el propio objeto) y el que espera lo mide |
| **Despertares espurios** | `RuntimeWait.spurious` | una espera que vuelve sin señal (bucle de re-chequeo del invitado) |
| **`scePthreadYield`** | `RuntimeWait.yield_count/yield_ns` | el *spin* del invitado: no es una espera, pero se come CPU. Ahora se cuenta aparte para que un hilo que gira no parezca inactivo |
| **`scePthreadJoin`** | marcado como espera "join" | antes no se marcaba: un hilo parado en un `join` salía como "running" |
| **CPU por hilo** | `RuntimeWait.cpu_ns` | `GetThreadTimes` (Windows) o `/proc/self/task/<tid>/stat` (Linux), muestreado por el monitor |
| **Hilos en el mismo objeto** | fin del informe de 5 s | el patrón "wait for all" del invitado se ve como un objeto con muchos hilos encima |

El informe de 5 s pasó de "acumulado desde el arranque" a **deltas por ventana**, con el
invitado y el host separados y el reparto de CPU del proceso:

```
Runtime: wait stats (last 5 s, TLG_WAIT_STATS): process CPU 9906 ms, guest 47 ms, host 0 ms,
        other 9859 ms, 5 threads
  guest benchHammer (tid 944): 12 waits, 173.8 ms blocked [mutex 173.8 ms], longest 61.7 ms in
        'mutex' 'benchMutex', cpu 0.0 ms
  guest benchWorker (tid 15480): 41 waits, 558.9 ms blocked [cond 558.9 ms], longest 555.4 ms
        in 'condition variable' 'jobCond' (11% of the window), wake 4/4 us p50/p95, cpu 0.0 ms
  5 threads are waiting on the same condition variable 'jobCond' 0x...
```

El informe de salida (`runtime_thread_report`) lleva el mismo mapa acumulado de la sesión.

**Coste**: con `TLG_WAIT_STATS` apagado (por defecto) el camino no cambia: el tipo, la
latencia y el histograma sólo se tocan dentro de la rama que ya comprobaba `wait_stats()`.
Con el interruptor encendido, el libro mayor cuesta ~130 ns por espera (dos lecturas del
reloj y el texto copiado), medido en 79,9 ns por par antes de este cambio.

## 3. El mapa de esperas del invitado (los números)

Sesión larga de 25,7 min en la escena SAFETY TRUST:
`evidence/20261019-213737-juego-usuario2/` (16.001 frames, 10,4 FPS de mediana, 299 de 303
ventanas de 5 s por debajo de 20 FPS).

| Contador del runtime | Total | Por frame | Por segundo |
|---|---|---|---|
| Esperas de condición variable | 1.211.703 | **75,7** | 787 |
| Despertares (signal+broadcast) | 1.114.036 | 69,6 | 723 |
| Locks de mutex | 29.558.958 | **1.847** | 19.194 |
| Lecturas de rwlock | 2.221 | 0,14 | 1,4 |
| Adquisiciones de semáforo | 78.586 | 4,9 | 51 |
| Hilos del invitado creados | 20 | | |
| Operaciones de memoria del invitado | 97.557.025 | 6.097 | |

Y el reparto de las esperas por hilo, de la sesión de 100 s con el mapa de esperas
(`evidence/20261019-200829-wait-stats2/`):

| Hilo del invitado | Esperas | Tiempo bloqueado (de 100 s) | Espera más larga |
|---|---|---|---|
| `main` | 1.937 | 875,4 ms (8,8 %) | 99,6 ms en 'GPU idle (submit done)' |
| `JobWorker1..5` | ~700 cada uno | **~4.840 ms cada uno (48 %)** | 617-632 ms en 'condition variable' |
| `MoviePlayer` | 428 | 4.235 ms | 602 ms en 'condition variable' |
| `MovieDecoder` | 1.148 | 3.220 ms | 354 ms en 'condition variable' |
| `SceFiosCallback` / `SceFiosIO` | ~300-373 | ~1.049 ms | 484 ms en 'SceFiosSchedulerWakeup' |
| `AK::BankManager` | 14 | 780,9 ms | 676 ms en 'event flag' 'AkEvent' |
| `Trophy` | 2 | 2,1 ms | 2,1 ms en 'event queue' |
| `GameState` | 8 | 0,2 ms | 0,1 ms en 'file open' |
| `AK::EventManager` | 1 | 0,0 ms | — (**running**, no espera) |

**Lectura**: el invitado crea 20 hilos, de los que 5 son `JobWorker` y uno es `main`. Las
esperas del invitado son *de trabajo disponible* (los workers esperando jobs), no de una
condición que nunca se cumple. `main`, que es el hilo que tiene que hacer avanzar el frame,
espera el 8,8 % del tiempo. El 91 % restante lo pasa ejecutando el juego.

## 4. La latencia de despertar (lo que pedía el encargo)

Banco reproducible: `port/tests/test_guest_threads.c` (`bash port/tests/run_guest_threads.sh`).
Reproduce la forma real del invitado (N hilos `scePthreadCreate` que sacan jobs de una cola
con mutex y se aparcan en **una** variable de condición) y mide el viaje de ida y vuelta
(post → job hecho = dos despertares + el mutex de la cola).

| Caso | Ida y vuelta p50 | p95 | máx |
|---|---|---|---|
| 1 worker, máquina libre | 6,3 µs | 16,2 µs | 119 µs |
| 5 workers, máquina libre | 6,2 µs | 20,9 µs | 118 µs |
| 5 workers + 12 hilos girando | 7,1 µs | 11,2 µs | 286 µs |
| 5 workers + 36 hilos girando | 6,9 µs | 10,2 µs | 154 µs |

Es decir, **un despertar del invitado cuesta 3-10 µs en una máquina descargada**, y el viaje
completo del sistema de jobs 6-20 µs. Los 100 µs que el encargo temía **no aparecen en el
hilo del invitado**: en el banco de sincronización del port (agente de GPU,
`port/tests/test_sync_latency.c`) el despertar de una condición con 8 esperadores es de
110 µs (p95 157 µs) y con uno solo de 18 µs — la diferencia es el *thundering herd* de la
condición, no el port.

**Consecuencia**: con 75,7 esperas de condición por frame y ~5 µs de latencia de despertar,
el frame gasta ~0,4 ms en despertares. **No es el frame time.**

### 4.1 Lo que se buscó y no se encontró

| Hipótesis del encargo | Resultado | Evidencia |
|---|---|---|
| Un mutex global en el camino del despertar | **No, en las condiciones** | `GuestCond` no tiene registro: `cond_signal` es un `pthread_cond_signal` directo, sin ningún mutex global (`runtime_mutex.c`) |
| Un mutex global en el camino del despertar | **Sí, en los semáforos** | `runtime_sema.c` usa un `lock` único para el registro y para todos los esperadores, y crea un `pthread_cond_t` por espera (1.898 ns por espera). **Pero** el tráfico de semáforos es de 51 operaciones/s: no puede ser el frame. El arreglo (lock por semáforo + tabla sin lock) está descrito en §8.3 y **no se ha hecho a propósito**: la medición dice que no compra nada |
| Espurious wakeups | **0** en todas las medidas del banco y del juego | `RuntimeWait.spurious`; con el patrón de POSIX (el invitado protege el predicado con el mutex) no hay despertares espurios |
| "Wait for all" del invitado | **Sí, y es real** | los 5 `JobWorker` comparten **una sola** variable de condición (los freeze reports: `0xc9d95c0` / `0x6e742e80`). Con `broadcast` los 5 despiertan y 4 vuelven a dormir. El coste medido de ese despertar extra está en el banco de sincronización del port (110 µs con 8 esperadores) |
| Despertar retrasado por el orden de los locks | **No** | el p50 de adquisición del mutex del invitado es 0,0 µs (§4.2): el recorrido de la cola no lo bloquea |

### 4.2 El mutex del invitado no está contending

Con 8 hilos machacando un mutex de invitado con sección crítica de 0, 200 ns y 2 µs
(`port/tests/test_guest_threads.c`):

| Sección crítica | p50 de adquisición | p95 | Total (8 × 10.000) |
|---|---|---|---|
| 0 ns | 0,0 µs | 0,1 µs | 10 ms |
| 200 ns | 0,0 µs | 0,0 µs | 35 ms |
| 2.000 ns | 0,0 µs | 0,1 µs | 219 ms |

Los 29,5 M locks de la sesión son casi todos **sin contención**: el mapa de esperas del
invitado lo confirma (`GameState`: 8 esperas, 0,2 ms bloqueados; `AK::EventManager`: 1
espera, 0,0 ms). El mutex del invitado no es un cuello de botella.

## 5. El reparto de CPU (el oversubscription)

En el freeze report hay **91 hilos en el proceso** para 12 procesadores lógicos (6 físicos):
18 del invitado y 73 del port y del driver de Vulkan (25 sin nombre, 6 `TLG:ShaderWarmup`,
3 `shadPS4:GpuSchedPriorityPendingOpsRunner`, los `D3D Background Thread 0..3`, los hilos de
`nvwgf2umx.dll`...). El invitado mapea 1:1 sus 20 hilos a hilos del host.

Con la nueva contabilidad, el informe de 5 s da el reparto exacto: `process CPU`, `guest CPU`
y `host CPU` (el resto es el driver, que no tiene registro). En la sesión de 100 s el
invitado estuvo bloqueado el 8,8 % de su tiempo y el port **esperando** ~2 % del frame (las
líneas `Stall:` del log: `readback 0,0 ms, GPU ticks 1,2 ms, recorder 0,8 ms, host copies
0,4 ms` sobre un frame de 150 ms).

**El reparto por hilo con y sin los hilos del port** es justo lo que el banco mide: añadir
36 hilos que giran (la carga del port) **no empeora** el viaje de ida y vuelta del sistema de
jobs del invitado (6,2 → 6,9 µs de mediana) porque los despertares van a un hilo que está
libre. Lo que sí empeora es lo contrario: **el hilo principal del invitado girando** (lo que
muestran los freeze reports) se lleva la CPU de los hilos del port.

## 6. La dependencia circular (invitado → fence de la GPU → port → invitado)

El caso medido es el write-fault: cuando el invitado escribe en memoria de la GPU, el
write-fault del port **sincroniza con el hilo de la GPU** y el hilo principal del invitado
se queda esperando en él. En el freeze report:

```
guest 'main': waiting on GPU thread (command) for 10797 ms (inside GPU page fault handler
              0x104bfdf3a0 for 10812 ms)
```

En la escena SAFETY TRUST el port anota **283-341 write faults/s** y 25-35 sincronizaciones
del grabador por frame. A 6,7 FPS son ~50 write faults por frame. **Si cada uno cuesta un
ida y vuelta al hilo de la GPU, el frame paga 50 × (la latencia de esa ida y vuelta)**: con
la GPU ociosa y el hilo de la GPU sin trabajo pendiente, esa latencia es la del
planificador (50-500 µs con 91 hilos en 12 CPUs) → **25-150 ms por frame**. Es la misma
magnitud que el frame time medido, y es dominio del agente de GPU (`port/gpu/**`:
`tlggpu_cpu_write` y el write-merge), no de este.

Desde el hilo del invitado, esto **queda visible** en el mapa nuevo: la espera del hilo
`main` aparece con el tipo `gpu` (antes sólo como texto suelto 'GPU idle (submit done)').

## 7. El spin del invitado (`scePthreadYield`)

`scePthreadYield` → `sched_yield()` de winpthreads (`SwitchToThread`): **125 ns por llamada**
medido. Antes no se contaba en ningún sitio, así que un hilo del invitado que gira con
`yield` salía como inactivo en el informe. Ahora `RuntimeWait.yield_count/yield_ns` lo
cuentan y el informe lo enseña ("yields 20 000 (2,0 ms)"). Con 125 ns por llamada, un
millón de yields por frame serían 125 ms: **si la próxima sesión muestra millones de yields
en `main`, ése es el cuello** (y el arreglo es del lado del invitado, no del port).

## 8. Cambios implementados y medidos

### 8.1 El mapa de esperas del invitado (el cambio principal)

Implementado en `runtime_thread.c` / `runtime_mutex.c` / `runtime_sema.c` / `runtime.h`, con
el banco `port/tests/test_guest_threads.c` como test (verde) y
`evidence/20261019-222857-guest-threads-bench/bench.txt` como evidencia. Sin cambio de
comportamiento: con `TLG_WAIT_STATS` apagado el camino caliente es idéntico.

### 8.2 El spin adaptativo del mutex del invitado (`TLG_MUTEX_SPIN`, **apagado**)

Implementado en `runtime_mutex.c`: cuando el mutex se ha liberado hace poco (40 pausas,
~2 µs, y sólo si el último desbloqueo fue hace < 50 µs) el que encuentra el mutex ocupado
espera en espacio de usuario en vez de hacer el viaje al kernel. **La medición lo
rechaza**: con la sección crítica de 0/200/2.000 ns la adquisición se queda en 0,0-0,1 µs
de mediana con y sin spin, y el spin **empeora** el p95 (0,1 → 1,6 µs) porque en el mutex
del invitado no hay contención (§4.2). Se queda **apagado por defecto** y documentado como
palanca de A/B para la sesión del juego (una variable, tres repeticiones), no como arreglo.

### 8.3 Lo que NO se ha tocado (y por qué)

| Candidato | Por qué no |
|---|---|
| Lock por semáforo + tabla de lookup sin lock | El tráfico es de 51 ops/s: no hay nada que ganar (y es un refactor con riesgo de perder despertares) |
| Relay de despertares (despertar uno y que el que despierta despierte al siguiente) | Cambia la semántica de `broadcast`: si el invitado cuenta con que todos los esperadores despierten, se rompe. No se hace sin evidencia |
| Prioridad del host para los hilos del invitado | El hilo `main` **gira** (§1): darle prioridad alta lo convierte en un starvation del hilo de la GPU. `TLG_THREAD_PRIO` (existente) ya baja los hilos de prioridad baja del invitado |

## 9. Receta para la próxima sesión (la que tiene que hacer el usuario)

**Antes de nada, hay que recompilar el port** (`bash port/build.sh`): el binario de
`port/out/` que hay ahora es anterior a este cambio y no trae el mapa de esperas. La
compilación privada de este agente (`port/tools/build-threads-private.sh`, en
`port/out/gpu-threads/`) ha comprobado que el port entero compila y enlaza con el cambio.

Con el port recién compilado, en la **misma escena** (SAFETY TRUST, 720p, 25 min):

```powershell
$env:TLG_WAIT_STATS = '1'          # el mapa de esperas, cada 5 s
$env:TLG_FRAME_STATS = '1'         # el frame time y las esperas del port
python port\run.py
```

Lo que sale en `port\out\logs\run-*.log` (y en el informe de salida):

1. **La línea del hilo `main`**: si dice `cpu 300 ms` y `blocked 5 ms`, el invitado está
   trabajando (o girando) y el cuello **no** es la sincronización del port → el caso cerrado
   del §1, con números del frame.
2. **Si dice `yields 1 000 000 (125,0 ms)`**: el invitado gira con `scePthreadYield` y ése es
   el cuello (§7).
3. **Si dice `[gpu 40 ms]`**: el write-fault de la GPU es el cuello → le toca al agente de
   GPU (§6).
4. **La línea de "N threads are waiting on the same ..."**: si los 5 `JobWorker` aparecen con
   millones de esperas de condición, el *thundering herd* de la variable compartida es el
   cuello → el relay de §8.3 es el experimento a hacer (con su riesgo medido antes).
5. **El reparto `process CPU / guest / host`**: si el invitado se lleva el 80 % de la CPU del
   proceso, la pregunta es por qué el invitado necesita tanta CPU (ahí sí es el juego).

## 10. Resumen de la evidencia

| Fuente | Qué dice |
|---|---|
| `evidence/20261019-213737-juego-usuario2/` (25,7 min) | 16.001 frames, 10,4 FPS; 75,7 esperas de condición y 1.847 locks por frame; las esperas del port son ~2,4 ms de un frame de 150 ms |
| `evidence/20261019-200829-wait-stats2/` (100 s) | el mapa por hilo: `main` bloqueado el 8,8 %, los `JobWorker` el 48 %, `AK::EventManager` y `main` "running" |
| `evidence/20261019-165749-720p-fsr3/` y `20261019-140541-g0-manual-paint4/` (freeze) | 91 hilos; `main` corriendo código del juego (o esperando al hilo de la GPU dentro de un write-fault) mientras no avanza ningún flip; los 5 `JobWorker` en una sola condición |
| `evidence/20261019-222857-guest-threads-bench/` (este agente) | el sistema de jobs del invitado: 6-20 µs por viaje; el despertar: 3-10 µs; el mutex del invitado: p50 0,0 µs (sin contención); `scePthreadYield`: 125 ns |

## 11. Cómo reproducir

```bash
# El banco del sistema de jobs del invitado (con el A/B del spin del mutex)
bash port/tests/run_guest_threads.sh            # --quick para una pasada corta
# La evidencia, con el timestamp en evidence/
bash port/tools/run-guest-threads-evidence.sh
# La compilación de todo el runtime con los flags del proyecto (no toca out/gpu)
bash port/tools/check-threads-build.sh
# El port entero, en un directorio de build privado (no toca out/gpu)
bash port/tools/build-threads-private.sh        # cmake -S gpu -B out/gpu-threads
```

El banco no necesita juego, GPU ni Vulkan: enlaza sólo `runtime_thread.c`,
`runtime_mutex.c`, `runtime_sema.c`, `runtime_rwlock.c` y `runtime_kernel.c`, y llama a las
primitivas del invitado por su NID (el mismo camino que el loader), de modo que lo que mide
es el código que el juego ejecuta de verdad.

El target de CMake que necesita este banco (está en el fichero del agente de GPU, así que
no se toca aquí; cuando pueda, el target es):

```cmake
add_executable(guest-threads-test EXCLUDE_FROM_ALL ../tests/test_guest_threads.c
    ../src/runtime_thread.c ../src/runtime_mutex.c ../src/runtime_sema.c
    ../src/runtime_rwlock.c ../src/runtime_kernel.c ../src/compat_win.c)
target_include_directories(guest-threads-test PRIVATE .. ../src)
target_link_libraries(guest-threads-test PRIVATE -pthread)
```
