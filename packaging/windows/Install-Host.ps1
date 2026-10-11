[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$Display0,
    [Parameter(Mandatory=$true)][string]$Display1
)
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (!([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run the installer in an elevated PowerShell session.'
}
foreach ($display in @($Display0, $Display1)) {
    if ($display -notmatch '^\\\\\.\\DISPLAY[0-9]+$') { throw 'Pass an explicit display mapping such as \\.\DISPLAY2.' }
}
if ($Display0 -eq $Display1) { throw 'The two virtual displays must be distinct.' }
$destination = Join-Path $env:ProgramFiles 'RemoteOS'
$state = Join-Path $env:ProgramData 'RemoteOS'
$binaries = @('remoteos-host.exe', 'remoteos-host-service.exe')
foreach ($name in $binaries) {
    $path = Join-Path $BuildDirectory $name
    if (!(Test-Path $path)) { throw "Missing binary: $path" }
    if ((Get-AuthenticodeSignature $path).Status -ne 'Valid') {
        throw "A production host requires valid Authenticode signatures: $path"
    }
}
if (Get-Service RemoteOSHost -ErrorAction SilentlyContinue) { throw 'Stop and uninstall the existing RemoteOSHost service before replacing its files.' }
New-Item -ItemType Directory -Path $destination, $state -Force | Out-Null
# Identity files and host configuration are accessible only to SYSTEM/Admins.
& icacls.exe $state /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Unable to restrict host state directory ACLs.' }
foreach ($name in $binaries) { Copy-Item (Join-Path $BuildDirectory $name) $destination }
# Runtime DLLs must be from the qualified distribution. Do not search or copy
# arbitrary DLLs from a writable PATH into this SYSTEM service directory.
$config = Join-Path $state 'host.json'
if (!(Test-Path $config)) {
    @{
        identity_dir = (Join-Path $state 'identity')
        bind = '0.0.0.0'
        port = 47010
        audio_source = 'wasapi'
        pairing_code_file = (Join-Path $state 'pairing-code')
    } | ConvertTo-Json | Set-Content -Path $config -Encoding UTF8
}
$binaryPath = '"' + (Join-Path $destination 'remoteos-host-service.exe') + '"'
New-Service -Name RemoteOSHost -BinaryPathName $binaryPath -DisplayName 'RemoteOS host broker' -StartupType Automatic | Out-Null
# Service-local environment avoids changing display mappings for other programs.
$serviceKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\RemoteOSHost'
New-ItemProperty -Path $serviceKey -Name Environment -PropertyType MultiString -Value @("REMOTEOS_DISPLAY_0=$Display0", "REMOTEOS_DISPLAY_1=$Display1") -Force | Out-Null
Write-Host 'Installed without starting the service or changing firewall/lock/UAC policy.'
Write-Host 'Complete signed VDD provisioning, runtime DLL installation, firewall scope, and the documented Software SAS policy, then Start-Service RemoteOSHost.'
