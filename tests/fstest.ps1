# NovaFS hardening test - three boots with different disk states:
#   1. clean image          -> baseline ops (write/cat/fsinfo roundtrip)
#   2. corrupt inode type   -> mount refused, "[FAIL] novafs corrupt metadata"
#   3. corrupt super magic  -> auto-format path, "fresh disk, auto-formatted"
$ErrorActionPreference = 'Continue'
Set-Location (Split-Path $PSScriptRoot -Parent)
$q = 'D:\qemu\qemu-system-i386.exe'
if (-not (Test-Path $q)) { $q = 'qemu-system-i386' }

function bootsession([string]$tag, [scriptblock]$corrupt, [string[]]$cmds, [int]$waitSec = 9) {
    Copy-Item data-seed.img data-nettest.img -Force
    if ($corrupt) { & $corrupt }
    Remove-Item "serial-$tag.log" -Force -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $q -PassThru -ArgumentList @(
        '-drive','format=raw,file=disk.img,if=ide,index=0,media=disk',
        '-drive','format=raw,file=data-nettest.img,if=ide,index=1,media=disk',
        '-m','256','-rtc','base=localtime,clock=host',
        '-serial',"file:serial-$tag.log",
        '-monitor','tcp:127.0.0.1:4444,server,nowait',
        '-nic','user,model=e1000',
        '-display','none'
    )
    Start-Sleep -Seconds $waitSec
    $c = New-Object Net.Sockets.TcpClient('127.0.0.1', 4444)
    $s = $c.GetStream()
    Start-Sleep -Milliseconds 600
    foreach ($cmd in $cmds) {
        $map = @{ ' ' = 'spc'; '.' = 'dot'; '-' = 'minus'; '/' = 'slash' }
        foreach ($ch in $cmd.ToCharArray()) {
            $k = if ($map.ContainsKey([string]$ch)) { $map[[string]$ch] } else { [string]$ch }
            $b = [Text.Encoding]::ASCII.GetBytes("sendkey $k`n")
            $s.Write($b, 0, $b.Length)
            Start-Sleep -Milliseconds 90
        }
        $b = [Text.Encoding]::ASCII.GetBytes("sendkey ret`n")
        $s.Write($b, 0, $b.Length)
        Start-Sleep -Milliseconds 320
    }
    Start-Sleep -Seconds 2
    $b = [Text.Encoding]::ASCII.GetBytes("quit`n")
    $s.Write($b, 0, $b.Length)
    Start-Sleep -Seconds 2
    $c.Close()
    if (-not $p.HasExited) { $p.Kill() }
    Start-Sleep -Milliseconds 500
}

# ---- boot 1: baseline on a clean image ----
bootsession "fs_clean" $null @(
    'root','',
    'write roundtrip.txt','line one of the roundtrip','line two','.','',
    'cat roundtrip.txt',
    'fsinfo'
) | Out-Null

# ---- boot 2: corrupt inode 5 type byte (nsh.nxp) -> mount must refuse ----
bootsession "fs_corrupt" {
    $b = [IO.File]::ReadAllBytes('data-nettest.img')
    $b[(130 * 512) + 5 * 64] = 0x7F          # bogus type on an allocated inode
    [IO.File]::WriteAllBytes('data-nettest.img', $b)
    Write-Host "  corrupted inode 5 type -> 0x7F"
} @('root','') | Out-Null

# ---- boot 3: corrupt superblock magic -> auto-format path ----
bootsession "fs_magic" {
    $b = [IO.File]::ReadAllBytes('data-nettest.img')
    $b[129 * 512 + 0] = 0x00                  # smash the NXFS magic
    [IO.File]::WriteAllBytes('data-nettest.img', $b)
    Write-Host "  smashed superblock magic"
} @('root','','format','fsinfo') | Out-Null

# ---- verdicts ----
$c1 = Get-Content serial-fs_clean.log -Raw -ErrorAction SilentlyContinue
$c2 = Get-Content serial-fs_corrupt.log -Raw -ErrorAction SilentlyContinue
$c3 = Get-Content serial-fs_magic.log -Raw -ErrorAction SilentlyContinue
Write-Host "===== fs hardening checks =====" -ForegroundColor Cyan
$checks = @(
    @{ n = 'baseline: roundtrip echoed';      pat = 'line one of the roundtrip'; log = $c1 },
    @{ n = 'baseline: fsinfo block counts';   pat = 'blocks used';               log = $c1 },
    @{ n = 'corrupt: mount refused';          pat = 'corrupt metadata';          log = $c2 },
    @{ n = 'corrupt: kernel kept running';    pat = 'NovaOS login: root';        log = $c2 },
    @{ n = 'magic smash: auto-formatted';     pat = 'fresh disk, auto-formatted'; log = $c3 },
    @{ n = 'post-format fsinfo works';        pat = 'blocks used';               log = $c3 }
)
$fail = 0
foreach ($k in $checks) {
    $hit = if ($k.log) { $k.log -match $k.pat } else { $false }
    if (-not $hit) { $fail++ }
    Write-Host ("  [{0}] {1}" -f ($(if ($hit) {'OK'} else {'--'})), $k.n) -ForegroundColor $(if ($hit) {'Green'} else {'Yellow'})
}
if ($fail -eq 0) { Write-Host "FSTEST: ALL PASS" -ForegroundColor Green }
else { Write-Host "FSTEST: $fail FAILURES" -ForegroundColor Red }
