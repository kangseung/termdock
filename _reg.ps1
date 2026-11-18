$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$env:QT_QPA_PLATFORM = 'offscreen'
Set-Location 'D:\git\termdock\build-chk'

$t = & .\termdock-tests.exe 2>&1 | Out-String
$tp = ($t -split "`n" | Select-String 'PASS').Count
$tf = ($t -split "`n" | Select-String 'FAIL').Count
Write-Host "unit tests: PASS=$tp FAIL=$tf"
$t -split "`n" | Select-Object -Last 2

$s = & .\termdock.exe --selftest 2>&1 | Out-String
$sp = ($s -split "`n" | Select-String 'PASS').Count
$sf = ($s -split "`n" | Select-String 'FAIL').Count
Write-Host "selftest: PASS=$sp FAIL=$sf"
$s -split "`n" | Select-Object -Last 2
