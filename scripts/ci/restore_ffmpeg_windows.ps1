param(
    [string]$ArtifactDir = "build/ci-ffmpeg",
    [string]$InstallDir = ""
)

$ErrorActionPreference = "Stop"

$artifactName = (python scripts/ci/ffmpeg_lock.py artifact-name windows-x64).Trim()
if (-not $InstallDir) {
    $InstallDir = (python scripts/ci/ffmpeg_lock.py install-path windows-x64).Trim()
}
$unpackDir = Join-Path $ArtifactDir "unpacked"
$zip = Get-ChildItem $ArtifactDir -Recurse -Filter "$artifactName.zip" | Select-Object -First 1

if (-not $zip) {
    throw "Windows FFmpeg artifact zip not found under $ArtifactDir"
}

python scripts/ci/ffmpeg_lock.py verify windows-x64 $zip.FullName
Remove-Item -Recurse -Force $unpackDir -ErrorAction SilentlyContinue
Expand-Archive -Force $zip.FullName $unpackDir
Remove-Item -Recurse -Force $InstallDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $InstallDir | Out-Null
Move-Item (Join-Path $unpackDir "*") "$InstallDir/"

$dll = Join-Path $InstallDir "bin/avcodec-62.dll"
if (-not (Test-Path $dll)) {
    throw "Windows FFmpeg DLL is missing: $dll"
}

if ((Get-Item $dll).Length -lt 1000000) {
    throw "Windows FFmpeg DLL is unexpectedly small: $dll"
}

$bytes = [System.IO.File]::ReadAllBytes($dll)
$prefix = [System.Text.Encoding]::ASCII.GetString($bytes, 0, [Math]::Min($bytes.Length, 64))
if ($prefix.StartsWith("version https://git-lfs.github.com/spec/v1")) {
    throw "Windows FFmpeg DLL is still a Git LFS pointer: $dll"
}
