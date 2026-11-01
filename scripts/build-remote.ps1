param(
    [string]$RemoteHost = $(if ($env:ENET_REMOTE_HOST) { $env:ENET_REMOTE_HOST } else { 'bit@192.168.32.129' })
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$sshOptions = @('-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes')

$remoteHome = (& ssh @sshOptions $RemoteHost 'pwd') -join ''
if ($LASTEXITCODE -ne 0 -or $remoteHome -notmatch '^/[A-Za-z0-9._/-]+$') {
    throw "Cannot determine a safe home directory on $RemoteHost"
}
$remoteProject = "$remoteHome/.cache/enet-vscode-build/ENET"

& ssh @sshOptions $RemoteHost "mkdir -p '$remoteProject'"
if ($LASTEXITCODE -ne 0) { throw 'Cannot create remote build directory' }

$sourceItems = @(
    'EdoyunNet', 'LoadBanceServer', 'LoginServer', 'RtmpServer',
    'SigServer', 'TCPServer', 'scripts', 'CMakeLists.txt', 'CMakePresets.json'
)

Push-Location $projectRoot
try {
    & scp @sshOptions -q -r @sourceItems "${RemoteHost}:${remoteProject}/"
    if ($LASTEXITCODE -ne 0) { throw 'Cannot copy sources to Linux VM' }
}
finally {
    Pop-Location
}

& ssh @sshOptions $RemoteHost "bash '$remoteProject/scripts/build-linux.sh'"
if ($LASTEXITCODE -ne 0) { throw 'Linux build failed' }

Write-Host "Linux binaries: $RemoteHost`:$remoteProject/build/make-debug/bin/"
