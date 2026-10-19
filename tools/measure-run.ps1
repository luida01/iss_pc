#Requires -Version 7.0
<#
.SYNOPSIS
    Muestrea GPU y proceso del port una vez por segundo y escribe un CSV.

.DESCRIPTION
    Fuentes:
      - nvidia-smi (opcional): utilization.gpu, memory.used, memory.total,
        temperature.gpu, power.draw y clocks.sm de la GPU 0.
      - Proceso objetivo: CPU (normalizada por nucleos logicos), WorkingSet64 y
        PrivateMemorySize64.

    Se puede dot-source para usar Start-Sampler/Stop-Sampler desde run-baseline.ps1:
        . .\tools\measure-run.ps1
        $ctx = Start-Sampler -TargetProcessId 1234 -CsvPath .\samples.csv
        Stop-Sampler -Context $ctx

    O invocar directamente (muestrea hasta que el proceso termine o Ctrl+C):
        pwsh -NoProfile -File .\tools\measure-run.ps1 `
            -ProcessName tlg-probe -OutputCsv .\samples.csv -IntervalSec 1

    Si nvidia-smi no existe o falla, las columnas GPU quedan vacias y el muestreo
    del proceso continua. No hay dependencias externas.

.PARAMETER ProcessId
    PID del proceso a medir. Tiene prioridad sobre -ProcessName.

.PARAMETER ProcessName
    Nombre del proceso cuando no se indica -ProcessId (por defecto tlg-probe).

.PARAMETER OutputCsv
    Ruta del CSV de salida. Start-Sampler crea la carpeta si no existe.

.PARAMETER IntervalSec
    Intervalo entre muestras, en segundos (por defecto 1).

.PARAMETER MaxSeconds
    Limite opcional de duracion del muestreo. 0 = sin limite.

.PARAMETER StopFile
    Si existe este archivo, el muestreo termina de forma ordenada. Lo usa
    Stop-Sampler; tambien sirve para parar una invocacion directa.

.PARAMETER Quiet
    No repite el aviso cuando nvidia-smi no esta disponible.

.EXAMPLE
    pwsh -NoProfile -File .\tools\measure-run.ps1 -ProcessName tlg-probe -OutputCsv .\samples.csv
#>
[CmdletBinding()]
param(
    [int]$ProcessId = 0,
    [string]$ProcessName = 'tlg-probe',
    [string]$OutputCsv,
    [double]$IntervalSec = 1.0,
    [int]$MaxSeconds = 0,
    [string]$StopFile,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

# Guardado al cargar el script: al hacer dot-source, $PSCommandPath dentro de una
# funcion puede apuntar al script llamador (run-baseline.ps1), no a este archivo.
$script:MeasureRunScriptPath = $PSCommandPath

$script:SamplerQuiet = [bool]$Quiet
$script:NvidiaSmiChecked = $false
$script:NvidiaSmiPath = $null
$script:NvidiaSmiWarned = $false

$script:CsvHeader = 'timestamp_utc,elapsed_sec,pid,process_name,cpu_percent,' +
    'working_set_mb,private_mb,gpu_util_percent,vram_used_mb,vram_total_mb,' +
    'gpu_temp_c,gpu_power_w,gpu_sm_clock_mhz'

function ConvertTo-InvariantString {
    param(
        $Value,
        [string]$Format = '0.###'
    )
    if ($null -eq $Value) { return '' }
    if ($Value -is [bool]) { return $(if ($Value) { '1' } else { '0' }) }
    if ($Value -is [datetime]) {
        return ([datetime]$Value).ToUniversalTime().ToString(
            'o', [Globalization.CultureInfo]::InvariantCulture)
    }
    try {
        return ([double]$Value).ToString($Format, [Globalization.CultureInfo]::InvariantCulture)
    } catch {
        return [string]$Value
    }
}

function ConvertFrom-SmiNumber {
    param([string]$Text)
    $value = "$Text".Trim()
    if (-not $value) { return $null }
    if ($value -match '^\[?N/?A\]?$' -or $value -match '(?i)not supported') { return $null }
    $value = $value -replace ',', ''
    $parsed = 0.0
    if ([double]::TryParse($value, [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) {
        return $parsed
    }
    return $null
}

function Get-NvidiaSmiPath {
    if ($script:NvidiaSmiChecked) { return $script:NvidiaSmiPath }
    $script:NvidiaSmiChecked = $true
    $command = Get-Command nvidia-smi -ErrorAction SilentlyContinue
    if ($command -and $command.Source) {
        $script:NvidiaSmiPath = $command.Source
    }
    return $script:NvidiaSmiPath
}

function Get-NvidiaSample {
    $smi = Get-NvidiaSmiPath
    if (-not $smi) {
        if (-not $script:SamplerQuiet -and -not $script:NvidiaSmiWarned) {
            Write-Warning 'nvidia-smi no esta disponible: las columnas GPU quedaran vacias.'
            $script:NvidiaSmiWarned = $true
        }
        return $null
    }

    try {
        $lines = & $smi '--id=0' '--query-gpu=utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw,clocks.sm' '--format=csv,noheader,nounits' 2>$null
        if ($LASTEXITCODE -ne 0) {
            throw "nvidia-smi termino con codigo $LASTEXITCODE"
        }
        $line = @($lines | Where-Object { "$_".Trim() } | Select-Object -First 1)
        if (-not $line) { throw 'nvidia-smi no devolvio ninguna fila' }
        $parts = @($line[0] -split ',')
        if ($parts.Count -lt 6) { throw 'nvidia-smi devolvio menos columnas de las esperadas' }
        return [ordered]@{
            gpu_util_percent  = ConvertFrom-SmiNumber $parts[0]
            vram_used_mb      = ConvertFrom-SmiNumber $parts[1]
            vram_total_mb     = ConvertFrom-SmiNumber $parts[2]
            gpu_temp_c        = ConvertFrom-SmiNumber $parts[3]
            gpu_power_w       = ConvertFrom-SmiNumber $parts[4]
            gpu_sm_clock_mhz  = ConvertFrom-SmiNumber $parts[5]
        }
    } catch {
        if (-not $script:SamplerQuiet -and -not $script:NvidiaSmiWarned) {
            Write-Warning "nvidia-smi fallo ($($_.Exception.Message)): las columnas GPU quedaran vacias."
            $script:NvidiaSmiWarned = $true
        }
        return $null
    }
}

function Get-TargetProcess {
    param(
        [int]$TargetProcessId,
        [string]$TargetProcessName
    )

    if ($TargetProcessId -gt 0) {
        try {
            return [Diagnostics.Process]::GetProcessById($TargetProcessId)
        } catch {
            return $null
        }
    }

    try {
        return Get-Process -Name $TargetProcessName -ErrorAction Stop |
            Sort-Object -Property StartTime -Descending |
            Select-Object -First 1
    } catch {
        return $null
    }
}

function ConvertTo-CsvField {
    param([string]$Text)
    if ($null -eq $Text) { return '' }
    if ($Text -match '[,"\r\n]') {
        return '"' + ($Text -replace '"', '""') + '"'
    }
    return $Text
}

function Invoke-MeasureRun {
    [CmdletBinding()]
    param(
        [int]$TargetProcessId = 0,
        [string]$TargetProcessName = 'tlg-probe',
        [Parameter(Mandatory)][string]$CsvPath,
        [double]$SampleIntervalSec = 1.0,
        [int]$LimitSeconds = 0,
        [string]$StopFile,
        [switch]$Quiet
    )

    if ($SampleIntervalSec -le 0) { throw 'SampleIntervalSec debe ser mayor que 0.' }
    if ($TargetProcessId -le 0 -and -not $TargetProcessName) {
        throw 'Indica TargetProcessId o TargetProcessName.'
    }

    $script:SamplerQuiet = [bool]$Quiet
    $CsvPath = [IO.Path]::GetFullPath($CsvPath)
    $csvDir = Split-Path -Parent $CsvPath
    if ($csvDir) { New-Item -ItemType Directory -Force -Path $csvDir | Out-Null }

    $cores = [Math]::Max(1, [Environment]::ProcessorCount)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $writer = [IO.StreamWriter]::new(
        $CsvPath, $false, [Text.UTF8Encoding]::new($false))
    $writer.AutoFlush = $true
    $lastCpuSec = $null
    $lastElapsedSec = $null

    try {
        $writer.WriteLine($script:CsvHeader)
        $nextSampleAt = 0.0

        while ($true) {
            if ($StopFile -and (Test-Path -LiteralPath $StopFile)) { break }
            if ($LimitSeconds -gt 0 -and $clock.Elapsed.TotalSeconds -ge $LimitSeconds) { break }

            $process = Get-TargetProcess -TargetProcessId $TargetProcessId `
                -TargetProcessName $TargetProcessName
            if (-not $process) { break }

            try {
                $process.Refresh()
                if ($process.HasExited) { break }
            } catch {
                break
            }

            $processName = $null
            $cpuSec = $null
            $workingSetMb = $null
            $privateMb = $null
            try { $processName = $process.ProcessName } catch { }
            try { $cpuSec = $process.TotalProcessorTime.TotalSeconds } catch { }
            try { $workingSetMb = $process.WorkingSet64 / 1MB } catch { }
            try { $privateMb = $process.PrivateMemorySize64 / 1MB } catch { }

            $cpuPercent = $null
            if ($null -ne $cpuSec) {
                if ($null -eq $lastCpuSec) {
                    # Primera fila: CPU media desde el arranque del proceso.
                    try {
                        $uptime = ([DateTime]::Now - $process.StartTime).TotalSeconds
                        if ($uptime -gt 0) {
                            $cpuPercent = ($cpuSec / $uptime / $cores) * 100.0
                        }
                    } catch { }
                } else {
                    $deltaSec = $clock.Elapsed.TotalSeconds - $lastElapsedSec
                    if ($deltaSec -gt 0) {
                        $cpuPercent = (($cpuSec - $lastCpuSec) / $deltaSec / $cores) * 100.0
                    }
                }
                if ($null -ne $cpuPercent) {
                    $cpuPercent = [Math]::Max(0.0, [Math]::Min(100.0, $cpuPercent))
                }
                $lastCpuSec = $cpuSec
                $lastElapsedSec = $clock.Elapsed.TotalSeconds
            }

            $gpu = Get-NvidiaSample
            $fields = @(
                [DateTime]::UtcNow.ToString('o', [Globalization.CultureInfo]::InvariantCulture)
                (ConvertTo-InvariantString $clock.Elapsed.TotalSeconds '0.000')
                (ConvertTo-InvariantString $process.Id '0')
                (ConvertTo-CsvField $processName)
                (ConvertTo-InvariantString $cpuPercent '0.0')
                (ConvertTo-InvariantString $workingSetMb '0.0')
                (ConvertTo-InvariantString $privateMb '0.0')
                (ConvertTo-InvariantString $gpu.gpu_util_percent '0.0')
                (ConvertTo-InvariantString $gpu.vram_used_mb '0.0')
                (ConvertTo-InvariantString $gpu.vram_total_mb '0.0')
                (ConvertTo-InvariantString $gpu.gpu_temp_c '0.0')
                (ConvertTo-InvariantString $gpu.gpu_power_w '0.0')
                (ConvertTo-InvariantString $gpu.gpu_sm_clock_mhz '0.0')
            )
            $writer.WriteLine(($fields -join ','))

            $nextSampleAt += $SampleIntervalSec
            if ($nextSampleAt -le $clock.Elapsed.TotalSeconds) {
                # La consulta a nvidia-smi tardo mas que el intervalo: no acumular atraso.
                $nextSampleAt = $clock.Elapsed.TotalSeconds + $SampleIntervalSec
            }
            $delayMs = [int][Math]::Ceiling(($nextSampleAt - $clock.Elapsed.TotalSeconds) * 1000.0)
            if ($delayMs -gt 0) { Start-Sleep -Milliseconds $delayMs }
        }
    } finally {
        $writer.Dispose()
    }
}

function ConvertTo-ProcessArgument {
    param([string]$Value)
    if ($Value -match '[\s"]') {
        return '"' + ($Value -replace '"', '\"') + '"'
    }
    return $Value
}

function Start-Sampler {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][int]$TargetProcessId,
        [Parameter(Mandatory)][string]$CsvPath,
        [double]$SampleIntervalSec = 1.0,
        [int]$LimitSeconds = 0,
        [string]$ChildLogPath,
        [switch]$Quiet
    )

    $samplerScript = $script:MeasureRunScriptPath
    if (-not $samplerScript) { $samplerScript = $PSCommandPath }
    if (-not $samplerScript) {
        throw 'Start-Sampler necesita ejecutarse desde measure-run.ps1 (no se pudo resolver la ruta del sampler).'
    }

    $csvFull = [IO.Path]::GetFullPath($CsvPath)
    $csvDir = Split-Path -Parent $csvFull
    if ($csvDir) { New-Item -ItemType Directory -Force -Path $csvDir | Out-Null }
    $stopFile = "$csvFull.stop"
    Remove-Item -LiteralPath $stopFile -Force -ErrorAction SilentlyContinue

    $pwshPath = (Get-Process -Id $PID).Path
    if (-not $pwshPath) { throw 'No se pudo resolver la ruta de pwsh.exe.' }

    $arguments = @(
        '-NoLogo'
        '-NoProfile'
        '-NonInteractive'
        '-File'
        (ConvertTo-ProcessArgument $samplerScript)
        '-ProcessId'
        (ConvertTo-InvariantString $TargetProcessId '0')
        '-OutputCsv'
        (ConvertTo-ProcessArgument $csvFull)
        '-IntervalSec'
        (ConvertTo-InvariantString $SampleIntervalSec)
        '-StopFile'
        (ConvertTo-ProcessArgument $stopFile)
    )
    if ($LimitSeconds -gt 0) {
        $arguments += @('-MaxSeconds', (ConvertTo-InvariantString $LimitSeconds '0'))
    }
    if ($Quiet) { $arguments += '-Quiet' }

    $startArgs = @{
        FilePath     = $pwshPath
        ArgumentList = $arguments
        PassThru     = $true
        WindowStyle  = 'Hidden'
    }
    if ($ChildLogPath) {
        $logFull = [IO.Path]::GetFullPath($ChildLogPath)
        $logDir = Split-Path -Parent $logFull
        if ($logDir) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
        $startArgs.RedirectStandardOutput = $logFull
        $startArgs.RedirectStandardError = "$logFull.err"
    }

    $child = Start-Process @startArgs
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not (Test-Path -LiteralPath $csvFull) -and
           -not $child.HasExited -and
           [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $child.Refresh()
    }

    if ($child.HasExited) {
        $code = 'desconocido'
        try { $code = $child.ExitCode } catch { }
        $hint = if ($ChildLogPath) { " Ver $ChildLogPath y $ChildLogPath.err." } else { '' }
        throw "El sampler termino al arrancar (codigo $code).$hint"
    }
    if (-not (Test-Path -LiteralPath $csvFull)) {
        throw "El sampler no creo el CSV: $csvFull"
    }

    return [pscustomobject]@{
        Process      = $child
        CsvPath      = $csvFull
        StopFile     = $stopFile
        ChildLogPath = $ChildLogPath
    }
}

function Stop-Sampler {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]$Context,
        [int]$GraceSec = 5
    )

    if ($null -eq $Context) { return }
    try {
        New-Item -ItemType File -Force -Path $Context.StopFile | Out-Null
    } catch { }

    $deadline = [DateTime]::UtcNow.AddSeconds($GraceSec)
    while (-not $Context.Process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        try { $Context.Process.Refresh() } catch { }
    }

    if (-not $Context.Process.HasExited) {
        try { Stop-Process -Id $Context.Process.Id -Force -ErrorAction Stop } catch { }
        try { $Context.Process.WaitForExit(5000) | Out-Null } catch { }
    }

    Remove-Item -LiteralPath $Context.StopFile -Force -ErrorAction SilentlyContinue
}

if ($MyInvocation.InvocationName -ne '.') {
    if (-not $OutputCsv) {
        throw 'Falta -OutputCsv. Uso: measure-run.ps1 -ProcessName tlg-probe -OutputCsv <archivo.csv>'
    }
    Invoke-MeasureRun -TargetProcessId $ProcessId -TargetProcessName $ProcessName `
        -CsvPath $OutputCsv -SampleIntervalSec $IntervalSec -LimitSeconds $MaxSeconds `
        -StopFile $StopFile -Quiet:$Quiet
}
