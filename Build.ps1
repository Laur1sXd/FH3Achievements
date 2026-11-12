param(
    [string]$ToolchainBin = "",
    [string]$OutDir = "$PSScriptRoot\build",
    [switch]$SmokeTest
)
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($ToolchainBin)) {
    $c = (Get-Command x86_64-w64-mingw32-clang.exe).Source
    $cpp = (Get-Command x86_64-w64-mingw32-clang++.exe).Source
} else {
    $c = Join-Path $ToolchainBin 'x86_64-w64-mingw32-clang.exe'
    $cpp = Join-Path $ToolchainBin 'x86_64-w64-mingw32-clang++.exe'
}
$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$src = Join-Path $PSScriptRoot 'src'
$sqlite = Join-Path $PSScriptRoot 'third_party\sqlite'
$sdk = Join-Path $PSScriptRoot 'sdk'
$obj = Join-Path $OutDir 'sqlite3.o'
$dll = Join-Path $OutDir 'FH3Achievements.dll'
& $c -c -O2 -DNDEBUG -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0 -DSQLITE_DQS=0 "$sqlite\sqlite3.c" -o $obj
if ($LASTEXITCODE) { throw 'SQLite build failed.' }
$common = @('-std=c++20', '-fms-extensions', '-O2', '-static', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_WIN32_WINNT=0x0A00',
            '-Wall', '-Wextra', '-Werror', "-I$src", "-I$sdk", "-I$sqlite")
& $cpp @common -shared "$src\mod.cpp" "$src\journal.cpp" "$src\journal_xaml.cpp" "$src\overlay.cpp" "$src\import_notice.cpp" "$src\events.cpp" "$src\rules.cpp" "$src\save.cpp" $obj -lbcrypt -luser32 -lgdi32 -o $dll
if ($LASTEXITCODE) { throw 'FH3 Achievements build failed.' }
if ($SmokeTest) {
    & $cpp @common -municode "$PSScriptRoot\tests\smoke.cpp" "$src\rules.cpp" "$src\save.cpp" $obj -lbcrypt -o "$OutDir\smoke.exe"
    if ($LASTEXITCODE) { throw 'Smoke test build failed.' }
}
