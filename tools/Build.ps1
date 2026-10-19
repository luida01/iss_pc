#Requires -Version 5.1
<#
.SYNOPSIS
    Compila inFAMOUS Second Son para Windows con MSYS2 CLANG64 y build.sh.

.DESCRIPTION
    Invoca C:\msys64\usr\bin\bash.exe -lc "cd /c/.../port && bash build.sh",
    guarda toda la salida en evidence/build-<timestamp>.log, comprueba los
    ejecutables esperados y devuelve un codigo de salida util para CI/scripts.

    -Clean borra solo los artefactos de build dentro de port\out (out\gpu,
    out\gpu-build.log y los tres .exe). Conserva out\logs, boot*.bin y los
    datos generados por run.py; no toca src, gpu, dist, user ni pgo.
    -Tests compila y ejecuta, despues del build, el subconjunto de tests C que
    gpu/CMakeLists.txt define para WIN32. No usa "build.sh --test": en Windows
    build.sh termina antes de llegar al bloque de tests.

.PARAMETER Clean
    Borra out\gpu (cache de CMake/Ninja y tests), out\gpu-build.log y los tres
    ejecutables de out\, forzando una recompilacion completa. Conserva el resto
    de port\out (boot*.bin, logs, datos de run.py).

.PARAMETER Tests
    Compila y ejecuta los tests C disponibles en Windows. Si alguno falla,
    el script devuelve 1.

.PARAMETER Msys2Root
    Ruta de la instalacion de MSYS2. Por defecto C:\msys64.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Build.ps1

.EXAMPLE
    pwsh -NoProfile -File .\tools\Build.ps1 -Clean -Tests
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Tests,
    [string]$Msys2Root = 'C:\msys64'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Find-RepoRoot {
    $candidate = [System.IO.Path]::GetFullPath($PSScriptRoot)
    while (-not [string]::IsNullOrEmpty($candidate)) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'port\build.sh') -PathType Leaf) {
            return $candidate
        }
        $parent = [System.IO.Path]::GetDirectoryName($candidate)
        if ([string]::IsNullOrEmpty($parent) -or $parent -eq $candidate) {
            break
        }
        $candidate = $parent
    }
    throw "No se encontro la raiz del repositorio (port\build.sh) desde $PSScriptRoot"
}

$script:RepoRoot = Find-RepoRoot
$script:PortDir = Join-Path $script:RepoRoot 'port'
$script:BashPath = Join-Path $Msys2Root 'usr\bin\bash.exe'
$script:LogWriter = $null
$LogPath = $null
$exitCode = 0

function ConvertTo-MsysPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    # Acepta una ruta MSYS ya convertida (/c/...) sin tocarla.
    if ($Path -match '^/[A-Za-z]/') {
        return ($Path -replace '/+$', '')
    }

    $full = [System.IO.Path]::GetFullPath($Path)
    if ($full -match '^([A-Za-z]):\\(.*)$') {
        $drive = $Matches[1].ToLowerInvariant()
        $tail = ($Matches[2] -replace '\\', '/').TrimEnd('/')
        if ([string]::IsNullOrEmpty($tail)) {
            return "/$drive"
        }
        return "/$drive/$tail"
    }
    if ($full -match '^\\\\') {
        return '//' + (($full.TrimStart('\') -replace '\\', '/').TrimEnd('/'))
    }
    return ($full -replace '\\', '/').TrimEnd('/')
}

function ConvertTo-BashLiteral {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value
    )
    return "'" + ($Value -replace "'", "'\''") + "'"
}

function Write-BuildLog {
    param(
        [Parameter(Position = 0)]
        [string]$Message = '',
        [Parameter(Position = 1)]
        [ConsoleColor]$Color = [ConsoleColor]::Gray
    )

    if ($null -ne $script:LogWriter) {
        $script:LogWriter.WriteLine($Message)
    }
    Write-Host $Message -ForegroundColor $Color
}

function Invoke-BashLogged {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command
    )

    # stderr de un ejecutable nativo se convierte en ErrorRecord al redirigirlo.
    # Con ErrorActionPreference=Stop eso abortaria el script ante cualquier aviso
    # de bash/clang; aqui se registra y se sigue hasta capturar el exit code.
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $script:BashPath -lc $Command 2>&1 | ForEach-Object {
            if ($_ -is [System.Management.Automation.ErrorRecord]) {
                Write-BuildLog ($_.ToString())
            }
            else {
                Write-BuildLog ([string]$_)
            }
        }
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorAction
    }

    if ($null -eq $code) {
        return 1
    }
    return [int]$code
}

function Assert-NoGameRunning {
    $running = @(Get-Process -Name 'tlg-probe', 'tlg-play' -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        $names = ($running | ForEach-Object { $_.ProcessName } | Sort-Object -Unique) -join ', '
        throw "Cierra el juego antes de compilar (procesos: $names)."
    }
}

function Remove-BuildOutput {
    $outDir = Join-Path $script:PortDir 'out'
    $fullOut = [System.IO.Path]::GetFullPath($outDir)
    $fullPort = [System.IO.Path]::GetFullPath($script:PortDir).TrimEnd('\')
    $expectedPrefix = $fullPort + '\'

    # Defensa contra un -Clean mal parametrizado: solo se permite port\out.
    if ($fullOut -ne (Join-Path $fullPort 'out')) {
        throw "Ruta de limpieza inesperada: $fullOut"
    }
    if (-not $fullOut.StartsWith($expectedPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "La ruta de limpieza no esta dentro de port: $fullOut"
    }

    # Limpieza dirigida: NO borrar out\logs, boot*.bin ni los archivos que
    # run.py genera y reutiliza. Solo lo que produce build.sh/CMake.
    $targets = @(
        'gpu',
        'gpu-build.log',
        'tlg-probe.exe',
        'tlg-gpu-capabilities.exe',
        'tlg-play.exe'
    )
    $outPrefix = $fullOut.TrimEnd('\') + '\'
    $removed = 0

    foreach ($name in $targets) {
        $path = Join-Path $outDir $name
        $fullPath = [System.IO.Path]::GetFullPath($path)
        if (-not $fullPath.StartsWith($outPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Ruta de limpieza inesperada: $fullPath"
        }
        if (-not (Test-Path -LiteralPath $path)) {
            continue
        }

        Write-BuildLog "Clean: eliminando $path" ([ConsoleColor]::Yellow)
        try {
            Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop
        }
        catch {
            Write-BuildLog ("Remove-Item fallo: " + $_.Exception.Message) ([ConsoleColor]::Yellow)
            if (Test-Path -LiteralPath $path -PathType Container) {
                Write-BuildLog "Clean: reintentando con cmd /c rd /s /q" ([ConsoleColor]::Yellow)
                & $env:ComSpec /d /c ('rd /s /q "' + $path + '"') | Out-Null
            }
        }

        if (Test-Path -LiteralPath $path) {
            throw "No se pudo eliminar $path (archivo bloqueado por el juego, antivirus o una shell)."
        }
        $removed++
    }

    if ($removed -eq 0) {
        Write-BuildLog "Clean: no habia artefactos de build en $outDir."
    }
}

function Test-BuildArtifacts {
    $names = @('tlg-probe.exe', 'tlg-gpu-capabilities.exe', 'tlg-play.exe')
    $ok = $true

    foreach ($name in $names) {
        $path = Join-Path (Join-Path $script:PortDir 'out') $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            Write-BuildLog "FALTA: $path" ([ConsoleColor]::Red)
            $ok = $false
            continue
        }

        $item = Get-Item -LiteralPath $path
        if ($item.Length -le 0) {
            Write-BuildLog "FALTA (0 bytes): $path" ([ConsoleColor]::Red)
            $ok = $false
            continue
        }

        Write-BuildLog ("OK  {0,-30} {1,12:N0} bytes  {2:yyyy-MM-dd HH:mm:ss}" -f `
            $name, $item.Length, $item.LastWriteTime)
    }

    return $ok
}

function Invoke-WindowsTests {
    # Tests definidos explicitamente para WIN32 en gpu/CMakeLists.txt.
    # No incluye tests que necesiten GPU, driver Vulkan, juego o fixtures no
    # disponibles. Los argumentos son rutas relativas a port/.
    $testSpecs = @(
        [pscustomobject]@{ Name = 'mp3-test'; Arguments = @('tests/data/sine.mp3') },
        [pscustomobject]@{ Name = 'videodec-test'; Arguments = @('tests/data/tiny.h264') },
        [pscustomobject]@{ Name = 'services-test'; Arguments = @() },
        [pscustomobject]@{ Name = 'pad-map-test'; Arguments = @() },
        [pscustomobject]@{ Name = 'mouse-stick-test'; Arguments = @() },
        [pscustomobject]@{ Name = 'audio-downmix-test'; Arguments = @() },
        [pscustomobject]@{ Name = 'cpu-check-test'; Arguments = @() },
        [pscustomobject]@{ Name = 'isa-emulate-test'; Arguments = @() },
        [pscustomobject]@{ Name = 'isa-stub-test'; Arguments = @('tests/data/isa_fixture.bin') },
        [pscustomobject]@{ Name = 'isa-sse4a-test'; Arguments = @() },
        [pscustomobject]@{ Name = 'isa-f16c-test'; Arguments = @() },
        [pscustomobject]@{ Name = 'rwlock-rp-test'; Arguments = @() },
        [pscustomobject]@{ Name = 'tsc-clock-test'; Arguments = @() }
    )

    $targets = ($testSpecs | ForEach-Object { $_.Name }) -join ' '
    $buildTestsCommand = "cd " + (ConvertTo-BashLiteral $script:PosixPort) +
        " && source ./msys2-env.sh && ninja -C out/gpu " + $targets

    Write-BuildLog ''
    Write-BuildLog '=== Tests C para Windows (targets WIN32 de gpu/CMakeLists.txt) ===' ([ConsoleColor]::Cyan)
    Write-BuildLog "Comando: $buildTestsCommand"
    $buildTestsExit = Invoke-BashLogged $buildTestsCommand
    if ($buildTestsExit -ne 0) {
        Write-BuildLog "ERROR: no se pudieron compilar los tests (codigo $buildTestsExit)." ([ConsoleColor]::Red)
        return 1
    }

    $failed = @()
    foreach ($spec in $testSpecs) {
        $runCommand = "cd " + (ConvertTo-BashLiteral $script:PosixPort) +
            " && source ./msys2-env.sh && out/gpu/" + $spec.Name
        foreach ($arg in @($spec.Arguments)) {
            $runCommand += ' ' + (ConvertTo-BashLiteral $arg)
        }

        Write-BuildLog ''
        Write-BuildLog "--- $($spec.Name) ---" ([ConsoleColor]::Cyan)
        $testExit = Invoke-BashLogged $runCommand
        if ($testExit -ne 0) {
            Write-BuildLog "FALLO: $($spec.Name) (codigo $testExit)" ([ConsoleColor]::Red)
            $failed += $spec.Name
        }
        else {
            Write-BuildLog "PASS: $($spec.Name)" ([ConsoleColor]::Green)
        }
    }

    if ($failed.Count -gt 0) {
        Write-BuildLog ("Tests fallidos: " + ($failed -join ', ')) ([ConsoleColor]::Red)
        return 1
    }

    Write-BuildLog ("Todos los tests Windows pasaron (" + $testSpecs.Count + ").") ([ConsoleColor]::Green)
    return 0
}

# Validacion previa: no se crea ningun log si falta lo esencial.
foreach ($required in @($script:BashPath, (Join-Path $script:PortDir 'build.sh'),
        (Join-Path $script:PortDir 'msys2-env.sh'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "No existe: $required"
    }
}

$script:PosixPort = ConvertTo-MsysPath $script:PortDir
$msys2Posix = ConvertTo-MsysPath $Msys2Root
$oldMsys2Root = [Environment]::GetEnvironmentVariable('MSYS2_ROOT', 'Process')

try {
    $evidenceDir = Join-Path $script:RepoRoot 'evidence'
    New-Item -ItemType Directory -Force -Path $evidenceDir | Out-Null

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $LogPath = Join-Path $evidenceDir "build-$stamp.log"
    $suffix = 1
    while (Test-Path -LiteralPath $LogPath) {
        $LogPath = Join-Path $evidenceDir ("build-$stamp-" + $suffix + ".log")
        $suffix++
    }

    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $script:LogWriter = New-Object System.IO.StreamWriter($LogPath, $false, $utf8)
    $script:LogWriter.AutoFlush = $true

    try {
        [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
        $OutputEncoding = [Console]::OutputEncoding
    }
    catch {
        # Algunos hosts no permiten cambiar la codificacion; no es fatal.
    }

    $env:MSYS2_ROOT = $msys2Posix
    $buildCommand = "cd " + (ConvertTo-BashLiteral $script:PosixPort) + " && bash build.sh"

    Write-BuildLog '=== Build Windows inFAMOUS Second Son (MSYS2 CLANG64) ===' ([ConsoleColor]::Cyan)
    Write-BuildLog "Repo:      $script:RepoRoot"
    Write-BuildLog "Bash:      $script:BashPath"
    Write-BuildLog "MSYS2:     $Msys2Root (MSYS2_ROOT=$msys2Posix)"
    Write-BuildLog "Comando:   $script:BashPath -lc `"$buildCommand`""
    Write-BuildLog "Log:       $LogPath"
    Write-BuildLog "Opciones:  Clean=$Clean Tests=$Tests"
    Write-BuildLog ''

    $clang = Join-Path $Msys2Root 'clang64\bin\clang.exe'
    if (-not (Test-Path -LiteralPath $clang -PathType Leaf)) {
        Write-BuildLog "ADVERTENCIA: no existe $clang. Faltan los paquetes CLANG64 o aun se estan instalando." ([ConsoleColor]::Yellow)
    }

    Assert-NoGameRunning

    if ($Clean) {
        Remove-BuildOutput
        Write-BuildLog ''
    }

    Write-BuildLog '=== build.sh ===' ([ConsoleColor]::Cyan)
    $buildExit = Invoke-BashLogged $buildCommand
    Write-BuildLog "build.sh termino con codigo $buildExit" ([ConsoleColor]::Cyan)

    if ($buildExit -ne 0) {
        Write-BuildLog 'ERROR: la compilacion fallo. Revisa el log completo.' ([ConsoleColor]::Red)
        $ninjaLog = Join-Path (Join-Path $script:PortDir 'out') 'gpu-build.log'
        if (Test-Path -LiteralPath $ninjaLog) {
            Write-BuildLog "--- ultimas 40 lineas de $ninjaLog ---" ([ConsoleColor]::Yellow)
            Get-Content -LiteralPath $ninjaLog -Tail 40 -ErrorAction SilentlyContinue |
                ForEach-Object { Write-BuildLog $_ }
        }
        $exitCode = [int]$buildExit
    }
    else {
        if (-not (Test-BuildArtifacts)) {
            Write-BuildLog 'ERROR: build.sh devolvio 0 pero faltan artefactos.' ([ConsoleColor]::Red)
            $exitCode = 1
        }
        elseif ($Tests) {
            $exitCode = Invoke-WindowsTests
        }
    }

    if ($exitCode -eq 0) {
        Write-BuildLog ''
        Write-BuildLog '=== BUILD OK ===' ([ConsoleColor]::Green)
        Write-BuildLog "Ejecutables: $(Join-Path $script:PortDir 'out')"
    }
}
catch {
    Write-BuildLog ("ERROR: " + $_.Exception.Message) ([ConsoleColor]::Red)
    if ($_.ScriptStackTrace) {
        Write-BuildLog $_.ScriptStackTrace
    }
    if ($exitCode -eq 0) {
        $exitCode = 1
    }
}
finally {
    if ($null -ne $script:LogWriter) {
        $script:LogWriter.Flush()
        $script:LogWriter.Dispose()
        $script:LogWriter = $null
    }
    if ($null -eq $oldMsys2Root) {
        Remove-Item Env:MSYS2_ROOT -ErrorAction SilentlyContinue
    }
    else {
        $env:MSYS2_ROOT = $oldMsys2Root
    }
    if ($null -ne $LogPath) {
        Write-Host "Log: $LogPath" -ForegroundColor DarkGray
    }
}

exit $exitCode
