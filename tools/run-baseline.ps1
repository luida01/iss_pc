#Requires -Version 7.0
<#
.SYNOPSIS
    Lanza una sesion controlada del port, muestrea GPU/proceso y guarda evidencia.

.DESCRIPTION
    Crea evidence/<timestamp>-<tag>/ con:
      result.json, run.log, stdout.log, stderr.log, samples.csv,
      config/tlg.ini (copia), logs/ (logs run-*.log nuevos del port, si los hay).

    Flujo:
      1. Preflight de run.py, tlg-probe.exe, eboot.bin, Python y procesos abiertos.
      2. Configura TLG_GAME_DIR, TLG_DATA_DIR, ISS_USER_DIR y TLG_FRAME_STATS=1.
      3. Lanza `python port\run.py` con la salida redirigida a la evidencia.
      4. Espera a que aparezca tlg-probe.exe; a partir de ahi cuenta DurationSec.
      5. Cierra la ventana del juego (CloseMainWindow); si no responde, taskkill /T /F.
      6. Recopila logs, muestras y escribe result.json.

    La resolucion se aplica con una copia local de tlg.ini en la evidencia
    (TLG_CONFIG); no modifica el tlg.ini del usuario. Sin -Resolution se usa la
    configuracion existente y solo se guarda una copia como referencia.

.PARAMETER DurationSec
    Segundos de juego medidos desde que aparece tlg-probe.exe (por defecto 120).

.PARAMETER Resolution
    native, 480p, 720p, 1440p, 1800p o 4K. Vacio = usar tlg.ini existente.

.PARAMETER ExtraEnv
    Hashtable con variables adicionales. Se aplican al final (ganan sobre los
    valores por defecto). TLG_PROBE permite medir un build fuera de port\out.
    Ejemplo: @{ ISS_GC_BUDGET_MB = '3500' }.

.PARAMETER Tag
    Etiqueta para la carpeta de evidencia (por defecto baseline).

.PARAMETER Python
    Interprete Python (por defecto python). Debe tener run.py como argumento.

.PARAMETER StartupTimeoutSec
    Maximo de espera hasta que aparezca tlg-probe.exe (por defecto 300).

.PARAMETER AutoPlayScenario
    Ruta a un JSON de escenario o nombre integrado (resuelto en tools\autoplay,
    p. ej. intro-g0). Cada paso lleva elapsed_sec, key, hold_ms y delay_ms; las
    teclas se envian con tools\GameWindow.ps1. Vacio = sin automatizacion.

.PARAMETER ScreenshotIntervalSec
    Cada cuantos segundos se guarda una captura de la ventana del juego como
    evidence\<run>\automation-<n>s.png. 0 = desactivado.

.PARAMETER BringToFront
    Enfoca la ventana del juego al arrancar y cada 30 s durante la sesion.

.PARAMETER CodexFeedback
    Cada N capturas se lanza tools\Invoke-CodexFeedback.ps1 en segundo plano
    para que el agente local de Codex analice la ultima captura. 0 = desactivado.

.EXAMPLE
    .\tools\run-baseline.ps1 -DurationSec 900 -Resolution native -Tag baseline-g0

.EXAMPLE
    .\tools\run-baseline.ps1 -DurationSec 960 -Resolution native -Tag g0-intro `
        -AutoPlayScenario intro-g0 -ScreenshotIntervalSec 30 -BringToFront -CodexFeedback 2

.EXAMPLE
    .\tools\run-baseline.ps1 -DurationSec 600 -Tag ab-gc3500 `
        -ExtraEnv @{ ISS_GC_BUDGET_MB = '3500'; ISS_PIPELINE_BINARIES = '0' }

.NOTES
    Requiere PowerShell 7 (pwsh). nvidia-smi es opcional. Sin -AutoPlayScenario,
    -ScreenshotIntervalSec, -BringToFront ni -CodexFeedback la sesion se juega a
    mano y el comportamiento es identico al harness original.
#>
[CmdletBinding()]
param(
    [ValidateRange(1, 86400)][int]$DurationSec = 120,

    [ValidateSet('', 'native', '480p', '720p', '1440p', '1800p', '4K')]
    [string]$Resolution = '',

    [hashtable]$ExtraEnv = @{},

    # Claves extra de tlg.ini para esta pasada (preset, live_resolution, rcas...). Se escriben
    # en la copia de configuracion de la evidencia, nunca en la del usuario.
    [hashtable]$IniExtra = @{},

    [ValidateNotNullOrEmpty()][string]$Tag = 'baseline',

    [string]$Python = 'python',

    [ValidateRange(30, 3600)][int]$StartupTimeoutSec = 300,

    [string]$AutoPlayScenario = '',

    [ValidateRange(0, 3600)][int]$ScreenshotIntervalSec = 0,

    [switch]$BringToFront,

    # Con -KeepOpen el harness NO cierra la ventana del juego al acabarse la duracion: se
    # queda hasta que el jugador la cierre (o hasta KeepOpenMaxSec). Sin esto, el cierre
    # automatico al final de la sesion se confundia con un crash del port.
    [switch]$KeepOpen,

    [ValidateRange(60, 28800)][int]$KeepOpenMaxSec = 14400,

    [ValidateRange(0, 1000)][int]$CodexFeedback = 0
)

$ErrorActionPreference = 'Stop'

# ValidateSet no distingue mayusculas: normaliza "4k" a "4K" antes de escribir tlg.ini.
if ($Resolution) {
    $resolutionCanonical = @{
        'native' = 'native'
        '480p'   = '480p'
        '720p'   = '720p'
        '1440p'  = '1440p'
        '1800p'  = '1800p'
        '4k'     = '4K'
    }
    $Resolution = $resolutionCanonical[$Resolution.ToLowerInvariant()]
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$portDir = Join-Path $repoRoot 'port'
$evidenceRoot = Join-Path $repoRoot 'evidence'
$measureScript = Join-Path $PSScriptRoot 'measure-run.ps1'
$gameWindowScript = Join-Path $PSScriptRoot 'GameWindow.ps1'
$autoplayDir = Join-Path $PSScriptRoot 'autoplay'
$codexFeedbackScript = Join-Path $PSScriptRoot 'Invoke-CodexFeedback.ps1'

if (-not (Test-Path -LiteralPath $measureScript -PathType Leaf)) {
    throw "No existe el sampler: $measureScript"
}
. $measureScript

# GameWindow.ps1 solo se carga si se pidio automatizacion: sin flags el harness
# se comporta igual que antes (input manual, sin capturas).
$script:AutomationRequested = [bool]($AutoPlayScenario -or $ScreenshotIntervalSec -gt 0 -or
    $BringToFront -or $CodexFeedback -gt 0)
$script:GameWindowReady = $false
if ($script:AutomationRequested) {
    if (-not (Test-Path -LiteralPath $gameWindowScript -PathType Leaf)) {
        Write-Warning "No existe $gameWindowScript; la automatizacion queda desactivada."
    } else {
        try {
            . $gameWindowScript
            $script:GameWindowReady = $true
        } catch {
            Write-Warning "GameWindow.ps1 no se pudo cargar: $($_.Exception.Message)"
        }
    }
}

# Rutas del proyecto (AGENTS.md). run.py usa TLG_DATA_DIR como raiz de out/ y de
# tlg.ini (por defecto, port/); las partidas y la cache GPU viven en userdata.
# Se pueden sobrescribir con TLG_GAME_DIR, TLG_DATA_DIR e ISS_USER_DIR.
$defaultGameDir = 'C:\Users\luisd\Documents\Codex\2026-09-29\trabajemos-en-esto-quiero-crear-una\work\games\CUSA00223-view'
$defaultDataDir = $portDir
$defaultUserDir = Join-Path $repoRoot 'userdata'

# run.py hace chdir(port) antes de interpretar TLG_*; una ruta relativa se
# resuelve contra port/, no contra el directorio desde el que se lanza pwsh.
function Resolve-HarnessPath {
    param([string]$Value, [string]$Base = $portDir)
    if ([IO.Path]::IsPathRooted($Value)) {
        return [IO.Path]::GetFullPath($Value)
    }
    return [IO.Path]::GetFullPath((Join-Path $Base $Value))
}

function Get-EffectivePath {
    param([string]$Name, [string]$Default)
    $value = [Environment]::GetEnvironmentVariable($Name, 'Process')
    if ($value) { return Resolve-HarnessPath $value }
    return Resolve-HarnessPath $Default
}

function ConvertTo-SafeTag {
    param([string]$Value)
    $safe = $Value.Trim()
    foreach ($char in [IO.Path]::GetInvalidFileNameChars()) {
        $safe = $safe.Replace([string]$char, '-')
    }
    $safe = ($safe -replace '\s+', '-').Trim('-', '.')
    if (-not $safe) { $safe = 'run' }
    return $safe
}

function New-EvidenceDirectory {
    param([string]$Root, [string]$SafeTag)
    if (-not (Test-Path -LiteralPath $Root)) {
        New-Item -ItemType Directory -Force -Path $Root | Out-Null
    }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $directory = Join-Path $Root "$stamp-$SafeTag"
    $suffix = 2
    while (Test-Path -LiteralPath $directory) {
        $directory = Join-Path $Root "$stamp-$SafeTag-$suffix"
        $suffix++
    }
    New-Item -ItemType Directory -Path $directory | Out-Null
    return $directory
}

function Write-JsonFile {
    param([string]$Path, $Object)
    $json = $Object | ConvertTo-Json -Depth 12
    [IO.File]::WriteAllText($Path, $json + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}

function Read-IniValue {
    param([string]$Path, [string]$Key)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    foreach ($line in [IO.File]::ReadLines($Path)) {
        if ($line.TrimStart().StartsWith('#')) { continue }
        $index = $line.IndexOf('=')
        if ($index -lt 1) { continue }
        if ($line.Substring(0, $index).Trim() -eq $Key) {
            return $line.Substring($index + 1).Trim()
        }
    }
    return $null
}

function Update-IniValue {
    param([string]$Path, [string]$Key, [string]$Value)
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $lines = @([IO.File]::ReadAllLines($Path))
    } else {
        $lines = @('# tlg settings (in-game menu: Insert / R3+L2)')
    }
    $seen = $false
    $output = [Collections.Generic.List[string]]::new()
    foreach ($line in $lines) {
        $isComment = $line.TrimStart().StartsWith('#')
        $index = $line.IndexOf('=')
        if (-not $isComment -and $index -ge 1 -and
            $line.Substring(0, $index).Trim() -eq $Key) {
            $output.Add("$Key=$Value")
            $seen = $true
        } else {
            $output.Add($line)
        }
    }
    if (-not $seen) { $output.Add("$Key=$Value") }
    [IO.File]::WriteAllLines($Path, $output, [Text.UTF8Encoding]::new($false))
}

function ConvertTo-EnvValue {
    param($Value)
    if ($null -eq $Value) { return '' }
    if ($Value -is [bool]) { return $(if ($Value) { '1' } else { '0' }) }
    if ($Value -is [IFormattable]) {
        return $Value.ToString($null, [Globalization.CultureInfo]::InvariantCulture)
    }
    return [string]$Value
}

# Los nombres de variable de Windows no distinguen mayusculas; ExtraEnv se
# canoniza a mayusculas para que TLG_CONFIG/TLG_DATA_DIR se detecten siempre.
function Get-MapValue {
    param([System.Collections.IDictionary]$Map, [string]$Name)
    if ($null -eq $Map) { return $null }
    foreach ($key in @($Map.Keys)) {
        if ([string]::Equals([string]$key, $Name,
                [StringComparison]::OrdinalIgnoreCase)) {
            return $Map[$key]
        }
    }
    return $null
}

function Resolve-PythonPath {
    param([string]$Command)
    $found = Get-Command $Command -ErrorAction SilentlyContinue
    if (-not $found) { throw "Python no encontrado: $Command" }
    $path = $found.Source
    if (-not $path) { $path = $found.Definition }
    if (-not $path -or -not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Python no es un ejecutable valido: $Command"
    }
    return $path
}

function Get-SystemInfo {
    $info = [ordered]@{
        os                  = [Runtime.InteropServices.RuntimeInformation]::OSDescription
        powershell          = $PSVersionTable.PSVersion.ToString()
        cpu                 = $env:PROCESSOR_IDENTIFIER
        logical_processors  = [Environment]::ProcessorCount
        ram_total_gb        = $null
        gpu_name            = $null
        gpu_driver          = $null
        vram_total_mb       = $null
        nvidia_smi          = $false
    }

    try {
        $computer = Get-CimInstance Win32_ComputerSystem -ErrorAction Stop
        if ($computer.TotalPhysicalMemory) {
            $info.ram_total_gb = [Math]::Round($computer.TotalPhysicalMemory / 1GB, 1)
        }
    } catch { }
    try {
        $processor = Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1
        if ($processor.Name) { $info.cpu = $processor.Name.Trim() }
    } catch { }

    $smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
    if ($smi -and $smi.Source) {
        $info.nvidia_smi = $true
        try {
            $line = @(& $smi.Source '--id=0' '--query-gpu=name,driver_version,memory.total' '--format=csv,noheader,nounits' 2>$null |
                Where-Object { "$_".Trim() } | Select-Object -First 1)
            if ($line) {
                $parts = @($line[0] -split ',')
                if ($parts.Count -ge 3) {
                    $info.gpu_name = $parts[0].Trim()
                    $info.gpu_driver = $parts[1].Trim()
                    $parsed = 0.0
                    if ([double]::TryParse($parts[2].Trim(),
                            [Globalization.NumberStyles]::Float,
                            [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) {
                        $info.vram_total_mb = $parsed
                    }
                }
            }
        } catch { }
    }
    return [pscustomobject]$info
}

function ConvertTo-QuotedArgument {
    param([string]$Value)
    if ($Value -match '[\s"]') {
        return '"' + ($Value -replace '"', '\"') + '"'
    }
    return $Value
}

function Set-ProcessEnvironment {
    param([System.Collections.IDictionary]$Map)
    $saved = @{}
    foreach ($key in @($Map.Keys)) {
        $saved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
        [Environment]::SetEnvironmentVariable($key, [string]$Map[$key], 'Process')
    }
    return $saved
}

function Restore-ProcessEnvironment {
    param([System.Collections.IDictionary]$Saved)
    foreach ($key in @($Saved.Keys)) {
        [Environment]::SetEnvironmentVariable($key, $Saved[$key], 'Process')
    }
}

function Wait-ProcessExit {
    param($Process, [int]$Seconds)
    if ($null -eq $Process) { return $true }
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $Process.Refresh()
            if ($Process.HasExited) { return $true }
        } catch {
            return $true
        }
        Start-Sleep -Milliseconds 200
    }
    try { return $Process.HasExited } catch { return $true }
}

function Stop-ProcessTree {
    param([int]$Id)
    if ($Id -le 0) { return }
    try {
        & taskkill.exe /PID $Id /T /F 2>$null | Out-Null
    } catch {
        try { Stop-Process -Id $Id -Force -ErrorAction SilentlyContinue } catch { }
    }
}

function Stop-GameProcess {
    param([Diagnostics.Process]$Game, [int]$GraceSec = 12)
    $outcome = [ordered]@{ forced = $false; method = 'already_exited' }
    if ($null -eq $Game) { return [pscustomobject]$outcome }

    try {
        if ($Game.HasExited) { return [pscustomobject]$outcome }
    } catch {
        return [pscustomobject]$outcome
    }

    $closed = $false
    try { $closed = $Game.CloseMainWindow() } catch { }
    if ($closed) {
        $outcome.method = 'close_main_window'
        Wait-ProcessExit $Game $GraceSec | Out-Null
    }

    try {
        if (-not $Game.HasExited) {
            $outcome.method = 'taskkill'
            $outcome.forced = $true
            Stop-ProcessTree $Game.Id
            Wait-ProcessExit $Game 8 | Out-Null
            if (-not $Game.HasExited) {
                try { Stop-Process -Id $Game.Id -Force -ErrorAction Stop } catch { }
            }
        }
    } catch { }
    return [pscustomobject]$outcome
}

function Find-GameProcess {
    param(
        [string]$ProbePath,
        [Diagnostics.Process]$Launcher,
        [int]$TimeoutSec,
        [datetime]$NotBeforeUtc
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            if ($Launcher.HasExited) { return $null }
        } catch { return $null }

        try {
            # The probe's own file name (TLG_PROBE can point at a build with another name).
            $probeName = [IO.Path]::GetFileNameWithoutExtension($ProbePath)
            $candidates = @(Get-Process -Name $probeName -ErrorAction SilentlyContinue)
            foreach ($candidate in $candidates) {
                try {
                    if ($candidate.StartTime.ToUniversalTime() -lt $NotBeforeUtc.AddSeconds(-2)) {
                        continue
                    }
                    $path = $null
                    try { $path = $candidate.Path } catch { }
                    if ($path -and -not [string]::Equals(
                            $path, $ProbePath, [StringComparison]::OrdinalIgnoreCase)) {
                        continue
                    }
                    $candidate.Refresh()
                    if (-not $candidate.HasExited) { return $candidate }
                } catch { }
            }
        } catch { }
        Start-Sleep -Milliseconds 400
    }
    return $null
}

function Read-SharedText {
    # El probe hereda el stdout/stderr del launcher: en Windows el archivo sigue
    # abierto mientras el proceso vive. FileShare.ReadWrite permite leer sin
    # bloquear al escritor; se reintenta por si el handle aun no esta listo.
    param([string]$Path)
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ($true) {
        try {
            $stream = [IO.File]::Open($Path, [IO.FileMode]::Open,
                [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
            try {
                $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
                try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
            } finally { $stream.Dispose() }
        } catch [IO.IOException] {
            if ([DateTime]::UtcNow -ge $deadline) { throw }
            Start-Sleep -Milliseconds 250
        }
    }
}

function Merge-OutputLogs {
    param([string]$StdoutPath, [string]$StderrPath, [string]$Destination)
    $builder = [Text.StringBuilder]::new()
    if (Test-Path -LiteralPath $StdoutPath -PathType Leaf) {
        [void]$builder.Append((Read-SharedText $StdoutPath))
    }
    if (Test-Path -LiteralPath $StderrPath -PathType Leaf) {
        $stderr = Read-SharedText $StderrPath
        if ($stderr.Length -gt 0) {
            if ($builder.Length -gt 0 -and -not $builder.ToString().EndsWith("`n")) {
                [void]$builder.AppendLine()
            }
            [void]$builder.AppendLine('===== stderr =====')
            [void]$builder.Append($stderr)
        }
    }
    [IO.File]::WriteAllText($Destination, $builder.ToString(),
        [Text.UTF8Encoding]::new($false))
}

function Copy-NewPortLogs {
    param([string]$DataDir, [datetime]$SinceLocal, [string]$EvidenceDir)
    $copied = [Collections.Generic.List[string]]::new()
    $logsDir = Join-Path $DataDir 'out\logs'
    $destDir = Join-Path $EvidenceDir 'logs'
    if (Test-Path -LiteralPath $logsDir -PathType Container) {
        foreach ($file in @(Get-ChildItem -LiteralPath $logsDir -Filter 'run-*.log' -File -ErrorAction SilentlyContinue)) {
            if ($file.LastWriteTime -ge $SinceLocal.AddSeconds(-2)) {
                if (-not (Test-Path -LiteralPath $destDir)) {
                    New-Item -ItemType Directory -Force -Path $destDir | Out-Null
                }
                Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $destDir $file.Name) -Force
                $copied.Add("logs/$($file.Name)")
            }
        }
    }
    $last = Join-Path $DataDir 'out\last_run.log'
    if (Test-Path -LiteralPath $last -PathType Leaf) {
        $item = Get-Item -LiteralPath $last
        if ($item.LastWriteTime -ge $SinceLocal.AddSeconds(-2)) {
            if (-not (Test-Path -LiteralPath $destDir)) {
                New-Item -ItemType Directory -Force -Path $destDir | Out-Null
            }
            Copy-Item -LiteralPath $last -Destination (Join-Path $destDir 'last_run.log') -Force
            $copied.Add('logs/last_run.log')
        }
    }
    return $copied.ToArray()
}

function ConvertTo-DoubleInvariant {
    param([string]$Text)
    $value = "$Text".Trim()
    if (-not $value) { return $null }
    $value = $value.Replace(',', '.')
    $parsed = 0.0
    if ([double]::TryParse($value, [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) {
        return $parsed
    }
    return $null
}

function Get-Median {
    param([double[]]$Values)
    $sorted = @($Values | Where-Object { $null -ne $_ } | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2 -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Get-Percentile {
    param([double[]]$Values, [double]$Percent)
    $sorted = @($Values | Where-Object { $null -ne $_ } | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    $rank = [Math]::Ceiling(($Percent / 100.0) * $sorted.Count)
    $index = [Math]::Min($sorted.Count - 1, [Math]::Max(0, [int]$rank - 1))
    return [double]$sorted[$index]
}

function Get-LogMetrics {
    param([string]$Path)
    $metrics = [ordered]@{
        fps_median            = $null
        fps_p95               = $null
        fps_samples           = 0
        fps_source            = $null
        frame_pacing_median_ms = $null
        frame_pacing_p99_ms   = $null
        build                 = $null
        has_config_block      = $false
        config_ini            = [ordered]@{}
        config_env            = [ordered]@{}
        issues                = @()
    }
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject]$metrics
    }

    $fpsValues = [Collections.Generic.List[double]]::new()
    $pacingMedians = [Collections.Generic.List[double]]::new()
    $pacingP99 = [Collections.Generic.List[double]]::new()
    $issues = [Collections.Generic.List[string]]::new()

    foreach ($line in [IO.File]::ReadLines($Path)) {
        if (-not $metrics.build -and $line -match '^Build:\s*(.+)$') {
            $metrics.build = $Matches[1].Trim()
        }
        if (-not $metrics.has_config_block -and $line -match '^Config:\s*-+') {
            $metrics.has_config_block = $true
        }
        if ($line -match '^Config ini:\s*(.+)$') {
            $entry = $Matches[1]
            $equals = $entry.IndexOf('=')
            if ($equals -ge 1) {
                $metrics.config_ini[$entry.Substring(0, $equals).Trim()] =
                    $entry.Substring($equals + 1).Trim()
            }
        } elseif ($line -match '^Config env:\s*([^=]+)=(.*)$') {
            $metrics.config_env[$Matches[1].Trim()] = $Matches[2].Trim()
        }
        if ($line -match '^\s*Frame stats:\s*([0-9]+(?:[.,][0-9]+)?)\s*FPS') {
            $value = ConvertTo-DoubleInvariant $Matches[1]
            if ($null -ne $value -and $value -gt 0) { $fpsValues.Add($value) }
        } elseif ($line -match '^\s*Frame pacing:\s*median\s*([0-9]+(?:[.,][0-9]+)?)\s*ms.*?\bp99\s*([0-9]+(?:[.,][0-9]+)?)\s*ms') {
            $median = ConvertTo-DoubleInvariant $Matches[1]
            $p99 = ConvertTo-DoubleInvariant $Matches[2]
            if ($null -ne $median) { $pacingMedians.Add($median) }
            if ($null -ne $p99) { $pacingP99.Add($p99) }
        }
        if ($issues.Count -lt 10 -and $line -match '^(Fault|STOP|FREEZE):') {
            $issues.Add($line.Trim())
        }
    }

    if ($fpsValues.Count -gt 0) {
        $metrics.fps_median = [Math]::Round((Get-Median $fpsValues.ToArray()), 1)
        $metrics.fps_p95 = [Math]::Round((Get-Percentile $fpsValues.ToArray() 95), 1)
        $metrics.fps_samples = $fpsValues.Count
        $metrics.fps_source = 'Frame stats (ventanas de 5 s)'
    }
    if ($pacingMedians.Count -gt 0) {
        $metrics.frame_pacing_median_ms = [Math]::Round((Get-Median $pacingMedians.ToArray()), 2)
    }
    if ($pacingP99.Count -gt 0) {
        $metrics.frame_pacing_p99_ms = [Math]::Round((Get-Median $pacingP99.ToArray()), 2)
    }
    $metrics.issues = $issues.ToArray()
    return [pscustomobject]$metrics
}

function Get-NumericColumn {
    param($Rows, [string]$Name)
    $values = [Collections.Generic.List[double]]::new()
    foreach ($row in $Rows) {
        $raw = $row.$Name
        if ($null -eq $raw -or "$raw".Trim() -eq '') { continue }
        $parsed = 0.0
        if ([double]::TryParse("$raw", [Globalization.NumberStyles]::Float,
                [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) {
            $values.Add($parsed)
        }
    }
    return $values.ToArray()
}

function Get-CsvMetrics {
    param([string]$Path)
    $metrics = [ordered]@{
        sample_count           = 0
        sample_span_sec        = $null
        cpu_median_pct         = $null
        cpu_max_pct            = $null
        working_set_max_mb     = $null
        private_max_mb         = $null
        vram_max_mb            = $null
        vram_total_mb          = $null
        gpu_utilization_median_pct = $null
        gpu_temp_max_c         = $null
        gpu_power_max_w        = $null
        gpu_sm_clock_median_mhz = $null
        gpu_samples            = 0
    }
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject]$metrics
    }

    $rows = @(Import-Csv -LiteralPath $Path)
    if ($rows.Count -eq 0) { return [pscustomobject]$metrics }
    $metrics.sample_count = $rows.Count

    $elapsed = Get-NumericColumn $rows 'elapsed_sec'
    if ($elapsed.Count -ge 2) {
        $metrics.sample_span_sec = [Math]::Round($elapsed[-1] - $elapsed[0], 2)
    }

    $cpu = Get-NumericColumn $rows 'cpu_percent'
    if ($cpu.Count -gt 0) {
        $metrics.cpu_median_pct = [Math]::Round((Get-Median $cpu), 1)
        $metrics.cpu_max_pct = [Math]::Round(($cpu | Measure-Object -Maximum).Maximum, 1)
    }
    $workingSet = Get-NumericColumn $rows 'working_set_mb'
    if ($workingSet.Count -gt 0) {
        $metrics.working_set_max_mb = [Math]::Round(($workingSet | Measure-Object -Maximum).Maximum, 0)
    }
    $private = Get-NumericColumn $rows 'private_mb'
    if ($private.Count -gt 0) {
        $metrics.private_max_mb = [Math]::Round(($private | Measure-Object -Maximum).Maximum, 0)
    }
    $vram = Get-NumericColumn $rows 'vram_used_mb'
    if ($vram.Count -gt 0) {
        $metrics.gpu_samples = $vram.Count
        $metrics.vram_max_mb = [Math]::Round(($vram | Measure-Object -Maximum).Maximum, 0)
    }
    $vramTotal = Get-NumericColumn $rows 'vram_total_mb'
    if ($vramTotal.Count -gt 0) {
        $metrics.vram_total_mb = [Math]::Round(($vramTotal | Measure-Object -Maximum).Maximum, 0)
    }
    $gpuUtil = Get-NumericColumn $rows 'gpu_util_percent'
    if ($gpuUtil.Count -gt 0) {
        $metrics.gpu_utilization_median_pct = [Math]::Round((Get-Median $gpuUtil), 1)
    }
    $gpuTemp = Get-NumericColumn $rows 'gpu_temp_c'
    if ($gpuTemp.Count -gt 0) {
        $metrics.gpu_temp_max_c = [Math]::Round(($gpuTemp | Measure-Object -Maximum).Maximum, 1)
    }
    $gpuPower = Get-NumericColumn $rows 'gpu_power_w'
    if ($gpuPower.Count -gt 0) {
        $metrics.gpu_power_max_w = [Math]::Round(($gpuPower | Measure-Object -Maximum).Maximum, 1)
    }
    $gpuClock = Get-NumericColumn $rows 'gpu_sm_clock_mhz'
    if ($gpuClock.Count -gt 0) {
        $metrics.gpu_sm_clock_median_mhz = [Math]::Round((Get-Median $gpuClock), 0)
    }
    return [pscustomobject]$metrics
}

function Test-PathLikeFlag {
    param([string]$Key)
    return $Key -match '(?i)(_DIR|_PATH|_CONFIG|_COMMAND|_ICON|_GLYPHS|_MODS_DIR|_MODS_CONFIG|_PATCHES_DIR|_PATCHES_CONFIG|_PROBE)$'
}

function Resolve-AutoPlayScenario {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Value,
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$AutoplayDir
    )

    if (-not (Get-Command Resolve-GameKeyDefinition -ErrorAction SilentlyContinue)) {
        throw 'Se necesita tools\GameWindow.ps1 para validar las teclas del escenario.'
    }

    $candidates = [Collections.Generic.List[string]]::new()
    if ($Value -match '[\\/]' -or $Value -match '\.json$') {
        if ([IO.Path]::IsPathRooted($Value)) {
            $candidates.Add($Value)
        } else {
            $candidates.Add((Join-Path (Get-Location).Path $Value))
            $candidates.Add((Join-Path $RepoRoot $Value))
            $candidates.Add((Join-Path $AutoplayDir $Value))
        }
    } else {
        $candidates.Add((Join-Path $AutoplayDir "$Value.json"))
        $candidates.Add((Join-Path $RepoRoot "$Value.json"))
    }

    $source = $null
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $source = [IO.Path]::GetFullPath($candidate)
            break
        }
    }
    if (-not $source) {
        throw "Escenario no encontrado: '$Value'. Probado: $($candidates -join ' | ')"
    }

    try {
        $json = Get-Content -LiteralPath $source -Raw | ConvertFrom-Json -ErrorAction Stop
    } catch {
        throw "JSON invalido en ${source}: $($_.Exception.Message)"
    }
    $rawSteps = @($json.steps)
    if ($rawSteps.Count -eq 0) {
        throw "El escenario $source no define 'steps'."
    }

    $steps = [Collections.Generic.List[object]]::new()
    $index = 0
    $previousElapsed = 0.0
    foreach ($rawStep in $rawSteps) {
        $keyText = [string]$rawStep.key
        if (-not $keyText) { throw "Paso $index sin 'key' en $source." }
        $keyNames = [Collections.Generic.List[string]]::new()
        foreach ($part in @($keyText -split '\+')) {
            $trimmed = $part.Trim()
            if ($trimmed) { $keyNames.Add((Resolve-GameKeyDefinition -Key $trimmed).Name) }
        }
        if ($keyNames.Count -eq 0) { throw "Paso $index con 'key' vacia en $source." }

        $elapsed = $previousElapsed
        if ($null -ne $rawStep.elapsed_sec -and "$($rawStep.elapsed_sec)" -ne '') {
            $elapsed = [double]$rawStep.elapsed_sec
        }
        $holdMs = 120
        if ($null -ne $rawStep.hold_ms -and "$($rawStep.hold_ms)" -ne '') {
            $holdMs = [int]$rawStep.hold_ms
        }
        $delayMs = 0
        if ($null -ne $rawStep.delay_ms -and "$($rawStep.delay_ms)" -ne '') {
            $delayMs = [int]$rawStep.delay_ms
        }
        if ($elapsed -lt 0) { throw "Paso $index con elapsed_sec negativo en $source." }
        if ($holdMs -lt 1 -or $holdMs -gt 30000) { throw "Paso $index con hold_ms fuera de rango (1-30000)." }
        if ($delayMs -lt 0) { throw "Paso $index con delay_ms negativo." }

        $steps.Add([pscustomobject]@{
            index       = $index
            elapsed_sec = [Math]::Round($elapsed, 1)
            key         = [string]::Join('+', $keyNames)
            hold_ms     = $holdMs
            delay_ms    = $delayMs
        })
        $previousElapsed = $elapsed + ($holdMs / 1000.0) + ($delayMs / 1000.0)
        $index++
    }

    $sorted = @($steps | Sort-Object -Property elapsed_sec, index)
    for ($i = 0; $i -lt $sorted.Count; $i++) { $sorted[$i].index = $i }

    $maxLateness = 5.0
    if ($null -ne $json.max_lateness_sec -and "$($json.max_lateness_sec)" -ne '') {
        $maxLateness = [double]$json.max_lateness_sec
    }
    $planned = $null
    if ($null -ne $json.planned_duration_sec -and "$($json.planned_duration_sec)" -ne '') {
        $planned = [double]$json.planned_duration_sec
    } else {
        $lastStep = $sorted[$sorted.Count - 1]
        $planned = $lastStep.elapsed_sec + ($lastStep.hold_ms / 1000.0)
    }

    return [pscustomobject]@{
        Name               = if ($json.name) { [string]$json.name } else { [IO.Path]::GetFileNameWithoutExtension($source) }
        Source             = $source
        Description        = [string]$json.description
        Steps              = $sorted
        MaxLatenessSec     = $maxLateness
        PlannedDurationSec = [Math]::Round($planned, 1)
    }
}

function ConvertTo-SerializableWindow {
    param($Window)
    if (-not $Window) { return $null }
    $foreground = $false
    try {
        $foreground = [TlgGameWindow.Native]::GetForegroundWindow().ToInt64() -eq $Window.Handle.ToInt64()
    } catch { }
    return [pscustomobject]@{
        handle     = $Window.HandleHex
        title      = $Window.Title
        process_id = $Window.ProcessId
        dpi        = $Window.Dpi
        minimized  = $Window.Minimized
        foreground = $foreground
        rect       = [pscustomobject]@{
            left   = $Window.Rect.Left
            top    = $Window.Rect.Top
            width  = $Window.Width
            height = $Window.Height
        }
    }
}

function Start-CodexFeedbackJob {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ImagePath,
        [Parameter(Mandatory)][string]$EvidenceDir,
        [string]$ContextFile,
        [Parameter(Mandatory)][string]$CodexScript,
        [Parameter(Mandatory)][string]$RepoRoot
    )

    # Archiva las salidas del analisis anterior para conservar historial por
    # captura; los nombres canonicos (chatgpt-feedback.*) quedan siempre con el
    # ultimo resultado.
    $archiveDir = Join-Path $EvidenceDir 'feedback'
    $baseName = [IO.Path]::GetFileNameWithoutExtension([IO.Path]::GetFileName($ImagePath))
    foreach ($name in @('chatgpt-feedback.json', 'chatgpt-feedback.md',
            'chatgpt-feedback.error.txt', 'chatgpt-feedback.raw.txt',
            'chatgpt-feedback.stdout.log', 'chatgpt-feedback.stderr.log')) {
        $file = Join-Path $EvidenceDir $name
        if (Test-Path -LiteralPath $file -PathType Leaf) {
            if (-not (Test-Path -LiteralPath $archiveDir)) {
                New-Item -ItemType Directory -Force -Path $archiveDir | Out-Null
            }
            $target = Join-Path $archiveDir ($name -replace '^chatgpt-feedback', "chatgpt-feedback-$baseName")
            Move-Item -LiteralPath $file -Destination $target -Force
        }
    }

    $outLog = Join-Path $EvidenceDir "codex-feedback-$baseName.out.log"
    $errLog = Join-Path $EvidenceDir "codex-feedback-$baseName.err.log"
    $pwshPath = (Get-Process -Id $PID).Path
    $arguments = @(
        '-NoLogo', '-NoProfile', '-NonInteractive',
        '-File', (ConvertTo-QuotedArgument $CodexScript),
        '-ImagePath', (ConvertTo-QuotedArgument $ImagePath),
        '-EvidenceDir', (ConvertTo-QuotedArgument $EvidenceDir),
        '-Repo', (ConvertTo-QuotedArgument $RepoRoot),
        '-TimeoutSec', '240'
    )
    if ($ContextFile -and (Test-Path -LiteralPath $ContextFile -PathType Leaf)) {
        $arguments += @('-ContextFile', (ConvertTo-QuotedArgument $ContextFile))
    }
    $process = Start-Process -FilePath $pwshPath -ArgumentList $arguments -PassThru `
        -WindowStyle Hidden -RedirectStandardOutput $outLog -RedirectStandardError $errLog
    return [pscustomobject]@{
        Process   = $process
        OutLog    = $outLog
        ErrLog    = $errLog
        ImagePath = $ImagePath
    }
}

function Invoke-AutomationTick {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]$State,
        [Parameter(Mandatory)]$Game,
        [Parameter(Mandatory)]$Config,
        [object[]]$Steps = @()
    )

    $native = [TlgGameWindow.Native]
    $elapsed = ([DateTime]::UtcNow - $State.started_utc).TotalSeconds
    $dirty = $false

    # 1) Ventana: revalidar la cache y volver a localizarla si desaparecio.
    $live = $Config.LiveWindow
    if ($live -and -not $native::IsWindow($live.Handle)) {
        $live = $null
        $Config.LiveWindow = $null
    }
    if (-not $live) {
        $live = Get-GameWindow -ProcessId $Game.Id
        $Config.LiveWindow = $live
        if ($live) { $State.errors += 'Ventana del juego re-localizada.' }
    }
    if ($live) {
        $State.window = ConvertTo-SerializableWindow -Window $live
    }

    # 2) Foco (solo con -BringToFront): al inicio y cada 30 s.
    if ($Config.BringToFront -and $elapsed -ge $State.counters.next_focus_at) {
        $State.counters.next_focus_at = $elapsed + 30
        if ($live) {
            $State.focus.attempts = [int]$State.focus.attempts + 1
            $ok = $false
            try { $ok = Set-GameWindowFocus -Window $live -Quiet } catch { }
            if ($ok) {
                $State.focus.last_ok = $true
            } else {
                $State.focus.failures = [int]$State.focus.failures + 1
                $State.focus.last_ok = $false
            }
            $State.focus.last_at_sec = [Math]::Round($elapsed, 1)
            try {
                $State.focus.foreground_title = (Get-ForegroundWindowInfo).Title
            } catch { }
            $dirty = $true
        }
    }

    # 3) Timeline del escenario: pasos vencidos, con limite de retraso.
    while ($State.counters.step_index -lt $Steps.Count -and
        [double]$Steps[$State.counters.step_index].elapsed_sec -le $elapsed) {
        $index = [int]$State.counters.step_index
        $step = $Steps[$index]
        $entry = $State.timeline[$index]
        $late = $elapsed - [double]$step.elapsed_sec
        if ($State.max_lateness_sec -gt 0 -and $late -gt $State.max_lateness_sec) {
            $entry.status = 'skipped'
            $entry.at_sec = [Math]::Round($elapsed, 1)
            $entry.note = "retraso de $([Math]::Round($late, 1)) s"
        } else {
            $ok = $false
            if ($live) {
                try {
                    $ok = Send-GameKeyScript -ProcessId $Game.Id -Window $live -Steps @($step) -Method Auto
                } catch {
                    $State.errors += "Paso $index ($($step.key)): $($_.Exception.Message)"
                }
            } else {
                $State.errors += "Paso $index ($($step.key)): sin ventana del juego."
            }
            $entry.status = if ($ok) { 'done' } else { 'failed' }
            $entry.at_sec = [Math]::Round($elapsed, 1)
            $entry.ok = $ok
            if (-not $ok) {
                $State.errors += "Paso $index ($($step.key)) fallo al enviar la tecla."
            }
        }
        $State.counters.step_index = $index + 1
        $dirty = $true
    }

    # 4) Capturas de ventana cada ScreenshotIntervalSec.
    if ($Config.ScreenshotIntervalSec -gt 0 -and $elapsed -ge $State.counters.next_capture_at) {
        $planned = [int][Math]::Floor($State.counters.next_capture_at)
        $fileName = 'automation-{0}s.png' -f $planned
        $entry = [ordered]@{
            file        = $fileName
            planned_sec = $planned
            at_sec      = $null
            method      = $null
            width       = $null
            height      = $null
            foreground  = $null
            error       = $null
        }
        if ($live) {
            try {
                $shot = Save-WindowScreenshot -Window $live -Quiet `
                    -Path (Join-Path $Config.EvidenceDir $fileName)
                if ($shot) {
                    $entry.at_sec = [Math]::Round($elapsed, 1)
                    $entry.method = $shot.Method
                    $entry.width = $shot.Width
                    $entry.height = $shot.Height
                    $entry.foreground = $shot.Foreground
                    $State.counters.captures_since_feedback = [int]$State.counters.captures_since_feedback + 1
                } else {
                    $entry.error = 'captura fallida'
                }
            } catch {
                $entry.error = $_.Exception.Message
            }
        } else {
            $entry.error = 'sin ventana del juego'
        }
        if ($entry.error) {
            $State.errors += "Captura $planned s: $($entry.error)"
        }
        $State.captures += $entry
        $State.counters.next_capture_at = $State.counters.next_capture_at + $Config.ScreenshotIntervalSec
        while ($State.counters.next_capture_at -le $elapsed) {
            $State.counters.skipped_captures = [int]$State.counters.skipped_captures + 1
            $State.counters.next_capture_at = $State.counters.next_capture_at + $Config.ScreenshotIntervalSec
        }
        $dirty = $true
    }

    # 5) Feedback de Codex cada N capturas (en segundo plano, sin bloquear).
    if ($Config.CodexFeedback -gt 0 -and
        $State.counters.captures_since_feedback -ge $Config.CodexFeedback -and
        $State.captures.Count -gt 0 -and
        $Config.CodexScript -and (Test-Path -LiteralPath $Config.CodexScript -PathType Leaf)) {
        $busy = $false
        foreach ($job in @($Config.FeedbackJobs)) {
            if (-not $job.Process.HasExited) { $busy = $true; break }
        }
        if (-not $busy) {
            $last = $State.captures[$State.captures.Count - 1]
            if ($last.file -and -not $last.error) {
                $imagePath = Join-Path $Config.EvidenceDir $last.file
                if (Test-Path -LiteralPath $imagePath -PathType Leaf) {
                    try {
                        $job = Start-CodexFeedbackJob -ImagePath $imagePath -EvidenceDir $Config.EvidenceDir `
                            -ContextFile $Config.ContextFile -CodexScript $Config.CodexScript -RepoRoot $Config.RepoRoot
                        $Config.FeedbackJobs.Add($job) | Out-Null
                        $State.counters.captures_since_feedback = 0
                        $State.feedback += [ordered]@{
                            image   = $last.file
                            at_sec  = [Math]::Round($elapsed, 1)
                            status  = 'started'
                            pid     = $job.Process.Id
                            out_log = [IO.Path]::GetFileName($job.OutLog)
                            err_log = [IO.Path]::GetFileName($job.ErrLog)
                        }
                        $dirty = $true
                    } catch {
                        $State.errors += "Feedback de Codex: $($_.Exception.Message)"
                    }
                }
            }
        }
    }

    return $dirty
}

function Invoke-Baseline {
    $safeTag = ConvertTo-SafeTag $Tag
    $evidenceDir = New-EvidenceDirectory -Root $evidenceRoot -SafeTag $safeTag
    $startedUtc = [DateTime]::UtcNow

    $gameDir = Get-EffectivePath 'TLG_GAME_DIR' $defaultGameDir
    $dataDir = Get-EffectivePath 'TLG_DATA_DIR' $defaultDataDir
    $userDir = Get-EffectivePath 'ISS_USER_DIR' $defaultUserDir
    $runPy = Join-Path $portDir 'run.py'
    $probePath = Join-Path $portDir 'out\tlg-probe.exe'

    $result = [ordered]@{
        schema                = 1
        status                = 'initializing'
        tag                   = $Tag
        requested_duration_sec = $DurationSec
        started_at            = $startedUtc.ToString('o')
        ended_at              = $null
        duration_sec          = $null
        launch_duration_sec   = $null
        game_started_at       = $null
        game_ended_at         = $null
        evidence_dir          = $evidenceDir
        command               = $null
        resolution            = [ordered]@{
            requested = $Resolution
            effective = $null
            source    = $null
        }
        paths                 = [ordered]@{
            repo         = $repoRoot
            port         = $portDir
            run_py       = $runPy
            probe        = $probePath
            game_dir     = $gameDir
            data_dir     = $dataDir
            user_dir     = $userDir
            config       = $null
            python       = $null
        }
        exit_codes            = [ordered]@{
            launcher = $null
            game     = $null
        }
        forced_stop           = $false
        stop_reason           = $null
        extra_env             = [ordered]@{}
        flags                 = [ordered]@{}
        system                = $null
        metrics               = [ordered]@{}
        build                 = $null
        log_config            = [ordered]@{
            ini = [ordered]@{}
            env = [ordered]@{}
        }
        log_issues            = @()
        log_file              = $null
        samples_file          = $null
        sampler_log           = $null
        collected_logs        = @()
        errors                = @()
        notes                 = @()
    }

    $result.system = Get-SystemInfo
    foreach ($rawKey in @($ExtraEnv.Keys)) {
        $key = [string]$rawKey
        if ($key -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') {
            throw "Nombre de variable invalido en -ExtraEnv: $key"
        }
        if ($key -match '^(?i)(TLG|ISS)_') { $key = $key.ToUpperInvariant() }
        $result.extra_env[$key] = ConvertTo-EnvValue $ExtraEnv[$rawKey]
    }

    # ExtraEnv tambien puede mover las carpetas principales; se resuelven antes
    # del preflight para comprobar la ruta que realmente vera el juego.
    $override = Get-MapValue $result.extra_env 'TLG_GAME_DIR'
    if ($null -ne $override) { $gameDir = Resolve-HarnessPath ([string]$override) }
    $override = Get-MapValue $result.extra_env 'TLG_DATA_DIR'
    if ($null -ne $override) { $dataDir = Resolve-HarnessPath ([string]$override) }
    $override = Get-MapValue $result.extra_env 'ISS_USER_DIR'
    if ($null -ne $override) { $userDir = Resolve-HarnessPath ([string]$override) }

    # TLG_PROBE manda; si no, se replica el orden de run.py (bin/ antes que out/).
    $probeOverride = Get-MapValue $result.extra_env 'TLG_PROBE'
    if (-not [string]::IsNullOrWhiteSpace([string]$probeOverride)) {
        $probePath = Resolve-HarnessPath ([string]$probeOverride)
    } else {
        $inheritedProbe = [Environment]::GetEnvironmentVariable('TLG_PROBE', 'Process')
        if (-not [string]::IsNullOrWhiteSpace([string]$inheritedProbe)) {
            $probePath = Resolve-HarnessPath $inheritedProbe
        } else {
            $binProbe = Join-Path $portDir 'bin\tlg-probe.exe'
            if (Test-Path -LiteralPath $binProbe -PathType Leaf) {
                $probePath = $binProbe
            }
        }
    }
    $result.paths.game_dir = $gameDir
    $result.paths.data_dir = $dataDir
    $result.paths.user_dir = $userDir
    $result.paths.run_py = $runPy
    $result.paths.probe = $probePath

    # Copia de tlg.ini para la evidencia; con -Resolution se usa ademas como
    # TLG_CONFIG para no tocar la configuracion del usuario.
    $configDir = Join-Path $evidenceDir 'config'
    New-Item -ItemType Directory -Force -Path $configDir | Out-Null
    $configOverride = Get-MapValue $result.extra_env 'TLG_CONFIG'
    if ($null -ne $configOverride) {
        $baseConfig = Resolve-HarnessPath ([string]$configOverride)
    } else {
        $inheritedConfig = [Environment]::GetEnvironmentVariable('TLG_CONFIG', 'Process')
        $baseConfig = if ($inheritedConfig) {
            Resolve-HarnessPath $inheritedConfig
        } else {
            Join-Path $dataDir 'tlg.ini'
        }
    }
    $launchConfig = $null

    if ($Resolution -or $IniExtra.Count -gt 0) {
        $launchConfig = Join-Path $configDir 'tlg.ini'
        if (Test-Path -LiteralPath $baseConfig -PathType Leaf) {
            Copy-Item -LiteralPath $baseConfig -Destination $launchConfig -Force
        }
        if ($Resolution) {
            Update-IniValue -Path $launchConfig -Key 'render_resolution' -Value $Resolution
            $result.resolution.effective = $Resolution
            $result.resolution.source = 'override en evidencia (copia de tlg.ini)'
        }
        Update-IniValue -Path $launchConfig -Key 'restart_unconfirmed' -Value '0'
        foreach ($key in $IniExtra.Keys) {
            Update-IniValue -Path $launchConfig -Key $key -Value ([string]$IniExtra[$key])
        }
        if ($IniExtra.Count -gt 0) {
            $result.notes += ("tlg.ini de la evidencia: " + (($IniExtra.Keys | ForEach-Object { "$_=$($IniExtra[$_])" }) -join ' '))
        }
        $result.paths.config = $launchConfig
    } else {
        if (Test-Path -LiteralPath $baseConfig -PathType Leaf) {
            $recordConfig = Join-Path $configDir 'tlg.ini'
            Copy-Item -LiteralPath $baseConfig -Destination $recordConfig -Force
            $effective = Read-IniValue $baseConfig 'render_resolution'
            $result.resolution.effective = if ($effective) { $effective } else { 'native (default)' }
            $result.resolution.source = $baseConfig
            $result.paths.config = $baseConfig
            $result.notes += 'Copia de tlg.ini para referencia: config/tlg.ini'
        } else {
            $result.resolution.effective = 'native (default)'
            $result.resolution.source = '(sin tlg.ini; valores por defecto)'
        }
    }

    $errors = [Collections.Generic.List[string]]::new()
    if (-not (Test-Path -LiteralPath $runPy -PathType Leaf)) {
        $errors.Add("No existe $runPy")
    }
    if (-not (Test-Path -LiteralPath $probePath -PathType Leaf)) {
        $errors.Add("No existe $probePath. Compila el port con port\build.sh (MSYS2 CLANG64) o define TLG_PROBE.")
    }
    if (-not (Test-Path -LiteralPath (Join-Path $gameDir 'eboot.bin') -PathType Leaf)) {
        $errors.Add("No existe eboot.bin en $gameDir")
    }
    $existingGames = @(Get-Process -Name 'tlg-probe' -ErrorAction SilentlyContinue)
    if ($existingGames.Count -gt 0) {
        $errors.Add("Hay $($existingGames.Count) proceso(s) tlg-probe.exe abiertos; cierralos antes de medir.")
    }

    $pythonPath = $null
    try {
        $pythonPath = Resolve-PythonPath $Python
        $result.paths.python = $pythonPath
        $versionOutput = @(& $pythonPath -c "import sys; print('.'.join(map(str, sys.version_info[:3])))" 2>$null)
        if ($LASTEXITCODE -ne 0) {
            $errors.Add("Python fallo al comprobar la version: $Python")
        } else {
            $result.notes += "Python $($versionOutput[-1])"
        }
    } catch {
        $errors.Add($_.Exception.Message)
    }

    if ($errors.Count -gt 0) {
        $result.status = 'preflight_failed'
        $result.errors = $errors.ToArray()
        $result.ended_at = [DateTime]::UtcNow.ToString('o')
        Write-JsonFile -Path (Join-Path $evidenceDir 'result.json') -Object ([pscustomobject]$result)
        return [pscustomobject]$result
    }

    # Entorno final: los valores explicitos pisan a los heredados; ExtraEnv pisa a todo.
    $envMap = [ordered]@{
        TLG_GAME_DIR    = $gameDir
        TLG_DATA_DIR    = $dataDir
        ISS_USER_DIR    = $userDir
        TLG_FRAME_STATS = '1'
        # Los defaults que aplica el launcher (port/launcher/infamous_run.py DEFAULT_ENV): al
        # lanzar port\run.py directamente no pasan, y el presupuesto de VRAM es el que decide si
        # el colector de texturas expulsa o no.
        ISS_GC_BUDGET_MB       = '3500'
        ISS_PIPELINE_BINARIES  = '0'
        ISS_UPSCALER           = 'off'
        ISS_JITTER             = '0'
        ISS_OBJECT_MOTION      = '0'
        PYTHONUNBUFFERED = '1'
        PYTHONIOENCODING = 'utf-8'
    }
    foreach ($key in @($result.extra_env.Keys)) {
        $envMap[$key] = [string]$result.extra_env[$key]
    }
    # -Resolution manda sobre un TLG_CONFIG heredado o pasado en ExtraEnv.
    if ($launchConfig) { $envMap['TLG_CONFIG'] = $launchConfig }

    $finalFlags = @{}
    $currentEnv = [Environment]::GetEnvironmentVariables('Process')
    foreach ($key in @($currentEnv.Keys)) {
        if ($key -match '^(TLG|ISS)_') { $finalFlags[$key] = [string]$currentEnv[$key] }
    }
    foreach ($key in @($envMap.Keys)) {
        if ($key -match '^(TLG|ISS)_') { $finalFlags[$key] = [string]$envMap[$key] }
    }
    foreach ($key in @($finalFlags.Keys | Sort-Object)) {
        if (-not (Test-PathLikeFlag $key)) {
            $result.flags[$key] = $finalFlags[$key]
        }
    }

    $stdoutPath = Join-Path $evidenceDir 'stdout.log'
    $stderrPath = Join-Path $evidenceDir 'stderr.log'
    $samplesPath = Join-Path $evidenceDir 'samples.csv'
    $samplerLogPath = Join-Path $evidenceDir 'sampler.log'
    $runLogPath = Join-Path $evidenceDir 'run.log'
    $launchUtc = [DateTime]::UtcNow
    $launchLocal = [DateTime]::Now
    $result.command = "`"$pythonPath`" `"$runPy`""

    $launcher = $null
    $game = $null
    $sampler = $null
    $gameStartUtc = $null
    $gameEndUtc = $null
    $automation = $null
    $automationPath = Join-Path $evidenceDir 'automation.json'
    $automationConfig = $null
    $feedbackJobs = [Collections.Generic.List[object]]::new()

    try {
        $savedEnv = $null
        try {
            $savedEnv = Set-ProcessEnvironment $envMap
            $launcher = Start-Process -FilePath $pythonPath `
                -ArgumentList @((ConvertTo-QuotedArgument $runPy)) `
                -WorkingDirectory $portDir `
                -RedirectStandardOutput $stdoutPath `
                -RedirectStandardError $stderrPath `
                -PassThru -WindowStyle Hidden
        } finally {
            if ($savedEnv) { Restore-ProcessEnvironment $savedEnv }
        }

        $game = Find-GameProcess -ProbePath $probePath -Launcher $launcher `
            -TimeoutSec $StartupTimeoutSec -NotBeforeUtc $launchUtc

        if (-not $game) {
            if ($launcher.HasExited) {
                $result.status = 'launch_failed'
                $result.stop_reason = 'launcher_exit'
                try { $result.exit_codes.launcher = $launcher.ExitCode } catch { }
                $result.errors += 'run.py termino antes de que apareciera tlg-probe.exe.'
            } else {
                $result.status = 'startup_timeout'
                $result.stop_reason = 'startup_timeout'
                $result.errors += "tlg-probe.exe no aparecio en $StartupTimeoutSec s."
                Stop-ProcessTree $launcher.Id
            }
        } else {
            $result.status = 'running'
            $gameStartUtc = $game.StartTime.ToUniversalTime()
            $result.game_started_at = $gameStartUtc.ToString('o')

            try {
                $sampler = Start-Sampler -TargetProcessId $game.Id -CsvPath $samplesPath `
                    -SampleIntervalSec 1 -LimitSeconds ($DurationSec + 60) `
                    -ChildLogPath $samplerLogPath -Quiet
            } catch {
                $result.errors += "Sampler: $($_.Exception.Message)"
                $result.notes += 'La sesion continua sin CSV de metricas.'
            }

            # Automatizacion opcional: capturas de la ventana, input por timeline
            # y feedback de Codex. Sin flags, $automation queda en $null y el
            # bucle mide igual que siempre.
            $scenario = $null
            $scenarioSteps = @()
            if ($script:AutomationRequested) {
                $automation = [ordered]@{
                    schema      = 1
                    enabled     = $true
                    scenario    = $null
                    window      = $null
                    started_at  = $gameStartUtc.ToString('o')
                    started_utc = $gameStartUtc
                    finished_at = $null
                    max_lateness_sec = 5.0
                    config      = [ordered]@{
                        screenshot_interval_sec = $ScreenshotIntervalSec
                        bring_to_front          = [bool]$BringToFront
                        codex_feedback_every    = $CodexFeedback
                        capture                 = 'ventana; PrintWindow y respaldo CopyFromScreen'
                    }
                    counters    = [ordered]@{
                        step_index              = 0
                        next_capture_at         = [double]$ScreenshotIntervalSec
                        next_focus_at           = 0.0
                        captures_since_feedback = 0
                        skipped_captures        = 0
                    }
                    timeline    = @()
                    captures    = @()
                    feedback    = @()
                    focus       = [ordered]@{
                        attempts         = 0
                        failures         = 0
                        last_ok          = $null
                        last_at_sec      = $null
                        foreground_title = $null
                    }
                    errors      = @()
                }
                $automationConfig = @{
                    ScreenshotIntervalSec = $ScreenshotIntervalSec
                    BringToFront          = [bool]$BringToFront
                    CodexFeedback         = $CodexFeedback
                    EvidenceDir           = $evidenceDir
                    CodexScript           = $codexFeedbackScript
                    RepoRoot              = $repoRoot
                    ContextFile           = $automationPath
                    LiveWindow            = $null
                    FeedbackJobs          = $feedbackJobs
                }
                if (-not $script:GameWindowReady) {
                    $automation.errors += 'GameWindow.ps1 no disponible: sin capturas ni input.'
                    $result.errors += 'Automatizacion: GameWindow.ps1 no disponible.'
                }
                if ($AutoPlayScenario) {
                    try {
                        $scenario = Resolve-AutoPlayScenario -Value $AutoPlayScenario `
                            -RepoRoot $repoRoot -AutoplayDir $autoplayDir
                        $scenarioSteps = $scenario.Steps
                        $automation.max_lateness_sec = $scenario.MaxLatenessSec
                        $automation.scenario = [pscustomobject]@{
                            name                 = $scenario.Name
                            source               = $scenario.Source
                            description          = $scenario.Description
                            steps_total          = $scenario.Steps.Count
                            planned_duration_sec = $scenario.PlannedDurationSec
                            max_lateness_sec     = $scenario.MaxLatenessSec
                        }
                        $automation.timeline = @($scenario.Steps | ForEach-Object {
                            [ordered]@{
                                index       = $_.index
                                elapsed_sec = $_.elapsed_sec
                                key         = $_.key
                                hold_ms     = $_.hold_ms
                                delay_ms    = $_.delay_ms
                                status      = 'pending'
                                at_sec      = $null
                                ok          = $null
                                note        = $null
                            }
                        })
                        $result.notes += "Automatizacion: escenario '$($scenario.Name)' ($($scenario.Steps.Count) pasos)."
                    } catch {
                        $automation.errors += "Escenario: $($_.Exception.Message)"
                        $result.errors += "Escenario de autoplay: $($_.Exception.Message)"
                    }
                }
                if ($ScreenshotIntervalSec -gt 0) {
                    $result.notes += "Capturas de ventana cada $ScreenshotIntervalSec s (automation-<n>s.png)."
                }
                if ($CodexFeedback -gt 0) {
                    if ($ScreenshotIntervalSec -gt 0) {
                        $result.notes += "Feedback de Codex cada $CodexFeedback captura(s)."
                    } else {
                        $automation.errors += 'CodexFeedback requiere -ScreenshotIntervalSec > 0.'
                        $result.errors += 'CodexFeedback sin capturas: no se lanzara ningun analisis.'
                    }
                }
                try { Write-JsonFile -Path $automationPath -Object $automation } catch { }
            }

            $deadline = $gameStartUtc.AddSeconds($DurationSec)
            if ($KeepOpen) { $deadline = $gameStartUtc.AddSeconds($KeepOpenMaxSec) }
            $automationDirty = $false
            while ([DateTime]::UtcNow -lt $deadline) {
                try {
                    $game.Refresh()
                    if ($game.HasExited) { break }
                } catch {
                    break
                }
                if ($null -ne $automation -and $script:GameWindowReady) {
                    try {
                        if (Invoke-AutomationTick -State $automation -Game $game `
                                -Config $automationConfig -Steps $scenarioSteps) {
                            $automationDirty = $true
                        }
                    } catch {
                        $automation.errors += "Tick: $($_.Exception.Message)"
                        $automationDirty = $true
                    }
                    if ($automationDirty) {
                        try {
                            Write-JsonFile -Path $automationPath -Object $automation
                            $automationDirty = $false
                        } catch { }
                    }
                }
                Start-Sleep -Milliseconds 250
            }

            try { $game.Refresh() } catch { }
            if ($game.HasExited) {
                $result.status = 'early_exit'
                $result.stop_reason = if ($KeepOpen) { 'user_closed' } else { 'game_exit' }
                if ($KeepOpen) { $result.notes += 'La ventana la cerro el jugador; el harness no toco el juego.' }
                $gameEndUtc = [DateTime]::UtcNow
            } else {
                $gameEndUtc = [DateTime]::UtcNow
                if ($KeepOpen) {
                    Write-Host 'Harness: se agota KeepOpenMaxSec; se cierra la ventana del juego.'
                    $result.notes += "El harness cerro la ventana al agotarse KeepOpenMaxSec ($KeepOpenMaxSec s)."
                } else {
                    Write-Host 'Harness: fin de la duracion; se cierra la ventana del juego (no es un crash).'
                }
                $stop = Stop-GameProcess -Game $game -GraceSec 12
                $result.forced_stop = [bool]$stop.forced
                if ($result.forced_stop) {
                    $result.notes += 'Cierre forzado: el final de run.log puede estar incompleto.'
                }
                $result.status = 'completed'
                $result.stop_reason = 'duration'
            }
        }
    } catch {
        if ($result.status -eq 'initializing' -or $result.status -eq 'running') {
            $result.status = 'error'
        }
        $result.errors += $_.Exception.Message
    } finally {
        if ($game -and -not $game.HasExited) {
            $stop = Stop-GameProcess -Game $game -GraceSec 12
            if ($stop.forced) {
                $result.forced_stop = $true
                $result.notes += 'Cierre forzado: el final de run.log puede estar incompleto.'
            }
            if (-not $gameEndUtc) { $gameEndUtc = [DateTime]::UtcNow }
        }

        # Ctrl+C o una parada externa dejan running: se registra como interrumpida.
        if ($result.status -eq 'running') {
            $result.status = 'interrupted'
            $result.stop_reason = 'interrupted'
        }

        if ($launcher -and -not $launcher.HasExited) {
            if (-not $launcher.WaitForExit(20000)) {
                $result.notes += 'run.py no termino 20 s despues del juego; se cerro su arbol de procesos.'
                Stop-ProcessTree $launcher.Id
                $launcher.WaitForExit(8000) | Out-Null
            }
        }

        if ($sampler) { Stop-Sampler -Context $sampler -GraceSec 5 }

        if ($game) {
            try { if (-not $gameEndUtc) { $gameEndUtc = [DateTime]::UtcNow } } catch { }
            if (-not $gameEndUtc) { $gameEndUtc = [DateTime]::UtcNow }
            try { $result.exit_codes.game = $game.ExitCode } catch { }
            $result.game_ended_at = $gameEndUtc.ToString('o')
            if ($gameStartUtc) {
                $result.duration_sec = [Math]::Round(($gameEndUtc - $gameStartUtc).TotalSeconds, 1)
            }
        }
        if ($launcher) {
            try { $result.exit_codes.launcher = $launcher.ExitCode } catch { }
        }
        if ($gameStartUtc) {
            $result.launch_duration_sec = [Math]::Round(($gameStartUtc - $launchUtc).TotalSeconds, 1)
        } else {
            $result.launch_duration_sec = [Math]::Round(([DateTime]::UtcNow - $launchUtc).TotalSeconds, 1)
        }

        try {
            Merge-OutputLogs -StdoutPath $stdoutPath -StderrPath $stderrPath -Destination $runLogPath
        } catch {
            $result.errors += "No se pudieron fusionar stdout/stderr: $($_.Exception.Message)"
            $result.notes += 'run.log puede estar incompleto; se conservan stdout.log y stderr.log.'
            if (-not (Test-Path -LiteralPath $runLogPath -PathType Leaf)) {
                [IO.File]::WriteAllText($runLogPath, '', [Text.UTF8Encoding]::new($false))
            }
        }
        if ((Get-Item -LiteralPath $runLogPath).Length -gt 0) {
            $result.log_file = 'run.log'
        }
        if (Test-Path -LiteralPath $samplesPath -PathType Leaf) {
            $result.samples_file = 'samples.csv'
        }
        if (Test-Path -LiteralPath $samplerLogPath -PathType Leaf) {
            $result.sampler_log = 'sampler.log'
        }

        $logMetrics = Get-LogMetrics $runLogPath
        if ($logMetrics.fps_samples -eq 0) {
            $alternatives = @(Copy-NewPortLogs -DataDir $dataDir -SinceLocal $launchLocal `
                -EvidenceDir $evidenceDir)
            $result.collected_logs = $alternatives
            foreach ($relative in $alternatives) {
                $candidatePath = Join-Path $evidenceDir $relative
                $alternative = Get-LogMetrics $candidatePath
                if ($alternative.fps_samples -gt 0) {
                    $logMetrics = $alternative
                    $result.notes += "FPS leidos de $relative"
                    break
                }
            }
        } else {
            $result.collected_logs = @(Copy-NewPortLogs -DataDir $dataDir `
                -SinceLocal $launchLocal -EvidenceDir $evidenceDir)
        }

        $csvMetrics = Get-CsvMetrics $samplesPath
        $result.build = $logMetrics.build
        $result.log_config = [ordered]@{
            ini = $logMetrics.config_ini
            env = $logMetrics.config_env
        }
        $result.log_issues = @($logMetrics.issues)
        $result.metrics = [ordered]@{
            fps = [ordered]@{
                median  = $logMetrics.fps_median
                p95     = $logMetrics.fps_p95
                samples = $logMetrics.fps_samples
                source  = $logMetrics.fps_source
            }
            frame_pacing = [ordered]@{
                median_ms = $logMetrics.frame_pacing_median_ms
                p99_ms    = $logMetrics.frame_pacing_p99_ms
                note      = 'Mediana de los valores reportados por ventana de 5 s.'
            }
            memory = [ordered]@{
                vram_max_mb     = $csvMetrics.vram_max_mb
                vram_total_mb   = $csvMetrics.vram_total_mb
                ram_max_mb      = $csvMetrics.working_set_max_mb
                private_max_mb  = $csvMetrics.private_max_mb
            }
            gpu = [ordered]@{
                utilization_median_pct = $csvMetrics.gpu_utilization_median_pct
                temperature_max_c      = $csvMetrics.gpu_temp_max_c
                power_max_w            = $csvMetrics.gpu_power_max_w
                sm_clock_median_mhz    = $csvMetrics.gpu_sm_clock_median_mhz
                samples                = $csvMetrics.gpu_samples
            }
            cpu = [ordered]@{
                median_pct = $csvMetrics.cpu_median_pct
                max_pct    = $csvMetrics.cpu_max_pct
            }
            samples = [ordered]@{
                count        = $csvMetrics.sample_count
                span_sec     = $csvMetrics.sample_span_sec
                interval_sec = 1
            }
        }

        # Cierre de la automatizacion: espera breve al feedback pendiente y
        # escribe automation.json definitivo con resumen y estado del foco.
        if ($null -ne $automation) {
            $pendingFeedback = 0
            foreach ($job in @($feedbackJobs)) {
                if (-not $job.Process.HasExited) {
                    try { $job.Process.WaitForExit(15000) | Out-Null } catch { }
                }
                if ($job.Process.HasExited) {
                    $status = 'finished'
                } else {
                    $status = 'still_running'
                    $pendingFeedback++
                }
                foreach ($entry in @($automation.feedback)) {
                    if ([int]$entry.pid -eq $job.Process.Id) {
                        $entry.status = $status
                        if ($status -eq 'finished') {
                            try { $entry.exit_code = $job.Process.ExitCode } catch { }
                        }
                    }
                }
            }
            if ($pendingFeedback -gt 0) {
                $automation.errors += "$pendingFeedback analisis de Codex seguian en ejecucion al cerrar."
                $result.notes += "$pendingFeedback analisis de Codex seguian en ejecucion; revisar feedback/ al terminar."
            }
            $automation.finished_at = [DateTime]::UtcNow.ToString('o')
            $executed = @($automation.timeline | Where-Object { $_.status -eq 'done' }).Count
            $failed = @($automation.timeline | Where-Object { $_.status -eq 'failed' }).Count
            $skipped = @($automation.timeline | Where-Object { $_.status -eq 'skipped' }).Count
            $doneCaptures = @($automation.captures | Where-Object { -not $_.error }).Count
            $automation.summary = [ordered]@{
                steps_executed   = $executed
                steps_failed     = $failed
                steps_skipped    = $skipped
                captures         = $doneCaptures
                captures_skipped = [int]$automation.counters.skipped_captures
                feedback_calls   = @($automation.feedback).Count
                focus_attempts   = [int]$automation.focus.attempts
                focus_failures   = [int]$automation.focus.failures
            }
            try {
                Write-JsonFile -Path $automationPath -Object $automation
                $result.notes += "Automatizacion: $executed pasos ejecutados, $doneCaptures capturas, $skipped pasos saltados."
                if ($failed -gt 0) { $result.notes += "$failed pasos fallaron al enviar la tecla." }
            } catch {
                $result.errors += "automation.json: $($_.Exception.Message)"
            }
        }

        $result.ended_at = [DateTime]::UtcNow.ToString('o')
        Write-JsonFile -Path (Join-Path $evidenceDir 'result.json') -Object ([pscustomobject]$result)
    }

    return [pscustomobject]$result
}

function Write-BaselineSummary {
    param($Result)
    Write-Host ''
    Write-Host "Evidencia : $($Result.evidence_dir)"
    Write-Host "Estado    : $($Result.status)"
    if ($null -ne $Result.duration_sec) {
        Write-Host "Duracion  : $($Result.duration_sec) s (solicitado: $($Result.requested_duration_sec) s)"
    }
    $fps = $Result.metrics.fps
    if ($null -ne $fps.median) {
        Write-Host "FPS       : mediana $($fps.median), p95 $($fps.p95) ($($fps.samples) ventanas)"
    } else {
        Write-Host 'FPS       : sin datos (se necesita TLG_FRAME_STATS=1 y frames presentados)'
    }
    $memory = $Result.metrics.memory
    if ($null -ne $memory.vram_max_mb) {
        Write-Host "VRAM max  : $($memory.vram_max_mb) MB"
    }
    if ($null -ne $memory.ram_max_mb) {
        Write-Host "RAM max   : $($memory.ram_max_mb) MB (working set)"
    }
    if ($Result.errors.Count -gt 0) {
        Write-Host "Errores   : $($Result.errors -join ' | ')"
    }
    Write-Host "Resultado : $(Join-Path $Result.evidence_dir 'result.json')"
}

if ($MyInvocation.InvocationName -ne '.') {
    $result = $null
    try {
        $result = Invoke-Baseline
        Write-BaselineSummary -Result $result
    } catch {
        Write-Error $_
        exit 1
    }

    switch ($result.status) {
        'completed'       { exit 0 }
        'early_exit'      { exit 2 }
        'startup_timeout' { exit 3 }
        'interrupted'     { exit 130 }
        default           { exit 1 }
    }
}
