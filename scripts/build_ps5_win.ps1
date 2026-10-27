# Builds the console ELF on Windows, without make or WSL.
#
# Same sources, CFLAGS and libraries as `make ps5` (read from ps5.mk), built
# with LLVM for Windows (clang + ld.lld; 18.x, like CI) against the
# ps5-payload-sdk. The compile and link arguments are the ones the SDK's
# bin/prospero-clang and bin/prospero-lld add on Linux (the path CI takes),
# so the ELF matches the CI build. Then scripts/check_imports.sh runs as in
# ps5.mk.
#
#   powershell -File scripts\build_ps5_win.ps1 -Sdk C:\path\to\ps5-payload-sdk
#   ... -ExtraCFlags -DBQD_LOG_TO_FILE=1      (like make ps5 CFLAGS+=...)
param(
  [string]$Sdk = $env:PS5_PAYLOAD_SDK,
  [string]$Llvm = "C:\Program Files\LLVM\bin",
  [string[]]$ExtraCFlags = @()
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$oldPath = $env:PATH
Push-Location $root
try {
  if (-not $Sdk) { throw "Pass -Sdk or set PS5_PAYLOAD_SDK to the ps5-payload-sdk folder" }
  $Sdk = (Resolve-Path -LiteralPath $Sdk).Path
  $clang = Join-Path $Llvm "clang.exe"
  $lld = Join-Path $Llvm "ld.lld.exe"
  $readobj = Join-Path $Llvm "llvm-readobj.exe"
  foreach ($t in $clang, $lld, $readobj, "$Sdk\win\prospero-lld.exe") {
    if (-not (Test-Path -LiteralPath $t)) { throw "Missing $t (winget install LLVM.LLVM --version 18.1.8)" }
  }
  $bash = Join-Path (Split-Path (Split-Path (Get-Command git).Source)) "bin\bash.exe"
  if (-not (Test-Path -LiteralPath $bash)) { throw "Git Bash not found (needed for check_imports.sh)" }

  # Sources, CFLAGS and LDLIBS straight from ps5.mk.
  $mk = Get-Content ps5.mk
  $srcs = @()
  $inSrcs = $false
  foreach ($l in $mk) {
    if ($l -match '^SRCS\s*:=') { $inSrcs = $true; continue }
    if (-not $inSrcs) { continue }
    $srcs += @(($l -replace '\\\s*$', '').Trim() -split '\s+' | Where-Object { $_ })
    if ($l -notmatch '\\\s*$') { break }
  }
  $cflags = @((($mk | Where-Object { $_ -match '^CFLAGS\s*:=' }) -replace '^CFLAGS\s*:=\s*', '').Trim() -split '\s+')
  $ldlibs = @((($mk | Where-Object { $_ -match '^LDLIBS\s*\+=' }) -replace '^LDLIBS\s*\+=\s*', '').Trim() -split '\s+')
  $version = [regex]::Match((Get-Content -Raw src\version.h), '#define HEARBRIDGE_VERSION "([^"]+)"').Groups[1].Value
  if (-not $srcs -or -not $version) { throw "Could not read SRCS / version from ps5.mk and src\version.h" }

  # What bin/prospero-clang puts around every call (clang < 20: crt1.o is
  # passed by hand). clang 18 always links PS5 through "prospero-lld", so
  # the SDK's win\prospero-lld.exe (which runs ld.lld) goes on PATH; it adds
  # -m elf_x86_64 -pie --eh-frame-hdr -z max-page-size=0x4000 and the
  # linker script, but not the last two flags bin/prospero-lld passes.
  $pre = @('--start-no-unused-arguments', '-target', 'x86_64-sie-ps5',
           '-fvisibility-nodllstorageclass=default',
           '-isysroot', $Sdk, '-isystem', "$Sdk\target\include",
           '-L', "$Sdk\target\lib", '-L', "$Sdk\target\user\homebrew\lib",
           '-fno-stack-protector', '-fno-plt', '-femulated-tls', '-lc',
           '--end-no-unused-arguments')
  $post = @('--start-no-unused-arguments', '--sysroot', $Sdk)
  $linkLibs = @("$Sdk\target\lib\crt1.o", '-lkernel_web', '-lSceLibcInternal', '-lSceNet')
  $lldFlags = @('-Wl,-mllvm,-emulated-tls', '-Wl,--hash-style=gnu')
  $env:PATH = "$Sdk\win;$Llvm;$env:PATH"

  $build = "build\ps5-win"
  $objs = @()
  Write-Host ("Compiling {0} files ({1})" -f $srcs.Count, (& $clang --version | Select-Object -First 1))
  foreach ($src in $srcs) {
    $obj = Join-Path $build (($src -replace '^src/', '') -replace '\.c$', '.o')
    New-Item -ItemType Directory -Force -Path (Split-Path $obj) | Out-Null
    & $clang @pre @cflags @ExtraCFlags -c -o $obj $src @post '--end-no-unused-arguments'
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $src" }
    $objs += $obj
  }

  New-Item -ItemType Directory -Force -Path dist | Out-Null
  $elf = "dist\BossQuickDock-$version.elf"
  Write-Host "Linking $elf"
  & $clang @pre @lldFlags -o $elf @objs @ldlibs @post @linkLibs '--end-no-unused-arguments'
  if ($LASTEXITCODE -ne 0) { throw "Link failed" }

  # LLVM for Windows ships llvm-readobj only; under the name llvm-readelf it
  # prints readelf's format, which check_imports.sh parses.
  $readelf = Join-Path $build "llvm-readelf.exe"
  Copy-Item -LiteralPath $readobj -Destination $readelf -Force
  $env:READELF = (Resolve-Path -LiteralPath $readelf).Path -replace '\\', '/'
  & $bash scripts/check_imports.sh ($elf -replace '\\', '/') scripts/imports-1.0.2.txt
  if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $elf; throw "check_imports failed, ELF removed" }
  Write-Host ("OK {0} ({1} bytes)" -f $elf, (Get-Item -LiteralPath $elf).Length)
} finally {
  $env:PATH = $oldPath
  Remove-Item Env:READELF -ErrorAction SilentlyContinue
  Pop-Location
}
