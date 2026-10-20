# El coste de las reescrituras del invitado (zona roja + ISA)

Fecha: 2026-10-20. Entorno: Windows 11, MSYS2 CLANG64 (clang 22.1.7), i5-13420H.
Alcance: `port/src/isa_rewrite.c`, `port/src/isa_rewrite.h`, `port/tests/**`.
Pregunta: la zona roja (6.453 instrucciones protegidas en 5.898 parches sobre 196 funciones) y los
sitios de ISA (1.915) ¿hacen que el hilo principal del invitado vaya más lento de lo que debería?

## 1. Resumen (respuesta corta)

**La hipótesis no se sostiene en esta máquina, con la evidencia medida hasta ahora:**

| Reescritura | Coste medido por ejecución | Dónde se paga |
|---|---|---|
| Zona roja, stub directo | **0,21 – 0,47 ns** (mediana ~0,44 ns, ~1 ciclo) | cada instrucción protegida que se ejecuta |
| Zona roja, stub con relé de salto corto | **0,86 – 1,04 ns** (~2,2-2,7 ciclos) | 46 de los 5.898 grupos |
| Stub de ISA BMI1 (`blsr`) | 10,3 – 10,9 ns (~27 ciclos) | sólo en CPUs sin BMI1 |
| Stub de ISA SSE4A (`extrq`) | 12,7 – 12,9 ns (~33 ciclos) | **sólo SSE4A en este CPU** |
| Stub de ISA F16C (`vcvtph2ps`) | 113 – 117 ns (~300 ciclos) | sólo en CPUs sin F16C |

En este i5-13420H el port **solo reescribe SSE4A** (96 sitios de EXTRQ/INSERTQ): MOVBE, BMI1,
LZCNT y F16C son nativos. Con el número del log del usuario (0x893b3a `extrq` 13.584.303 veces en
30 min = 7.500/s), la reescritura de ISA cuesta 13,58 M × 13 ns = **176 ms en 30 min = 0,010 % de
un núcleo**. La zona roja, con 6.453 instrucciones protegidas, costaría el 1 % de un núcleo sólo si
el invitado ejecutara **25 millones** de instrucciones protegidas por segundo (0,4 ns × 25 M/s =
10 ms/s); el hilo principal del invitado no llega a esa tasa de instrucciones protegidas.

Lo que **no** se puede afirmar sin medir el juego en marcha es *cuáles* de las 196 funciones son
calientes. Para eso queda implementado el contador por instrucción protegida (§4) y el análisis
que lo ordena (§5).

## 2. Qué reescribe el port (los números reales de la tabla)

De `port/out/isa-rewrite.bin` (la tabla que genera `scripts/isa_rewrite.py` sobre
`port/out/boot-linked.bin`):

```
ISA: 1915 sites (MOVBE 27, BMI1 1615, LZCNT 164, F16C 13, SSE4A 96);
     148 relocated, 3 int3, 0 unsupported
Red zone: 6453 instructions protected in 5898 patches (46 through short-jump relays)
           over 196 red-zone functions of 45655 (5 analysed conservatively);
           6478 candidates, 0 use rsp, 0 control flow, 25 unrelocatable
```

Puntos clave:

- El juego usa la zona roja en **196 funciones** de 45.655; en 175 de ellas hay instrucciones
  protegidas (las otras 21 usan la zona roja pero no tienen accesos que puedan fallar con la zona
  viva: no hay nada que proteger).
- El reparto por función es muy desigual: la mediana es de 28 instrucciones protegidas por
  función, la máxima 219 (88 ns por pasada completa) y 43 funciones tienen 50 o más; 21 funciones
  usan la zona roja pero no tienen nada que proteger.
- **46 grupos** (0,8 %) entran por un relé de salto corto de 2 bytes: el sitio no cabe en un
  `jmp rel32` de 5 bytes y el port coloca un `jmp short` a una ranura de 5 bytes en el relleno
  entre funciones. Esos grupos pagan un salto más (0,9-1,0 ns en vez de 0,44 ns).

## 3. El banco de medición (`port/tests/test_rewrite_cost.c`)

Método: el mismo código de invitado se llama en bucle, una vez con la reescritura aplicada y otra
sin ella; la diferencia es el coste. La llamada y el bucle están en las dos medidas, así que se
cancelan. La página de datos del fixture queda escribible: no hay fallo, y lo que se mide es la
reescritura sola (el camino del fallo cuesta lo mismo con y sin reescritura).

```
bash port/tests/run_rewrite_cost.sh 2000000      # 2 M de llamadas por función
```

Salida (2 M de llamadas, la mejor de 5 rondas):

```
Red zone: 10 fixture functions, 2000000 calls each
  function             plain ns   patched ns   delta ns  delta cyc
  jmp                      0.90         1.34       0.44       1.15
  forward                  0.89         1.32       0.43       1.11
  backward                 0.88         1.33       0.46       1.19
  rip                      1.10         1.32       0.22       0.57
  flags                    0.89         1.34       0.44       1.16
  vex                      0.87         1.15       0.27       0.72
  push_sub                 1.10         1.34       0.24       0.64
  dead                     0.88         0.88      -0.00      -0.01     <- control: no protegida
  padding_relay            1.11         1.96       0.86       2.23     <- relé de salto corto
  host_relay               3.49         3.63       0.14       0.36
Red zone: rewrite cost 0.35 ns per call (0.91 cycles), plain 1.21 ns
ISA: blsr   native   0.90 ns, stub  11.63 ns, stub cost  10.73 ns (28.01 cycles)
ISA: extrq  native   0.00 ns, stub  12.93 ns, stub cost  12.93 ns (33.77 cycles)
ISA: f16c   native   0.00 ns, stub 114.87 ns, stub cost 114.87 ns (299.96 cycles)
```

(Los "ciclos" son ticks del TSC, que corre a la frecuencia nominal; el núcleo sube de reloj, así
que en ciclos reales son menos.)

Lecturas:

- **La zona roja no guarda 128 bytes ni llama a un helper.** El stub es
  `lea rsp,[rsp-128]` + la instrucción + `lea rsp,[rsp+128]` + `jmp` de vuelta: el `rsp` se baja
  128 bytes (fuera de la zona roja) mientras corre la instrucción que puede fallar, y se sube
  después. No toca ningún registro (el `lea` no toca las banderas) y no llama a nadie. Lo que
  cuesta es el salto de ida y vuelta, no un guardado.
- **El stub de ISA sí es caro**: guarda 11-16 registros + las banderas, copia el XMM/YMM a un
  búfer y **llama a un helper en C** (`isa_emulate`, `isa_emulate_sse4a`, `isa_emulate_f16c`).
  De ahí los 10-13 ns (y los 113-117 ns del F16C, que copia 64 bytes de YMM y hace 4 conversiones
  en software).
- El **contador de aciertos** de los stubs de ISA cuesta ~0,2-0,4 ns (se puede quitar con
  `TLG_ISA_STATS=0`: el stub de `blsr` pasa de 11,63 a 11,20 ns y el de `extrq` de 12,93 a
  12,66 ns).

## 4. Los contadores por instrucción protegida (implementado)

`TLG_REDZONE_STATS` pone un contador en cada instrucción protegida:

- `TLG_REDZONE_STATS=1` → cuenta y lo dice en el informe de salida.
- `TLG_REDZONE_STATS=<fichero>` → cuenta y además vuelca `0x<sitio> <ejecuciones>` al fichero.
- `TLG_ISA_STATS=<fichero>` → vuelca también los contadores de los sitios de ISA.

Cómo está hecho (todo en `port/src/isa_rewrite.c`): el area de stubs de la zona roja crece con
una **trampolín por grupo** (`inc qword [rip+despl]` + `jmp rel32`, 12 bytes) y con el array de
contadores. El sitio salta a la trampolín, que cuenta y salta al stub. La trampolín no toca ningún
registro (el incremento es relativo a `rip`), así que los registros del invitado salen del stub
igual que entraron. La trampolín también cubre el caso del relé: se redirige el `jmp near` del
relé, no el `jmp short` del sitio.

Comprobado con el fixture (9 grupos: stubs directos, el caso "backward" (el span empieza antes
del sitio) y los dos casos de relé): cada instrucción protegida se cuenta a sí misma
(`port/tests/test_rewrite_cost.c`, y el validador de la tabla real en §5 dice que **los 5.898
grupos del juego** son contables: 5.852 directos + 46 relés, 0 sin contar).

Coste de la instrumentación: **+0,2 – 0,8 ns por instrucción protegida** (la trampolín + el
`inc` a memoria; la mediana de las diez funciones del fixture: 0,35 ns sin contadores → 0,57 ns
con ellos). Es opt-in: con `TLG_REDZONE_STATS` sin valor no hay trampolines ni contadores y el
port se comporta igual que antes.

## 5. Qué funciones son calientes (`port/tests/red_zone_hot.py`)

El tool une la tabla (dónde está cada grupo) con la imagen enlazada (a qué función pertenece cada
dirección, con las tablas `.eh_frame_hdr` que ya lee `scripts/red_zone.py`):

```
python port/tests/red_zone_hot.py --table port/out/isa-rewrite.bin --image port/out/boot-linked.bin \
    --hits redzone-hits.txt --isa-hits isa-hits.txt
```

Salida estática (las 175 funciones con instrucciones protegidas, por coste de una pasada
completa):

```
            function  groups  protected  relays  cost per full pass
  0x008a1140-0x008a2352     219        219       0      87.6 ns
  0x00884130-0x008853a2     152        161       0      64.4 ns
  0x007a46b0-0x007a4b8f     128        128       0      51.2 ns
  0x007a61d0-0x007a66f6     128        128       0      51.2 ns
  0x001cf4e0-0x001cfb4e     105        108       0      43.2 ns
  ...
all of them: 6453 protected instructions, 46 relays; a function that runs all of them pays about 2.6 us
```

Con `--hits` (los contadores de una sesión de juego) el mismo tool ordena las funciones por
ejecuciones y estima el coste total:

```
estimated cost of the red-zone rewrite: X ms of CPU (Y% of one core per second of run)
```

**Ésta es la medición que falta** y la deja lista: sin ella, cualquier afirmación sobre las
funciones calientes sería una suposición.

## 6. El interruptor para el A/B en caliente

| Variable | Efecto | Dónde |
|---|---|---|
| `TLG_REDZONE_STATS=1` | cuenta cada instrucción protegida (+0,2-0,8 ns) | `src/isa_rewrite.c` (nuevo) |
| `TLG_REDZONE_STATS=<fichero>` | lo mismo y vuelca los contadores | `src/isa_rewrite.c` (nuevo) |
| `TLG_ISA_STATS=0` | quita los contadores de los stubs de ISA (~0,2-0,4 ns por emulación) | `src/isa_rewrite.c` (nuevo) |
| `TLG_ISA_STATS=<fichero>` | vuelca los contadores de los sitios de ISA | `src/isa_rewrite.c` (nuevo) |
| `TLG_RED_ZONE_REWRITE=0` | **quita la protección de la zona roja** (A/B del coste; el juego puede corromper la partida: "disc is corrupt") | `src/probe.c` (ya existía) |
| `TLG_RED_ZONE_REWRITE=1` | la fuerza (por defecto en Windows) | `src/probe.c` (ya existía) |
| `TLG_ISA_REWRITE=0\|1\|lista` | qué conjuntos de ISA se reescriben | `src/probe.c` (ya existía) |

Procedimiento para la sesión de juego (la hace el usuario; aquí no se ejecuta el juego):

```powershell
# 1) el banco, para tener las cifras de referencia
bash port/tests/run_rewrite_cost.sh 2000000

# 2) el juego con los contadores
$env:TLG_REDZONE_STATS = "$PWD\redzone-hits.txt"
$env:TLG_ISA_STATS     = "$PWD\isa-hits.txt"
python port\run.py

# 3) el análisis
python port/tests/red_zone_hot.py --hits redzone-hits.txt --isa-hits isa-hits.txt

# 4) el A/B de la protección (OJO: sin protección el juego puede corromper la partida)
$env:TLG_RED_ZONE_REWRITE = '0'   # y luego '1'
```

## 7. Lo que se ha cambiado (y su medida)

1. **Contadores por instrucción protegida** (`TLG_REDZONE_STATS`): trampolín por grupo + array de
   contadores en el area de la zona roja, informe con los más ejecutados y vuelco a fichero.
   Medido: +0,2-0,8 ns por instrucción protegida (opt-in).
2. **Volcado de los contadores de ISA** (`TLG_ISA_STATS=<fichero>`) y **total de emulaciones** en
   el informe (`ISA: N rewrites executed over M sites`), que antes no existía.
3. **Interruptor `TLG_ISA_STATS=0`**: quita el `inc` de memoria de cada stub de ISA. Medido:
   ~0,2-0,4 ns por emulación.
4. **Banco de medición** (`port/tests/test_rewrite_cost.c` + `port/tests/run_rewrite_cost.sh`):
   el coste por llamada y por ejecución, con control (una función sin proteger da 0,00 ns).
5. **Tool de análisis** (`port/tests/red_zone_hot.py`): tabla ↔ funciones, ranking y estimación de
   coste.

## 8. Conclusiones y recomendaciones

- La reescritura de **ISA no es un problema en este equipo**: aquí sólo se reescribe SSE4A
  (96 sitios) y su coste medido es 13 ns por ejecución; con el número del log son 0,010 % de un
  núcleo. En un equipo sin BMI1/MOVBE (Sandy Bridge) el coste subiría a ~1.915 sitios × 11 ns,
  pero el número de ejecuciones de esos sitios también baja.
- La **zona roja** cuesta ~0,44 ns (1 ciclo) por instrucción protegida, no un guardado de 128
  bytes: el port mueve la instrucción fuera de línea y baja el `rsp` 128 bytes. El coste total
  depende de cuántas instrucciones protegidas se ejecuten, y eso lo mide el contador nuevo.
- **Si saliera una función caliente** (una de las de la lista de §5 con muchas instrucciones
  protegidas en un bucle), el sitio donde mirar no es el stub sino el **análisis de vivacidad**:
  `scripts/red_zone.py` protege una instrucción cuando la zona roja está viva en ese punto; 5
  funciones se analizan "de forma conservadora" (todo vivo) y 25 sitios quedaron sin proteger por
  no ser relocalizables. Ahí es donde se podría ganar (mejorar el análisis), no en el stub.
- **No se recomienda** tocar el stub de la zona roja (es ya mínimo: 2 `lea` + 2 saltos) ni
  reescribir el stub de ISA en ensamblador para ahorrar ~10 ns: en este equipo no se paga.
- Lo **recomendado** es la **medición**: una sesión de 15-30 min con `TLG_REDZONE_STATS` y
  `TLG_ISA_STATS` da el ranking real y el coste total; con eso se decide si hay algo que hacer.

## 9. Observaciones laterales (fuera de alcance)

- `scripts/isa_rewrite.py` marca como *unsupported* los sitios F16C de menos de 5 bytes (la forma
  de registro de `VCVTPH2PS` son 4 bytes) porque el camino `int3` no tiene el estado YMM. En este
  juego los 13 sitios F16C son de memoria (≥ 5 bytes) y no hay ninguno *unsupported*, pero un
  `vcvtph2ps xmm0, xmm1` en el código del juego dejaría el puerto parado con un mensaje claro en
  vez de emularlo. Es un cambio de una línea en el script (el sitio de 4 bytes podría usar el stub
  con `jmp` a una zona de 5 bytes, como los `JMP_RELOCATE`).
- `port/tests/test_isa_sse4a.c` falla ya antes de estos cambios en el caso
  `extrq xmm0, xmm0 (length 58, index 39)`: el resultado difiere del esperado por el estado de la
  zona roja. No lo he tocado (no es de este encargo) pero queda anotado.
