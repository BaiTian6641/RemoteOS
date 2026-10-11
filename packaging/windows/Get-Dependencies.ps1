[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference = 'Stop'
$manifest = Get-Content (Join-Path $PSScriptRoot 'dependencies.lock.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
foreach ($name in @('virtual_display_driver', 'usbip_win2')) {
    $dependency = $manifest.$name
    $file = Join-Path $Destination ([IO.Path]::GetFileName($dependency.url))
    if (!(Test-Path $file)) { Invoke-WebRequest -Uri $dependency.url -OutFile $file }
    if ((Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $dependency.sha256) {
        throw "Checksum mismatch for $name. The dependency has not been installed."
    }
    if ($file.EndsWith('.exe')) {
        $signature = Get-AuthenticodeSignature $file
        if ($signature.Status -ne 'Valid') { throw "Authenticode verification failed for $file" }
    } else {
        $unpacked = Join-Path $Destination $name
        Expand-Archive -LiteralPath $file -DestinationPath $unpacked -Force
        $catalogs = @(Get-ChildItem $unpacked -Filter '*.cat' -Recurse)
        if ($catalogs.Count -eq 0) { throw "No signed catalog in $file" }
        foreach ($catalog in $catalogs) {
            if ((Get-AuthenticodeSignature $catalog.FullName).Status -ne 'Valid') {
                throw "Invalid driver catalog signature: $($catalog.FullName)"
            }
        }
    }
    Write-Host "Verified download: $file"
}
Write-Host 'Driver installation remains explicit. Use the signed upstream installers; never enable test signing. PnP installation performs catalog membership verification.'
