---
name: PS4 Native Port
description: Flujo de trabajo para port nativo de PS4 con loader, HLE y núcleo Vulkan de shadPS4
---
# PS4 Native Port

Skill operativa del proyecto. Úsala antes de tocar código, compilar o medir.

## Antes de tocar código

1. Lee `AGENTS.md`, `docs/architecture.md` y `docs/adr/`.
2. Identifica la capa: loader, runtime, GPU, launcher, build o tests.
3. Comprueba que el juego y los assets están fuera de Git.
4. Declara qué archivos vas a tocar y qué variable vas a medir.

## Arquitectura

- `eboot.bin` → `port/scripts/prepare.py` → `boot-linked.bin` → `probe.c` → código x86-64 nativo.
- `runtime_*.c` → HLE de librerías PS4 (memoria, hilos, FS, audio, video, input, servicios).
- `gpu/shadps4/` → GNM/PM4 + GCN → Vulkan/SPIR-V (texturas, buffers, pipelines, present).
- No hay emulación de CPU. No confundir con shadPS4 (emulador completo).

## Build Windows

```powershell
& 'C:\msys64\usr\bin\bash.exe' -lc 'cd /c/Users/luisd/proyectos/infamous-port/port && bash build.sh'
```

- Toolchain: MSYS2 CLANG64. Ver `docs/build-windows.md`.
- Artefactos: `port/out/tlg-probe.exe`, `tlg-gpu-capabilities.exe`, `tlg-play.exe`.
- Log: `port/out/gpu-build.log` y `evidence/build-*.log`.
- No mezcles un cambio de build con un cambio de runtime/GPU.

## Ejecución

```powershell
$env:TLG_GAME_DIR = 'C:\Users\luisd\Documents\Codex\2026-09-29\trabajemos-en-esto-quiero-crear-una\work\games\CUSA00223-view'
$env:TLG_DATA_DIR = 'C:\Users\luisd\proyectos\infamous-port\port'
$env:ISS_USER_DIR = 'C:\Users\luisd\proyectos\infamous-port\userdata'
python port\run.py
```

- `F10`: FPS. `Insert`: menú del port.
- Logs: `port/out/logs/run-*.log`.
- No uses `ISS_PIPELINE_BINARIES=1` con el driver NVIDIA 595.

## Experimento

1. Reproduce el fallo con una etiqueta nueva.
2. Formula una hipótesis causal.
3. Cambia **una sola variable**.
4. Ejecuta 3 veces la misma escena (misma resolución, mismos flags).
5. Guarda log, CSV, captura, hash y `result.json` en `evidence/`.
6. Compara mediana, p95, VRAM máxima, RAM máxima y calidad visual.
7. Si no hay evidencia, no lo declares mejora.

## Reglas

- No archivos del juego, claves, saves ni rutas personales en Git o en un release.
- No elimines sincronización sin medir.
- No copies código de fuentes no GPL.
- Para tras dos intentos fallidos y reporta con evidencia.
- Mantén `LICENSE` y atribuciones (GPL-2.0).
