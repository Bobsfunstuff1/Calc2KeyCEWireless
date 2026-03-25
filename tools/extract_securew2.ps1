$src = if ($args.Count -ge 1) { $args[0] } else { Join-Path $HOME 'Downloads\SecureW2_JoinNow.run' }
$dst = if ($args.Count -ge 2) { $args[1] } else { Join-Path $HOME 'Downloads\SecureW2_JoinNow_extracted' }

if (-not (Test-Path $src)) {
    throw "Source archive not found: $src"
}

Remove-Item $dst -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $dst | Out-Null

$bytes = [IO.File]::ReadAllBytes($src)
$marker = [Text.Encoding]::ASCII.GetBytes("#ARCHIVE#`n")
$start = -1

for ($i = 0; $i -le $bytes.Length - $marker.Length; $i++) {
    $ok = $true
    for ($j = 0; $j -lt $marker.Length; $j++) {
        if ($bytes[$i + $j] -ne $marker[$j]) {
            $ok = $false
            break
        }
    }
    if ($ok) {
        $start = $i + $marker.Length
        break
    }
}

if ($start -lt 0) {
    throw 'marker not found'
}

$archive = Join-Path $dst 'archive.tar.gz'
[IO.File]::WriteAllBytes($archive, $bytes[$start..($bytes.Length - 1)])
tar -xzf $archive -C $dst
Get-ChildItem $dst -Recurse | Select-Object FullName, Length
