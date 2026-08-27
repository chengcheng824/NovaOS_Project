# NovaOS build script
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$BOOT='boot';$KERNEL='kernel';$BUILD='build'
if(!(Test-Path $BUILD)){New-Item -ItemType Directory -Path $BUILD|Out-Null}
function Step($e,$a,$l){Write-Host "==> $l"-ForegroundColor Cyan;&$e @a;if($LASTEXITCODE -ne 0){exit 1}}
Step nasm @('-f','bin','-I',"$BOOT\","$BOOT\stage1.asm","-o","$BUILD\stage1.bin") 'Stage1'
Step nasm @('-f','bin','-I',"$BOOT\","$BOOT\stage2.asm","-o","$BUILD\stage2.bin") 'Stage2'
Step nasm @('-f','win32',"$KERNEL\entry.asm","-o","$BUILD\entry.o") 'entry'
$cc = '-m32','-ffreestanding','-fno-pie','-fno-stack-protector','-fno-asynchronous-unwind-tables','-Os'
Step gcc ($cc + @('-c','-I',$KERNEL,"$KERNEL\kernel.c","-o","$BUILD\kernel.o")) 'kernel.c'
Step gcc ($cc + @('-c','-I',$KERNEL,"$KERNEL\ata.c","-o","$BUILD\ata.o")) 'ata.c'
Step gcc ($cc + @('-c','-I',$KERNEL,"$KERNEL\novafs.c","-o","$BUILD\novafs.o")) 'novafs.c'
Step gcc ($cc + @('-c','-I',$KERNEL,"$KERNEL\acpi.c","-o","$BUILD\acpi.o")) 'acpi.c'
Step gcc ($cc + @('-c','-I',$KERNEL,"$KERNEL\gfx.c","-o","$BUILD\gfx.o")) 'gfx.c'
Step gcc ($cc + @('-c','-I',$KERNEL,"$KERNEL\paging.c","-o","$BUILD\paging.o")) 'paging.c'
Step ld @('-m','i386pe','-Ttext','0x100000','--file-alignment','16','--section-alignment','16','-o',"$BUILD\kernel.elf","$BUILD\entry.o","$BUILD\kernel.o","$BUILD\ata.o","$BUILD\novafs.o","$BUILD\acpi.o","$BUILD\gfx.o","$BUILD\paging.o") 'Link'
Step objcopy @('-O','binary','-j','.text','-j','.rdata','-j','.data',"$BUILD\kernel.elf","$BUILD\kernel.bin") 'objcopy'
$binsize=(Get-Item "$BUILD\kernel.bin").Length
Write-Host ("    kernel.bin = {0} bytes"-f $binsize)-ForegroundColor Gray
# disk image: stage1(1 sector) + stage2(2 sectors) + kernel + NovaFS area.
# NovaFS uses LBA 83+ (superblock/inodes/bitmap/data). Make the image 1 MiB.
$kernelSecs=[Math]::Ceiling($binsize/512.0)
if($kernelSecs -gt 126){ Write-Host "[ERR] kernel.bin ($binsize B) exceeds the 126-sector boot budget (LBA 3..128, NovaFS starts at LBA 129)"-ForegroundColor Red; exit 1 }
$totalSecs=2048
if(3+$kernelSecs+4 -gt $totalSecs){ $totalSecs = 3+$kernelSecs+4 }
$d=New-Object byte[](512*$totalSecs)
[Array]::Copy([IO.File]::ReadAllBytes("$BUILD\stage1.bin"),0,$d,0,(Get-Item "$BUILD\stage1.bin").Length)
[Array]::Copy([IO.File]::ReadAllBytes("$BUILD\stage2.bin"),0,$d,512,(Get-Item "$BUILD\stage2.bin").Length)
[Array]::Copy([IO.File]::ReadAllBytes("$BUILD\kernel.bin"),0,$d,512*3,$binsize)

# ---- compile & inject .nxp programs (programs\*.c) into the NovaFS image ----
if (Test-Path "$PSScriptRoot\programs") {
    $cc = @('-m32','-ffreestanding','-fno-pie','-fno-stack-protector','-fno-asynchronous-unwind-tables','-I',"$PSScriptRoot\programs")
    Step gcc ($cc + @('-c',"$PSScriptRoot\programs\nxp_entry.c",'-o',"$BUILD\nxp_entry.o")) 'nxp_entry.c'

    $SB_OFF  = 129 * 512     # superblock
    $INO_OFF = 130 * 512     # inode table (256 x 64B)
    $BMP_OFF = 162 * 512     # block bitmap
    $DAT_OFF = 170 * 512     # data blocks
    $nextInode = 1
    $nextBlock = 1

    foreach ($src in Get-ChildItem "$PSScriptRoot\programs\*.c" | Where-Object { $_.Name -ne 'nxp_entry.c' }) {
        $name = [IO.Path]::GetFileNameWithoutExtension($src.Name)
        Step gcc ($cc + @('-c',$src.FullName,'-o',"$BUILD\nxp_$name.o")) "$name.c"
        # NOTE on i386pe: -Ttext only sets .text VMA; DEFAULT ImageBase is
        # 0x00400000 which leaves .data/.bss globals at 0x0040???? — way
        # outside our mapped NXP region (0x300000..). Kernel memcpy()s the
        # flat binary to 0x300000 with a 4-byte 'NXP' header, so .text
        # lands at 0x300004 exactly. Setting --image-base 0x300000 forces
        # .bss/.rdata/.data VMA to live inside 0x300??? (ptl user pages).
        Step ld   @('-m','i386pe','--image-base','0x300000','-Ttext','0x300004','--file-alignment','16','--section-alignment','16','-e','_nxp_entry','-o',"$BUILD\nxp_$name.elf","$BUILD\nxp_entry.o","$BUILD\nxp_$name.o") "nxp_$name.elf"
        Step objcopy @('-O','binary',"$BUILD\nxp_$name.elf","$BUILD\nxp_$name.bin") "nxp_$name.bin"

        $code = [IO.File]::ReadAllBytes("$BUILD\nxp_$name.bin")
        $file = New-Object byte[] (4 + $code.Length)
        $file[0]=0x4E; $file[1]=0x58; $file[2]=0x50; $file[3]=0x01
        [Array]::Copy($code,0,$file,4,$code.Length)
        if ($file.Length -gt 6*512) { Write-Host "[ERR] $name.nxp exceeds the 3072 B file limit"-ForegroundColor Red; exit 1 }
        if ($nextInode -ge 256) { Write-Host "[ERR] too many programs"-ForegroundColor Red; exit 1 }

        # inode entry: type=1(T_FILE) parent=0(root) name size blocks[]
        $ino = $INO_OFF + $nextInode * 64
        $d[$ino+0] = 1
        $fname = "$name.nxp"
        $nb = [Text.Encoding]::ASCII.GetBytes($fname)
        if ($nb.Length -gt 23) { Write-Host "[ERR] name too long: $fname"-ForegroundColor Red; exit 1 }
        [Array]::Copy($nb,0,$d,$ino+4,$nb.Length)
        [BitConverter]::GetBytes([uint32]$file.Length).CopyTo($d,$ino+28)
        $nblocks = [Math]::Ceiling($file.Length / 512.0)
        for ($b = 0; $b -lt $nblocks; $b++) {
            $blk = $nextBlock++
            [BitConverter]::GetBytes([uint32]$blk).CopyTo($d,$ino+32+$b*4)
            [Array]::Copy($file,$b*512,$d,$DAT_OFF+$blk*512,[Math]::Min(512,$file.Length-$b*512))
            $d[$BMP_OFF + ($blk -shr 3)] = $d[$BMP_OFF + ($blk -shr 3)] -bor [byte](1 -shl ($blk -band 7))
        }
        Write-Host ("    + $fname ({0} B)" -f $file.Length) -ForegroundColor Gray
        $nextInode++
    }

    if ($nextInode -gt 1) {
        # make the image a valid NovaFS disk: guest mounts instead of formatting
        [BitConverter]::GetBytes([uint32]0x4E584653).CopyTo($d,$SB_OFF+0)    # magic
        [BitConverter]::GetBytes([uint32]32768).CopyTo($d,$SB_OFF+4)         # total_blocks
        [BitConverter]::GetBytes([uint32]256).CopyTo($d,$SB_OFF+8)           # total_inodes
        [BitConverter]::GetBytes([uint32]170).CopyTo($d,$SB_OFF+12)          # first_data_lba
        [BitConverter]::GetBytes([uint32]130).CopyTo($d,$SB_OFF+16)          # first_inode_lba
        $r = $INO_OFF                                                          # root inode 0
        $d[$r+0] = 2                                                           # T_DIR
        $d[$r+4] = [byte][char]'/'                                             # name "/"
        $d[$BMP_OFF] = $d[$BMP_OFF] -bor 1                                     # block 0 reserved
    }
}

[IO.File]::WriteAllBytes("$PSScriptRoot\disk.img",$d)
Write-Host ("    disk.img = {0} bytes ({1} sectors)"-f (512*$totalSecs),$totalSecs)-ForegroundColor Gray
Write-Host "[OK] disk.img built"-ForegroundColor Green
