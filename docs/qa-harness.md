# QA harness: baseline, métricas y comparación A/B

Este documento describe el harness local para medir el port nativo de
**inFAMOUS Second Son (CUSA00223)** en Windows. Los scripts viven en `tools/`
y no forman parte del runtime ni del juego:

- `tools/run-baseline.ps1`: lanza una sesión controlada y escribe la evidencia.
- `tools/measure-run.ps1`: muestrea GPU y proceso a CSV; también se puede
  usar como librería desde otro script.
- `tools/compare-runs.ps1`: compara dos carpetas de evidencia.

Requieren **PowerShell 7** (`pwsh`) y no usan dependencias externas.
`nvidia-smi` es opcional: si no existe, las columnas de GPU quedan vacías y el
muestreo del proceso continúa.

## 1. Requisitos y rutas

| Elemento | Valor |
|---|---|
| Proyecto | `C:\Users\luisd\proyectos\infamous-port` |
| Ejecutable | `port\out\tlg-probe.exe` (o `port\bin`; `TLG_PROBE` lo sobrescribe) |
| Lanzador | `port\run.py` |
| Vista del juego | `C:\Users\luisd\Documents\Codex\2026-09-29\trabajemos-en-esto-quiero-crear-una\work\games\CUSA00223-view` |
| Datos de ejecución (`out\`, `tlg.ini`) | `C:\Users\luisd\proyectos\infamous-port\port` |
| Datos/saves y caché GPU | `C:\Users\luisd\proyectos\infamous-port\userdata` |
| Evidencia | `C:\Users\luisd\proyectos\infamous-port\evidence` |
| Logs del port (si existen) | `<TLG_DATA_DIR>\out\logs\run-*.log` (`port\out\logs` por defecto) |

`run-baseline.ps1` calcula el repositorio desde su propia ubicación
(`tools\..`). Si ya existen en el entorno `TLG_GAME_DIR`, `TLG_DATA_DIR` o
`ISS_USER_DIR`, se respetan; si no, usa los valores de la tabla. `TLG_DATA_DIR`
apunta a `port` porque `run.py` escribe ahí `out\` y busca `tlg.ini`;
`ISS_USER_DIR` apunta a `userdata`. Las rutas quedan registradas en
`result.json`.

Comprobaciones antes de medir:

```powershell
pwsh
cd C:\Users\luisd\proyectos\infamous-port

Test-Path .\port\out\tlg-probe.exe   # o .\port\bin, o la ruta de TLG_PROBE
Test-Path .\port\run.py
Test-Path 'C:\Users\luisd\Documents\Codex\2026-09-29\trabajemos-en-esto-quiero-crear-una\work\games\CUSA00223-view\eboot.bin'
Get-Process tlg-probe -ErrorAction SilentlyContinue
```

`run-baseline.ps1` se niega a medir si ya hay un `tlg-probe.exe` abierto.
Para medir un build empaquetado o una copia externa, define `TLG_PROBE` en el
entorno o pásalo por `-ExtraEnv`; el preflight y la detección del proceso usan
esa ruta.

## 2. Baseline: `run-baseline.ps1`

### Uso

Desde una consola **PowerShell 7**:

```powershell
cd C:\Users\luisd\proyectos\infamous-port

.\tools\run-baseline.ps1 -DurationSec 900 -Resolution native -Tag baseline-g0
```

Si la política de ejecución bloquea el script:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\run-baseline.ps1 `
  -DurationSec 900 -Resolution native -Tag baseline-g0
```

Para flags adicionales, usa una sesión `pwsh` interactiva (un hashtable no se
convierte bien al pasar por `-File` desde `cmd.exe`):

```powershell
pwsh
cd C:\Users\luisd\proyectos\infamous-port

.\tools\run-baseline.ps1 -DurationSec 600 -Tag ab-gc3500 `
  -ExtraEnv @{ ISS_GC_BUDGET_MB = '3500'; ISS_PIPELINE_BINARIES = '0' }
```

Para un build fuera de `port\out`:

```powershell
.\tools\run-baseline.ps1 -DurationSec 600 -Tag dist-upstream `
  -ExtraEnv @{ TLG_PROBE = 'C:\ruta\al\build\out\tlg-probe.exe' }
```

`-ExtraEnv` se aplica al final y puede sobrescribir cualquier variable, incluidas
las rutas principales. Los valores booleanos se normalizan a `1`/`0`; los
nombres `TLG_*`/`ISS_*` se guardan en mayúsculas.

### Parámetros

| Parámetro | Por defecto | Descripción |
|---|---|---|
| `-DurationSec` | `120` | Segundos de juego medidos desde que aparece `tlg-probe.exe`. |
| `-Resolution` | vacío | `native`, `480p`, `720p`, `1440p`, `1800p` o `4K`. Vacío = usar el `tlg.ini` existente. |
| `-ExtraEnv` | `@{}` | Hashtable de variables adicionales (`ISS_*`, `TLG_*`, etc.). `TLG_PROBE` selecciona el ejecutable. |
| `-Tag` | `baseline` | Etiqueta de la carpeta. Se sanean caracteres inválidos y espacios. |
| `-Python` | `python` | Intérprete que ejecuta `port\run.py`. |
| `-StartupTimeoutSec` | `300` | Máximo de espera hasta que aparezca `tlg-probe.exe` (incluye preparación de parches/enlaces). |

### Qué hace

1. Crea `evidence\<timestamp>-<tag>\` (hora local, `yyyyMMdd-HHmmss`).
2. Resuelve rutas y comprueba `run.py`, `tlg-probe.exe`, `eboot.bin`, Python y
   que no haya otro `tlg-probe.exe`.
3. Configura el entorno del hijo:
   - `TLG_GAME_DIR` (vista), `TLG_DATA_DIR` (`port`) e `ISS_USER_DIR` (`userdata`).
   - `TLG_FRAME_STATS=1` para que el port escriba `Frame stats:` cada 5 s.
   - `PYTHONUNBUFFERED=1`, `PYTHONIOENCODING=utf-8`.
   - `TLG_CONFIG` solo si se pidió `-Resolution`; un `TLG_CONFIG` heredado se
     respeta y se usa como base de la copia de evidencia.
4. Lanza `python port\run.py` con `stdout`/`stderr` redirigidos a la evidencia.
5. Espera a que aparezca el proceso `tlg-probe.exe`; a partir de ahí cuenta
   `-DurationSec` y arranca el sampler de 1 s.
6. Al terminar la duración, intenta cerrar la ventana con `CloseMainWindow()`.
   Si el juego no responde en 12 s, usa `taskkill /T /F` y marca
   `forced_stop: true`.
7. Espera a que `run.py` termine, copia logs nuevos de `<TLG_DATA_DIR>\out\logs`,
   calcula métricas y escribe `result.json`.

La preparación de `run.py` (enlaces, parches, backups) **no** cuenta como
duración de juego; sí cuenta para `-StartupTimeoutSec`. La duración registrada
va desde el arranque de `tlg-probe.exe` hasta que se pide el cierre.

### Resolución sin tocar la configuración del usuario

Con `-Resolution`, el script copia el `tlg.ini` base a
`evidence\...\config\tlg.ini`, cambia `render_resolution` y
`restart_unconfirmed=0`, y pasa esa copia como `TLG_CONFIG`. El `tlg.ini` del
usuario no se modifica. Sin `-Resolution`, se usa el `tlg.ini` existente y solo
se guarda una copia de referencia.

Si el build no tiene el parche de resolución solicitado, el log del port lo
indicará; el harness no valida la calidad visual ni que el parche se haya
aplicado.

### Códigos de salida

| Código | Estado | Significado |
|---|---|---|
| `0` | `completed` | Se alcanzó `-DurationSec` y el juego se cerró. |
| `1` | `preflight_failed`, `launch_failed`, `error` | No se pudo medir; revisar `result.json.errors`. |
| `2` | `early_exit` | El juego terminó antes de la duración pedida. |
| `3` | `startup_timeout` | No apareció `tlg-probe.exe` a tiempo. |
| `130` | `interrupted` | Parada por Ctrl+C (si PowerShell ejecutó el bloque de limpieza). |

`compare-runs.ps1` devuelve `0` cuando puede leer ambas carpetas, incluso si el
veredicto es regresión: es un informe, no un test de CI.

## 3. Sampler: `measure-run.ps1`

### Uso directo

```powershell
pwsh -NoProfile -File .\tools\measure-run.ps1 `
  -ProcessName tlg-probe -OutputCsv .\samples.csv -IntervalSec 1
```

Se detiene cuando el proceso desaparece, al llegar a `-MaxSeconds`, al crearse
el archivo `-StopFile` o con Ctrl+C. Si no se indica `-ProcessId`, usa el
proceso más reciente con el nombre indicado.

### Uso como librería (dot-source)

```powershell
. .\tools\measure-run.ps1

$ctx = Start-Sampler -TargetProcessId 1234 `
  -CsvPath .\evidence\20261019-120000-baseline\samples.csv `
  -SampleIntervalSec 1 -LimitSeconds 960 `
  -ChildLogPath .\evidence\20261019-120000-baseline\sampler.log

# ... el juego corre ...

Stop-Sampler -Context $ctx -GraceSec 5
```

`Start-Sampler` lanza un `pwsh` hijo y espera a que exista el CSV.
`Stop-Sampler` crea un archivo de parada para que el sampler cierre el CSV de
forma ordenada y, si no termina en 5 s, lo fuerza. `run-baseline.ps1` ya hace
esto automáticamente.

### CSV

Cabecera exacta:

```text
timestamp_utc,elapsed_sec,pid,process_name,cpu_percent,working_set_mb,private_mb,gpu_util_percent,vram_used_mb,vram_total_mb,gpu_temp_c,gpu_power_w,gpu_sm_clock_mhz
```

| Columna | Fuente / unidad |
|---|---|
| `timestamp_utc` | ISO 8601 UTC. |
| `elapsed_sec` | Segundos desde el inicio del sampler. |
| `pid`, `process_name` | Proceso medido. |
| `cpu_percent` | CPU del proceso, normalizada por núcleos lógicos. `100%` = todos los núcleos. |
| `working_set_mb` | `WorkingSet64` en MiB. |
| `private_mb` | `PrivateMemorySize64` en MiB. |
| `gpu_util_percent` | `nvidia-smi utilization.gpu` de la GPU 0. |
| `vram_used_mb` | `nvidia-smi memory.used` de la GPU 0 (todo el dispositivo, no solo el juego). |
| `vram_total_mb` | `nvidia-smi memory.total`. |
| `gpu_temp_c` | `temperature.gpu`. |
| `gpu_power_w` | `power.draw` (puede no estar soportado en algunas laptops). |
| `gpu_sm_clock_mhz` | `clocks.sm`. |

Los números usan punto decimal y UTF-8 sin BOM. La primera fila de CPU es la
media desde el arranque del proceso; las siguientes son deltas del intervalo.

## 4. Comparación: `compare-runs.ps1`

```powershell
.\tools\compare-runs.ps1 `
  -Reference .\evidence\20261019-120000-baseline `
  -Candidate .\evidence\20261019-130000-gc3500
```

También acepta las carpetas en posición:

```powershell
.\tools\compare-runs.ps1 .\evidence\run-a .\evidence\run-b
```

Tabla Markdown para pegar en un informe:

```powershell
.\tools\compare-runs.ps1 .\evidence\run-a .\evidence\run-b -Markdown > comparison.md
```

La tabla incluye estado, FPS mediana/p95, frametime p99, VRAM máxima, RAM máxima
(working set), RAM privada máxima, duración, número de muestras CSV, flags y
veredicto. Para las flags usa `log_config.env` (entorno efectivo del juego) y
completa con `flags` (variables configuradas); si no hay `result.json`, intenta
leer `run.log`/`stdout.log` y `samples.csv` directamente, y la etiqueta pasa a
ser el nombre de la carpeta.

### Veredicto

Con `-ImprovementPct` (2% por defecto), sobre la **FPS mediana**:

- `MEJORA (+x%)` si sube al menos el umbral.
- `REGRESIÓN (-x%)` si baja al menos el umbral.
- `SIN CAMBIO (±x%)` dentro del umbral.
- `NO COMPARABLE (...)` si alguna sesión no está en `completed`.
- `SIN DATOS DE FPS` si no se pudo parsear ninguna ventana.

Se añaden avisos al veredicto si el p95 de FPS empeora ≥5% o si VRAM/RAM suben
≥10% sin una mejora de FPS. Si las flags no son idénticas, el script lo marca:
la comparación no aísla una sola variable.

## 5. Formato de la evidencia

```text
evidence/
  20261019-120000-baseline/
    result.json
    run.log
    stdout.log
    stderr.log
    samples.csv
    sampler.log
    sampler.log.err
    config/
      tlg.ini
    logs/                     # solo si el port creó run-*.log durante la sesión
      run-20261019-120500.log
```

- `run.log`: salida combinada para leer y parsear. Se escribe como `stdout.log`
  y después un bloque `===== stderr =====`; el orden exacto entre ambos flujos
  no se conserva.
- `stdout.log` / `stderr.log`: flujos originales.
- `samples.csv`: muestras de 1 s.
- `sampler.log` / `sampler.log.err`: diagnóstico del proceso sampler; suelen
  estar vacíos.
- `config/tlg.ini`: copia de la configuración usada o copia con el override de
  resolución.
- `logs/`: copia de los `<TLG_DATA_DIR>\out\logs\run-*.log` creados durante la
  sesión (`port\out\logs` con la ruta por defecto). Al lanzar directamente con
  `run.py` no se crea ese log de launcher; la salida queda en `run.log`. Si en
  el futuro aparece, se recopila igualmente.

### `result.json` (schema 1)

Campos principales:

| Campo | Contenido |
|---|---|
| `schema` | `1`. |
| `status` | `completed`, `early_exit`, `startup_timeout`, `preflight_failed`, `launch_failed`, `interrupted`, `error`, `initializing`. |
| `tag` | Etiqueta pedida. |
| `requested_duration_sec` | Duración solicitada. |
| `duration_sec` | Duración real desde `tlg-probe.exe` hasta el cierre. |
| `launch_duration_sec` | Tiempo desde lanzar `run.py` hasta que apareció el juego. |
| `game_started_at`, `game_ended_at` | ISO UTC. |
| `resolution` | `requested`, `effective`, `source`. |
| `paths` | Repo, port, `run.py`, probe, juego, datos, usuario, config y Python. |
| `exit_codes` | `launcher` y `game` (puede faltar el del juego si se forzó el cierre). |
| `forced_stop`, `stop_reason` | Cierre forzado y motivo (`duration`, `game_exit`, `startup_timeout`, `interrupted`). |
| `extra_env` | Variables pasadas en `-ExtraEnv`, ya normalizadas. |
| `flags` | Variables `TLG_*`/`ISS_*` configuradas por el harness/entorno antes del lanzamiento, sin claves de rutas/comandos. |
| `log_config.ini` | Claves del bloque `Config ini:` del log (lo que leyó el port del `tlg.ini`). |
| `log_config.env` | Variables del bloque `Config env:` del log (entorno efectivo que vio el juego; el log omite rutas y valores largos). |
| `system` | SO, PowerShell, CPU, núcleos, RAM, GPU, driver y VRAM total. |
| `metrics` | FPS, frametime, memoria, GPU, CPU y muestras. |
| `build` | Línea `Build:` del log, si existe. |
| `log_issues` | Primeras líneas `Fault:`, `STOP:` o `FREEZE:`. |
| `log_file`, `samples_file`, `sampler_log` | Nombres relativos dentro de la evidencia. |
| `collected_logs` | Logs de `<TLG_DATA_DIR>\out\logs` copiados (dentro de `logs/`). |
| `errors` | Errores de preflight, sampler o lanzamiento. |
| `notes` | Notas (Python, cierres forzados, FPS leídos de otro log...). |

Ejemplo abreviado:

```json
{
  "schema": 1,
  "status": "completed",
  "tag": "baseline-g0",
  "requested_duration_sec": 900,
  "duration_sec": 901.2,
  "resolution": { "requested": "native", "effective": "native", "source": "..." },
  "flags": { "TLG_FRAME_STATS": "1" },
  "metrics": {
    "fps": { "median": 31.4, "p95": 38.2, "samples": 180, "source": "Frame stats (ventanas de 5 s)" },
    "frame_pacing": { "median_ms": 31.8, "p99_ms": 48.2, "note": "..." },
    "memory": { "vram_max_mb": 4020, "vram_total_mb": 4096, "ram_max_mb": 7800, "private_max_mb": 6500 },
    "gpu": { "utilization_median_pct": 97.0, "temperature_max_c": 74.0, "power_max_w": 62.0, "sm_clock_median_mhz": 1740, "samples": 900 },
    "cpu": { "median_pct": 22.0, "max_pct": 48.0 },
    "samples": { "count": 900, "span_sec": 899.0, "interval_sec": 1 }
  }
}
```

## 6. Cómo se calculan las métricas

### FPS mediana y p95

El port escribe cada 5 s, con `TLG_FRAME_STATS=1`:

```text
Frame stats: 42.3 FPS, worst frame 80.0 ms (vblank 60 Hz); ...
```

El harness recoge todos los valores `> 0` (ventanas de 5 s) y calcula:

- **mediana**: mediana de la serie de FPS por ventana.
- **p95**: percentil 95 por rango más cercano de esa misma serie. Es un valor
  alto: el 95% de las ventanas quedó por debajo. No es un p95 por frame.

No se usa el `p99` de `Frame pacing` como FPS global: ese valor es por ventana.
La tabla de comparación muestra la mediana de los `p99` de las ventanas como
referencia de frametime, no como un p99 global.

### Memoria

- **VRAM máxima**: máximo de `memory.used` de `nvidia-smi`. Es memoria de todo
  el dispositivo, no solo del proceso.
- **RAM máxima**: máximo de `WorkingSet64` de `tlg-probe.exe`.
- **RAM privada máxima**: máximo de `PrivateMemorySize64`.

### CPU

Porcentaje normalizado por núcleos lógicos del sistema. Un valor de 100% usa
todos los núcleos; no es “un núcleo al 100%”.

## 7. Limitaciones

- **Solo NVIDIA para GPU.** `nvidia-smi` puede no existir, no soportar
  `power.draw` o cambiar el formato entre drivers. Las columnas GPU quedan
  vacías sin abortar la sesión.
- **Solo GPU 0.** En equipos multi-GPU se mide la primera. En laptops con GPU
  híbrida, el juego podría estar usando otra GPU.
- **Muestreo de 1 s.** Se pierden picos de menos de un segundo. La VRAM máxima
  es una aproximación, no un pico instrumentado dentro del juego.
- **FPS por ventanas de 5 s.** No hay FPS por frame en el log estándar. Para un
  p95 con sentido hacen falta muchas ventanas; 2 minutos dan solo 24. Para el
  Gate G0 usa 15–30 minutos.
- **Calentamiento de shaders.** La primera sesión tras borrar cachés es más
  lenta. No mezcles una sesión fría con una caliente. Si se investiga el primer
  arranque, regístralo como escena aparte.
- **Sin automatización de input.** El harness no mueve al personaje ni envía
  teclas. La ruta debe jugarse a mano y ser la misma en cada repetición.
- **Sin captura visual.** No se guardan screenshots ni se valida la calidad de
  imagen. Para artefactos visuales usa una captura externa y guárdala junto a la
  evidencia.
- **Cierre forzado.** Si `CloseMainWindow()` no funciona, `taskkill /T /F` puede
  dejar el final de `run.log` incompleto (`forced_stop: true`).
- **Reinicio desde el menú del juego.** El harness sigue un solo
  `tlg-probe.exe`; “Apply and restart” lanza otro proceso que no se muestrea.
  No lo uses durante una captura.
- **`TLG_TIMEOUT` no es un watchdog en Windows.** `probe.c` no lo implementa en
  esta plataforma; la duración la controla el harness. Si se pasa por
  `-ExtraEnv`, se registra, pero no sustituye a `-DurationSec`.
- **Contexto de la máquina.** FPS depende de VSync, modo de ventana, resolución,
  plan de energía, temperatura, carga de fondo y foco de la ventana. Mantén esas
  condiciones iguales entre A y B.
- **Privacidad.** `run.log`, `result.json` y `system` contienen rutas y nombres
  locales. Redáctalos antes de publicarlos.
- **`tools/` está ignorado por Git en este clon.** Si quieres versionar el
  harness, muévelo a `scripts/devtools/` o ajusta el `.gitignore` local.

## 8. Cómo añadir una escena nueva

Una “escena” es una condición reproducible, no una función del harness. No hace
falta tocar los scripts:

1. **Crea un save fijo** en el punto de inicio (menú, intro, tutorial, misión,
   zona abierta). Anota el nombre del save y el punto exacto.
2. **Fija la ruta de input**: qué se hace desde que aparece el juego hasta que
   termina la medición. Si hay diálogos o cinemáticas, decide si se saltan o se
   esperan y hazlo igual siempre.
3. **Fija la configuración**: resolución, VSync, modo de ventana, flags y
   duración. Una sola variable por A/B.
4. **Haz una pasada de calentamiento** para compilar shaders/pipelines. No la
   uses como medida.
5. **Haz 3 repeticiones medidas** con tags ordenados:

   ```powershell
   .\tools\run-baseline.ps1 -DurationSec 600 -Resolution native -Tag escena-tutorial-r1
   .\tools\run-baseline.ps1 -DurationSec 600 -Resolution native -Tag escena-tutorial-r2
   .\tools\run-baseline.ps1 -DurationSec 600 -Resolution native -Tag escena-tutorial-r3
   ```

6. **Compara** las repeticiones o el A/B:

   ```powershell
   .\tools\compare-runs.ps1 .\evidence\...-r1 .\evidence\...-r2
   .\tools\compare-runs.ps1 .\evidence\...-baseline .\evidence\...-cambio
   ```

Recomendaciones:

- Para menús o intro, 60–120 s suele bastar; para gameplay real, 15–30 min.
- Si la escena incluye carga de nivel, decide si la carga entra en la medida y
  documenta la decisión.
- Si cambias de save o de ruta, es otra escena: no compares contra la anterior.
- Para aislar una variable usa `-ExtraEnv` con una sola clave distinta y revisa
  el aviso de “flags distintas”.
- Si necesitas saves separados, usa un `ISS_USER_DIR` propio para esa escena
  (por ejemplo, una copia de `userdata`), pásalo por entorno o por `-ExtraEnv`
  y anota la ruta en el informe.

## 9. Primera prueba real

Cuando exista el ejecutable (`port\out\tlg-probe.exe`, `port\bin\tlg-probe.exe`
o la ruta de `TLG_PROBE`) y la vista del juego esté creada:

```powershell
pwsh
cd C:\Users\luisd\proyectos\infamous-port

# Humo corto: valida lanzamiento, cierre, CSV y log.
.\tools\run-baseline.ps1 -DurationSec 60 -Resolution native -Tag smoke-g0

# Baseline del Gate G0.
.\tools\run-baseline.ps1 -DurationSec 1800 -Resolution native -Tag baseline-g0

# Variante con un build empaquetado/externo.
.\tools\run-baseline.ps1 -DurationSec 60 -Tag smoke-dist `
  -ExtraEnv @{ TLG_PROBE = 'C:\ruta\al\build\out\tlg-probe.exe' }
```

Antes de lanzar:

- Cierra otros `tlg-probe.exe`.
- Deja la ventana del juego en primer plano y con el mismo modo de ventana.
- No ejecutes el harness desde Windows PowerShell 5.1; usa `pwsh`.
- Comprueba que `nvidia-smi` responde si quieres métricas de GPU.
- Revisa `result.json.errors` y `notes` aunque el estado sea `completed`.
