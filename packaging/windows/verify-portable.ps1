#Requires -Version 7.0
param([Parameter(Mandatory=$true)][string]$Package)
$ErrorActionPreference='Stop'
$Package=(Resolve-Path $Package).Path
$Dumpbin=(Get-Command dumpbin -ErrorAction Stop).Source
$Manifest=Get-Content "$Package/build-manifest.json" -Raw | ConvertFrom-Json
foreach ($Entry in $Manifest.files) {
    if ((Get-FileHash (Join-Path $Package $Entry.path)).Hash.ToLower() -ne $Entry.sha256) { throw "Package hash mismatch: $($Entry.path)" }
}
$Bundled=@{};Get-ChildItem "$Package/bin" -Filter '*.dll' | ForEach-Object {$Bundled[$_.Name.ToLower()]=$true}
foreach ($File in (Get-ChildItem $Package -Recurse -File | Where-Object {$_.Extension -in @('.exe','.dll')})) {
    $Headers=& $Dumpbin /headers $File.FullName
    if ($LASTEXITCODE -or !($Headers -match '8664 machine')) { throw "Non-x64 PE: $($File.Name)" }
    $Imports=& $Dumpbin /dependents $File.FullName
    foreach ($Line in $Imports) {
        if ($Line -match '^\s+([\w.-]+\.dll)\s*$') {
            $Name=$Matches[1].ToLower()
            if ($Name -match '^(api-ms-|ext-ms-)') { continue }
            if ($Name -match '^(msvcp|vcruntime|concrt)' -and !$Bundled.ContainsKey($Name)) {throw "CRT DLL not bundled: $Name"}
            if (!$Bundled.ContainsKey($Name) -and !(Test-Path "$env:SystemRoot/System32/$Name")) {throw "Missing DLL: $Name, imported by $($File.Name)"}
        }
    }
}
$Saved=$env:PATH
try {
    $env:PATH="$env:SystemRoot/System32;$env:SystemRoot"
    Remove-Item Env:GST_PLUGIN_PATH,Env:GST_PLUGIN_PATH_1_0,Env:GST_PLUGIN_SCANNER_1_0,Env:GST_REGISTRY_1_0 -ErrorAction SilentlyContinue
    & "$Package/bin/remoteos-runtime-check.exe" 2>&1 | Tee-Object -Variable RuntimeOutput
    if ($LASTEXITCODE) {throw ("Bundled runtime test failed: " + (($RuntimeOutput | Select-Object -Last 25) -join '; '))}
    & "$Package/bin/remoteos-host.exe" --help
    if ($LASTEXITCODE) {throw 'Host command could not start'}
    $App=Start-Process "$Package/bin/remoteos-host-app.exe" -PassThru
    Start-Sleep -Seconds 3
    if ($App.HasExited) {throw 'Portable host GUI exited during startup'}
    $null=$App.CloseMainWindow()
    if (!$App.WaitForExit(5000)) {$App.Kill();throw 'Host GUI did not close normally'}
} finally {$env:PATH=$Saved}
Write-Output 'Portable PE closure, MPEG-2, TLS initialization and GUI startup passed.'
