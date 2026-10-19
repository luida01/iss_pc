# Build nativo de Windows (MSYS2 CLANG64)

Guía operativa para compilar el port en Windows con el mismo árbol de fuentes que Linux.
Está basada en los scripts reales del repositorio (`port/build.sh`, `port/msys2-env.sh`,
`port/gpu/CMakeLists.txt`, `port/packaging/windows/README.md` y
`.github/workflows/build.yml`), no en pasos supuestos.

Rutas de esta máquina:

- Repositorio: `C:\Users\luisd\proyectos\infamous-port`
- MSYS2: `C:\msys64`
- Shell de compilación: **MSYS2 CLANG64** (no MINGW64, no UCRT64, no Git Bash, no WSL)

> **Aviso sobre `tools/Build.ps1`:** el `.gitignore` del repositorio excluye `/tools/`
> ("herramientas de terceros instaladas localmente"). El script creado en
> `tools/Build.ps1` funciona, pero Git no lo versiona. Si debe formar parte del
> repositorio, hay que moverlo a una ruta versionada (por ejemplo
> `scripts/devtools/Build.ps1`) o ajustar el `.gitignore`. En esta tarea no se modificó
> el `.gitignore`; `AGENTS.md` ya documenta esta trampa.

## 1. Requisitos

- Windows 10 1903+ o Windows 11, 64 bits.
- MSYS2 instalado en `C:\msys64` (o pasar otra ruta con `-Msys2Root`).
- Git para Windows/MSYS2 y los submódulos.
- PowerShell 5.1 o PowerShell 7 (`pwsh`).
- Espacio en disco y red para la primera instalación de paquetes MSYS2.
- Para **compilar** no hace falta una GPU Vulkan ni archivos del juego. Para **ejecutar**
  sí hacen falta un driver Vulkan y una copia propia de `CUSA00223`.

Si MSYS2 acaba de instalarse, actualiza primero y vuelve a abrir la shell:

```bash
pacman -Syu
```

Si la actualización de `msys2-runtime` pide cerrar las shells, ciérralas todas y abre
CLANG64 de nuevo antes de continuar. `Build.ps1` no ejecuta `pacman`.

## 2. Paquetes exactos

Abre **MSYS2 CLANG64** (menú Inicio, o `C:\msys64\msys2_shell.cmd -clang64`) e instala:

```bash
pacman -S --needed \
  git patch \
  mingw-w64-clang-x86_64-toolchain \
  mingw-w64-clang-x86_64-cmake \
  mingw-w64-clang-x86_64-ninja \
  mingw-w64-clang-x86_64-pkgconf \
  mingw-w64-clang-x86_64-python \
  mingw-w64-clang-x86_64-vulkan-headers \
  mingw-w64-clang-x86_64-vulkan-loader \
  mingw-w64-clang-x86_64-vulkan-memory-allocator \
  mingw-w64-clang-x86_64-glslang \
  mingw-w64-clang-x86_64-sdl3 \
  mingw-w64-clang-x86_64-fmt \
  mingw-w64-clang-x86_64-boost \
  mingw-w64-clang-x86_64-robin-map \
  mingw-w64-clang-x86_64-xxhash \
  mingw-w64-clang-x86_64-ffmpeg \
  mingw-w64-clang-x86_64-zydis \
  mingw-w64-clang-x86_64-spirv-tools \
  mingw-w64-clang-x86_64-spirv-headers \
  mingw-w64-clang-x86_64-spirv-cross \
  mingw-w64-clang-x86_64-freetype
```

Notas verificadas:

- `mingw-w64-clang-x86_64-toolchain` aporta `clang`, `clang++`, `lld` y las herramientas
  LLVM que usa el empaquetado (`llvm-strip`, `llvm-readobj`).
- **`mingw-w64-clang-x86_64-freetype` es obligatorio**, aunque el comando del
  `port/packaging/windows/README.md` no lo incluye: `gpu/CMakeLists.txt` hace
  `pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)` y el workflow de
  Windows sí lo instala.
- `mingw-w64-clang-x86_64-python` está en la lista del README, pero el build C no lo
  necesita. El empaquetado (`package.sh`) usa un Python **de Windows** (python.org),
  no el de MSYS2.
- `zydis` no publica un `.pc` en CLANG64; CMake lo encuentra mediante
  `zydis-config.cmake`. Que `pkg-config --exists zydis` falle no es un problema.
- `git` y `patch` son paquetes MSYS2, no CLANG64. El resto debe ser CLANG64: mezclar
  UCRT64/MINGW64 produce DLLs incompatibles o `pkg-config` sin las librerías correctas.

Comprobación rápida dentro de CLANG64:

```bash
command -v clang cmake ninja pkg-config glslang
pkg-config --modversion vulkan sdl3 freetype2 libavcodec
```

Los cinco ejecutables deben resolver a `/clang64/bin/...`.

## 3. Submódulos

Desde la raíz del repositorio:

```bash
cd /c/Users/luisd/proyectos/infamous-port
git submodule update --init --recursive
```

`build.sh` intenta actualizar submódulos, pero su comprobación en Windows solo mira
`fsr-vulkan`, `imgui` y `xbyak`. Si falta otro (por ejemplo `miniz` o
`vulkan-headers`) y esos tres existen, CMake puede fallar. Ejecuta el comando completo
de arriba antes del primer build.

Submódulos: `fsr-vulkan`, `imgui`, `magic_enum`, `miniz`, `vulkan-headers`, `xbyak` y
`LibAtrac9`. `sirit`, `fonts`, `gcn` y `half` están versionados dentro del repositorio,
no son submódulos. No se descarga ningún archivo del juego.

## 4. Compilar

### 4.1 Desde PowerShell (comando directo)

```powershell
Set-Location C:\Users\luisd\proyectos\infamous-port
& C:\msys64\usr\bin\bash.exe -lc "cd /c/Users/luisd/proyectos/infamous-port/port && bash build.sh"
```

Usa la ruta absoluta `C:\msys64\usr\bin\bash.exe`. Un `bash` cualquiera puede ser el de
WSL (`C:\Windows\System32\bash.exe`) o Git Bash; el build de Windows espera MSYS2.
`build.sh` detecta `MINGW*`/`MSYS*`, carga `port/msys2-env.sh` y pone CLANG64 primero
en `PATH`.

`MSYS2_ROOT` es opcional. Si se define para el build debe ser una ruta **MSYS**
(`/c/msys64` o `/d/msys64`), no `C:\msys64`: `msys2-env.sh` la usa como
`"$msys2_root/clang64/bin"`. `Build.ps1` hace esta conversión automáticamente.

Valores opcionales:

```powershell
$env:TLG_LTO = 'ON'   # por defecto Windows compila con LTO OFF
```

`ISS_LTO`/`ISS_PGO` son alias de compatibilidad, pero en Windows `build.sh` fuerza
`TLG_PGO=off`; PGO no está disponible en esta plataforma.

### 4.2 Desde PowerShell con el script

Desde la raíz del repositorio:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\luisd\proyectos\infamous-port\tools\Build.ps1
```

O con PowerShell 7:

```powershell
pwsh -NoProfile -File C:\Users\luisd\proyectos\infamous-port\tools\Build.ps1
```

Si la política de ejecución bloquea el script:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\tools\Build.ps1
```

Opciones:

```powershell
# Borra out\gpu, out\gpu-build.log y los tres .exe, y recompila
.\tools\Build.ps1 -Clean

# Compila y después ejecuta el subconjunto de tests C disponibles en Windows
.\tools\Build.ps1 -Tests

# Ambas cosas
.\tools\Build.ps1 -Clean -Tests

# Otra instalacion de MSYS2
.\tools\Build.ps1 -Msys2Root D:\msys64
```

Qué hace `Build.ps1`:

- Invoca exactamente
  `C:\msys64\usr\bin\bash.exe -lc "cd /c/.../port && bash build.sh"`.
- Escribe toda la salida en `evidence\build-<timestamp>.log` (UTF-8) y la muestra en
  consola.
- Comprueba que existan y no estén vacíos `tlg-probe.exe`, `tlg-gpu-capabilities.exe`
  y `tlg-play.exe`.
- Devuelve `0` si todo va bien; el código de `build.sh` si la compilación falla; `1`
  si faltan artefactos, fallan los tests o falla el propio script.
- `-Clean` borra solo artefactos de build: `out\gpu` (cache CMake/Ninja y tests),
  `out\gpu-build.log` y los tres `.exe`. **Conserva** `out\logs\run-*.log` y los
  archivos generados por `run.py` (`boot*.bin`, `content.bin`, etc.), además de
  `port\src`, `port\gpu`, `port\dist`, `port\user` y `port\pgo`. Si
  `tlg-probe`/`tlg-play` están ejecutándose, aborta antes de compilar para no
  sobrescribir un `.exe` bloqueado.

### 4.3 Desde MSYS2 CLANG64

Abre la shell CLANG64 (menú Inicio o, desde PowerShell,
`C:\msys64\msys2_shell.cmd -clang64 -here`) y ejecuta:

```bash
cd /c/Users/luisd/proyectos/infamous-port
git submodule update --init --recursive
cd port
bash build.sh
```

Limpieza manual (solo artefactos, sin borrar `out/logs` ni los `boot*.bin`):

```bash
rm -rf out/gpu out/gpu-build.log out/tlg-probe.exe out/tlg-gpu-capabilities.exe out/tlg-play.exe
bash build.sh
```

### 4.4 Qué ejecuta `build.sh` en Windows

1. Entra en `port/`, calcula `TLG_BUILD_COMMIT` (SHA de Git, con `+dirty` si hay cambios).
2. Carga `msys2-env.sh` y comprueba `clang`, `cmake`, `ninja` y `pkg-config`.
3. Actualiza submódulos si faltan los tres directorios que comprueba y aplica
   `gpu/patches/fsr-vulkan/*.patch` al submódulo FSR (una sola vez).
4. Configura CMake en `out/gpu` con Ninja, `RelWithDebInfo`, `TLG_PGO=off` y
   `TLG_LTO` (OFF por defecto; el CI usa ON).
5. Compila los targets `tlg-probe`, `tlg-gpu-capabilities` y `tlg-play`; el log
   completo de Ninja queda en `out/gpu-build.log`.
6. Copia los tres `.exe` a `out/` y termina.

En Windows `libtlggpu` es **estática** (`libtlggpu.a`) y se enlaza dentro de
`tlg-probe.exe`; no se genera `libtlggpu.so` ni una DLL aparte.

## 5. Artefactos esperados

| Ruta | Qué es |
|---|---|
| `port\out\tlg-probe.exe` | Loader/runtime principal del juego. |
| `port\out\tlg-gpu-capabilities.exe` | Diagnóstico de capacidades Vulkan (no es necesario para arrancar con `run.py`). |
| `port\out\tlg-play.exe` | Arranque directo con la configuración guardada del launcher. |
| `port\out\gpu\libtlggpu.a` | Núcleo de GPU estático enlazado en `tlg-probe.exe`. |
| `port\out\gpu\` | Árbol CMake/Ninja, cabeceras de shaders y binarios de test. |
| `port\out\gpu-build.log` | Salida completa de Ninja de la última compilación. |
| `evidence\build-<timestamp>.log` | Log de `Build.ps1` (consola + salida de bash). |
| `port\dist\infamous-windows-x86_64.zip` | Solo si se ejecuta `packaging/windows/package.sh`. |

`Build.ps1` valida los tres primeros. `out/gpu-build.log` se sobrescribe en cada
build; conserva el log de `evidence` si necesitas comparar.

## 6. Ejecutar desde el árbol de fuentes

Necesitas tu copia legal del juego (por ejemplo `CUSA00223` con `eboot.bin`). No se
incluye ningún archivo del juego en el repositorio ni en el build.

```powershell
Set-Location C:\Users\luisd\proyectos\infamous-port\port
$env:TLG_GAME_DIR = 'F:\ruta\a\CUSA00223'
python .\run.py
```

`run.py`:

- Hace `chdir` a `port/`.
- Busca `tlg-probe.exe` en `bin/` (paquete) o `out/` (build local).
- Si no hay `SDL3.dll` junto al ejecutable, añade
  `C:\msys64\clang64\bin` (o `$env:MSYS2_ROOT\clang64\bin`) al `PATH`.
- Prepara el `eboot.bin` con los scripts de `port/scripts/` y lanza el juego.

Para `run.py`, `MSYS2_ROOT` debe ser una ruta **Windows** (`C:\msys64`), al contrario
que para `build.sh`, que la espera POSIX. Si usas la instalación por defecto no hace
falta definirlo.

Diagnóstico mínimo sin cargar el juego (con las DLLs en `PATH`):

```powershell
$env:PATH = 'C:\msys64\clang64\bin;' + $env:PATH
.\port\out\tlg-probe.exe --cpu-features
.\port\out\tlg-gpu-capabilities.exe
```

`tlg-gpu-capabilities.exe` necesita el loader y un driver Vulkan; si no hay driver,
fallará aunque la compilación sea correcta.

## 7. Tests

### Limitación importante

En **Windows**, `bash build.sh --test` y `bash build.sh --test-only` **no ejecutan
tests**. El bloque `MINGW*|MSYS*` de `port/build.sh` termina con `exit 0` antes de
llegar al bloque de tests (la rama de tests es para Linux). El README de Windows
también indica que varios programas de `tests/` no están portados. El workflow de
Windows **no** ejecuta tests C: solo Build, Package y el selftest del launcher Qt.

No uses `build.sh --test` como validación en Windows: devuelve éxito sin probar nada.

### Tests disponibles

`port/gpu/CMakeLists.txt` define explícitamente un subconjunto de tests para `WIN32`
con `EXCLUDE_FROM_ALL`. `Build.ps1 -Tests` compila y ejecuta estos trece:

```text
mp3-test            tests/data/sine.mp3
videodec-test       tests/data/tiny.h264
services-test
pad-map-test
mouse-stick-test
audio-downmix-test
cpu-check-test
isa-emulate-test
isa-stub-test       tests/data/isa_fixture.bin
isa-sse4a-test
isa-f16c-test
rwlock-rp-test
tsc-clock-test
```

Comando manual equivalente desde MSYS2 CLANG64:

```bash
cd /c/Users/luisd/proyectos/infamous-port/port
source ./msys2-env.sh
ninja -C out/gpu mp3-test videodec-test services-test pad-map-test mouse-stick-test \
  audio-downmix-test cpu-check-test isa-emulate-test isa-stub-test isa-sse4a-test \
  isa-f16c-test rwlock-rp-test tsc-clock-test

out/gpu/mp3-test tests/data/sine.mp3
out/gpu/videodec-test tests/data/tiny.h264
out/gpu/services-test
out/gpu/pad-map-test
out/gpu/mouse-stick-test
out/gpu/audio-downmix-test
out/gpu/cpu-check-test
out/gpu/isa-emulate-test
out/gpu/isa-stub-test tests/data/isa_fixture.bin
out/gpu/isa-sse4a-test
out/gpu/isa-f16c-test
out/gpu/rwlock-rp-test
out/gpu/tsc-clock-test
```

Ejecuta desde `port/` porque los argumentos son rutas relativas. `source
./msys2-env.sh` deja las DLLs de CLANG64 en `PATH`.

Estos tests no forman parte del CI de Windows y pueden necesitar ajustes si cambia el
entorno. Los targets de GPU que requieren Vulkan, driver, shaders o archivos del juego
no se incluyen en `-Tests`; no hay un `ctest` oficial para esta suite en Windows.

## 8. Empaquetar (opcional)

El build nativo no genera el zip de distribución. Para eso, desde MSYS2 CLANG64:

```bash
cd /c/Users/luisd/proyectos/infamous-port/port
bash packaging/windows/package.sh
```

Requisitos y efectos:

- Deben existir los tres `.exe` de `out/`.
- Usa un Python **de Windows** 3.10+ (`python.org` o `actions/setup-python`); se puede
  forzar con `WINPYTHON`.
- Crea `out/pyenv` e instala PyInstaller, PySide6 e `iced-x86` desde la red. No forma
  parte del build C y no es necesario para compilar.
- Congela los launchers, copia los `.exe` y todas las DLL de `/clang64/bin` que
  aparecen en las tablas de importación, y genera
  `dist/infamous-windows-x86_64.zip`.
- `package.sh` busca las DLL directamente en `/clang64/bin` (no usa `MSYS2_ROOT` para
  esa copia). Si tu instalación no expone `/clang64`, el build puede funcionar pero el
  empaquetado no encontrará esas DLL; usa la instalación estándar `C:\msys64` para
  empaquetar.
- `llvm-strip`/`llvm-readobj` vienen del paquete `toolchain`; si faltan, el empaquetado
  falla aunque el build haya funcionado.

## 9. Fallos comunes

### El mirror de MSYS2 va lento o `pacman` se corta

- Reintenta el mismo `pacman -S --needed ...`: los paquetes ya descargados se
  reutilizan.
- Si `mirror.msys2.org` redirige a un servidor lento, usa `repo.msys2.org` (trampa
  conocida del proyecto). También puedes cambiar el mirror en
  `/etc/pacman.d/mirrorlist.*` por uno cercano y ejecutar `pacman -Syy` antes de
  reintentar.
- Si se actualizó `msys2-runtime`, cierra **todas** las shells MSYS2 y vuelve a abrir
  CLANG64.
- No mezcles paquetes de repositorios UCRT64/MINGW64 con un build CLANG64.

### `clang`, `cmake`, `ninja` o `pkg-config` no se encuentran

- Estás en la shell equivocada. Usa MSYS2 **CLANG64** o el comando absoluto
  `C:\msys64\usr\bin\bash.exe -lc ...`; `build.sh` carga `msys2-env.sh`.
- Comprueba `command -v clang` dentro de la shell. Debe ser `/clang64/bin/clang`.
- Si `MSYS2_ROOT` está definido como `C:\msys64`, `msys2-env.sh` no encontrará
  `/clang64/bin`. Usa `/c/msys64` o deja que `Build.ps1` lo convierta.
- Un `bash` de WSL o Git Bash no es MSYS2. `uname -s` debe empezar por `MINGW`/`MSYS`,
  no `Linux`.

### `pkg-config` no encuentra `freetype2` / `sdl3` / `vulkan`

- Falta el paquete CLANG64 correspondiente. Instala `freetype`, `sdl3`,
  `vulkan-headers`/`vulkan-loader`, `ffmpeg`, etc.
- Comprueba `pkg-config --modversion freetype2 sdl3 vulkan`.
- No mezcles un `PKG_CONFIG_PATH` de Linux/Nix con la shell de Windows.

### CMake usa un compilador o una cache anteriores

- Ejecuta `Build.ps1 -Clean` o borra `port\out\gpu` y vuelve a configurar.
- CMake guarda el compilador en `out/gpu/CMakeCache.txt`; cambiar `PATH` no basta si
  la cache se creó con otro toolchain.
- No reutilices un `out/` copiado de Linux.

### Error al aplicar los patches de `fsr-vulkan`

`build.sh` aplica `gpu/patches/fsr-vulkan/*.patch` si no están ya aplicados. Si el
submódulo tiene cambios locales o está en otro commit, `git apply` falla. Revisa:

```bash
git -C port/gpu/third_party/fsr-vulkan status
git submodule status
```

No edites el submódulo a mano ni reviertas cambios sin comprobar qué los introdujo.

### Faltan DLLs al ejecutar desde `out`

- `SDL3.dll`, `avcodec-*.dll`, `libwinpthread-1.dll`, etc. viven en
  `C:\msys64\clang64\bin`. Añade esa carpeta al `PATH` o usa `run.py`, que lo hace.
- No copies solo el `.exe`: el paquete oficial copia las DLL necesarias al lado de los
  ejecutables.
- `0xc000007b` suele indicar mezcla de 32/64 bits o DLLs de otro entorno MSYS2.

### Symlinks y Git en Windows

El repositorio no tiene archivos versionados como symlinks, así que
`core.symlinks=false` no impide compilar. Los problemas de symlinks aparecen sobre todo
en los mods: el runtime usa symlinks si Windows lo permite y, si no, junctions para
carpetas y hard links para archivos. Activar el **Modo para desarrolladores** de Windows
ayuda si necesitas symlinks reales; no es un requisito del build.

### `out\gpu` o los `.exe` no se pueden borrar (`-Clean`)

- Cierra el juego (`tlg-probe`, `tlg-play`) y cualquier shell que esté dentro de `out`.
- Antivirus/Windows Defender puede bloquear archivos recién compilados; espera o
  excluye el árbol de trabajo.
- `Build.ps1` reintenta con `cmd /c rd /s /q` para directorios; si aun así falla,
  revisa qué proceso tiene el archivo abierto.
- `-Clean` no borra `out\logs` ni `boot*.bin`: si el problema es uno de esos archivos,
  ciérralo o bórralo manualmente.

### Falla el empaquetado pero el build está bien

- `package.sh` necesita un Python de Windows, red para PyInstaller/PySide6 y
  `llvm-strip`/`llvm-readobj` del toolchain.
- Si hay varias instalaciones de Python, define `WINPYTHON` apuntando al `python.exe`
  correcto.
- El empaquetado no es necesario para validar el build C.

### `build.sh` devuelve 0 pero no hay artefactos

No debería ocurrir: `set -euo pipefail` y el `cp` final lo impiden. Si pasa, revisa el
log de `Build.ps1` y `out/gpu-build.log`; `Build.ps1` marcará los tres `.exe` que
falten y devolverá 1.

## 10. Diferencias con Linux

| Aspecto | Linux | Windows |
|---|---|---|
| Entorno | `shell.nix` / GCC 14 y dependencias del sistema | MSYS2 CLANG64 y paquetes `mingw-w64-clang-x86_64-*` |
| Librería de GPU | `libtlggpu.so` compartida, con `rpath` | `libtlggpu.a` estática, enlazada en `tlg-probe.exe` |
| Artefactos | `out/tlg-probe`, `tlg-gpu-capabilities`, `tlg-play` | Los mismos con extensión `.exe` |
| LTO/PGO | LTO ON por defecto; PGO automático si hay perfil | LTO OFF por defecto (el CI usa ON); PGO forzado a OFF |
| Tests | `build.sh --test`/`--test-only`; CI ejecuta C, Python y GPU | `build.sh --test` no hace nada; CI solo Build/Package/selftest; `Build.ps1 -Tests` cubre un subconjunto C |
| Memoria | `memfd`, `mmap(MAP_FIXED)` | sección respaldada por pagefile; `VirtualAlloc2`/`MapViewOfFile3` |
| ABI del guest | FS/GS y `arch_prctl` | GS es el TEB; el loader usa `TlsSlots` |
| Excepciones | SIGSEGV y `sigsetjmp` | Vectored Exception Handler y `tlg_setjmp`/`tlg_longjmp` |
| Pilas del guest | `runtime_low_map` | pilas totalmente comprometidas; principal de 16 MiB |
| Timers | `nanosleep` y condition variables monotónicas | waitable timers de alta resolución; deadlines con `CLOCK_REALTIME` |
| Reinicio | `execlp bash run.sh` | `run.py` y `TLG_RESTART_COMMAND` con `--after PID` |
| Paquete | `infamous-linux-x86_64.tar.gz` | `infamous-windows-x86_64.zip` con DLLs |
| No portado | — | watchdog/`--timeout` con dumps, userfaultfd, MangoHud, PGO y varios tests de `tests/` |

## 11. Resumen de comandos

```powershell
# 1) Dependencias: una vez, en MSYS2 CLANG64 (ver lista exacta arriba)
# 2) Submodulos
Set-Location C:\Users\luisd\proyectos\infamous-port
git submodule update --init --recursive

# 3) Build directo desde PowerShell
& C:\msys64\usr\bin\bash.exe -lc "cd /c/Users/luisd/proyectos/infamous-port/port && bash build.sh"

# 4) Build con log, validacion y tests opcionales
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Build.ps1 -Clean -Tests

# 5) Ejecutar con el juego
Set-Location .\port
$env:TLG_GAME_DIR = 'F:\ruta\a\CUSA00223'
python .\run.py
```

```bash
# Equivalente en MSYS2 CLANG64
cd /c/Users/luisd/proyectos/infamous-port/port
bash build.sh
bash packaging/windows/package.sh   # opcional; requiere Python de Windows y red
```
