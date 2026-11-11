param(
    [Parameter(Mandatory = $true)][string]$OriginalApp,
    [Parameter(Mandatory = $true)][string]$Evidence
)

$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$device = 'COM4'
$anchorMac = '28:56:2f:72:e7:18'
$originalSha = 'a9c67adc701eae75a9851f906453e8e6bf851efb5706bab74170bd6c44592eb1'
$esp = 'C:/Users/Myoungsun Kim/AppData/Local/Arduino15/packages/esp32/tools/esptool_py/5.1.0/esptool.exe'
$appName = 'ANCHOR_DUAL_DS_CIR.ino.bin'
$app = Join-Path $workspace "dist/anchor/$appName"
$elf = Join-Path $workspace 'dist/anchor/ANCHOR_DUAL_DS_CIR.ino.elf'

New-Item -ItemType Directory -Path $Evidence | Out-Null
$Evidence = (Resolve-Path -LiteralPath $Evidence).Path
Push-Location $workspace
try {
    $preflight = @'
import json
import sys
from pathlib import Path
from hashlib import sha256

root = Path.cwd()
sys.path.insert(0, str(root / 'tools'))
from source_contract import matches

receipt = json.loads((root / 'dist/target_build_receipt.json').read_text(encoding='utf-8'))
assert receipt['target_build'] is True
assert receipt['fqbn'] == 'esp32:esp32:esp32doit-devkit-v1'
assert receipt['required_core'] == '3.3.7'
role = receipt['roles']['anchor']
assert role['exit_code'] == 0
assert matches(root, 'anchor', role['source_manifest'])
for name in ('ANCHOR_DUAL_DS_CIR.ino.bin', 'ANCHOR_DUAL_DS_CIR.ino.elf'):
    data = (root / 'dist/anchor' / name).read_bytes()
    assert data and role['artifacts'][name] == {'bytes': len(data), 'sha256': sha256(data).hexdigest()}
    print(name, len(data), sha256(data).hexdigest())
print('Source manifest', role['source_manifest']['sha256'])
'@
    $preflight | py -3 -B -X utf8 - | Tee-Object (Join-Path $Evidence 'preflight.log')
    if ($LASTEXITCODE -ne 0) { throw 'Source or build artifact verification failed' }
    if ((Get-FileHash -LiteralPath $OriginalApp -Algorithm SHA256).Hash -ne $originalSha) {
        throw 'Original anchor recovery image hash mismatch'
    }
    & $esp --chip esp32 image-info $app | Tee-Object (Join-Path $Evidence 'image_info.log')
    if ($LASTEXITCODE -ne 0) { throw 'ESP32 application image verification failed' }
    $common = @('--chip','esp32','--port',$device,'--baud','460800')
    $baseline = Join-Path $Evidence 'original_verify.log'
    & $esp @common verify-flash '0x10000' $OriginalApp | Tee-Object $baseline
    if ($LASTEXITCODE -ne 0 -or -not (Select-String -LiteralPath $baseline -SimpleMatch $anchorMac -Quiet)) {
        throw 'Original R5 application or anchor identity mismatch'
    }
    $write = Join-Path $Evidence 'write.log'
    & $esp @common write-flash -z --flash-mode keep --flash-freq keep --flash-size keep '0x10000' $app |
        Tee-Object $write
    if ($LASTEXITCODE -ne 0 -or -not (Select-String -LiteralPath $write -SimpleMatch $anchorMac -Quiet)) {
        throw 'Anchor application write failed or anchor identity mismatch'
    }
    $verified = Join-Path $Evidence 'new_verify.log'
    & $esp @common verify-flash '0x10000' $app | Tee-Object $verified
    if ($LASTEXITCODE -ne 0 -or -not (Select-String -LiteralPath $verified -SimpleMatch $anchorMac -Quiet)) {
        throw 'Written anchor application verification failed'
    }
    [ordered]@{
        utc = [DateTimeOffset]::UtcNow.ToString('o')
        port = $device
        mac = $anchorMac
        flash_offset = '0x10000'
        original_app_sha256 = $originalSha
        written_app_sha256 = (Get-FileHash -LiteralPath $app -Algorithm SHA256).Hash.ToLowerInvariant()
        elf_sha256 = (Get-FileHash -LiteralPath $elf -Algorithm SHA256).Hash.ToLowerInvariant()
        esptool_version = '5.1.0'
        verified = $true
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Evidence 'flash_receipt.json') -Encoding utf8
} finally {
    Pop-Location
}
