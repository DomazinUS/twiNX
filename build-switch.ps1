param(
    [int]$Jobs = 1,
    [string]$DevkitPro = $env:DEVKITPRO,
    [string]$FfmpegRoot = $env:TWINX_FFMPEG_ROOT
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($DevkitPro)) {
    $DevkitPro = "C:\devkitPro"
}

$env:DEVKITPRO = $DevkitPro
$env:DEVKITA64 = Join-Path $DevkitPro "devkitA64"
$env:Path = "$env:DEVKITA64\bin;$DevkitPro\tools\bin;$env:Path"
$pkgConfig = Join-Path $DevkitPro "portlibs\switch\bin\aarch64-none-elf-pkg-config"

function Convert-ToCMakePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $resolved = (Resolve-Path -LiteralPath $Path).Path

    # devkitPro's CMake runs in an MSYS2-style environment. A raw Windows path
    # such as C:\dev\ffmpeg would otherwise be treated as a relative path and
    # appended to the repository directory. Prefer cygpath when available.
    $cygpathCandidates = @(
        (Join-Path $DevkitPro "msys2\usr\bin\cygpath.exe"),
        (Join-Path $DevkitPro "usr\bin\cygpath.exe")
    )

    foreach ($candidate in $cygpathCandidates) {
        if (Test-Path -LiteralPath $candidate) {
            $converted = & $candidate -u $resolved
            if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($converted)) {
                return $converted.Trim()
            }
        }
    }

    if ($resolved -match '^([A-Za-z]):[\\/](.*)$') {
        $drive = $Matches[1].ToLowerInvariant()
        $tail = $Matches[2] -replace '\\', '/'
        return "/$drive/$tail"
    }

    return ($resolved -replace '\\', '/')
}

$cmakeArgs = @(
    "-S", ".",
    "-B", "build_switch",
    "-G", "Ninja",
    "-DPLATFORM_SWITCH=ON",
    "-DBUILTIN_NSP=OFF",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DPKG_CONFIG_EXECUTABLE=$pkgConfig"
)

if (-not [string]::IsNullOrWhiteSpace($FfmpegRoot)) {
    $cmakeFfmpegRoot = Convert-ToCMakePath -Path $FfmpegRoot
    Write-Host "Using custom FFmpeg root: $cmakeFfmpegRoot"
    $cmakeArgs += "-DTWINX_FFMPEG_ROOT=$cmakeFfmpegRoot"
}

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed with exit code $LASTEXITCODE." }

& ninja -C build_switch "-j$Jobs" TwiNXPortraitExperimental.nro
if ($LASTEXITCODE -ne 0) { throw "Nintendo Switch build failed with exit code $LASTEXITCODE." }
