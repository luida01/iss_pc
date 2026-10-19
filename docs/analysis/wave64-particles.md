# Wave64 en un host de 32 lanes: por qué faltaban partículas

> **Fecha:** 2026-10-19 · **Alcance:** `port/gpu/shadps4/shader_recompiler/**` (pass de wave64 y
> sus interruptores), `port/tests/**` (test unitario y sonda de GPU) ·
> **Estado:** fallo encontrado y corregido; verificado con test unitario y con la GPU real
> (RTX 3050, 32 lanes) · **sin validar en vivo** (el juego no se ejecutó: no hay sesión manual).

## 1. Resumen

El aviso del log

```
GPU [Render.Recompiler] <Warning> lower_wave64_pass.cpp:360 LowerWave64BallotPass:
  tlg: wave64 compute shader 0x7b77ce6 (64 threads, 64x1x1): ReadFirstLane 2
```

**no es un error**: es el censo que el propio port imprime (una línea por shader compute de
wave64 que usa operaciones de lane) para decir qué hace ese shader con sus lanes.

El problema real que había detrás es otro: **331 de los 346 shaders compute de wave64 del juego
(949 instrucciones `v_readfirstlane_b32`) NO se bajaban** a la forma wave64, porque el port solo
bajaba el `ReadFirstLane` en los shaders que además usaban un *ballot* o un `v_mbcnt`. En un host
de 32 lanes (NVIDIA), la mitad superior de cada wave64 leía **su propia lane 32** en vez de la
primera lane de la wave: en el patrón "append" (la wave reserva un hueco con un atómico y difunde
la base) la base de la mitad superior salía **0**, sus partículas se escribían encima de las de la
mitad inferior y **la mitad de la lista se perdía**. Eso es exactamente el síntoma "faltan muchas
partículas".

El arreglo (mínimo) es el interruptor `TLG_RFL_ALL`, que ya existía pero estaba **apagado por
defecto**: ahora está **encendido** (es un arreglo de correctitud, como `TLG_WQM`), y el `ReadFirstLane`
solo se baja cuando su valor puede diferir entre lanes (`IsProvablyWaveUniform`).

## 2. Qué significa exactamente el aviso

`LowerWave64BallotPass` (`port/gpu/shadps4/shader_recompiler/ir/passes/lower_wave64_pass.cpp`) es
una pasada **solo de compute** que, cuando el host no tiene subgrupos de 64 lanes, reescribe las
operaciones de lane de la wave64 del invitado para que den el mismo resultado que en la PS4.

Antes de reescribir nada, la pasada cuenta qué operaciones de lane usa el shader
(`CountLaneOps`) e imprime **una línea por shader**:

| Parte de la línea | Significado |
|---|---|
| `wave64 compute shader 0x7b77ce6` | hash del programa (PGM hash) del shader |
| `(64 threads, 64x1x1)` | tamaño del workgroup que el invitado despacha: **una wave64 completa** |
| `ReadFirstLane 2` | census de operaciones de lane: 2 instrucciones `v_readfirstlane_b32` |
| `, LtMask 4` | 4 máscaras `v_mbcnt_lo/hi` (`SubgroupLtMask`) |
| `Ballot 2` | 2 `v_cmp`/`v_cndmask` con exec ( ballots ) |

O sea: "este shader compute de 64 threads usa 2 `ReadFirstLane`; estos son los candidatos a
comportarse mal en hardware de 32 lanes". La línea es **informativa** (nivel `Warning` por
legibilidad del log, no por error); el trabajo real se hace en las líneas siguientes de la pasada
(`LOG_INFO "Lowering ... instruction for wave64"`, que el log de corrida filtra).

## 3. Cómo ejecuta el port un shader compute de wave64 en un host de 32 lanes

Dato del log de la sesión (`evidence/20261019-130735-g0-manual-paint`):

```
GPU: host subgroup size 32 (device default 32, min 32, max 32, required-size stages 0x3fff), LDS barriers on
```

La RTX 3050 no tiene wave64 (`maxSubgroupSize = 32`), así que:

* el pipeline de compute **no** puede pedir `requiredSubgroupSize = 64`
  (`vk_compute_pipeline.cpp` solo lo hace si `IsRequiredSubgroupSize64ValidFor(eCompute)`, es decir
  si el driver soporta 64 lanes), y corre con subgrupo de 32;
* `profile.subgroup_size = instance.ComputeSubgroupSize()` vale **32**, que es el tamaño para el
  que se bajan los shaders (`vk_pipeline_cache.cpp:837`);
* una wave64 del invitado (64 threads) cae en **dos subgrupos del host**: las lanes 0..31 en el
  subgrupo 0 y las lanes 32..63 en el subgrupo 1.

Consecuencia: **toda operación de subgroup del SPIR-V es por subgrupo, no por wave64**:

| Operación del invitado | En la PS4 (wave64) | En el host (32 lanes) sin bajar | Necesita intercambio |
|---|---|---|---|
| `v_readfirstlane_b32` | valor de la **primera lane activa de la wave** | valor de la primera lane activa **de cada subgrupo** → la mitad superior lee la lane 32 | sí |
| `v_readlane_b32 v, s` | valor de la lane `s` de la wave | `OpGroupNonUniformBroadcast` dentro del subgrupo → la lane 32..63 es "lane 0" del subgrupo 1 | sí |
| `v_cmp` + exec (ballot) | máscara de 64 bits de la wave | 32 bits por subgrupo | sí |
| `v_mbcnt_lo/hi` | cuenta de lanes activas por debajo, numeración de wave64 | por subgrupo | sí |

El `LocalInvocationId.x` sí es 0..63 (el workgroup del invitado tiene 64 threads), así que la
*numeración de lanes* del invitado se conserva; lo que se parte en dos son las operaciones de
subgrupo.

## 4. Qué hace la pasada y si sus resultados son correctos

`LowerWave64BallotPass` reescribe cuatro cosas (todas solo en compute, todas en **flujo uniforme**,
donde las 64 lanes de la wave están activas):

| Reescritura | Cómo | ¿Correcto? |
|---|---|---|
| `Ballot` | cada mitad de 32 lanes escribe su máscara de 32 bits en su slot de LDS; las dos mitades leen **el par de slots de la wave** (dirección con el bit 2 cleared) y componen la máscara de 64 bits | **sí** (arreglado en `e51a229`: antes la mitad impar leía su propio slot y el de la wave siguiente) |
| `ReadLane` con lane constante | cada host subgroup lee su lane local (`s & 31`) y la escribe en su slot; la lane `s` se lee del slot de la mitad `s >> 5` | **sí** |
| `SubgroupLtMask` (`v_mbcnt`) | se calcula la máscara `(1 << (lane & 63)) - 1` en numeración de wave64 | **sí** (uniforme) |
| `ReadFirstLane` | se intercambia por LDS: cada subgrupo escribe su `BroadcastFirst` en su slot y **todos** leen el slot de la mitad par (la primera lane de la wave) | **solo si se aplica** |

El intercambio de `ReadFirstLane` es correcto (es el que usa el test `ReadFirstLaneBarriers` y el
modelo `AppendPattern` del test), pero **la condición con la que se aplicaba era demasiado
estrecha**:

```cpp
// antes (lower_wave64_pass.cpp)
} else if (inst.GetOpcode() == IR::Opcode::ReadFirstLane && is_uniform &&
           (lane_math ||
            (Toggles::ReadFirstLaneAll() && !IsProvablyWaveUniform(inst.Arg(0))))) {
```

`lane_math` es true solo si el shader usa un *ballot* o un `v_mbcnt`. La hipótesis del autor
("el patrón append siempre cuenta con un ballot/mbcnt") **no se cumple en este juego**: 331 de los
346 shaders de wave64 usan `ReadFirstLane` **y nada más**. Con `TLG_RFL_ALL=0` (el valor por
defecto de entonces) esos 331 shaders se recompilaban **sin bajar**, y su mitad superior leía la
lane 32.

## 5. El fallo, con evidencia

### 5.1 Cuántos shaders

Del log de la sesión (346 líneas de censo, 346 hashes distintos, **todos con workgroup 64x1x1**):

| | shaders | instrucciones `ReadFirstLane` |
|---|---|---|
| **No se bajaban** (solo `ReadFirstLane`) | **331** | **949** |
| Se bajaban (con `Ballot` o `LtMask`) | 15 | 56 |
| Total | 346 | 1005 |

### 5.2 Por qué la mitad superior leía la lane 32

`EmitReadFirstLane` (`backend/spirv/emit_spirv_warp.cpp`) emite
`OpGroupNonUniformBroadcastFirst` con **scope Subgroup**: el valor de la primera lane activa *del
subgrupo del host*. En la mitad superior de la wave, la primera lane activa del subgrupo 1 es la
lane 32 del invitado, que en el patrón append **no reservó nada** (solo lo reserva la lane 0).

### 5.3 Qué se pierde (modelo `AppendPattern` del test)

La wave reserva un hueco (por ejemplo el 100) y cada lane escribe en `base + lane`:

| Forma del shader | base que ve la lane 63 | slots que toca la wave | dentro de la lista? |
|---|---|---|---|
| Referencia PS4 (wave64) | 100 | 100..163 (64) | sí |
| **Host sin intercambio** (lo que hacía el port) | **0** | 0..31 y 100..131 | **no**: la mitad superior escribe en otra lista |
| Host con intercambio (el arreglo) | 100 | 100..163 (64) | sí |

### 5.4 En la GPU real (RTX 3050, subgrupo de 32 lanes)

Sonda: `port/tests/wave64_rfl_probe.cpp` + `port/tests/wave64_gfx/*.comp` (las dos formas del
shader; la segunda es la que genera el port cuando baja el `ReadFirstLane`):

```
device: NVIDIA GeForce RTX 3050 Laptop GPU (driver NVIDIA), subgroup size 32
guest wave64: 64 lanes; host subgroup: 32 lanes

wave64_rfl.spv          (broadcast por subgrupo, lo que emitía el port sin bajar)
  lanes 0..31  : 32 of them have the wave's first lane (7)
  lanes 32..63 : 0 of them have the wave's first lane, lane 32 holds 3207
  whole wave   : 32 of 64 lanes match the wave64 reference
  RESULT: does NOT match the wave64 reference (32 of 64 lanes)

wave64_rfl_exchange.spv (intercambio por LDS, lo que emite el port con el arreglo)
  lanes 0..31  : 32 of them have the wave's first lane (7)
  lanes 32..63 : 32 of them have the wave's first lane, lane 32 holds 7
  whole wave   : 64 of 64 lanes match the wave64 reference
  RESULT: matches the wave64 reference
```

Es decir: **en el hardware del usuario, el shader recompilado sin intercambio NO daba el mismo
resultado que la referencia wave64**: la mitad superior de cada wave leía el valor de la lane 32.

### 5.5 El SPIR-V que genera el port (validado)

El test escribe el SPIR-V del shader bajado (`wave64-rfl.spv`); `spirv-val` lo acepta y el
desensamblado muestra exactamente el intercambio:

```
%63 = OpAtomicIAdd ...                          (el atómico que reserva el hueco)
%64 = OpLoad %gl_LocalInvocationIndex
%66 = OpShiftRightLogical %64 5                 (subgroup_id = index >> 5)
%69 = OpIAdd 256 %67                            (scratch_base + subgroup_id * 4)
%71 = OpGroupNonUniformBroadcastFirst %63       (el broadcast del host: la primera lane del subgrupo)
        OpStore %73 %71                         (cada subgrupo escribe su slot)
        OpControlBarrier 2 2 264                (barrera de workgroup: scope 2)
%76 = OpBitwiseAnd %69 4294967288              (offset & ~7: el slot de la mitad par)
%79 = OpLoad %78                                (toda la wave lee el slot de la mitad par)
        OpControlBarrier 2 2 264
```

El LDS del shader crece de 256 a 264 bytes (los 8 del scratch), como debe.

### 5.6 Lo que NO es la causa

* **El aviso no es un error.** Son 346 líneas de censo; ninguna dice "unsupported", "skip" ni
  "failed" (0 coincidencias en el log de la sesión). Ningún shader compute se descartó en esa
  corrida: la ruta de descarte del recompilador es
  `Shader::UnsupportedShaderError` (`flatten_extended_userdata_pass.cpp:848`), y no aparece.
* **El camino de compute es válido en Vulkan.** Las barreras que mete el intercambio son
  `OpControlBarrier` con scope **Workgroup** (`emit_spirv_barriers.cpp`), y se insertan solo en
  bloques de flujo uniforme, donde las 64 invocaciones del workgroup llegan a la barrera. El
  intercambio no introduce comportamiento indefinido: la sonda corre en la GPU real y el
  resultado coincide con la referencia.
* **El bajo FPS** no lo causa este fallo: el intercambio añade 2 barreras de workgroup por
  `ReadFirstLane`Instruction bajada (ver "riesgos"), pero el síntoma del usuario (partículas que
  faltan) sí lo causa.

## 6. El arreglo

1. `port/gpu/shadps4/shader_recompiler/recompiler_toggles.h`:
   `TLG_RFL_ALL` pasa a **encendido por defecto** (`Detail::AlwaysOn`, como `TLG_WQM`, que también
   es un arreglo de correctitud). `TLG_RFL_ALL=0` deja el comportamiento anterior para el A/B.
2. `port/gpu/shadps4/shader_recompiler/ir/passes/lower_wave64_pass.cpp`:
   * el `ReadFirstLane` se baja en **todo** shader compute de wave64 con flujo uniforme, y solo
     cuando su valor **puede** diferir entre lanes (`!IsProvablyWaveUniform`): si el valor es
     provablemente el mismo en todas las lanes, el broadcast por subgrupo ya da la respuesta
     correcta y no se paga ninguna barrera;
   * un `ReadFirstLane` en **flujo divergente** no se puede bajar (la barrera quedaría en control
     de flujo no uniforme): ahora se **cuenta** y se dice en la línea de censo
     (`... : ReadFirstLane 2 (1 ReadFirstLane in divergent flow, not lowered)`), que antes pasaba
     desapercibido.

El cambio de las fuentes del recompilador cambia la clave de la caché de shaders
(`TLG_SHADER_CACHE_KEY`), así que la **primera** partida tras el arreglo recompila los shaders
(cache invalidada una vez).

## 7. Interruptores para comparar (A/B)

Todos se leen **una vez** al arrancar (no se pueden cambiar en vivo) y se pueden poner por variable
de entorno o en `tlg.ini`:

| Variable | Efecto | Valor por defecto (este cambio) |
|---|---|---|
| `TLG_RFL_ALL` | baja el `ReadFirstLane` de wave64 en todo shader compute ancho (intercambio por LDS) | **1** (antes 0) |
| `TLG_LDS_SMALL` | barreras LDS en workgroups de menos de 64 threads | 1 (inFAMOUS) |
| `TLG_LDS_MULTI` | barreras LDS en workgroups de más de 64 threads | 1 (inFAMOUS) |
| `TLG_LDS_READLANE` | un `ReadLane` de lane constante en flujo uniforme se trata como uniforme | 1 (inFAMOUS) |
| `TLG_WQM` | `S_WQM_B64` agranda la máscara a cuads | 1 (todos) |
| `TLG_LANE_DIVERGENCE` | `mbcnt`, lane ids y resultados de atómicos hacen un branch divergente | 1 (inFAMOUS) |

El estado se imprime al arrancar el GPU:

```
GPU: shader switch TLG_RFL_ALL=1 (wave64 ReadFirstLane lowered in every wide compute shader ...)
```

A/B recomendado (una variable por vez): `TLG_RFL_ALL=0` (comportamiento anterior) frente a
`TLG_RFL_ALL=1` (arreglo), con la misma escena.

## 8. Shaders afectados (hashes del log)

**No se bajaban (331)** — con el arreglo, sus 949 `ReadFirstLane` se intercambian por LDS:

```
0x8fcf18fd 0x307fd9b4 0x412f4917 0x88ee83d 0x80ae565e 0x1f5cf710 0x826f997a 0xeb5375a9
0xf23bc319 0x42bea99e 0x8269e91c 0xa6380cf4 0xde242bc3 0x148b963a 0x1f48a6e6 0x998ade58
0x1adb3e24 0x8937e7ce 0x9de8b9f8 0xa4f5911e 0xa7e053c0 0x785ccb3 0x75ba8b31 0x6ab1edc
0xc45aae90 0xbc591672 0xbd38fb59 0xf2b30dd8 0xf81eeb3 0x635bda37 0x59e7568b 0x139d69ef
0x624f8567 0x50231e9b 0xd7ad709 0x33b6a082 0xc0f16912 0x166bc469 0x108ab422 0x21ed80c9
0xef805e39 0xccd7238e 0x885b0c21 0xfa607e7a 0xe6725dda 0x635bb4f1 0x772356be 0x31a66b7c
0x9cd53a1f 0xea1491d7 0xac47a84d 0x2af229f3 0x8ef70b73 0x7022bab0 0xdba471c5 0x93ef7708
0x32c8d320 0xbc54e17 0x7393bb91 0xee8da1de 0xb4d3269a 0xfbb6754f 0x7fcde757 0x3b3b0c5a
0xc5cd6893 0x7eb39747 0xb12f0f9d 0x3839cbea 0x29f590f3 0x19d3d32f 0x524bc500 0x1fc9e69c
0x873686aa 0x90077bbc 0xe0e09ba0 0x640139f5 0x5c2ebf73 0x24138a19 0xae1426d5 0x3188efb5
0x4274add5 0xdfea49e8 0xe48a1b96 0x75453dfa 0x2a1c2235 0xc4ff58ea 0xb84e1f13 0x65b5ada0
0xcbda3f53 0x5a0678c5 0xb01d343b 0xf4b085ab 0xabfa9551 0x5e294eae 0x8fd36dd1 0x5f5e8c53
0x56950967 0x4ca57581 0x632128c4 0xdacc9046 0xe6820c60 0x2daac6ac 0x732a08af 0xc12a9543
0x2347d40d 0xfc8218f1 0x32da6bef 0x5ac485d7 0x544712bd 0xa678eb4 0xafec2249 0xc2d8cecb
0x10ed83c2 0x99e2e2e9 0xc8a740fe 0xa135b623 0xa28f4c69 0x8ebe4238 0x2bad441d 0x55623552
0x380b8594 0x15f98f1 0xf91084e3 0x2383eee6 0x11fc6f56 0xe468bcf1 0x7dc1ee70 0x4e523719
0x29a303c3 0xc1788f73 0xd04b6a27 0xefda83e9 0xf7eedfcb 0x75ea4679 0x9de18ca8 0x8f0de050
0x18be2580 0xc77fcaf7 0x9ffd51a3 0xce188040 0xe4331629 0x2ddefab0 0x688da873 0xdd16c141
0xbda2ae2b 0xbae487fc 0x724457c9 0x38d91fd1 0xff51f69 0x8b02a9c9 0x399ba926 0x70ac8764
0xbe4d7c31 0x7833f76e 0x4e6e0512 0xee8d4fb1 0xe18ae602 0xe5a13ee5 0x86b4cb1a 0x840a5049
0x5ca57114 0x108a8dfd 0xef839591 0xedf941df 0x168145ef 0x70a492f6 0xa05d57e4 0x1ad1ad98
0xbfbc5254 0x6ab2b083 0x67a034e3 0xec3d7241 0x2b90da2b 0x390c80d5 0x8beb67d6 0x9242127c
0x3ea59f0c 0x7bf17531 0xe16ff9bb 0x5222d66f 0x742e8bc8 0x232393b4 0xb4d6bb8f 0xc0d1a5ea
0xf020b3f6 0x44d4153c 0x1abe9902 0x93252946 0x62d1cd02 0xf2d90f24 0x9afb1a71 0x5a256054
0xaf1e8ff2 0xc74a2bf2 0x279c7806 0xf83ff1ff 0x8641ee97 0xd783f500 0x7049708d 0x7720b348
0x9535894c 0x1e063fa1 0x98d249c8 0x60d78708 0x2343e528 0xe00e8a49 0x15aef4c2 0x348ee4c6
0xc3b7ca38 0xb646882f 0xe03f4b11 0x3ad451f2 0x7520c221 0x3470bd61 0x579334f0 0xbe6c3daf
0x9936e1cc 0x8bf3d2e0 0x131abba3 0xfa06188d 0x50400ed8 0xb2a3dc40 0x9acbd01f 0x9da87d7a
0x30bc4d02 0x8fbf1997 0xd4f6d74 0xb2c91d8d 0x78a95b08 0x9e3e1bb2 0xf3acc20 0xb0c97213
0xad5257d2 0x415c3acf 0xfeebf633 0x74229409 0x37280e27 0x88d364b8 0x255fb7de 0x6798448d
0xea81b698 0x7802026a 0x99296402 0xd78bcfb4 0xfb05a6f 0x9ebc8bb 0xe4ceeba4 0xa1d1f520
0xb4344501 0x706fa5d2 0x39a18a3a 0x971d244e 0x5e260336 0x7e64b582 0x3da7a35f 0x3299c2b8
0x97e426b2 0x40ec3b6e 0xd2dd5607 0x77a32c82 0x7a6aa06d 0x11873eb4 0x201dd847 0xa0437a93
0x96d847b2 0xeebc337 0x5797b59d 0x35598f8d 0x6eefbc6c 0xa1ab94d6 0xe8e4ba26 0x8c4e25ff
0xc5e52832 0x40430ca4 0xf7b1d4cd 0x98408e93 0x71c3d54f 0x5bf4f78d 0xb4c7566b 0xbd03537
0x3a4022e8 0x1fd3f53d 0xe717b605 0x9e2cffa5 0x64241091 0x1b1d1add 0xd9332247 0xc1a26909
0x85fd8258 0x9a0c1910 0x7174dd05 0x2a241ab5 0x3f661bd2 0xb5a02ede 0xef4e84fc 0xcdaa9825
0xa1f49361 0xbcd779f9 0xd5dafe52 0xef69deea 0xd681e3f3 0x32f33966 0xf766f22b 0xf86d1e67
0xcc284140 0xe39b5be5 0xbc00c5ca 0xe13b7993 0xcc50a429 0x7317e541 0xda840aae 0xda813abd
0xeb0e7aab 0xff6fcbd1 0x9f10c600 0xc4d3c63 0xc2342223 0xb5227ea3 0x89d1fb10 0xd4dd13ef
0xc8af9f7b 0x1266bee 0xaaf20a9a 0xb11a49a8 0x481b4300 0xf63559b2 0xa481c446 0x6c6ab24b
0xfd8e612e 0x2ea55832 0x64b8331b
```

**Ya se bajaban (15)**, por usar además `Ballot` o `v_mbcnt`:

```
0x5498a7eb 0xda5dd586 0x3d02a050 0xfdcb478e 0x53259f19 0xc73c37c1 0xd956bb50 0xcca43ce8
0x80bce9dd 0x20610fb2 0x1ace2629 0x622ba29f 0xa95ecf97 0x48298430 0x7fd68ce0
```

## 9. Limitaciones conocidas (siguen sin bajar)

| Caso | Por qué | Dónde se ve |
|---|---|---|
| `ReadFirstLane` en **flujo divergente** | el intercambio necesita una barrera de workgroup, que en control de flujo no uniforme es comportamiento indefinido en Vulkan | ahora se cuenta en la línea de censo del shader |
| `ReadLane` con el lane **en un registro** (`v_readlane_b32 v, s, v`) | solo se bajan los `ReadLane` con lane constante | ninguno en este juego (el censo no muestra `ReadLaneVar`) |
| `Ballot` que no se consume con `UnpackUint2x32` | el lowering del ballot solo se aplica a los ballots cuyo uso es un `UnpackUint2x32` | 1 shader en el juego (`Ballot 2`) |
| `ReadFirstLane` con el **exec mask** del invitado recortado (p. ej. `s_mov exec, 0xffff0000`) | el intercambio lee el slot de la mitad par; si la wave no tiene lanes activas en la mitad par, el valor es indefinido | no aparece en este juego (los shaders de wave64 trabajan con la wave completa) |
| **Etapas de gráficos** (VS/PS) | `LowerWave64BallotPass` es **solo de compute**: un lane op en un shader de gráficos sigue siendo un broadcast por subgrupo en un host de 32 lanes | con `TLG_VERBOSE_LOG=1` el port imprime el censo de las etapas de gráficos: `tlg: <stage> shader 0x... lane ops (not lowered for wave64): ...` |

### Sobre el "océano de avisos"

Las 346 líneas son **una por shader**, no una por instrucción, y son informativas: el log del
shader se compila una vez por combinación de specialising (aquí, con la caché de pipelines
calentada, ninguna). Si molesta, el sitio natural para bajarlas de nivel es la línea
`LOG_WARNING` de `lower_wave64_pass.cpp` (pasarla a `LOG_INFO` la sacaría del log de corrida, que
filtra `LOG_INFO`); se deja como está porque es el único sitio donde se ve qué shaders de wave64
usa el juego.

## 10. Riesgos del arreglo

* **Coste**: 2 barreras de workgroup por instrucción bajada (las que ya se metían en los 15
  shaders de la lista "ya se bajaban"). En este juego son 949 instrucciones en 331 shaders, todas
  de workgroups de 64 threads. Si el usuario ve una bajada de FPS en las escenas con muchas
  partículas, el A/B es `TLG_RFL_ALL=0`.
* **Cache de shaders**: el cambio de las fuentes del recompilador invalida la caché de shaders
  una vez (la primera partida recompila todo).

## 11. Reproducir

### Test unitario (sin juego, sin GPU)

```powershell
& 'C:\msys64\usr\bin\bash.exe' -lc 'source /c/Users/luisd/proyectos/infamous-port/port/msys2-env.sh; cd /c/Users/luisd/proyectos/infamous-port/port/out/gpu && ninja recompiler-fixes-test && ./recompiler-fixes-test'
```

Cubre: el `ReadFirstLane` del patrón append se baja (o no, con `TLG_RFL_ALL=0`), el intercambio
lee el slot de la mitad par, el `ReadFirstLane` en flujo divergente **no** se baja, el modelo
`AppendPattern` (referencia wave64 vs broadcast por subgrupo vs intercambio) y que un workgroup de
128 threads (dos waves) usa un par de slots por wave. Con un directorio como argumento escribe el
SPIR-V del shader bajado (`wave64-rfl.spv`) para `spirv-val`.

> **Parche propuesto (fuera del alcance de este agente):** en Windows el target
> `recompiler-fixes-test` no enlaza: `shadps4/common/io_file.cpp` usa `CreateFileMapping2`, que en
> MSYS2 solo está en el API set **onecore** (`tlg-probe` lo enlaza; este target no). El enlace
> falla con `undefined symbol: __declspec(dllimport) CreateFileMapping2`. En
> `port/gpu/CMakeLists.txt`:
>
> ```cmake
> if (WIN32)
>     # CreateFileMapping2 (shadps4/common/io_file.cpp) solo está en el API set "onecore".
>     target_link_libraries(recompiler-fixes-test PRIVATE onecore)
> endif()
> ```
>
> (El `sigjmp_buf` de `test_gpu_runtime_stubs.cpp` ya está resuelto en este commit: el stub usa el
> tipo del port cuando el target no lo define en la línea de comandos.)
>
> Con ese parche el enlace funciona; verificado con el mismo enlazado a mano.

### Sonda de GPU (la evidencia de la sección 5.4)

```powershell
& 'C:\msys64\usr\bin\bash.exe' -lc 'source /c/Users/luisd/proyectos/infamous-port/port/msys2-env.sh; cd /c/Users/luisd/proyectos/infamous-port/port/tests/wave64_gfx && glslang --target-env vulkan1.1 -o wave64_rfl.spv wave64_rfl.comp && glslang --target-env vulkan1.1 -o wave64_rfl_exchange.spv wave64_rfl_exchange.comp'
& 'C:\msys64\usr\bin\bash.exe' -lc 'source /c/Users/luisd/proyectos/infamous-port/port/msys2-env.sh; cd /c/Users/luisd/proyectos/infamous-port/port/tests && clang++ -std=c++20 -O2 -I ../gpu/third_party/vulkan-headers/include wave64_rfl_probe.cpp -lvulkan-1 -o ../out/gpu/wave64-rfl-probe.exe'
& 'C:\msys64\usr\bin\bash.exe' -lc 'cd /c/Users/luisd/proyectos/infamous-port/port/out/gpu && WAVE64_PROBE_DEVICE=NVIDIA ./wave64-rfl-probe.exe ../tests/wave64_gfx/wave64_rfl.spv ../tests/wave64_gfx/wave64_rfl_exchange.spv'
```

La sonda devuelve 1 si algún shader **no** coincide con la referencia wave64: con el broadcast por
subgrupo (el estado anterior del port) falla; con el intercambio (el estado nuevo) pasa.

## 12. Qué queda por hacer (fuera del alcance de este agente)

* Validar en vivo: la sesión manual del usuario con el arreglo (partículas completas) y el A/B de
  FPS (`TLG_RFL_ALL=0` / `1`) en la misma escena.
* Revisar el camino de compute del renderer (`video_core/renderer_vulkan/**`, fuera del alcance de
  este agente): el usuario reporta "ProcessCompute" hasta el 30,7 % del tiempo de CPU con ~860
  draws por frame. Este arreglo no toca ese camino; solo cambia el SPIR-V de 331 shaders.
* Los 4 casos de la sección 9 (divergente, `ReadLane` variable, `Ballot` sin `UnpackUint2x32`,
  exec mask recortado) necesitan un lowering propio (por ejemplo con `OpControlBarrier` de
  subgrupo y una máscara de exec por lane), no un intercambio por LDS.
