$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
python tools/build.py @args
exit $LASTEXITCODE
