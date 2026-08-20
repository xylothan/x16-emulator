<#
.SYNOPSIS
    Configure, build, test and run the X16Emu ADD emulator on Windows.

.DESCRIPTION
    Wraps the MSVC/CMake/vcpkg build so the common jobs are one word instead of
    a remembered incantation. It works out which Visual Studio is installed,
    which CMake can actually drive that generator, and where vcpkg lives, then
    picks the matching preset from CMakePresets.json.

    The discovery matters more than the typing it saves. The system CMake on
    PATH is often older than the installed Visual Studio and does not know its
    generator, so `cmake --preset vs2026` fails with a wall of generator names
    that does not mention the real problem. Visual Studio ships its own CMake
    that always knows, so this prefers that one.

.EXAMPLE
    .\build.ps1
    Debug build of the emulator.

.EXAMPLE
    .\build.ps1 -Config Release
    Release build.

.EXAMPLE
    .\build.ps1 -Tests
    Build and run the unit test suite.

.EXAMPLE
    .\build.ps1 -DapTest -FetchRom
    Build, download a ROM, boot the emulator and run the DAP testbench against it.

.EXAMPLE
    .\build.ps1 -Run -EmuArgs '-prg','MYPROG.PRG','-run'
    Build, then launch the emulator with extra arguments passed through to it.
#>

[CmdletBinding()]
param(
    # Build configuration.
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',

    # A specific CMake target instead of the emulator, e.g. joystick_test.
    [string]$Target,

    # Build and run the unit test suite (implies -DBUILD_TESTS=ON).
    [switch]$Tests,

    # Launch the emulator when the build succeeds.
    [switch]$Run,

    # Boot the emulator and run testbench/test_dap.py against it.
    [switch]$DapTest,

    # Path to rom.bin. Otherwise the usual places are searched.
    [string]$Rom,

    # Download rom.bin from X16Community/x16-rom with gh, as CI does.
    [switch]$FetchRom,

    # Re-run the configure step even when the build tree is already set up.
    [switch]$Reconfigure,

    # Delete the build tree first.
    [switch]$Clean,

    # Stop a running emulator that is holding x16emu.exe open.
    [switch]$Force,

    # Override the CMakePresets.json preset (vs2022, vs2026, ninja).
    [string]$Preset,

    # DAP port for -Run and -DapTest. 0 picks a free one.
    [int]$Port = 0,

    # Seconds to let the machine reach BASIC before -DapTest starts asserting.
    [int]$BootWait = 25,

    # Extra arguments passed straight to x16emu, e.g. -EmuArgs '-prg','A.PRG','-run'.
    # Named rather than trailing, because a bare -run would collide with this
    # script's own -Run switch and PowerShell would bind it here instead.
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$EmuArgs
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = $PSScriptRoot
$RomDir = Join-Path $RepoRoot 'latest_rom'

function Write-Step { param([string]$m) Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Note { param([string]$m) Write-Host "    $m" -ForegroundColor DarkGray }
function Fail { param([string]$m) Write-Host "!!! $m" -ForegroundColor Red; exit 1 }

# ─── Toolchain discovery ────────────────────────────────────────────────────

function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        Fail "vswhere.exe not found. Install Visual Studio with the C++ workload."
    }

    # -latest alone can pick an install without the C++ tools, which then fails
    # much later with a compiler error rather than an obvious one here.
    $path = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null | Select-Object -First 1
    if (-not $path) {
        $path = & $vswhere -latest -products * -property installationPath 2>$null | Select-Object -First 1
    }
    if (-not $path) { Fail "No Visual Studio installation found." }

    $line = & $vswhere -latest -products * -property catalog_productLineVersion 2>$null | Select-Object -First 1
    $version = & $vswhere -latest -products * -property installationVersion 2>$null | Select-Object -First 1
    $major = if ("$version" -match '^(\d+)') { $Matches[1] } else { '' }

    [pscustomobject]@{ Path = $path; ProductLine = "$line"; Major = "$major" }
}

function Find-CMake {
    param([string]$VsPath)
    # Visual Studio's own CMake always understands its own generator; the one on
    # PATH frequently does not, which is the failure this script exists to avoid.
    $bundled = Join-Path $VsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path $bundled) { return $bundled }
    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    Fail "No cmake.exe found, in Visual Studio or on PATH."
}

function Find-CTest {
    param([string]$CMakePath)
    $ctest = Join-Path (Split-Path $CMakePath) 'ctest.exe'
    if (Test-Path $ctest) { return $ctest }
    $onPath = Get-Command ctest -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    Fail "No ctest.exe found beside cmake or on PATH."
}

function Find-VcpkgRoot {
    param([string]$VsPath)
    # The presets reference $env:VCPKG_ROOT in their toolchain file, so this has
    # to end up in the environment whether or not the user has set it.
    $candidates = @(
        $env:VCPKG_ROOT,
        $env:VCPKG_INSTALLATION_ROOT,
        (Join-Path $VsPath 'VC\vcpkg')
    ) | Where-Object { $_ }

    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c 'scripts\buildsystems\vcpkg.cmake')) { return $c }
    }
    Fail "No vcpkg found. Set VCPKG_ROOT, or install the vcpkg component with Visual Studio."
}

function Resolve-Preset {
    param([string]$Major)
    if ($Preset) { return $Preset }

    # Match the installed Visual Studio against the preset generators rather than
    # hardcoding a version table. vswhere reports the internal major version (17
    # for 2022, 18 for 2026) and the generator names carry the same number, so a
    # preset added for a future Visual Studio is picked up without touching this.
    $presetFile = Join-Path $RepoRoot 'CMakePresets.json'
    if (Test-Path $presetFile) {
        $presets = (Get-Content $presetFile -Raw | ConvertFrom-Json).configurePresets
        $match = $presets | Where-Object {
            $_.PSObject.Properties.Name -contains 'generator' -and
            $_.generator -match "^Visual Studio $Major "
        } | Select-Object -First 1
        if ($match) { return $match.name }
    }

    Fail ("No CMakePresets.json preset matches Visual Studio major version $Major. " +
          "Add one, or pass -Preset <name>.")
}

# ─── ROM handling ───────────────────────────────────────────────────────────

function Resolve-Rom {
    if ($Rom) {
        if (-not (Test-Path $Rom)) { Fail "ROM not found: $Rom" }
        return (Resolve-Path $Rom).Path
    }

    # Built one at a time rather than as a list expression: Join-Path throws on a
    # null base, so an unset X16_ROM_DIR would take the whole lookup down before
    # the emptiness filter ever ran.
    $places = New-Object System.Collections.Generic.List[string]
    if ($env:X16_ROM) { $places.Add($env:X16_ROM) }
    $places.Add((Join-Path $RomDir 'rom.bin'))
    $places.Add((Join-Path $RepoRoot 'rom.bin'))
    if ($env:X16_ROM_DIR) { $places.Add((Join-Path $env:X16_ROM_DIR 'rom.bin')) }

    foreach ($p in $places) {
        if (Test-Path $p) { return (Resolve-Path $p).Path }
    }
    return $null
}

function Get-Rom {
    # Mirrors .github/actions/fetch-rom: prefer the release asset, which never
    # expires, over a workflow artifact that does.
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
        Fail "-FetchRom needs the GitHub CLI (gh) on PATH."
    }
    Write-Step "Fetching rom.bin from X16Community/x16-rom"
    $staging = Join-Path $RomDir 'zip'
    New-Item -ItemType Directory -Force $RomDir | Out-Null
    New-Item -ItemType Directory -Force $staging | Out-Null
    & gh release download -R X16Community/x16-rom --pattern '*.zip' --dir $staging --clobber
    if ($LASTEXITCODE -ne 0) { Fail "gh release download failed." }
    $zip = Get-ChildItem (Join-Path $staging '*.zip') | Select-Object -First 1
    if (-not $zip) { Fail "No zip downloaded from x16-rom." }
    Expand-Archive -Path $zip.FullName -DestinationPath $RomDir -Force
    $rom = Join-Path $RomDir 'rom.bin'
    if (-not (Test-Path $rom)) { Fail "rom.bin missing from the downloaded archive." }
    Write-Note "ROM at $rom"
    return $rom
}

# ─── Process and port helpers ───────────────────────────────────────────────

function Stop-RunningEmulator {
    param([string]$ExePath)
    # A running emulator holds its own .exe open and the link step fails with a
    # bare LNK1104 that says nothing about why.
    $running = Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path -ieq $ExePath }
    if (-not $running) { return }

    if (-not $Force) {
        Write-Host "!!! x16emu.exe is running (PID $($running.Id -join ', ')) and will block the link step." -ForegroundColor Yellow
        Write-Host "    Close it, or re-run with -Force." -ForegroundColor Yellow
        exit 1
    }
    foreach ($p in $running) {
        Write-Note "Stopping running emulator (PID $($p.Id))"
        Stop-Process -Id $p.Id -Force
    }
    Start-Sleep -Milliseconds 500
}

function Get-FreePort {
    if ($Port -ne 0) { return $Port }
    $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $l.Start()
    $p = $l.LocalEndpoint.Port
    $l.Stop()
    return $p
}

function Wait-ForPort {
    param([int]$P, [int]$TimeoutSec = 60)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        try {
            $c = [System.Net.Sockets.TcpClient]::new()
            $c.Connect('127.0.0.1', $P)
            $c.Close()
            return $true
        } catch {
            Start-Sleep -Milliseconds 250
        }
    }
    return $false
}

# ─── Main ───────────────────────────────────────────────────────────────────

$vs = Find-VisualStudio
$cmake = Find-CMake -VsPath $vs.Path
$ctest = Find-CTest -CMakePath $cmake
$vcpkg = Find-VcpkgRoot -VsPath $vs.Path
$presetName = Resolve-Preset -Major $vs.Major
$buildDir = Join-Path $RepoRoot "out\build\$presetName"

$env:VCPKG_ROOT = $vcpkg

Write-Step "Toolchain"
Write-Note "Visual Studio : $($vs.Path)"
Write-Note "CMake         : $cmake"
Write-Note "vcpkg         : $vcpkg"
Write-Note "Preset        : $presetName ($Config)"

if ($Clean -and (Test-Path $buildDir)) {
    Write-Step "Removing $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

# Reconfigure when the cache is missing, when asked, or when the BUILD_TESTS
# setting no longer matches what was asked for -- otherwise -Tests on a tree
# configured without them silently builds nothing and ctest reports an empty run.
# A named *_test target needs the same option, or the build fails claiming the
# target does not exist when the truth is that it was never declared.
$wantsTests = $Tests -or ($Target -and $Target -match '_test$')

$cache = Join-Path $buildDir 'CMakeCache.txt'
$needConfigure = $Reconfigure -or -not (Test-Path $cache)
if (-not $needConfigure -and $wantsTests) {
    $cached = Select-String -Path $cache -Pattern '^BUILD_TESTS:BOOL=(.*)$' -ErrorAction SilentlyContinue
    if (-not $cached -or $cached.Matches[0].Groups[1].Value -ne 'ON') { $needConfigure = $true }
}

if ($needConfigure) {
    Write-Step "Configuring"
    $cfgArgs = @('--preset', $presetName)
    if ($wantsTests) { $cfgArgs += '-DBUILD_TESTS=ON' }
    & $cmake @cfgArgs
    if ($LASTEXITCODE -ne 0) { Fail "Configure failed." }
}

$exePath = Join-Path $buildDir "$Config\x16emu.exe"

$targets = @()
if ($Target) { $targets += $Target }
elseif ($Tests) { $targets += 'unit_tests' }
else { $targets += 'x16emu' }

if ($targets -contains 'x16emu' -or $Run -or $DapTest) {
    if ($targets -notcontains 'x16emu') { $targets += 'x16emu' }
    Stop-RunningEmulator -ExePath $exePath
}

Write-Step "Building $($targets -join ', ') ($Config)"
& $cmake --build $buildDir --config $Config --target @targets
$buildStatus = $LASTEXITCODE

if ($buildStatus -ne 0) {
    # -Tests is expected to be partial on MSVC: several pre-existing test files
    # define int main(void) while SDL's headers have already redefined main to
    # SDL_main, which is C4026 and fatal under /WX. Report it rather than
    # pretending the whole build failed, so the tests that do build still run.
    if ($Tests) {
        Write-Host "!!! Some test targets failed to build; running the ones that did." -ForegroundColor Yellow
    } else {
        Fail "Build failed."
    }
}

if (-not $Tests -and -not $Run -and -not $DapTest) {
    Write-Step "Done"
    if (Test-Path $exePath) { Write-Note $exePath }
    exit 0
}

if ($Tests) {
    Write-Step "Running unit tests"
    Push-Location $buildDir
    try {
        & $ctest -C $Config --output-on-failure
        $testStatus = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    if ($testStatus -ne 0) {
        Write-Host "!!! ctest reported failures (see above)." -ForegroundColor Yellow
        exit $testStatus
    }
    Write-Step "Unit tests passed"
    exit 0
}

# Everything below needs a working emulator binary and a ROM.
if (-not (Test-Path $exePath)) { Fail "Emulator not found at $exePath" }

$romPath = if ($FetchRom) { Get-Rom } else { Resolve-Rom }
if (-not $romPath) {
    Fail "No rom.bin found. Pass -Rom <path>, set X16_ROM, or use -FetchRom."
}

if ($Run) {
    $runArgs = @('-rom', $romPath)
    if ($Port -ne 0) { $runArgs += @('-debugport', "$Port") }
    if ($EmuArgs) { $runArgs += $EmuArgs }
    Write-Step "Running x16emu"
    Write-Note "$exePath $($runArgs -join ' ')"
    & $exePath @runArgs
    exit $LASTEXITCODE
}

if ($DapTest) {
    if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
        Fail "-DapTest needs python on PATH."
    }
    $dapPort = Get-FreePort
    $runArgs = @('-rom', $romPath, '-debugport', "$dapPort", '-warp')
    if ($EmuArgs) { $runArgs += $EmuArgs }

    Write-Step "Booting the emulator on DAP port $dapPort"
    $proc = Start-Process -FilePath $exePath -ArgumentList $runArgs -PassThru

    try {
        if (-not (Wait-ForPort -P $dapPort)) { Fail "DAP server never came up on port $dapPort." }
        # The port opens long before the machine reaches BASIC, and the testbench
        # types into the KERNAL buffer, so it has to wait for the boot as well as
        # the socket or the early assertions race the ROM.
        Write-Note "Waiting ${BootWait}s for the machine to boot"
        Start-Sleep -Seconds $BootWait

        Write-Step "Running testbench/test_dap.py"
        & python (Join-Path $RepoRoot 'testbench\test_dap.py') $dapPort
        $dapStatus = $LASTEXITCODE
    } finally {
        if ($proc -and -not $proc.HasExited) {
            Write-Note "Stopping the emulator (PID $($proc.Id))"
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    }

    if ($dapStatus -ne 0) { Fail "DAP testbench reported failures." }
    Write-Step "DAP testbench passed"
    exit 0
}
