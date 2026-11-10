$ErrorActionPreference='Stop'
Set-Location 'C:\oracle\vitreo-readonly-fixtures'
$dxc='C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe'
foreach($entry in @('ps32','ps16','ps64','psnested')) {
 & $dxc -nologo -T ps_6_2 -E $entry -enable-16bit-types -O3 -Fo "$entry.dxil" -Fc "$entry.dxil.txt" readonly.hlsl
 if($LASTEXITCODE -ne 0){throw "FAILED $entry"}; Write-Output "COMPILED $entry"
}
& $dxc -nologo -T hs_6_0 -E hs -O3 -Fo hs.dxil -Fc hs.dxil.txt hull.hlsl
if($LASTEXITCODE -ne 0){throw 'FAILED hs'}; Write-Output 'COMPILED hs'
