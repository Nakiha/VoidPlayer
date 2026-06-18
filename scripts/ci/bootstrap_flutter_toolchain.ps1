$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "../..")
Set-Location $root

$python = if ($env:PYTHON) { $env:PYTHON } else { "python" }
$target = if ($env:VOIDPLAYER_FLUTTER_TOOLCHAIN_PATH) {
    $env:VOIDPLAYER_FLUTTER_TOOLCHAIN_PATH
} else {
    ".toolchains/flutter"
}

if ([System.IO.Path]::IsPathRooted($target)) {
    $targetAbs = $target
} else {
    $targetAbs = Join-Path $root $target
}

$flutter = Join-Path $targetAbs "bin/flutter.bat"
$env:VOIDPLAYER_FLUTTER_TOOLCHAIN_PATH = $targetAbs
$env:VOIDPLAYER_FLUTTER_BIN = $flutter

& $python dev.py toolchain bootstrap-flutter
& $python dev.py toolchain doctor

if ($env:GITHUB_PATH) {
    Join-Path $targetAbs "bin" | Out-File -FilePath $env:GITHUB_PATH -Append -Encoding utf8
}

if ($env:GITHUB_ENV) {
    "VOIDPLAYER_FLUTTER_TOOLCHAIN_PATH=$targetAbs" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
    "VOIDPLAYER_FLUTTER_BIN=$flutter" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
}
