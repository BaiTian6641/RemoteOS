#Requires -Version 7.0
param(
    [Parameter(Mandatory=$true)][string]$Package,
    [Parameter(Mandatory=$true)][string[]]$SdkRoots
)
$ErrorActionPreference='Stop'
$Dumpbin=(Get-Command dumpbin -ErrorAction Stop).Source
$Index=@{}
foreach ($Root in ($SdkRoots | Select-Object -Unique)) {
    foreach ($File in (Get-ChildItem $Root -Recurse -File -Filter '*.dll')) {
        $Name=$File.Name.ToLower()
        if (!$Index.ContainsKey($Name)) {$Index[$Name]=@()}
        $Index[$Name]+=$File.FullName
    }
}
# The SDK also installs imported DLLs under lib (for example pxbackend).
# Complete the import closure before hiding the SDK and testing the package.
do {
    $Added=$false
    $Bundled=@{}
    Get-ChildItem "$Package/bin" -Filter '*.dll' | ForEach-Object {$Bundled[$_.Name.ToLower()]=$true}
    $Files=Get-ChildItem $Package -Recurse -File | Where-Object {$_.Extension -in @('.exe','.dll')}
    foreach ($File in $Files) {
        $Imports=& $Dumpbin /dependents $File.FullName
        if ($LASTEXITCODE) {throw "Cannot read PE imports: $($File.FullName)"}
        foreach ($Line in $Imports) {
            if ($Line -notmatch '^\s+([\w.-]+\.dll)\s*$') {continue}
            $Name=$Matches[1].ToLower()
            if ($Bundled.ContainsKey($Name) -or $Name -match '^(api-ms-|ext-ms-)' -or (Test-Path "$env:SystemRoot/System32/$Name")) {continue}
            if (!$Index.ContainsKey($Name)) {throw "SDK does not supply $Name, imported by $($File.Name)"}
            $Candidates=@($Index[$Name] | Select-Object -Unique)
            $Hashes=@($Candidates | ForEach-Object {(Get-FileHash $_ -Algorithm SHA256).Hash} | Select-Object -Unique)
            if ($Hashes.Count -ne 1) {throw "Ambiguous SDK DLL: $Name"}
            Copy-Item $Candidates[0] "$Package/bin/$Name"
            $Bundled[$Name]=$true
            $Added=$true
            Write-Host "Bundled imported runtime: $Name"
        }
    }
} while ($Added)
