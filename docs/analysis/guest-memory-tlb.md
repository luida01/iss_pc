# La memoria del invitado: páginas de 4 KiB, TLB y páginas de 2 MiB

Fecha: 2026-10-21. Entorno: Windows 11, MSYS2 CLANG64 (clang 22.1.7), i5-13420H, 16 GB de RAM,
NVMe en C:, pagefile de 15 GiB en D: (disco SATA ST500LM034).
Alcance: `port/src/runtime_memory.c`, `port/tests/test_guest_tlb*.c`, `port/tests/test_guest_pool.c`.
Pregunta: la memoria del invitado es de 4 KiB, ¿eso provoca fallos constantes de TLB, y con
páginas de 2 MiB el código del invitado iría mucho más rápido?

## 1. Resumen (respuesta corta)

**La hipótesis no se sostiene en esta máquina, y por dos motivos independientes:**

| Comprobación | Resultado | Lectura |
|---|---|---|
| Tamaño de página real del pool | **1,00 fallos/página → 4 KiB** | la premisa es cierta |
| Las tres rutas de páginas de 2 MiB | **error 1314** (privilegio) y **1300** (el privilegio no está en el token) | aquí no se pueden obtener ni probar |
| Alcance del TLB (persecución de punteros) | 4,9 ns a 1 MiB, 21,7 ns a 8 MiB, **52,5 ns a 16 MiB** | el escalón está entre 8 y 16 MiB |
| Coste de la indirección de la sección | **idéntico a memoria anónima** en todos los tamaños | la sección no cuesta nada |
| Cambio de protección (el del rastreador del GPU) | 48-59 ns/página (el log: 4,4 ms por 74 944 páginas) | el 0,08 % de un núcleo |
| Lo que el pool cuesta frente a memoria normal | el primer toque **2x** (0,8-1,3 µs/página contra 0,4-0,7) | ~0,5-0,8 s al arrancar, una sola vez |
| Escena de introducción | 58,2 FPS, mediana 16,76 ms, espera de vsync 1,9 ms | **ya está atascada en el vsync** |

Conclusión: la memoria del invitado **no es el cuello de botella del fotograma**. El coste que
queda medible (el commit de las páginas del pool, el doble que una página anónima) es de
arranque, no de fotograma; y el culpable siguiente del tiempo de fotograma está en la ruta de
fallos de escritura del rastreador del GPU (`port/gpu/**`), no en el runtime de memoria.

La ruta de páginas grandes queda implementada y **apagada por defecto** (`TLG_HUGE_PAGES=1`), con
caída limpia si el privilegio no está: así la pregunta se puede cerrar en una sesión elevada, en
cualquier máquina, sin tocar nada más.

## 2. Lo que hay que medir y con qué banco

El banco (`port/tests/test_guest_tlb.c`, `test_guest_tlb3.c`, `test_guest_tlb4.c`) reproduce la
geometría del pool del runtime y mide **solo el host** (sin juego, sin GPU, sin Vulkan), así que
puede correr junto a una sesión:

```
bash port/tests/run_guest_tlb.sh          # construye y ejecuta los tres bancos:
                                           #   test_guest_tlb.c  (página real, rutas de 2 MiB,
                                           #                      alcance del TLB, backing)
                                           #   test_guest_tlb3.c (sección contra memoria normal:
                                           #                      dónde vive, cuántas vistas, write fault)
                                           #   test_guest_tlb4.c (crear el pool de 6080 MiB,
                                           #                      SEC_COMMIT frente a SEC_RESERVE)
bash port/tests/run_guest_pool.sh         # el pool real (enlaza src/runtime_memory.c)
```

`port/tests/run_guest_pool.sh` enlaza el **runtime de verdad** (`src/runtime_memory.c`) con
tachones de los servicios que usa, y ejercita: reserva, reserva directa, mapeo, escritura,
lectura por la vista de respaldo, protección y liberación. Con `TLG_HUGE_PAGES=align` ejercita la
geometría de 2 MiB sin necesitar el privilegio.

## 3. Medición 1: el pool es de 4 KiB (la premisa, confirmada)

El pool es una **sección respaldada por el pagefile** (`CreateFileMapping2` con
`INVALID_HANDLE_VALUE` y `SEC_COMMIT`, `port/src/runtime_memory.c`), mapeada dos veces: la vista
del invitado y la vista de respaldo. Escribiendo un byte por página de 4 KiB:

```
pagefile section              131072 pages of 4 KiB ->   131328 faults (1.00 faults/page -> 4 KiB pages)
```

**1,00 fallos por página**: el pool usa páginas de 4 KiB. La premisa de la hipótesis es cierta;
lo que hay que comprobar es la consecuencia.

## 4. Medición 2: las tres rutas de páginas de 2 MiB no están disponibles aquí

```
SEC_LARGE_PAGES on the section: failed with 1314 (87=invalid parameter, 1314=privilege not held)
VirtualAlloc(MEM_LARGE_PAGES): failed with 1314 (8=not enough memory, 1314=privilege not held)
AWE AllocateUserPhysicalPages: failed with 1314 (1314=privilege not held)
GetLargePageMinimum=2097152 (the OS knows 2 MiB pages)
SeLockMemoryPrivilege: NOT available (error 1300; 0=assigned, 1300=not in the token)
```

El SO conoce las páginas de 2 MiB (`GetLargePageMinimum()` = 2 MiB) pero **el usuario
`MSI\luisd` no es administrador**: `SeLockMemoryPrivilege` no está en su token (1300) y las tres
rutas fallan con 1314. Sin elevación **no se puede ni construir el caso de prueba**, y por tanto
tampoco se puede medir un A/B de páginas grandes en esta máquina.

## 5. Medición 3: el alcance del TLB y lo que cuesta la sección

Persecución de punteros (un nodo por página de 4 KiB, el recorrido no se puede predecir):

| Conjunto de trabajo | Vista de la sección | Memoria anónima |
|---|---|---|
| 1 MiB (256 páginas) | 4,9 ns/acceso | 4,9 ns |
| 4 MiB (1 024) | 14,7 ns | 14,9 ns |
| 8 MiB (2 048) | 21,7 ns | 20,1 ns |
| 16 MiB (4 096) | **52,5 ns** | 53,5 ns |
| 64 MiB (16 384) | 99,6 ns | 99,9 ns |
| 256 MiB (65 536) | 110,4 ns | 109,8 ns |

Dos lecturas:

1. **La indirección de la sección no cuesta nada**: a todos los tamaños la vista de la sección y
   la memoria anónima dan el mismo tiempo. El pool no es "más lento por ser una sección".
2. **El escalón del TLB está entre 8 y 16 MiB** (2 048-4 096 páginas), que es justo el alcance de
   la TLB L2 de este CPU (1 536-2 048 entradas). Por debajo de 8 MiB el código que se repite cabe
   en la TLB.

Con páginas de 2 MiB el escalón se movería a 128-256 MiB, así que **el techo del beneficio** es
la diferencia entre el conjunto caliente y el escalón: ~30 ns por acceso que falla la TLB. Con
10-50 M de instrucciones por segundo en el hilo del invitado y ~1 % de ellas fallando la TLB, el
techo son **3-15 ms por segundo (0,3-1,5 % de un núcleo)**. Y el segundo paso (que el conjunto
caliente pase de 8 MiB) no está medido: en la escena de introducción el marco ya está atascado en
el vsync (§9).

## 6. Medición 4: el cambio de protección (y la corrección de un 10x falso)

El rastreador del GPU protege las páginas del invitado en solo lectura y las devuelve en cada
fallo de escritura, así que el runtime cambia la protección de las páginas del pool miles de
veces por minuto. La primera medición dio un resultado engañoso:

```
pagefile section             protect+write fault+unprotect over 32 MiB:   1.26 ms per cycle
anonymous memory             protect+write fault+unprotect over 32 MiB:   0.12 ms per cycle
```

Eso parece un impuesto del 10x por usar una sección. **Era una comparación injusta**: la sección
estaba *escrita* (páginas presentes) y la memoria anónima *no* (páginas ausentes, y una protección
sobre una página que no está no tiene nada que actualizar). Repitiendo la matriz completa:

| Respaldo | Par protección 32 MiB (páginas presentes) | Rango nuevo cada vez | Páginas ausentes |
|---|---|---|---|
| Fichero en el NVMe (C:) | 0,323-0,324 ms (39,5 ns/pág.) | 0,471-0,473 ms | |
| Fichero en el disco SATA (D:) | 0,328-0,331 ms (40,0-40,4 ns/pág.) | 0,468-0,502 ms | |
| **Sección del pagefile (hoy)** | **0,398-0,429 ms (48,6-52,4 ns/pág.)** | 1,243-1,289 ms | 0,115-0,125 ms (14,2-15,3 ns/pág.) |
| Memoria anónima | 0,337-0,509 ms (41,2-62,1 ns/pág.) | 0,463-1,039 ms | 0,060-0,061 ms (7,3 ns/pág.) |

(los rangos son de dos ejecuciones; el estado de la máquina los mueve dentro de esos límites)

Con las mismas condiciones, **la sección no es más cara que la memoria anónima** (es más barata
por página cuando las páginas están presentes). Tampoco influyen:

- **el número de vistas** de la sección (1, 2, 4 y 8 vistas: 0,114-0,134 ms por par, 13,9-16,3
  ns/página): el runtime mapea el pool una vez por cada mapeo del invitado más la vista de
  respaldo, y no cuesta nada;
- **la alineación** del rango protegido (4 KiB o 2 MiB: 0,116 contra 0,123 ms);
- **dónde vive la sección** (el pagefile del SATA, un fichero en el NVMe o uno en el SATA).

Además, el *write fault* en sí cuesta lo mismo en los dos casos (3,4 µs/página la sección, 3,0 µs
la anónima: la sección no paga un peaje por estar respaldada por el pagefile).

**Consecuencia para el fotograma**: el log del usuario muestra `protect calls 957 (74944 pages)`
entre dos informes de tirón, y en ese mismo bloque el hilo del GPU dice `ms in: ... protect 4,4`
→ **59 ns por página**, que es justo lo que mide el banco (48-52 ns/página). Son 4,4 ms de CPU
por cada periodo entre informes: **el 0,08 % de un núcleo**. La ruta de protección no es un cuello
de botella.

## 7. Medición 5: lo que el pool sí paga frente a memoria normal

| | Sección del pagefile | Memoria anónima | Fichero en el NVMe | Fichero en el SATA |
|---|---|---|---|---|
| Primer toque | 0,83-1,31 µs/pág. | 0,41-0,67 µs/pág. | 1,22-1,39 µs/pág. | 1,23-1,43 µs/pág. |
| Re-lectura tras recortar el working set | 0,78-0,90 µs/pág. | 0,65-0,82 µs/pág. | 0,66-0,78 µs/pág. | 0,74-0,79 µs/pág. |
| Creación del pool (6 080 MiB) | ~0 ms | ~0 ms | | |

Lo único que la sección cuesta de más es **el commit de la primera escritura**: el doble que una
página anónima (la página del pagefile se trae de cero y se marca sucia). El juego reserva
5 301 534 720 bytes de memoria directa (1 294 323 páginas), así que eso son **~0,5-0,8 s de CPU
repartidos durante el arranque**, una sola vez (después las páginas ya están residentes).

Y el experimento inverso: **un fichero en el NVMe es peor** que el pagefile para el primer toque
(1,22-1,39 contra 0,83-1,31 µs/página), así que **no conviene cambiar el backing** de la sección
por un fichero en el SSD. `SEC_RESERVE` en lugar de `SEC_COMMIT` tampoco aporta: la creación del
pool ya cuesta ~0 ms y con `SEC_RESERVE` la primera escritura de la vista de respaldo **falla con
una violación de acceso** (0xC0000005: la página se confirmaría bajo demanda y el fallo no se
puede recuperar). Con `SEC_COMMIT` ese mismo primer toque cuesta 1,01 µs/página.

## 8. Medición 6: lo que el port paga de verdad (del log del usuario)

`evidence/20261020-202436-perfil-final/run.log`:

| Dato del log | Lectura |
|---|---|
| `protect calls 957 (74944 pages)` y `ms in: ... protect 4,4` en el mismo bloque | 59 ns por página (el banco mide 48-52): 4,4 ms de CPU entre informes, el 0,08 % de un núcleo |
| `Runtime: flexible maps=0 ... protects=0` | el juego no usa la memoria flexible (1 GiB del pool) ni llama a `sceKernelMprotect` |
| `Runtime: direct memory allocations=3, maps=3` | 3 reservas y 3 mapeos en toda la sesión: la geometría del pool no cambia durante el juego |
| `1574 write faults/s` (hasta 2 485/s) | fallos del rastreador del GPU: el invitado escribe en una página que el GPU tiene en solo lectura |
| `fault handlers: ... write faults 299,3 ms` | el tiempo en los manejadores de fallos entre dos informes de tirón |

Coste de un fallo de escritura: el runtime hace una búsqueda en su tabla de regiones y **un
`VirtualProtect` de la ventana de 256 KiB** que el rastreador deja suelta (64 páginas × ~50 ns ≈
3,2 µs) más el recorrido de la tabla. A 2 485 fallos/s son **~8 ms/s (el 0,8 % de un núcleo)**.

## 9. El culpable siguiente

**No es la TLB, ni el tamaño de página, ni la sección, ni la protección: es la ruta de fallos de
escritura del rastreador del GPU**, y el cuello de botella del fotograma está en el mismo sitio:

- En la escena de introducción el marco ya está **atascado en el vsync**: 58,2 FPS con una
  mediana de 16,76 ms (el vsync es a 60 Hz) y solo 1,9 ms de "free frame wait" por marco. No hay
  hueco para ganar con la memoria.
- Los tirones (106,5 ms y 311,9 ms) **no son de memoria**: el propio bloque del log dice
  `fault handlers: 0 read faults 0,0 ms, write faults 0,1 ms`, o sea que el hilo del invitado
  estaba ejecutando código del juego.
- En la escena pesada el número grande es `write faults 299,3 ms` acumulado, y el reparto de
  tiempo de esa ventana deja `GPU ticks 25,5 %` y `GPU thread ... idle 1,4 %`: el hilo del
  invitado está esperando al GPU y el GPU está invalidando memoria en cada fallo de escritura.

El coste por fallo de escritura medido en el log (299,3 ms repartidos entre 1 574-2 485 fallos/s,
con una ventana entre informes de entre 1 y 5 s) sale en **38-190 µs por fallo**, y la parte del
runtime son 3-5 µs: el resto está en la invalidación del rastreador y de la caché de texturas y de
búferes (`port/gpu/shadps4/video_core/page_manager.cpp`, `vk_rasterizer.cpp`,
`video_core/buffer_cache/buffer_cache.cpp`), que es de los agentes del renderer.

## 10. Lo implementado: la ruta de páginas de 2 MiB, apagada por defecto

`port/src/runtime_memory.c` incorpora la ruta de páginas grandes con `TLG_HUGE_PAGES`:

| Valor | Qué hace |
|---|---|
| (sin definir) | **exactamente lo de hoy**: sección del pagefile, 4 KiB |
| `TLG_HUGE_PAGES=1` | pide una sección de páginas grandes (`SEC_LARGE_PAGES`); sin `SeLockMemoryPrivilege` **avisa y sigue con el pool normal** (el flag no cambia nada) |
| `TLG_HUGE_PAGES=align` | la **misma geometría de 2 MiB con páginas de 4 KiB**, para comprobar la geometría sin el privilegio |

Las vistas de una sección de páginas grandes tienen que estar alineadas a 2 MiB en dirección, en
desplazamiento dentro de la sección y en tamaño, así que en ese modo el runtime:

- reparte **desplazamientos físicos alineados a 2 MiB** y redondea los tamaños a 2 MiB (el relleno
  se queda dentro de la reserva, así que no se aliasa nada más),
- **alinea hacia arriba la dirección del invitado** (el invitado recibe la dirección real),
- mapea la vista de respaldo en una dirección alineada a 2 MiB,
- **rechaza** los `munmap` y las protecciones del invitado que no estén alineados a 2 MiB (partir
  una vista en dos no alineados no se puede mapear con páginas grandes), con un mensaje claro,
- **comprueba** al arrancar que la sección usa de verdad páginas de 2 MiB (tocando 64 MiB y
  contando los fallos: 32 con 2 MiB, 16 384 con 4 KiB) y, si no, vuelve al pool normal.

El log lo dice todo:

```
Runtime: guest pool: 6080 MiB, 2 MiB pages
Runtime: pool page size check: 32 faults for 64 MiB of 2 MiB chunks
```

Sin el privilegio:

```
Runtime: TLG_HUGE_PAGES=1 needs SeLockMemoryPrivilege (error 1300: it is not in this user's token); the pool keeps 4 KiB pages
```

El banco del pool (`port/tests/run_guest_pool.sh`) pasa en los tres modos (normal, `align` y `1`):
reserva, mapeo, escritura, lectura por la vista de respaldo, protección y liberación.

## 11. Receta de A/B (para el usuario)

```powershell
# 1) La referencia: el port como está, escena de introducción
& tools\run-baseline.ps1

# 2) Con la ruta de páginas grandes (en una consola elevada, para tener el privilegio)
$env:TLG_HUGE_PAGES = "1"
& tools\run-baseline.ps1

# 3) Comparar en el log, en los primeros bloques "Frame stats":
#    - FPS y mediana de frame ("Frame pacing: median ...")
#    - "write faults/s"
#    - y la línea del pool: "Runtime: guest pool: 6080 MiB, 2 MiB pages"
```

Qué esperar, según las mediciones: **lo mismo**. El banco dice que las páginas de 2 MiB
compran como mucho un 0,3-1,5 % de un núcleo, y que la escena de introducción está atascada en
el vsync (58,2 FPS con mediana de 16,76 ms). Si la sesión elevada mostrara una diferencia clara,
sería un dato que estos números no explican y habría que mirar con la herramienta de perfiles
del propio juego.

Si el flag se usa **sin privilegio**, el port avisa y sigue con el pool de 4 KiB: el A/B mide
entonces exactamente lo mismo que la referencia. Con `TLG_HUGE_PAGES=align` el pool sigue con
4 KiB pero con la geometría de 2 MiB (desplazamientos y direcciones alineados): sirve para
comprobar que la geometría no rompe el juego, no para medir páginas grandes.

## 12. Archivos

- `port/src/runtime_memory.c`: la ruta de páginas grandes (`TLG_HUGE_PAGES`), la comprobación del
  tamaño de página al arrancar y la geometría de 2 MiB (desactivada por defecto), más la
  corrección de un bucle infinito en `direct_release` (un `munmap` fallido dejaba la región en la
  tabla y el bucle no terminaba).
- `port/tests/test_guest_tlb.c`: el banco principal (página real, rutas de 2 MiB, alcance del TLB,
  protección, backing).
- `port/tests/test_guest_tlb3.c`: por qué la sección parecía 10x más cara (matriz completa) y el
  coste del write fault.
- `port/tests/test_guest_tlb4.c`: la creación del pool real (6 080 MiB) y `SEC_COMMIT` frente a
  `SEC_RESERVE`.
- `port/tests/test_guest_pool.c` + `port/tests/run_guest_pool.sh`: el pool del runtime de verdad
  (enlaza `src/runtime_memory.c`) en los tres modos.
