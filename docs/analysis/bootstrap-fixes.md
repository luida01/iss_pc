# Bootstrap en Windows — hallazgos y fixes

Fecha: 2026-10-19. Entorno: Windows 11, Python 3.14, MSYS2 CLANG64, dump CUSA00223 1.00 + 1.07.

## 1. `link_libc.py`: símbolos locales sin formato `n#lib#mod`

### Síntoma

```
ValueError: unsupported symbol encoding '__start__Ztext'
```

`port/run.py` abortaba antes de compilar o ejecutar el juego.

### Causa

`link_libc.py` parsea la tabla de símbolos de `sce_module/libc.prx` y llamaba a `identity(name)`
para **todos** los símbolos con nombre. `identity()` exige el formato `n#lib#mod` que usa el
enlazador de Sony para imports/exports. `libc.prx` contiene también símbolos locales y de
sección (`__start__Ztext`, etc.) que no siguen ese formato.

### Fix aplicado

- `identity(name, strict=True)` acepta `strict=False` y devuelve `None` cuando el nombre no
  tiene tres partes.
- La tabla de símbolos usa `identity(name, strict=False)`.
- `imported()` falla con un error claro si una relocación externa necesita un símbolo sin
  identidad, en vez de un `TypeError` posterior.

Archivo: `port/scripts/link_libc.py`.

### Evidencia

Antes:

```
ValueError: unsupported symbol encoding '__start__Ztext'
link_libc exit=1
```

Después:

```
Linked native libc: base=0x7fb0000, 101 fallback exports, TLS=1168 bytes;
eboot TLS=48 bytes, fs->gs patched=2927
link_libc exit=0
```

`link_modules.py` también pasa:

```
Linked modules: libc.prx@0x7fb0000, libSceFios2.prx@0x80b0000;
162 native bindings, 402 imports left to the host runtime, fs->gs patched=3616
```

### Propuesta upstream

PR pequeño y aislado: tolerar símbolos locales en `link_libc.py`, con un test que use una
tabla con un símbolo sin `#`. No cambia el comportamiento de imports válidos.

## 2. Dependencia Python `iced-x86`

`port/scripts/isa_rewrite.py` necesita `iced_x86`. Sin ella:

```
ISA: rewrite table unavailable (iced_x86 is not installed)
isa_rewrite exit=1
```

Instalación:

```powershell
python -m pip install iced-x86
```

Resultado con el dump del usuario:

```
ISA: 1915 sites (MOVBE 27, BMI1 1615, LZCNT 164, F16C 13, SSE4A 96);
148 relocated, 3 int3, 0 unsupported
Red zone: 6453 instructions protected in 5898 patches ... 0 unrelocatable
```

Esto es importante en Windows: la protección de red-zone está activa por defecto y evita el
crash `0x1cfaaf` (#29/#41). El i5-13420H no tiene SSE4A (AMD), así que la tabla también cubre
las 96 instrucciones SSE4A del juego.

## 3. Vista del juego con hardlinks

`scripts/make_game_view.py` usa `os.symlink`, que en Windows requiere Developer Mode o admin.
En esta máquina los symlinks están bloqueados. Se añadió `tools/New-GameView.ps1`, que crea
la misma vista con **hardlinks** (mismo volumen NTFS) y junctions no son necesarias porque
recorre los directorios.

Resultado:

```
1854 archivos base enlazados
481 archivos del update aplicados
eboot.bin SHA256 2D1CA79630D7BBE6FA29575F59AA7996D43071D74AC8E4F7D40137967D9039AB (1.07)
param.sfo 01.07
espacio adicional: 8.46 MB (solo entradas de directorio)
```

## 4. Orden de preparación validado

```
prepare.py        → eboot.elf, boot.bin, analysis.json
link_libc.py      → boot-libc.bin, libc-link.json
link_modules.py   → boot-linked.bin, link.json
isa_rewrite.py    → isa-rewrite.bin
content_profile.py→ content.bin, content-profile.json
patches.py        → patches.bin (vacío si no hay perfil CUSA00223)
```

`patches.py` imprime `no patch profile for CUSA00223: none applied`; es esperado. Los mods de
inFAMOUS se gestionan con `infamous_patches.py` desde el launcher, no con los perfiles GOW3.

## 5. Pendientes inmediatos

- Terminar la instalación del toolchain CLANG64 (`cmake`, `ninja`, `pkg-config`).
- Compilar `tlg-probe.exe` y verificar que enlaza Vulkan/SDL3/FFmpeg.
- Primera ejecución en la RTX 3050 4 GB.
- Medir VRAM y FPS; decidir Gate G0.
