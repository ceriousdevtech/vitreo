Set-Location 'C:\oracle\vitreo-readonly-fixtures'
& 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe' -nologo -T ps_6_2 -E pslarge -O3 -Fo pslarge.dxil -Fc pslarge.dxil.txt large.hlsl
if($LASTEXITCODE -ne 0){throw 'FAILED pslarge'}; Write-Output 'COMPILED pslarge'
& 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe' -nologo -T cs_6_2 -E cslarge -O3 -Fo cslarge.dxil -Fc cslarge.dxil.txt large.hlsl
if($LASTEXITCODE -ne 0){throw 'FAILED cslarge'}; Write-Output 'COMPILED cslarge'
