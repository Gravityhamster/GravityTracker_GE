$f = Join-Path $PSScriptRoot "version.h"
$line = (Get-Content $f) -match 'VERSION_BUILD'
$n = [int]($line -split '\s+')[2]
(Get-Content $f) -replace "VERSION_BUILD\s+\d+", "VERSION_BUILD $($n+1)" | Set-Content $f   