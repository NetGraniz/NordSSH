param([Parameter(Mandatory=$true)][string]$Installer)
$ErrorActionPreference='Stop'
if($env:GITHUB_ACTIONS -ne 'true') {throw 'Installer tests run only on isolated GitHub runners'}
$fixture=Join-Path $env:RUNNER_TEMP 'NordSSH-install-fixture'
if(Test-Path -LiteralPath $fixture) {throw 'Fixture folder already exists'}
New-Item -ItemType Directory -Path $fixture | Out-Null
$installPath=Join-Path $fixture 'installed'
$appData=Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'com.nordssh.desktop'
$vault=Join-Path $appData 'vault.nordvault'
if(Test-Path -LiteralPath $vault) {throw 'Existing app-data vault detected; refusing to overwrite'}
New-Item -ItemType Directory -Path $appData -Force | Out-Null
Copy-Item -LiteralPath test/fixtures/windows-v1.json -Destination $vault
$before=(Get-FileHash -LiteralPath $vault).Hash
$deviceDir=Join-Path $appData 'device-keys'
New-Item -ItemType Directory -Path $deviceDir -Force | Out-Null
$deviceMarker=Join-Path $deviceDir 'installer-retention-fixture.txt'
[IO.File]::WriteAllText($deviceMarker,'synthetic device-key retention marker')
function Execute-Installer([string]$File,[string[]]$Arguments) {
 $p=Start-Process -FilePath (Resolve-Path -LiteralPath $File).Path -ArgumentList $Arguments -WindowStyle Hidden -PassThru
 try {if(-not $p.WaitForExit(120000)){throw 'Installer timed out'};if($p.ExitCode -ne 0){throw "Installer exit code $($p.ExitCode)"}} finally {if(-not $p.HasExited){Stop-Process -Id $p.Id}}
}
function Assert-Preserved {
 if((Get-FileHash -LiteralPath $vault).Hash -ne $before){throw 'Installer changed vault data'}
 if([IO.File]::ReadAllText($deviceMarker) -ne 'synthetic device-key retention marker'){throw 'Installer changed remembered-key directory'}
}
function Smoke([string]$Executable) {
 $p=Start-Process -FilePath $Executable -WindowStyle Hidden -PassThru
 try {
  $responding=$false
  for($attempt=0;$attempt -lt 30;$attempt++){Start-Sleep -Milliseconds 500;$p.Refresh();if($p.HasExited){throw 'Installed app exited early'};if($p.MainWindowTitle -eq 'NordSSH' -and $p.Responding){$responding=$true;break}}
  if(-not $responding){throw 'Installed native NordSSH window did not respond'}
 } finally {$p.Refresh();if(-not $p.HasExited){Stop-Process -Id $p.Id;$p.WaitForExit()}}
}
$baseline=(Resolve-Path artifacts/baseline/setup.exe).Path
Execute-Installer $baseline @('/S',"/D=$installPath")
$installed=Join-Path $installPath 'nordssh.exe'
if((Get-FileHash $installed).Hash -ne (Get-FileHash artifacts/baseline/NordSSH.exe).Hash){throw 'Baseline install did not deploy the baseline binary'}
Smoke $installed
Assert-Preserved
Execute-Installer $Installer @('/S')
if((Get-FileHash $installed).Hash -ne (Get-FileHash src-tauri/target/release/nordssh.exe).Hash){throw 'Upgrade did not replace the older binary'}
if(Test-Path (Join-Path $installPath 'nordssh.portable')){throw 'Installed app unexpectedly entered portable mode'}
Assert-Preserved
Smoke $installed
Execute-Installer $Installer @('/S')
Assert-Preserved
$shortcut=Join-Path ([Environment]::GetFolderPath('StartMenu')) 'Programs/NordSSH/NordSSH.lnk'
if(-not (Test-Path -LiteralPath $shortcut)){throw 'Start menu shortcut was not installed'}
$link=(New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
if([IO.Path]::GetFullPath($link.TargetPath) -ine [IO.Path]::GetFullPath($installed)){throw 'Start menu shortcut targets the wrong executable'}
Execute-Installer (Join-Path $installPath 'uninstall.exe') @('/S')
Assert-Preserved
Write-Output 'PASS: Windows per-user install, older-binary upgrade, repeated install, responding native windows, Start menu shortcut, uninstall and byte-identical encrypted vault/device-marker retention.'

# Portable update uses the real older helper, signed new EXE and a synthetic vault.
$portable=Join-Path $fixture 'portable'
$stage=Join-Path $fixture 'update-stage'
New-Item -ItemType Directory -Path $portable,$stage,(Join-Path $portable 'NordSSH-data') | Out-Null
Copy-Item artifacts/baseline/NordSSH.exe (Join-Path $portable 'NordSSH.exe')
Copy-Item assets/portable/nordssh.portable (Join-Path $portable 'nordssh.portable')
Copy-Item test/fixtures/windows-v1.json (Join-Path $portable 'NordSSH-data/vault.nordvault')
$version=(Get-Content package.json -Raw | ConvertFrom-Json).version
Copy-Item "artifacts/release/NordSSH-$version-windows-x64.exe" (Join-Path $stage 'payload.exe')
Copy-Item artifacts/baseline/NordSSH.exe (Join-Path $stage 'NordSSH-update-helper.exe')
$request=@{target=(Join-Path $portable 'NordSSH.exe');signature=([IO.File]::ReadAllText((Join-Path $PWD "artifacts/release/NordSSH-$version-windows-x64.exe.sig"))).Trim();version=$version;parent_pid=0}
[IO.File]::WriteAllText((Join-Path $stage 'request.json'),($request | ConvertTo-Json))
$p=Start-Process -FilePath (Join-Path $stage 'NordSSH-update-helper.exe') -ArgumentList '--apply-portable-update' -WindowStyle Hidden -PassThru
try {if(-not $p.WaitForExit(60000)){throw 'Portable update helper timed out'}} finally {if(-not $p.HasExited){Stop-Process -Id $p.Id}}
if(Test-Path (Join-Path $stage 'error.txt')) {throw ([IO.File]::ReadAllText((Join-Path $stage 'error.txt')))}
if((Get-FileHash (Join-Path $portable 'NordSSH.exe')).Hash -ne (Get-FileHash src-tauri/target/release/nordssh.exe).Hash){throw 'Portable executable was not updated'}
if((Get-FileHash (Join-Path $portable 'NordSSH-data/vault.nordvault')).Hash -ne $before){throw 'Portable update changed the vault'}
Start-Sleep -Seconds 2
Get-CimInstance Win32_Process -Filter "name='NordSSH.exe'" | Where-Object { $_.ExecutablePath -ieq (Join-Path $portable 'NordSSH.exe') } | ForEach-Object { Stop-Process -Id $_.ProcessId }
Write-Output 'PASS: signed portable executable update from older helper, restart and preserved encrypted vault.'
