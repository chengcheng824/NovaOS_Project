# NovaOS TUI visual test: boots headless, drives the TUI over the QEMU
# monitor, takes screendumps at each step (PPM converted to PNG).
$ErrorActionPreference = 'Continue'
Set-Location (Split-Path $PSScriptRoot -Parent)

$q = 'D:\qemu\qemu-system-i386.exe'
if (-not (Test-Path $q)) { $q = 'qemu-system-i386' }

Copy-Item data-seed.img data-nettest.img -Force
Remove-Item serial-nettest.log -Force -ErrorAction SilentlyContinue
Remove-Item build\shot*.ppm, build\shot*.png -Force -ErrorAction SilentlyContinue

$proc = Start-Process -FilePath $q -PassThru -ArgumentList @(
    '-drive','format=raw,file=disk.img,if=ide,index=0,media=disk',
    '-drive','format=raw,file=data-nettest.img,if=ide,index=1,media=disk',
    '-m','256','-rtc','base=localtime,clock=host',
    '-serial','file:serial-nettest.log',
    '-monitor','tcp:127.0.0.1:4444,server,nowait',
    '-nic','user,model=e1000',
    '-display','none'
)

Start-Sleep -Seconds 9
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', 4444)
$s = $c.GetStream()
Start-Sleep -Milliseconds 600

function sk([string]$keys) {
    $map = @{ ' ' = 'spc'; '.' = 'dot'; '-' = 'minus' }
    foreach ($ch in $keys.ToCharArray()) {
        $k = if ($map.ContainsKey([string]$ch)) { $map[[string]$ch] } else { [string]$ch }
        $b = [Text.Encoding]::ASCII.GetBytes("sendkey $k`n")
        $s.Write($b, 0, $b.Length)
        Start-Sleep -Milliseconds 110
    }
    $b = [Text.Encoding]::ASCII.GetBytes("sendkey ret`n")
    $s.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 300
}
function shot([string]$name) {
    Start-Sleep -Milliseconds 900
    $b = [Text.Encoding]::ASCII.GetBytes("screendump build\$name.ppm`n")
    $s.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 900
    Write-Host "shot: $name"
}
function rawkey([string]$k, [int]$ms = 200) {
    $b = [Text.Encoding]::ASCII.GetBytes("sendkey $k`n")
    $s.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds $ms
}

sk "root"; sk "nova"; Start-Sleep -Seconds 1

# create a small text file so the viewer has something readable (it will be
# the last directory entry)
sk "write t.txt"
sk "hello from the NovaOS TUI"
sk "second line of text"
sk "third line - viewer demo"
sk "."

sk "run tui.nxp"; Start-Sleep -Seconds 3
shot "shot1_files"                                   # Files panel

# walk to the LAST entry (= t.txt) and open the viewer
for ($i = 0; $i -lt 45; $i++) { rawkey "down" 60 }
shot "shot2_viewer"                                  # after Enter below
rawkey "ret" 1200
shot "shot2_viewer"

rawkey "esc" 800                                     # leave viewer (any key)
shot "shot3_files_back"

rawkey "2" 1500; shot "shot4_tasks"                  # Tasks panel
rawkey "3" 1500; shot "shot5_system"                 # System panel
rawkey "1" 1500                                      # back to Files
rawkey "q" 1500                                      # quit the TUI
shot "shot6_shell"                                   # shell restored

$b = [Text.Encoding]::ASCII.GetBytes("quit`n")
$s.Write($b, 0, $b.Length)
Start-Sleep -Seconds 2
$c.Close()
if (-not $proc.HasExited) { $proc.Kill() }

foreach ($n in @('shot1_files','shot2_viewer','shot3_files_back','shot4_tasks','shot5_system','shot6_shell')) {
    $ppm = "build\$n.ppm"
    if (Test-Path $ppm) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File tests\ppm2png.ps1 -In $ppm -Out "build\$n.png"
    } else { Write-Host "MISSING $ppm" -ForegroundColor Red }
}
Get-Content serial-nettest.log -Tail 12
