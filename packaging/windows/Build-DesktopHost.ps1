#Requires -Version 7.0
# Run in a normal x64 Visual Studio developer PowerShell. No service or driver
# installation, registry changes, firewall rules or elevation are performed.
param(
    [Parameter(Mandatory=$true)][string]$GStreamerRoot,
    [string]$OpenSSLRoot = "",
    [string]$PkgConfigExe = "",
    [string]$OutputDirectory = "",
    [string]$BuildDirectory = ""
)
$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if (!$BuildDirectory) { $BuildDirectory = Join-Path $Repo "build-desktop-windows" }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $Repo "output/desktop-host-windows" }
if (Test-Path $OutputDirectory) { throw "Choose a new output directory; existing packages are preserved." }
$GStreamerRoot = (Resolve-Path $GStreamerRoot).Path
$OpenSSLRoot = if ($OpenSSLRoot) { (Resolve-Path $OpenSSLRoot).Path } else { $GStreamerRoot }
if (!$PkgConfigExe) { $PkgConfigExe = Join-Path $GStreamerRoot "bin/pkg-config.exe" }
$PkgConfigExe = (Get-Command $PkgConfigExe -ErrorAction Stop).Source
if (!(Test-Path (Join-Path $GStreamerRoot "lib/pkgconfig/gstreamer-1.0.pc"))) {
    throw "Use matching x64 MSVC GStreamer runtime AND development SDKs (1.24 or newer)."
}
$SavedPath = $env:PATH
$SavedPkgPath = $env:PKG_CONFIG_PATH
try {
    $env:PATH = "$(Join-Path $GStreamerRoot 'bin');$(Join-Path $OpenSSLRoot 'bin');$SavedPath"
    $env:PKG_CONFIG_PATH = "$(Join-Path $GStreamerRoot 'lib/pkgconfig');$SavedPkgPath"
    & cmake -S $Repo -B $BuildDirectory -G Ninja -DCMAKE_BUILD_TYPE=Release `
        "-DOPENSSL_ROOT_DIR=$OpenSSLRoot" "-DPKG_CONFIG_EXECUTABLE=$PkgConfigExe" `
        -DREMOTEOS_WITH_MEDIA=ON -DREMOTEOS_WITH_GTK=OFF -DREMOTEOS_WITH_BROWSER=OFF `
        -DREMOTEOS_WITH_SERVICE_HOST=OFF -DREMOTEOS_WITH_DESKTOP_HOST=ON -DREMOTEOS_BUILD_HOST_APP=ON `
        -DREMOTEOS_BUILD_TESTS=OFF
    if ($LASTEXITCODE) { throw "CMake configuration failed" }
    & cmake --build $BuildDirectory --target remoteos-host remoteos-host-app remoteos-runtime-check
    if ($LASTEXITCODE) { throw "Host application build failed" }
    New-Item -ItemType Directory -Path "$OutputDirectory/bin", "$OutputDirectory/lib/gstreamer-1.0" | Out-Null
    Copy-Item "$BuildDirectory/remoteos-host.exe", "$BuildDirectory/remoteos-host-app.exe", "$BuildDirectory/remoteos-runtime-check.exe" "$OutputDirectory/bin"
    Copy-Item "$GStreamerRoot/bin/*.dll" "$OutputDirectory/bin"
    Copy-Item "$OpenSSLRoot/bin/*.dll" "$OutputDirectory/bin"
    $Plugins = @('coreelements','app','udp','rtp','rtpmanager','dtls','srtp','libav','audioconvert','audioresample','opus','videoconvertscale','videoparsersbad','jpeg','videotestsrc','wasapi','wasapi2')
    foreach ($Plugin in $Plugins) {
        $Files = Get-ChildItem "$GStreamerRoot/lib/gstreamer-1.0" -Filter "*gst$Plugin.dll"
        if (!$Files -and $Plugin -notin @('wasapi','wasapi2')) { throw "Missing SDK plugin: $Plugin" }
        $Files | Copy-Item -Destination "$OutputDirectory/lib/gstreamer-1.0"
    }
    if (!$env:VCToolsRedistDir) { throw 'Use the Visual Studio developer shell so the app-local CRT can be bundled.' }
    $Crt = Get-ChildItem "$env:VCToolsRedistDir/x64" -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
    if (!$Crt) { throw 'Missing x64 Visual C++ redistributable runtime' }
    Copy-Item "$($Crt.FullName)/*.dll" "$OutputDirectory/bin"
    Copy-Item "$Repo/packaging/windows/MSVC-runtime-notice.txt" "$OutputDirectory/MSVC-runtime-notice.txt"
    Copy-Item "$Repo/packaging/native-sdk.lock.json" "$OutputDirectory/native-sdk.lock.json"
    Copy-Item "$Repo/VERSION" "$OutputDirectory/VERSION"
    $GstLicenses = Join-Path $GStreamerRoot 'share/licenses'
    if (!(Test-Path $GstLicenses)) { throw 'The SDK must contain redistribution licenses.' }
    if (Test-Path "$GStreamerRoot/libexec/gstreamer-1.0") {
        New-Item -ItemType Directory -Path "$OutputDirectory/libexec" | Out-Null
        Copy-Item "$GStreamerRoot/libexec/gstreamer-1.0" "$OutputDirectory/libexec" -Recurse
    }
    foreach ($Base in @($GStreamerRoot, $OpenSSLRoot)) {
        foreach ($Name in @('share/licenses', 'share/doc', 'LICENSE.txt', 'LICENSE', 'COPYING')) {
            if (Test-Path (Join-Path $Base $Name)) {
                $LicenseDirectory = Join-Path $OutputDirectory "licenses/$(Split-Path $Base -Leaf)"
                New-Item -ItemType Directory -Path $LicenseDirectory -Force | Out-Null
                Copy-Item (Join-Path $Base $Name) $LicenseDirectory -Recurse
            }
        }
    }
    Copy-Item "$Repo/docs/portable-builds.md" "$OutputDirectory/README.md"
    @'
@echo off
setlocal
set "PATH=%~dp0bin;%PATH%"
set "GST_PLUGIN_PATH_1_0=%~dp0lib\gstreamer-1.0"
set "GST_PLUGIN_SYSTEM_PATH_1_0="
if exist "%~dp0libexec\gstreamer-1.0\gst-plugin-scanner.exe" set "GST_PLUGIN_SCANNER_1_0=%~dp0libexec\gstreamer-1.0\gst-plugin-scanner.exe"
start "RemoteOS Host" "%~dp0bin\remoteos-host-app.exe"
'@ | Set-Content -Encoding ASCII "$OutputDirectory/Start-RemoteOS-Host.cmd"
    $Manifest = [ordered]@{version=(Get-Content "$Repo/VERSION" -Raw).Trim(); platform='windows-x86_64'; format='portable zip'; service=$false; files=@()}
    Get-ChildItem $OutputDirectory -File -Recurse | Sort-Object FullName | ForEach-Object {
        $Manifest.files += [ordered]@{path=[IO.Path]::GetRelativePath($OutputDirectory,$_.FullName).Replace('\','/'); sha256=(Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower()}
    }
    $Manifest | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 "$OutputDirectory/build-manifest.json"
    Write-Host "Open $OutputDirectory/bin/remoteos-host-app.exe as your normal desktop user."
} finally {
    $env:PATH = $SavedPath
    $env:PKG_CONFIG_PATH = $SavedPkgPath
}
