param(
    [Parameter(Mandatory=$true)] [string]$OrigCpio,
    [Parameter(Mandatory=$true)] [string]$NewInit,
    [Parameter(Mandatory=$true)] [string]$OutCpio
)

$ErrorActionPreference = "Stop"

$b = [System.IO.File]::ReadAllBytes($OrigCpio)
$initBytes = [System.IO.File]::ReadAllBytes($NewInit)

$ms = New-Object System.IO.MemoryStream
$off = 0
$replaced = $false

while ($off + 110 -le $b.Length) {
    $hdr = [System.Text.Encoding]::ASCII.GetString($b, $off, 110)
    if ($hdr.Substring(0, 6) -ne "070701") {
        throw "Bad cpio magic at offset $off"
    }
    $fsize = [Convert]::ToInt32($hdr.Substring(54, 8), 16)
    $nsize = [Convert]::ToInt32($hdr.Substring(94, 8), 16)
    $name = [System.Text.Encoding]::ASCII.GetString($b, $off + 110, $nsize - 1)

    $hdrPad = (110 + $nsize + 3) -band (-4)
    $datPad = ($fsize + 3) -band (-4)

    if ($name -eq "init") {
        $newHdrStr = $hdr.Substring(0, 54) + $initBytes.Length.ToString("X8") + $hdr.Substring(62)
        $newHdrBytes = [System.Text.Encoding]::ASCII.GetBytes($newHdrStr)
        $ms.Write($newHdrBytes, 0, 110)
        $ms.Write($b, $off + 110, $hdrPad - 110)
        $ms.Write($initBytes, 0, $initBytes.Length)
        $newDatPad = ($initBytes.Length + 3) -band (-4)
        $padCount = $newDatPad - $initBytes.Length
        for ($p = 0; $p -lt $padCount; $p++) { $ms.WriteByte(0) }
        $replaced = $true
        Write-Host "Replaced 'init': old=$fsize bytes -> new=$($initBytes.Length) bytes"
    } else {
        $ms.Write($b, $off, $hdrPad + $datPad)
    }

    $off += $hdrPad + $datPad
    if ($name -eq "TRAILER!!!") {
        break
    }
}

if (-not $replaced) {
    throw "Entry 'init' not found in CPIO"
}

$rem = $ms.Length % 512
if ($rem -ne 0) {
    for ($p = 0; $p -lt (512 - $rem); $p++) { $ms.WriteByte(0) }
}

[System.IO.File]::WriteAllBytes($OutCpio, $ms.ToArray())
Write-Host "Wrote $OutCpio ($($ms.Length) bytes)"
