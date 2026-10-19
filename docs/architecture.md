# Arquitectura del port nativo

Este documento describe cómo se ejecuta inFAMOUS Second Son en PC sin emular la CPU.
Está basado en el código de `HayatoG/iss_pc` y en la documentación upstream.

## 1. Pipeline de arranque

```
dump CUSA00223 (base 1.00 + update 1.07)
        │
        ├─ scripts/prepare.py       → extrae y convierte eboot.bin en un ELF cargable
        ├─ scripts/link_libc.py     → enlaza libc.prx / libSceFios2.prx del update
        ├─ scripts/link_modules.py  → resuelve imports y módulos del juego
        ├─ scripts/isa_rewrite.py   → reescribe instrucciones no soportadas o peligrosas
        └─ scripts/patches.py       → aplica parches por hash de eboot.bin
        │
        ▼
port/out/boot-linked.bin + content.bin + patches.bin
        │
        ▼
port/src/probe.c  (loader Windows/Linux)
        │  mapea segmentos, TLS, relocaciones, heaps, hilos
        ▼
código x86-64 original del juego
        │  imports de libSce*
        ▼
port/src/runtime_*.c  (HLE)
        │  GNM / PM4 / shaders GCN
        ▼
port/gpu/shadps4/  (Vulkan)
        │
        ▼
GPU del PC
```

## 2. Capas y responsabilidades

| Capa | Rutas | Responsabilidad |
|---|---|---|
| Preparación | `port/scripts/` | Parsear SELF/ELF, enlazar módulos, parches, contenido |
| Loader | `port/src/probe.c`, `cpu_check.c`, `isa_*` | Cargar el juego, memoria, TLS, excepciones, ISA |
| Runtime HLE | `port/src/runtime_*.c` | Hilos, semáforos, FS, audio, video, input, servicios |
| Runtime del juego | `port/src/runtime_infamous.c` | Comportamiento específico de inFAMOUS |
| GPU | `port/gpu/shadps4/` | Traducción GNM/PM4, texture/buffer cache, pipelines, present |
| Launcher | `port/launcher/` | Configuración, saves, perfiles, reportes |
| Packaging | `port/packaging/` | ZIP Windows/Linux, launchers congelados |

## 3. Windows vs Linux

El código comparte fuentes, pero Windows tiene diferencias críticas:

- **TLS:** en Linux se reescribe `fs:[0]` a `gs:[0]`; en Windows GS es el TEB y no se puede mover.
  El loader reescribe el desplazamiento a `gs:[0x1480 + 8*slot]` (TlsSlots) y guarda el TCB del
  hilo invitado en ese slot.
- **Memoria:** Linux usa memfd + `mmap(MAP_FIXED)`. Windows reserva el rango PS4 64 GiB–1 TiB
  como placeholder y usa `VirtualAlloc2` / `MapViewOfFile3` para las vistas.
- **Excepciones:** Linux usa SIGSEGV + `sigsetjmp`; Windows usa un Vectored Exception Handler
  y `tlg_setjmp`/`tlg_longjmp` para restaurar registros Win64 sin unwind data.
- **Hilos:** Windows asigna stacks fully committed; el stack del hilo principal es de 16 MiB.
- **Temporizadores:** se usan waitable timers de alta resolución; winpthreads redondea a 15.6 ms.
- **GPU:** `libtlggpu.a` es estático y se enlaza en `tlg-probe.exe`.
- **Reinicio:** `run.py` pasa `TLG_RESTART_COMMAND`; el nuevo proceso espera al anterior.

## 4. Memoria y GPU (foco 4 GB)

El juego usa direcciones de 64 bits y un modelo de memoria de consola. El port traduce:

- **Guest memory:** arena virtual + vistas; el GPU tracker marca páginas sucias/limpias.
- **Texture cache:** sube texturas GNM a imágenes Vulkan, con evicción cuando falta VRAM.
- **Buffer cache:** buffers de vértices/índices/indirectos, con write-merge y readbacks.
- **Pipelines:** caché en memoria y en disco (BP08); la compilación de shaders causa stutter.
- **Readbacks:** algunas decisiones del juego dependen de datos leídos de la GPU; deben ser
  precisos o se rompe la imagen.

En 4 GB, los puntos de fallo esperados son:
1. Presupuesto de texturas por encima de la VRAM real.
2. Allocaciones grandes/sparse que el driver no puede respaldar.
3. Readbacks que fuerzan sincronización y reducen FPS.
4. Presión de memoria que provoca evicción constante (thrashing).

Ver `docs/analysis/vram-budget.md` para el mapa detallado y los experimentos.

## 5. Flujo de trabajo del fork

```
upstream/main ──fetch──> fork/main ──rama──> cambio aislado ──build──> evidencia ──PR
```

- Un cambio por rama; mensaje en inglés, estilo upstream.
- Rebase frecuente contra `upstream/main`; evitar patches que toquen cientos de líneas.
- Los flags nuevos usan el prefijo `ISS_` y se documentan en `docs/EXPERIMENTAL_FIXES.md`.
- Los cambios genéricos de GPU se proponen también a shadPS4.

## 6. Enlaces

- `README.md` upstream: uso, flags, roadmap, limitaciones.
- `docs/ARQUIVOS_NECESSARIOS.md`: cómo montar el dump.
- `docs/ROADMAP.md`: pendientes upstream.
- `docs/EXPERIMENTAL_FIXES.md`: BP01–BP12 y flags A/B.
- `docs/build-windows.md`: build reproducible.
- `docs/qa-harness.md`: medición y evidencia.
