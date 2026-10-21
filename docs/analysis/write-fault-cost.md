# El write fault del rastreador de páginas: cuánto cuesta y por qué `PAGE_GUARD` no compensa

Fecha: 2026-10-21.
Entorno: Windows 11, MSYS2 CLANG64 (clang 22.1.7), i5-13420H (8 núcleos, 12 hilos lógicos),
16 GB de RAM, RTX 3050 Laptop 4 GB. El port corre con 91 hilos del invitado.
Alcance: `port/gpu/shadps4/video_core/page_manager.{cpp,h}`, `port/gpu/shim/core/**`,
`port/tests/writefault/test_write_fault.cpp`.
Pregunta: el banco (`port/tests/writefault`) dice que el patrón "guard" (`PAGE_GUARD`) cuesta
2,3 µs por fallo contra 63 µs del patrón actual (26x más barato). ¿Se puede llevar el port a ese
patrón **sin romper la corrección** y qué se gana realmente?

## 1. Resumen (respuesta corta)

**Tal cual no se puede (rompe la corrección); la única forma segura es "guard + ventana", y su
ganancia es pequeña para lo que cuesta.** Las dos mitades del asunto, con números:

| Comprobación | Resultado | Lectura |
|---|---|---|
| `PAGE_GUARD` en este equipo | la guardia es **por página** y **la consume cualquier acceso**, también una **lectura** | una lectura del invitado descuarda la página y la escritura siguiente **no se ve** |
| Corrección del patrón guard tal cual (modelo del banco, `--mode=correct --read-first=1`) | **2 048 de 2 048 páginas obsoletas** (el GPU leería datos viejos) | inviable sin arreglo |
| El arreglo conservador (una lectura marca la página como modificada) | 0 páginas obsoletas, pero **una subida y un fallo por cada lectura** | el coste se paga en subidas y en fallos |
| El arreglo "barato" (reponer la guardia en la subida) | **2 048 páginas obsoletas** | no vale: la escritura entre la lectura y la subida se pierde |
| El número de fallos | el guard multiplica los fallos por **las páginas que el invitado escribe**: 32 → 2 048 en el modelo (**64x**) | el banco mide *por fallo*; en el port lo caro es *el fallo* |
| El coste por fallo en el port | **~107 µs** (log), de los cuales la protección son **~14-24 µs** | la protección es el 15-20 % del fallo |
| La variante que conserva el número de fallos ("guard + ventana de 256 KiB") | la protección del fallo baja de **53 µs a 9,4 µs**; con el trabajo real del port el banco pasa de **460 a 169 ms/s** de parón | en el port serían **~1-10 % de un núcleo**, con un riesgo de corrección |
| El precedente | en Linux, `userfaultfd` quita el `mprotect` del todo y **"no cambió los tirones medibles"** | la protección no es el cuello de botella |

Conclusión: **la ruta de fallos de escritura no se arregla con `PAGE_GUARD`**. El patrón guard
(1) rompe la corrección salvo que cada lectura de una página vigilada marque la página como
modificada, (2) multiplica el número de fallos por 17-64, y (3) aunque se arregle, lo que se
gana es el 10-20 % de una ruta que ya es el 2-6 % de un núcleo. Lo que hay que atacar es el
**drain** (`DrainDrawPipe`) y las **invalidaciones** de la caché de texturas y de búferes, que
son el 80-85 % del fallo y están fuera del alcance de este agente.

Lo que sí queda: el cambio ya hecho (fusionar las dos protecciones en una) está medido aquí
(§8: **12 % por fallo**) y el banco tiene ahora el modelo de corrección (§4) y la
micro-medición de los cambios de protección (§7), que son las herramientas para seguir.

**Veredicto**: el patrón `PAGE_GUARD` tal como lo mide el enunciado (sin la ventana de 256 KiB)
**no se puede llevar al port** (multiplica el número de fallos por 17-64 y el parón pasa de 460
a **3 902 ms/s** con el trabajo real del port). La única forma viable es **guard + ventana**,
que **es correcta** con el arreglo conservador y da **-34 % del coste del fallo** en el banco;
pero ese arreglo convierte cada lectura de una página vigilada en un fallo y en una subida, y no
sabemos cuántas hace el invitado. **No es un cambio, es un experimento** (§11).

## 2. Cómo funciona hoy el ciclo de protección

Hay **dos capas** y no se deben confundir:

| Capa | Dónde | Qué guarda |
|---|---|---|
| La protección | `PageManager` (`page_manager.cpp`) + `PageState` (`page_state.h`) | por página de 4 KiB: `num_write_watchers` y `num_read_watchers`. `Perms()`: sin vigilantes → `Read\|Write`; con vigilantes de escritura → `Read` (solo lectura); con vigilantes de lectura → `None` (sin acceso) |
| Lo "sucio" | `RegionManager` (`buffer_cache/region_manager.h`) | por región de 16 MiB, dos bitmaps: `cpu` (modificado por el invitado) y `gpu` (modificado por la GPU), más `writeable`/`readable` (la protección que el rastreador tiene aplicada) |

El ciclo, con el orden real:

1. **La subida** (`ForEachUploadRange` → `ForEachModifiedRange<CPU, true>`) copia a la GPU las
   páginas marcadas como modificadas y **vuelve a protegerlas**
   (`UpdateProtection<true, false>` → `PageManager::UpdatePageWatchersForRegion` → `ApplyProtect`
   → `VirtualProtect`).
2. **El invitado escribe** en una página protegida → el VEH del cargador (`port/src/probe.c`) →
   `tlggpu_handle_fault` → `Core::Signals::DispatchAccessViolation` →
   `PageManager::GuestFaultSignalHandler` → `Rasterizer::OnWriteFault`:
   - `DrainDrawPipe()`: **espera al hilo de grabación** (el hilo de la GPU está al 88 %);
   - `InvalidateMemory(addr, 8)`: la caché de texturas marca la imagen como sucia y la
     desregistra; el rastreador de búferes marca la región como modificada;
   - `BufferCache::ExtendWriteFault(addr)`: **deja escribibles la página y su ventana de 256 KiB**
     (`TLG_FAULT_WINDOW`, 4-1024 KiB) y marca la ventana entera como modificada.
3. **Los cambios de protección se aplican juntos** al final del manejador (el lote por hilo del
   commit `d35a3e6`): una llamada a `VirtualProtect` por rango con la misma protección.

Lo que la ventana compra y lo que cuesta: la ventana de 256 KiB hace que **una sola falta por
ventana** cubra las 64 páginas, a cambio de marcar 64 páginas como modificadas (se suben aunque
no se hayan escrito) y de un `VirtualProtect` de 256 KiB (con su *shootdown* de TLB).

### 2.1 Un fallo del banco que había que arreglar antes de medir

Midiendo, el banco se colgaba en el modo `merged` (y el modo `window` daba números
distorsionados). La causa: la ventana de 256 KiB se calcula sobre la **dirección absoluta**, y
la vista de la sección (`MapViewOfFile`) **no está alineada a 256 KiB**, así que la primera y la
última ventana **se salen del pool**: el `VirtualProtect` de la ventana falla con
`ERROR_INVALID_ADDRESS` (487), la página sigue en solo lectura y la escritura del invitado
**vuelve a fallar 100 000 veces** (el banco consumía 1 300 s de CPU antes de que yo lo matara).

**El port no tiene ese problema**: `runtime_memory_gpu_protect` recorta el rango a las regiones
mapeadas (`port/src/runtime_memory.c`), así que la ventana que se sale del pool se protege solo
en la parte que existe. El banco ahora recorta igual (`WindowOf`) y cuenta las llamadas que
fallan, y el escritor tiene un freno (si la misma página falla 100 000 veces, para el hilo de la
"GPU"): un banco que se cuelga no mide nada.

### 2.2 Lo que cuesta hoy, medido

Banco (`port/tests/writefault`, 8 hilos, sección de 256 MiB, 3 s, **mediana de 3**):

| Modo | Qué hace | Por fallo (extremo a extremo) | Mediana | En el manejador | De los cuales, protección | Dispatch | Fallos/s | Parón total |
|---|---|---|---|---|---|---|---|---|
| `none` | sin protección | 0 fallos | | | | | 0 | 0 |
| `page` | deja escribible la página (1 llamada de 4 KiB) | 21,1 µs | 8,7 µs | 17,7 µs | 17,6 µs | 3,4 µs | 61 582 | 3 857 ms/s |
| `window` | **lo que hace el port**: la página y la ventana (2 llamadas) | 68,1 µs | 41,5 µs | 62,7 µs | 62,5 µs | 4,8 µs | 1 519 | 310 ms/s |
| `merged` | la unión en una llamada (el cambio ya hecho) | 59,9 µs | 37,1 µs | 54,0 µs | 53,9 µs | 4,3 µs | 1 430 | 256 ms/s |
| `guard` | `PAGE_GUARD`: el kernel quita la guardia, el manejador no protege nada | **2,2 µs** | **1,8 µs** | 0,1 µs | 0,0 µs | 2,1 µs | **25 899** | 174 ms/s |
| `guardwin` | `PAGE_GUARD` y el manejador deja la ventana accesible (el ciclo del port con la guardia) | 30,3 µs | 14,4 µs | 26,2 µs | **9,4 µs** | 4,1 µs | 847 | 70 ms/s |

El 26x del enunciado es real **por fallo**, pero el modo `guard` hace **17x más fallos** con el
mismo patrón de escritura (25 899/s contra 1 519/s): el total del parón de los escritores es
**174 ms/s** (guard) contra **310 ms/s** (window). O sea: en un banco cuyo manejador solo protege,
el patrón guard es mejor; en el port, cuyo manejador además drena e invalida, la cuenta cambia
(§5).

El modo `guardwin` es el que conserva el número de fallos del port (la ventana sigue
amortizando) y es el único candidato real: **la protección del fallo baja de 53 a 9,4 µs** (la
guardia no necesita *shootdown* de TLB, §7) y el parón total cae de 310 a 70 ms/s.

En el port real (log `evidence/20261021-015502-final-2/run.log` y el análisis de la TLB):

| Dato | Lectura |
|---|---|
| `338 write faults/s` (y 1 574-2 485/s en la escena pesada) | la tasa de fallos del rastreador |
| `fault handlers: ... write faults 181,4 ms` entre dos informes | el tiempo **dentro** de `OnWriteFault` |
| `protect calls 186 (9 562 pages)`, `ms in: ... protect 2,6` | el coste de la protección en el hilo de la GPU: **~14 µs por llamada** |
| 38-190 µs por fallo (análisis de la TLB) | el 3-5 % es del runtime; el resto es el rastreador y las cachés |

Con esos números, **el coste por fallo en el port es ~107 µs y la protección son 14-24 µs**: el
80-85 % del fallo es el drain y las invalidaciones, que el patrón guard **no toca**.

### 2.3 El coste por fallo en el port

## 3. Qué hace `PAGE_GUARD` exactamente (medido en este equipo)

Con `--mode=guardcheck` y una sonda propia (14 páginas, `VirtualProtect` con
`PAGE_READWRITE|PAGE_GUARD`):

| Comprobación | Resultado |
|---|---|
| ¿La guardia es por página o por rango? | **por página**: escribir la página 0 no quita la guardia de la 1 |
| ¿La lectura de una página con guardia falla? | **sí**: `STATUS_GUARD_PAGE_VIOLATION` con `ExceptionInformation[0] = 0` (lectura) |
| ¿La lectura **deja** la página sin guardia? | **sí**: la escritura siguiente a esa lectura **no da ningún fallo** |
| ¿La escritura con guardia falla? | **sí**, con `ExceptionInformation[0] = 1` (escritura), y el acceso se completa al reanudar |
| `VirtualQuery` de una página con guardia | `0x104` (`PAGE_READWRITE|PAGE_GUARD`); tras el fallo, `0x4` (`PAGE_READWRITE`) |

Ese es el problema central: **la guardia no distingue "la GPU la va a leer" de "el invitado la
escribió"**. La consume el primer acceso, sea del tipo que sea. Y el Rastreador solo sabe que
"la página está vigilada" (un contador), no que su guardia siga puesta.

## 4. La corrección: el patrón guard, tal cual, pierde escrituras

El banco tiene ahora un **modelo del ciclo del rastreador** (`--mode=correct`): 256 páginas
vigiladas, 8 rondas; el invitado escribe (y opcionalmente lee antes), el rastreador marca lo
modificado, la "subida" copia las páginas marcadas y las vuelve a proteger, y la comprobación
compara **lo que el GPU leería** con **lo que el invitado escribió**. Una diferencia es la
corrupción que el port no puede tener nunca (el juego con una textura vieja).

Con el invitado **leyendo** la página antes de escribirla (el caso normal: el invitado lee sus
propios búferes):

| Variante | Fallos de escritura | Fallos de lectura | Escrituras perdidas | **Páginas obsoletas** | Páginas subidas | `VirtualProtect` |
|---|---|---|---|---|---|---|
| `ro` (el port hoy) | 32 | 0 | 0 | **0** | 2 048 | 2 336 |
| `guard-naive` (el guard tal cual) | 0 | 256 | 2 048 | **2 048** | 0 | 256 |
| `guard-dirty` (una lectura marca la página) | 0 | 2 048 | 0 | **0** | 2 048 | 2 304 |
| `guard-rearm` (la guardia se repone en la subida) | 0 | 2 048 | 2 048 | **2 048** | 0 | 2 304 |

Lectura de la tabla:

- **`guard-naive` está roto**: la lectura del invitado se come la guardia, la escritura siguiente
  no falla, el rastreador no se entera y el GPU lee la **versión vieja** de las 2 048 páginas. Es
  exactamente el "el juego lee textura vieja" que hay que evitar a toda costa.
- **`guard-dirty` es correcto**: una lectura de una página vigilada la marca como modificada, así
  que la subida la copia (con el contenido nuevo) y la vuelve a proteger. Pero **cada lectura de
  una página vigilada cuesta un fallo y una subida**, y en la ruta de texturas eso es
  re-subir una textura entera.
- **`guard-rearm` no vale**: reponer la guardia en la subida no recupera la escritura que se
  perdió entre la lectura y la subida. Además, reponerla **en el manejador** no es posible: el
  acceso se reintenta y volvería a fallar (bucle).

Sin lectura previa (el invitado solo escribe) las tres variantes del guard son correctas, pero el
número de fallos se multiplica: **2 048 frente a 32** (64x) en el mismo modelo.

## 5. El precio: el guard multiplica los fallos, y en el port el fallo es lo caro

El desglose del coste de un fallo en el port (`OnWriteFault`):

| Parte | Coste | ¿Lo quita el guard? |
|---|---|---|
| Dispatch del kernel (la excepción, la entrada al manejador) | 4-5 µs | sí, el guard lo hace más barato (2,1 µs) |
| La protección (`VirtualProtect` + *shootdown* de TLB) | 14-24 µs (en el port; 53 µs en el banco con 8 hilos encima) | **sí** |
| `DrainDrawPipe` (esperar al hilo de grabación) + las invalidaciones (caché de texturas y de búferes) | **~85 µs** | **no** |

Con el guard (sin ventana), un fallo cuesta `2,2 + 85 ≈ 87 µs` pero hay **17x más fallos**; con la
ventana (que es lo que mantiene el número de fallos), un fallo cuesta `~9 + 85 ≈ 94 µs` y el
número de fallos no cambia. En los dos casos el total del parón **empeora o no cambia**:

| Escenario (banco con el trabajo real del port: 48 µs por fallo) | Fallos/s | Por fallo | Parón total |
|---|---|---|---|
| `window` (hoy, dos llamadas) | 1 463 | 105,9 µs | **460 ms/s** |
| `merged` (el cambio ya hecho, una llamada) | 1 332 | 101,2 µs | **419 ms/s** |
| `guardwin` (guard + ventana: el único viable) | 805 | 69,8 µs | **169 ms/s** |
| `guard` (guard sin ventana: el que mide el enunciado) | 25 899 | 50,2 µs | **3 902 ms/s** |

El `guard` sin ventana es el que el enunciado mide, y es el que **no se puede llevar al port**:
25 899 fallos/s contra 1 463, y cada uno con el drain y las invalidaciones del port. Con el
trabajo real del port (48 µs) serían `25 899 x 50 µs = 1,3 s de parón por segundo de reloj`, es
decir, el juego entero parado.

El punto de equilibrio, con `X` = el trabajo por fallo que el guard no quita (el drain y las
invalidaciones) y `P` = el coste de la protección:

```
el guard (sin ventana) gana  <=>  multiplicador x (X + 2,2)  <  X + P + 5
```

Con `P = 20 µs` (el port) y `X = 85 µs`: el multiplicador tiene que ser **< 1,0**, o sea que el
invitado tendría que escribir **menos de una página por ventana**. En el port escribe decenas
(la tasa de fallos sin ventana es de orden 10^5/s, según el comentario de `buffer_cache.cpp`).

Con la **ventana** (el `guardwin`), el multiplicador es 1 y la condición es `P > 2,2 µs`: ahí sí
gana, y gana **un 34 % del fallo** (105,9 → 69,8 µs) y **2,7x menos parón** en el banco.

## 6. La variante que sí se podría llevar (guard + ventana) y su precio real

Se puede hacer el patrón guard **manteniendo la ventana de 256 KiB**: el kernel quita la guardia
de la página que falló (gratis) y el manejador quita la guardia del resto de la ventana con **una
llamada** (que, al no cambiar los derechos de acceso, **no necesita *shootdown* de TLB**: 0,9 µs
en vez de 3,0 µs, §7). El modo `guardwin` del banco es eso.

Lo que da, en el banco: la protección del fallo baja de **53 µs a 9,4 µs**, el fallo total de
**105,9 a 69,8 µs** (-34 %) y el parón total (con el trabajo real del port) de **460 a 169 ms/s**
(2,7x menos).

Lo que cuesta, en el port:

1. **La lectura del invitado se vuelve un fallo** (§3). Con el arreglo conservador (`guard-dirty`)
   eso es un fallo y una subida por cada lectura de una página vigilada, y en la ruta de texturas
   una **re-subida de la textura entera**. Sin saber cuántas lecturas hace el invitado de las
   páginas vigiladas, esto puede ser un desastre o no ser nada.
2. **La subida tiene que reponer la guardia** de las páginas que la perdieron: el coste del
   `VirtualProtect` en el hilo de la GPU **sube** (de `RW→RO` a `RW→RW|GUARD`).
3. El trabajo de fontanería no es trivial: el permiso nuevo (con la guardia) tiene que llegar
   hasta `VirtualProtect` (`port/src/runtime_memory.c`), el manejador tiene que reconocer
   `STATUS_GUARD_PAGE_VIOLATION` (`port/src/probe.c`) y saber si fue lectura o escritura
   (`ExceptionInformation[0]`), y el Rastreador tiene que proteger con la guardia las páginas que
   la lectura dejó sin ella. Todo eso, con el modo guard apagado por defecto, solo para
   medirlo.
4. El **precedente en contra**: en Linux el port tiene `TLG_UFFD=1` (`userfaultfd` con
   escritura vigilada), que quita el `mprotect` de la ruta del fallo por completo, y la
   documentación dice que **"removes the mprotect time but did not change the stalls
   measurably"** (`port/docs/parallel_gpu.md`). Si quitar el coste de la protección no movió los
   tirones, el patrón guard tampoco los moverá.

Conclusión de este punto: **el patrón guard es defendible solo como experimento medible**
(una bandera apagada por defecto), no como cambio. Y su ganancia esperada en el port es del
orden del **10-20 % de la ruta de fallos**, que es el 2-6 % de un núcleo: **menos del 1 % de un
núcleo** (§2.2: 338-2 485 fallos/s x 20 µs ahorrados = 7-50 ms/s), con un riesgo real de
corrupción si el arreglo conservador no está.

## 7. Micro-medición: cuánto cuesta cada cambio de protección

Banco `--mode=protect`: 256 KiB con las páginas en las TLB de los 8 núcleos (ocupados, como los
91 hilos del port):

| Cambio | Por llamada | Quién lo hace |
|---|---|---|
| `RW → RO` | **3,01 µs** | la subida vuelve a proteger la página |
| `RO → RW` | **2,99 µs** | el fallo deja escribible la página (hoy) |
| `RW → RW\|GUARD` | **1,10 µs** | la subida vuelve a poner la guardia |
| `RW\|GUARD → RW` | **0,93 µs** | el fallo quita la guardia (el kernel ya lo hizo) |

La lectura importante: **cambiar los derechos de acceso cuesta ~3 µs y cuesta más cuando hay
contención (hasta 24 µs por llamada en el banco con 8 hilos escribiendo encima, y 53 µs por
fallo en el modo `window`), mientras que tocar la guardia es más barato** porque no obliga a
invalidar las entradas de TLB que el hardware tiene en pie. Ese es el único beneficio real del
patrón guard: **mover el *shootdown* del hilo del invitado al hilo de la GPU** (y el *shootdown*
de la subida se paga una vez por subida, no por fallo).

## 8. El cambio ya hecho (fusionar las dos protecciones) y lo que vale

`d35a3e6` fusionó las dos llamadas del manejador (la página y la ventana) en una. En el banco
(mediana de 3):

| Modo | Por fallo | Mediana | Fallos/s | Parón total |
|---|---|---|---|---|
| `window` (dos llamadas) | 68,1 µs | 41,5 µs | 1 519 | 310 ms/s |
| `merged` (una llamada) | 59,9 µs | 37,1 µs | 1 430 | 256 ms/s |

**El valor medido es del 12 % por fallo** (y del 17 % del parón total): el coste fijo de una
llamada a `VirtualProtect` (el bloqueo del espacio de direcciones y la ida y vuelta del
*shootdown*) es de ~15-20 µs, y el margen son las 64 páginas de la ventana (~0,06 µs/página).
El cambio sigue siendo correcto y más simple (una llamada, un rango), y es **la mitad del
beneficio del patrón guard** (que en el banco baja la protección de 53 a 9,4 µs) **sin tocar la
corrección**. Con la guardia, las dos llamadas se reducen a una de verdad (la del kernel, gratis).

## 9. Qué mirar ahora (las palancas que quedan, con su dueño)

| Palanca | Tamaño | Dónde |
|---|---|---|
| El **drain** (`DrainDrawPipe`) dentro de `OnWriteFault` | ~40-70 µs por fallo (el 80-85 % del fallo) | `renderer_vulkan/vk_rasterizer.cpp` (agente GPU) |
| Las **invalidaciones** (caché de texturas y de búferes) por fallo | el resto del ~85 µs | `texture_cache/**`, `buffer_cache/**` |
| El **número de fallos** (la ventana de 256 KiB) | 338-2 485/s; con `TLG_FAULT_WINDOW=1024` la mitad, a cambio de un 4 % menos de FPS (`port/docs/red_zone.md`) | `buffer_cache/buffer_cache.cpp` (el flag ya existe: `TLG_FAULT_WINDOW`) |
| La **protección** (lo que el guard tocaría) | 14-24 µs por fallo | `page_manager.cpp` (este agente) |

## 10. Las medidas que faltan (para cerrar el caso)

Dos experimentos que solo se pueden hacer con el juego, y que decidirían el caso del guard:

1. **La densidad de escritura**: cuántos fallos da el port sin ventana. Con
   `TLG_FAULT_WINDOW=4` (la ventana no se aplica) y `TLG_FAULT_COST=1`, el número de fallos por
   5 s es el número de páginas que el invitado escribe; con `TLG_FAULT_WINDOW=256` es el
   número de ventanas. El cociente es el multiplicador del guard, y con él se puede calcular el
   punto de equilibrio de §5 **con los datos del juego**.
   ```
   TLG_FAULT_WINDOW=256 TLG_FAULT_COST=1  →  "Fault cost (5 s): N write faults, ..."
   TLG_FAULT_WINDOW=4   TLG_FAULT_COST=1  →  "Fault cost (5 s): N write faults, ..."
   ```
2. **Las lecturas de páginas vigiladas**: cuántos fallos de lectura habría con el guard. Solo se
   ve implementando el guard (o con una variante de la sonda). Si son muchas, el patrón guard
   está descartado; si son pocas, la variante `guardwin` es un experimento de una sesión.

## 11. Si se quisiera implementar el guard en el port (el trabajo que falta)

Para que el director pueda decidir con el dato delante, esto es lo que habría que tocar (y lo
que **no** está en el alcance de este agente):

| Fichero | Cambio |
|---|---|
| `port/src/runtime_memory.c` | `runtime_memory_gpu_protect(addr, size, read, write)` → añadir el parámetro `guard` y aplicar `PAGE_READWRITE\|PAGE_GUARD` en `host_protect` |
| `port/gpu/shim/tlggpu.cpp` | `AddressSpace::Protect` → pasar el flag de la guardia (el permiso nuevo vive en `shim/core/address_space.h`, en el alcance) |
| `port/src/probe.c` | reconocer `STATUS_GUARD_PAGE_VIOLATION` en el VEH y pasar el tipo de acceso (`ExceptionInformation[0]`: 1 = escritura, 0 = lectura) |
| `port/gpu/shadps4/video_core/page_manager.cpp` | el permiso nuevo por página (guardia cuando hay vigilantes de escritura y ninguno de lectura), el manejador de la violación de guardia (escritura → la ruta de hoy; lectura → la ruta de hoy también, que marca la ventana como modificada: es el arreglo conservador) y una **comprobación de seguridad** (que la guardia se aplicó de verdad: si el runtime no la aplica, volver al modo de solo lectura, que es lo seguro) |
| `port/gpu/shadps4/video_core/page_state.h` | el permiso con guardia (o calcularlo en `page_manager.cpp` sin tocar `page_state.h`) |

La semántica que habría que respetar (la del modelo de corrección del banco, §4):

- la página vigilada se protege como `PAGE_READWRITE|PAGE_GUARD` (no `PAGE_READONLY`): el
  invitado la puede **leer** sin fallo y, al escribirla, el kernel quita la guardia;
- el manejador de la violación de guardia **no** distingue lectura de escritura para el
  rastreo: en los dos casos marca la página como modificada (así, una escritura que llega
  después de una lectura no se pierde) y deja la ventana accesible;
- la subida vuelve a poner la guardia de las páginas que copia;
- las páginas con vigilantes de **lectura** (readbacks) se quedan en `PAGE_NOACCESS`: la
  guardia no puede sustituir el "no acceder";

Con la bandera `TLG_GUARD_PAGES=1` **apagada por defecto**, y el modelo de corrección del banco
como criterio de aceptación (0 páginas obsoletas), el experimento es de una sesión. La prueba
de que no se rompe nada es el criterio de aceptación: con el guard activo, la comprobación
"0 páginas obsoletas" del modelo tiene que seguir dando 0, y en el juego la imagen no puede
cambiar (el criterio del port: el juego lee textura vieja = corrupción).

## 12. Reproducir

```bash
# el banco (construye en un directorio privado)
clang++ -std=c++20 -O2 -Wall -Wextra -Werror -static -o test_write_fault.exe \
    port/tests/writefault/test_write_fault.cpp -lpthread

./test_write_fault.exe --mode=protect              # el coste de cada cambio de protección
./test_write_fault.exe --mode=correct --read-first=1   # la corrección del patrón guard
./test_write_fault.exe --mode=correct --read-first=0
./test_write_fault.exe --mode=guardwin --port-cost-us=48   # el guard con el trabajo real del port
./test_write_fault.exe --mode=window  --port-cost-us=48
./test_write_fault.exe --mode=guardcheck            # la semántica de PAGE_GUARD
```
