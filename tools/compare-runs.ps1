#Requires -Version 7.0
<#
.SYNOPSIS
    Compara dos carpetas de evidencia del QA harness.

.DESCRIPTION
    Lee result.json, samples.csv y el log principal de cada carpeta y muestra una
    tabla con FPS mediana/p95 (ventanas de 5 s), frametime p99, VRAM maxima,
    RAM maxima (working set), duracion, flags y veredicto.

    Umbrales del veredicto:
      - MEJORA si la FPS mediana sube >= ImprovementPct (2% por defecto).
      - REGRESION si baja <= -ImprovementPct.
      - SIN CAMBIO dentro del umbral.
      - NO COMPARABLE si alguna sesion no termino en estado completed.
    Se anexan avisos si el p95 empeora >=5% o si la memoria sube >=10% sin
    mejora de FPS. Si las flags no son identicas, la comparacion no aisla una
    sola variable y se avisa.

    No hay dependencias externas. Los FPS se leen de las lineas
    "Frame stats: X FPS" que el port escribe cada 5 s con TLG_FRAME_STATS=1.

.PARAMETER Reference
    Carpeta de evidencia base (la referencia).

.PARAMETER Candidate
    Carpeta de evidencia candidata (el cambio).

.PARAMETER ImprovementPct
    Umbral de mejora/regresion para la FPS mediana (por defecto 2%).

.PARAMETER Markdown
    Emite una tabla Markdown en vez de la tabla de consola.

.EXAMPLE
    .\tools\compare-runs.ps1 -Reference .\evidence\20261019-120000-baseline `
        -Candidate .\evidence\20261019-130000-gc3500

.EXAMPLE
    .\tools\compare-runs.ps1 .\evidence\run-a .\evidence\run-b -Markdown > comparison.md
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)][string]$Reference,
    [Parameter(Mandatory, Position = 1)][string]$Candidate,
    [ValidateRange(0.0, 100.0)][double]$ImprovementPct = 2.0,
    [switch]$Markdown
)

$ErrorActionPreference = 'Stop'
$invariant = [Globalization.CultureInfo]::InvariantCulture

function Resolve-EvidenceDirectory {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "No existe la evidencia: $Path"
    }
    $item = Get-Item -LiteralPath $Path
    if (-not $item.PSIsContainer) {
        if ($item.Name -eq 'result.json') {
            return $item.Directory.FullName
        }
        throw "Se esperaba una carpeta de evidencia (o result.json): $Path"
    }
    return $item.FullName
}

function Get-PropertyValue {
    param($Object, [string]$Path)
    $current = $Object
    foreach ($part in $Path.Split('.')) {
        if ($null -eq $current) { return $null }
        $property = $current.PSObject.Properties[$part]
        if ($null -eq $property) { return $null }
        $current = $property.Value
    }
    return $current
}

# Un OrderedDictionary expone Count/Keys/... via PSObject, no sus claves. Hay que
# recorrer .Keys para no confundir las flags con propiedades .NET.
function Get-ObjectEntries {
    param($Object)
    if ($null -eq $Object) { return }
    if ($Object -is [System.Collections.IDictionary]) {
        foreach ($key in @($Object.Keys)) {
            [pscustomobject]@{ Name = [string]$key; Value = $Object[$key] }
        }
        return
    }
    foreach ($property in $Object.PSObject.Properties) {
        [pscustomobject]@{ Name = $property.Name; Value = $property.Value }
    }
}

function ConvertTo-DoubleInvariant {
    param([string]$Text)
    if ($null -eq $Text) { return $null }
    $value = "$Text".Trim()
    if (-not $value) { return $null }
    $value = $value.Replace(',', '.')
    $parsed = 0.0
    if ([double]::TryParse($value, [Globalization.NumberStyles]::Float,
            $invariant, [ref]$parsed)) {
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

function Find-PrimaryLog {
    param([string]$Directory, $Result)
    $recorded = Get-PropertyValue $Result 'log_file'
    if ($recorded) {
        $candidate = if ([IO.Path]::IsPathRooted([string]$recorded)) {
            [string]$recorded
        } else {
            Join-Path $Directory ([string]$recorded)
        }
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }

    foreach ($name in @('run.log', 'stdout.log', 'game.log')) {
        $candidate = Join-Path $Directory $name
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }

    $logsDir = Join-Path $Directory 'logs'
    if (Test-Path -LiteralPath $logsDir -PathType Container) {
        $log = Get-ChildItem -LiteralPath $logsDir -Filter 'run-*.log' -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($log) { return $log.FullName }
    }

    $any = Get-ChildItem -LiteralPath $Directory -Filter '*.log' -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($any) { return $any.FullName }
    return $null
}

function Find-SamplesCsv {
    param([string]$Directory, $Result)
    $recorded = Get-PropertyValue $Result 'samples_file'
    if ($recorded) {
        $candidate = if ([IO.Path]::IsPathRooted([string]$recorded)) {
            [string]$recorded
        } else {
            Join-Path $Directory ([string]$recorded)
        }
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    $candidate = Join-Path $Directory 'samples.csv'
    if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    return $null
}

function Get-LogMetrics {
    param([string]$Path)
    $metrics = [ordered]@{
        fps_median            = $null
        fps_p95               = $null
        fps_samples           = 0
        frame_pacing_median_ms = $null
        frame_pacing_p99_ms   = $null
        build                 = $null
        config_ini            = [ordered]@{}
        config_env            = [ordered]@{}
        issues                = @()
    }
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject]$metrics
    }

    $fps = [Collections.Generic.List[double]]::new()
    $medians = [Collections.Generic.List[double]]::new()
    $p99 = [Collections.Generic.List[double]]::new()
    $issues = [Collections.Generic.List[string]]::new()

    foreach ($line in [IO.File]::ReadLines($Path)) {
        if (-not $metrics.build -and $line -match '^Build:\s*(.+)$') {
            $metrics.build = $Matches[1].Trim()
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
            if ($null -ne $value -and $value -gt 0) { $fps.Add($value) }
        } elseif ($line -match '^\s*Frame pacing:\s*median\s*([0-9]+(?:[.,][0-9]+)?)\s*ms.*?\bp99\s*([0-9]+(?:[.,][0-9]+)?)\s*ms') {
            $median = ConvertTo-DoubleInvariant $Matches[1]
            $p99Value = ConvertTo-DoubleInvariant $Matches[2]
            if ($null -ne $median) { $medians.Add($median) }
            if ($null -ne $p99Value) { $p99.Add($p99Value) }
        }
        if ($issues.Count -lt 10 -and $line -match '^(Fault|STOP|FREEZE):') {
            $issues.Add($line.Trim())
        }
    }

    if ($fps.Count -gt 0) {
        $metrics.fps_median = [Math]::Round((Get-Median $fps.ToArray()), 1)
        $metrics.fps_p95 = [Math]::Round((Get-Percentile $fps.ToArray() 95), 1)
        $metrics.fps_samples = $fps.Count
    }
    if ($medians.Count -gt 0) {
        $metrics.frame_pacing_median_ms = [Math]::Round((Get-Median $medians.ToArray()), 2)
    }
    if ($p99.Count -gt 0) {
        $metrics.frame_pacing_p99_ms = [Math]::Round((Get-Median $p99.ToArray()), 2)
    }
    $metrics.issues = $issues.ToArray()
    return [pscustomobject]$metrics
}

function Get-NumericColumn {
    param($Rows, [string]$Name)
    $values = [Collections.Generic.List[double]]::new()
    foreach ($row in $Rows) {
        $raw = $row.$Name
        $parsed = ConvertTo-DoubleInvariant $raw
        if ($null -ne $parsed) { $values.Add($parsed) }
    }
    return $values.ToArray()
}

function Get-CsvMetrics {
    param([string]$Path)
    $metrics = [ordered]@{
        sample_count       = 0
        sample_span_sec    = $null
        vram_max_mb        = $null
        ram_max_mb         = $null
        private_max_mb     = $null
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
    $vram = Get-NumericColumn $rows 'vram_used_mb'
    if ($vram.Count -gt 0) {
        $metrics.vram_max_mb = [Math]::Round(($vram | Measure-Object -Maximum).Maximum, 0)
    }
    $ram = Get-NumericColumn $rows 'working_set_mb'
    if ($ram.Count -gt 0) {
        $metrics.ram_max_mb = [Math]::Round(($ram | Measure-Object -Maximum).Maximum, 0)
    }
    $private = Get-NumericColumn $rows 'private_mb'
    if ($private.Count -gt 0) {
        $metrics.private_max_mb = [Math]::Round(($private | Measure-Object -Maximum).Maximum, 0)
    }
    return [pscustomobject]$metrics
}

function Get-FlagMap {
    param($Flags)
    $map = @{}
    if ($null -eq $Flags) { return $map }
    foreach ($property in $Flags.PSObject.Properties) {
        $map[$property.Name] = [string]$property.Value
    }
    return $map
}

function Get-EffectiveFlags {
    param($LogEnvironment, $Configured)
    $merged = [ordered]@{}
    foreach ($entry in @(Get-ObjectEntries $LogEnvironment)) {
        $merged[$entry.Name] = [string]$entry.Value
    }
    foreach ($entry in @(Get-ObjectEntries $Configured)) {
        if (-not $merged.Contains($entry.Name)) {
            $merged[$entry.Name] = [string]$entry.Value
        }
    }
    if ($merged.Keys.Count -eq 0) { return $null }
    return [pscustomobject]$merged
}

function Format-Flags {
    param($Flags)
    if ($null -eq $Flags) { return '(no registradas)' }
    $parts = [Collections.Generic.List[string]]::new()
    foreach ($property in $Flags.PSObject.Properties) {
        $parts.Add("$($property.Name)=$($property.Value)")
    }
    if ($parts.Count -eq 0) { return '(ninguna)' }
    return ($parts -join '; ')
}

function Format-Number {
    param($Value, [string]$Format = '0.0', [string]$Suffix = '')
    if ($null -eq $Value) { return 'n/d' }
    return ([double]$Value).ToString($Format, $invariant) + $Suffix
}

function Format-PercentDelta {
    param($ReferenceValue, $CandidateValue)
    if ($null -eq $ReferenceValue -or $null -eq $CandidateValue) { return 'n/d' }
    $referenceNumber = [double]$ReferenceValue
    if ($referenceNumber -eq 0) { return 'n/d' }
    $delta = (([double]$CandidateValue - $referenceNumber) / $referenceNumber) * 100.0
    return [string]::Format($invariant, '{0:+0.0;-0.0;0.0}%', $delta)
}

function Get-PercentChange {
    param($ReferenceValue, $CandidateValue)
    if ($null -eq $ReferenceValue -or $null -eq $CandidateValue) { return $null }
    $referenceNumber = [double]$ReferenceValue
    if ($referenceNumber -eq 0) { return $null }
    return (([double]$CandidateValue - $referenceNumber) / $referenceNumber) * 100.0
}

function Get-RunSummary {
    param([string]$Directory)
    $directory = Resolve-EvidenceDirectory $Directory
    $result = $null
    $resultPath = Join-Path $directory 'result.json'
    if (Test-Path -LiteralPath $resultPath -PathType Leaf) {
        try {
            $result = Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
        } catch {
            Write-Warning "No se pudo leer $resultPath : $($_.Exception.Message)"
        }
    }

    $logPath = Find-PrimaryLog -Directory $directory -Result $result
    $logMetrics = Get-LogMetrics $logPath
    $csvPath = Find-SamplesCsv -Directory $directory -Result $result
    $csvMetrics = Get-CsvMetrics $csvPath

    $fpsMedian = Get-PropertyValue $result 'metrics.fps.median'
    $fpsP95 = Get-PropertyValue $result 'metrics.fps.p95'
    $fpsSamples = Get-PropertyValue $result 'metrics.fps.samples'
    if ($null -eq $fpsMedian -and $logMetrics.fps_samples -gt 0) {
        $fpsMedian = $logMetrics.fps_median
        $fpsP95 = $logMetrics.fps_p95
        $fpsSamples = $logMetrics.fps_samples
    }

    $frameP99 = Get-PropertyValue $result 'metrics.frame_pacing.p99_ms'
    if ($null -eq $frameP99) { $frameP99 = $logMetrics.frame_pacing_p99_ms }

    $vramMax = Get-PropertyValue $result 'metrics.memory.vram_max_mb'
    if ($null -eq $vramMax) { $vramMax = $csvMetrics.vram_max_mb }
    $ramMax = Get-PropertyValue $result 'metrics.memory.ram_max_mb'
    if ($null -eq $ramMax) { $ramMax = $csvMetrics.ram_max_mb }
    $privateMax = Get-PropertyValue $result 'metrics.memory.private_max_mb'
    if ($null -eq $privateMax) { $privateMax = $csvMetrics.private_max_mb }

    $duration = Get-PropertyValue $result 'duration_sec'
    if ($null -eq $duration) { $duration = $csvMetrics.sample_span_sec }

    $label = Get-PropertyValue $result 'tag'
    if (-not $label) { $label = Split-Path -Leaf $directory }
    $status = Get-PropertyValue $result 'status'
    if (-not $status) { $status = 'sin result.json' }

    $configuredFlags = Get-PropertyValue $result 'flags'
    $logFlags = Get-PropertyValue $result 'log_config.env'
    if (-not $logFlags) { $logFlags = $logMetrics.config_env }
    $flags = Get-EffectiveFlags $logFlags $configuredFlags
    $build = Get-PropertyValue $result 'build'
    if (-not $build) { $build = $logMetrics.build }

    return [pscustomobject]@{
        Directory    = $directory
        Label        = [string]$label
        Status       = [string]$status
        FpsMedian    = $fpsMedian
        FpsP95       = $fpsP95
        FpsSamples   = $fpsSamples
        FrameP99     = $frameP99
        VramMax      = $vramMax
        RamMax       = $ramMax
        PrivateMax   = $privateMax
        Duration     = $duration
        CsvSamples   = $csvMetrics.sample_count
        Flags        = $flags
        FlagsText    = (Format-Flags $flags)
        FlagMap      = (Get-FlagMap $flags)
        Build        = $build
        LogPath      = $logPath
        CsvPath      = $csvPath
        Issues       = @($logMetrics.issues)
    }
}

function Get-ChangedFlags {
    param($ReferenceRun, $CandidateRun)
    $keys = @($ReferenceRun.FlagMap.Keys + $CandidateRun.FlagMap.Keys | Sort-Object -Unique)
    $changed = [Collections.Generic.List[string]]::new()
    foreach ($key in $keys) {
        $referenceValue = $ReferenceRun.FlagMap[$key]
        $candidateValue = $CandidateRun.FlagMap[$key]
        if ($referenceValue -ne $candidateValue) {
            $changed.Add("$key=$referenceValue -> $candidateValue")
        }
    }
    return $changed.ToArray()
}

function Get-Verdict {
    param($ReferenceRun, $CandidateRun, [double]$Threshold)
    if ($ReferenceRun.Status -ne 'completed' -or $CandidateRun.Status -ne 'completed') {
        return "NO COMPARABLE ($($ReferenceRun.Status) vs $($CandidateRun.Status))"
    }
    if ($null -eq $ReferenceRun.FpsMedian -or $null -eq $CandidateRun.FpsMedian) {
        return 'SIN DATOS DE FPS'
    }

    $fpsDelta = Get-PercentChange $ReferenceRun.FpsMedian $CandidateRun.FpsMedian
    $fpsText = [string]::Format($invariant, '{0:+0.0;-0.0;0.0}%', $fpsDelta)
    if ($fpsDelta -ge $Threshold) {
        $verdict = "MEJORA ($fpsText)"
    } elseif ($fpsDelta -le -$Threshold) {
        $verdict = "REGRESION ($fpsText)"
    } else {
        $verdict = "SIN CAMBIO ($fpsText)"
    }

    $notes = [Collections.Generic.List[string]]::new()
    $p95Delta = Get-PercentChange $ReferenceRun.FpsP95 $CandidateRun.FpsP95
    if ($null -ne $p95Delta -and $p95Delta -le -5.0) {
        $notes.Add(('p95 {0:+0.0;-0.0}%' -f $p95Delta))
    }

    $memoryDeltas = @(@(
        (Get-PercentChange $ReferenceRun.VramMax $CandidateRun.VramMax),
        (Get-PercentChange $ReferenceRun.RamMax $CandidateRun.RamMax)
    ) | Where-Object { $null -ne $_ })
    if ($memoryDeltas.Count -gt 0) {
        $memoryDelta = ($memoryDeltas | Measure-Object -Maximum).Maximum
        if ($memoryDelta -ge 10.0 -and $fpsDelta -lt $Threshold) {
            $notes.Add(('memoria {0:+0.0;-0.0}%' -f $memoryDelta))
        }
    }

    if ($notes.Count -gt 0) { $verdict += ' | ' + ($notes -join '; ') }
    return $verdict
}

function Escape-Markdown {
    param([string]$Text)
    if ($null -eq $Text) { return '' }
    return ($Text -replace '\|', '\|' -replace "`r?`n", ' ')
}

$referenceRun = Get-RunSummary $Reference
$candidateRun = Get-RunSummary $Candidate
$verdict = Get-Verdict $referenceRun $candidateRun $ImprovementPct
$changedFlags = @(Get-ChangedFlags $referenceRun $candidateRun)

$rows = [Collections.Generic.List[object]]::new()
$rows.Add([pscustomobject]@{
    Metrica    = 'Estado'
    Referencia = $referenceRun.Status
    Candidato  = $candidateRun.Status
    Delta      = ''
})
$rows.Add([pscustomobject]@{
    Metrica    = 'FPS mediana (ventanas 5 s)'
    Referencia = (Format-Number $referenceRun.FpsMedian '0.0')
    Candidato  = (Format-Number $candidateRun.FpsMedian '0.0')
    Delta      = (Format-PercentDelta $referenceRun.FpsMedian $candidateRun.FpsMedian)
})
$rows.Add([pscustomobject]@{
    Metrica    = 'FPS p95 (ventanas 5 s)'
    Referencia = (Format-Number $referenceRun.FpsP95 '0.0')
    Candidato  = (Format-Number $candidateRun.FpsP95 '0.0')
    Delta      = (Format-PercentDelta $referenceRun.FpsP95 $candidateRun.FpsP95)
})
$rows.Add([pscustomobject]@{
    Metrica    = 'Frametime p99 (ms, mediana de ventanas)'
    Referencia = (Format-Number $referenceRun.FrameP99 '0.00')
    Candidato  = (Format-Number $candidateRun.FrameP99 '0.00')
    Delta      = (Format-PercentDelta $referenceRun.FrameP99 $candidateRun.FrameP99)
})
$rows.Add([pscustomobject]@{
    Metrica    = 'VRAM maxima (MB)'
    Referencia = (Format-Number $referenceRun.VramMax '0')
    Candidato  = (Format-Number $candidateRun.VramMax '0')
    Delta      = (Format-PercentDelta $referenceRun.VramMax $candidateRun.VramMax)
})
$rows.Add([pscustomobject]@{
    Metrica    = 'RAM maxima (MB, working set)'
    Referencia = (Format-Number $referenceRun.RamMax '0')
    Candidato  = (Format-Number $candidateRun.RamMax '0')
    Delta      = (Format-PercentDelta $referenceRun.RamMax $candidateRun.RamMax)
})
$rows.Add([pscustomobject]@{
    Metrica    = 'RAM privada maxima (MB)'
    Referencia = (Format-Number $referenceRun.PrivateMax '0')
    Candidato  = (Format-Number $candidateRun.PrivateMax '0')
    Delta      = (Format-PercentDelta $referenceRun.PrivateMax $candidateRun.PrivateMax)
})
$rows.Add([pscustomobject]@{
    Metrica    = 'Duracion (s)'
    Referencia = (Format-Number $referenceRun.Duration '0.0')
    Candidato  = (Format-Number $candidateRun.Duration '0.0')
    Delta      = (Format-PercentDelta $referenceRun.Duration $candidateRun.Duration)
})
$rows.Add([pscustomobject]@{
    Metrica    = 'Muestras CSV'
    Referencia = (Format-Number $referenceRun.CsvSamples '0')
    Candidato  = (Format-Number $candidateRun.CsvSamples '0')
    Delta      = ''
})
$rows.Add([pscustomobject]@{
    Metrica    = 'Flags'
    Referencia = $referenceRun.FlagsText
    Candidato  = $candidateRun.FlagsText
    Delta      = $(if ($changedFlags.Count -eq 0) { 'iguales' } else { 'distintas' })
})
$rows.Add([pscustomobject]@{
    Metrica    = 'Veredicto'
    Referencia = ''
    Candidato  = $verdict
    Delta      = ''
})

if ($Markdown) {
    Write-Output "Referencia: ``$($referenceRun.Directory)`` ($($referenceRun.Label))"
    Write-Output "Candidato: ``$($candidateRun.Directory)`` ($($candidateRun.Label))"
    Write-Output ''
    Write-Output '| Metrica | Referencia | Candidato | Delta |'
    Write-Output '|---|---|---|---|'
    foreach ($row in $rows) {
        Write-Output ("| {0} | {1} | {2} | {3} |" -f
            (Escape-Markdown $row.Metrica),
            (Escape-Markdown $row.Referencia),
            (Escape-Markdown $row.Candidato),
            (Escape-Markdown $row.Delta))
    }
    if ($changedFlags.Count -gt 0) {
        Write-Output ''
        Write-Output "> Aviso: las flags no son identicas. La comparacion mezcla variables:"
        foreach ($change in $changedFlags) {
            Write-Output "> - $change"
        }
    }
    $refBuild = $referenceRun.Build
    $candBuild = $candidateRun.Build
    if ($refBuild -or $candBuild) {
        Write-Output ''
        Write-Output "Build referencia: $refBuild"
        Write-Output "Build candidato : $candBuild"
    }
    exit 0
}

Write-Host ''
Write-Host "Referencia: $($referenceRun.Directory)  [$($referenceRun.Label), $($referenceRun.Status)]"
Write-Host "Candidato : $($candidateRun.Directory)  [$($candidateRun.Label), $($candidateRun.Status)]"
Write-Host ''
$rows | Format-Table -AutoSize | Out-String -Width 4096 | Write-Host

if ($changedFlags.Count -gt 0) {
    Write-Warning 'Las flags no son identicas; la comparacion no aisla una sola variable:'
    foreach ($change in $changedFlags) {
        Write-Host "  - $change"
    }
}
if ($referenceRun.Build -or $candidateRun.Build) {
    Write-Host "Build referencia: $($referenceRun.Build)"
    Write-Host "Build candidato : $($candidateRun.Build)"
}
exit 0
