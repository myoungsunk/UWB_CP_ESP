param(
  [Parameter(Mandatory=$true)][string]$PriorApp,
  [Parameter(Mandatory=$true)][string]$Evidence,
  [Parameter(Mandatory=$true)][string]$RemoteDir
)
$ErrorActionPreference = 'Stop'
if ($RemoteDir -notmatch '^/tmp/uwb-mode8-tag-[a-z0-9-]+$') { throw 'Invalid Pi staging directory' }
if (Test-Path -LiteralPath $Evidence) { throw 'Evidence directory already exists' }
New-Item -ItemType Directory -Path $Evidence | Out-Null
$pi = 'slam-robot-tailscale'
$config = 'D:/SLAM_bot/.codex-robot-ssh-config'
$sshArgs = @('-F',$config,'-o','BatchMode=yes','-o','ConnectTimeout=8')
$port = '/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0'
$mac = '28:56:2f:73:06:54'
$app = (Resolve-Path -LiteralPath 'dist/tag/TAG_DUAL_DS_CIR.ino.bin').Path
$prior = (Resolve-Path -LiteralPath $PriorApp).Path
$manifest = @'
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path.cwd() / 'tools'))
from offline_preflight import scan
checked = scan()
checked = {"common": checked["checks"], "tag": checked["roles"]["tag"]}
print(json.dumps(checked, indent=2))
raise SystemExit(0 if all(checked["common"].values()) and checked["tag"]["flash_preflight_pass"] else 2)
'@
$manifest | py -3 -B -X utf8 - | Tee-Object -FilePath (Join-Path $Evidence 'preflight.json')
if ($LASTEXITCODE -ne 0) { throw 'Tag target preflight failed' }
$priorSha = (Get-FileHash -LiteralPath $prior -Algorithm SHA256).Hash.ToLowerInvariant()
$appSha = (Get-FileHash -LiteralPath $app -Algorithm SHA256).Hash.ToLowerInvariant()
& ssh -T @sshArgs $pi "umask 077; mkdir '$RemoteDir'"
if ($LASTEXITCODE -ne 0) { throw 'Pi staging failed' }
& scp @sshArgs $prior ('{0}:{1}/prior.bin' -f $pi,$RemoteDir)
if ($LASTEXITCODE -ne 0) { throw 'Pi prior image transfer failed' }
& scp @sshArgs $app ('{0}:{1}/new.bin' -f $pi,$RemoteDir)
if ($LASTEXITCODE -ne 0) { throw 'Pi new image transfer failed' }
$common = "cd '$RemoteDir' && printf '%s\n' '$priorSha  prior.bin' '$appSha  new.bin' | sha256sum -c - && python3 -m esptool --chip esp32 --port '$port' --baud 460800"
$log = Join-Path $Evidence 'prior_verify.log'
& ssh -T @sshArgs $pi "$common verify-flash 0x10000 prior.bin" | Tee-Object -FilePath $log
if ($LASTEXITCODE -ne 0 -or -not (Select-String -LiteralPath $log -SimpleMatch $mac -Quiet)) { throw 'Live tag identity or prior flash mismatch' }
$log = Join-Path $Evidence 'write.log'
& ssh -T @sshArgs $pi "$common write-flash --flash-mode keep --flash-freq keep --flash-size keep 0x10000 new.bin" | Tee-Object -FilePath $log
if ($LASTEXITCODE -ne 0 -or -not (Select-String -LiteralPath $log -SimpleMatch $mac -Quiet)) { throw 'Tag flash failed' }
$log = Join-Path $Evidence 'verify.log'
& ssh -T @sshArgs $pi "$common verify-flash 0x10000 new.bin" | Tee-Object -FilePath $log
if ($LASTEXITCODE -ne 0 -or -not (Select-String -LiteralPath $log -SimpleMatch $mac -Quiet)) { throw 'Tag flash verification failed' }
[ordered]@{
  utc = [DateTimeOffset]::UtcNow.ToString('o')
  role = 'tag'
  host = $pi
  port = $port
  mac = $mac
  offset = '0x10000'
  prior_app_sha256 = $priorSha
  new_app_sha256 = $appSha
  remote_staging = $RemoteDir
  verified = $true
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Evidence 'flash_receipt.json') -Encoding utf8
