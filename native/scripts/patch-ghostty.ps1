param([Parameter(Mandatory = $true)][string]$Source)
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath $Source).Path
$taskPin = '08a9518340e19487bd5fdf96963583fb32f0c41f'
$taskHead = git -C $taskRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $taskHead -ne $taskPin) { throw 'Unexpected Ghostty commit; review patch before repinning' }
$taskChanges = git -C $taskRoot status --porcelain --untracked-files=no
if ($LASTEXITCODE -ne 0 -or $taskChanges) { throw 'Ghostty tracked files are not pristine; refusing to overwrite changes' }
$taskPatch = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../patches/ghostty-tinyio-readlink.patch')).Path
git -C $taskRoot apply --check --recount --whitespace=error $taskPatch
if ($LASTEXITCODE -ne 0) { throw 'Ghostty patch check failed' }
git -C $taskRoot apply --recount --whitespace=error $taskPatch
if ($LASTEXITCODE -ne 0) { throw 'Ghostty patch application failed' }
Write-Output 'Applied reviewed TinyIo readlink fix and regression to pinned Ghostty checkout'
