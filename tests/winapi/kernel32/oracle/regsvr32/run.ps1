# regsvr32 probe driver: run each case for one bitness and print exit codes
# plus the DLL's log. Usage: run.ps1 <dir-with-dlls> <regsvr32-path>
param($dir, $exe)
$log = Join-Path $env:TEMP 'regsvr32_probe.log'
function case($label, $hr, [string[]]$args2) {
    Remove-Item $log -ErrorAction SilentlyContinue
    if ($hr) { $env:REGDLL_HR = $hr } else { Remove-Item Env:REGDLL_HR -ErrorAction SilentlyContinue }
    $p = Start-Process -FilePath $exe -ArgumentList $args2 -Wait -PassThru -NoNewWindow
    "$label exit=$($p.ExitCode)"
    if (Test-Path $log) { Get-Content $log | ForEach-Object { "  $_" } }
}
case 'register' $null @('/s', "$dir\regdll.dll")
case 'register-fail' '80004005' @('/s', "$dir\regdll.dll")
case 'unregister' $null @('/s', '/u', "$dir\regdll.dll")
case 'unregister-fail' '80004005' @('/s', '/u', "$dir\regdll.dll")
case 'install' $null @('/s', '/i:hello', "$dir\regdll.dll")
case 'install-noreg' $null @('/s', '/n', '/i:hello', "$dir\regdll.dll")
case 'install-nocmd' $null @('/s', '/n', '/i', "$dir\regdll.dll")
case 'uninstall' $null @('/s', '/u', '/i:bye', "$dir\regdll.dll")
case 'install-fail' '80004005' @('/s', '/n', '/i:x', "$dir\regdll.dll")
case 'missing-dll' $null @('/s', "$dir\nope.dll")
case 'no-entry' $null @('/s', "$dir\noentry.dll")
case 'no-args' $null @('/s')
case 'n-without-i' $null @('/s', '/n', "$dir\regdll.dll")
case 'two-dlls' $null @('/s', "$dir\regdll.dll", "$dir\regdll.dll")
case 'bad-switch' $null @('/s', '/q', "$dir\regdll.dll")
case 'relative' $null @('/s', 'regdll.dll')
case 'dash-switch' $null @('-s', "$dir\regdll.dll")
