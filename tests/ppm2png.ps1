# convert a QEMU screendump (P6 PPM) to PNG via System.Drawing
param([string]$In, [string]$Out)
Add-Type -AssemblyName System.Drawing
$bytes = [IO.File]::ReadAllBytes($In)

# header: "P6\n<w> <h>\n255\n" then raw RGB triples
$hdr = [Text.Encoding]::ASCII.GetString($bytes, 0, [Math]::Min(64, $bytes.Length))
$m = [regex]::Match($hdr, 'P6\s+(\d+)\s+(\d+)\s+(\d+)\s')
if (-not $m.Success) { Write-Host "bad ppm header" -ForegroundColor Red; exit 1 }
$w = [int]$m.Groups[1].Value
$h = [int]$m.Groups[2].Value
$pos = $m.Groups[3].Index + $m.Groups[3].Length + 1

$bmp = New-Object System.Drawing.Bitmap($w, $h)
$rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
$fmt = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
$bd = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $fmt)
$stride = $bd.Stride
$row = New-Object byte[] ($w * 3)
for ($y = 0; $y -lt $h; $y++) {
    [Array]::Copy($bytes, $pos + $y * $w * 3, $row, 0, $w * 3)
    $line = New-Object byte[] $stride
    for ($x = 0; $x -lt $w; $x++) {
        $line[$x * 4 + 0] = $row[$x * 3 + 2]   # B
        $line[$x * 4 + 1] = $row[$x * 3 + 1]   # G
        $line[$x * 4 + 2] = $row[$x * 3 + 0]   # R
        $line[$x * 4 + 3] = 255
    }
    [Runtime.InteropServices.Marshal]::Copy($line, 0, $bd.Scan0 + $y * $stride, $stride)
}
$bmp.UnlockBits($bd)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "wrote $Out ($w x $h)"
