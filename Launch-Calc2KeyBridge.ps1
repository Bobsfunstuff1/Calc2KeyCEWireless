Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

[System.Windows.Forms.Application]::EnableVisualStyles()

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$hostExe = Join-Path $repoRoot 'Calc2KeyCE.BridgeHostWin\build\Release\Calc2KeyCEBridgeHostWin.exe'
$hostWorkingDir = Split-Path -Parent $hostExe
$hostLog = Join-Path $hostWorkingDir 'bridge_host.log'
$leptonicaBin = $env:LEPTONICA_BIN
$defaultKeyPath = Join-Path $HOME '.ssh\calc2key_pi_temp'
$piRelayPath = '/usr/local/bin/Calc2KeyPiRelay'
$piRelayLog = '~/calc2key-bridge.log'
$localPresetPath = Join-Path $repoRoot 'bridge-host.local.env'
$relayPresets = @()

function Get-LocalRelayPresets {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        return @()
    }

    $values = @{}
    foreach ($line in Get-Content $Path -ErrorAction SilentlyContinue) {
        $trimmed = $line.Trim()
        if (-not $trimmed -or $trimmed.StartsWith('#') -or -not $trimmed.Contains('=')) {
            continue
        }

        $parts = $trimmed.Split('=', 2)
        $values[$parts[0].Trim()] = $parts[1].Trim()
    }

    $presets = @()
    foreach ($index in 1..2) {
        $label = $values["CALC2KEY_PRESET${index}_LABEL"]
        if (-not $label) {
            continue
        }

        $presets += [pscustomobject]@{
            Label = $label
            PiHost = $values["CALC2KEY_PRESET${index}_PI_HOST"]
            PiUser = $values["CALC2KEY_PRESET${index}_PI_USER"]
            SshKeyPath = $values["CALC2KEY_PRESET${index}_SSH_KEY_PATH"]
            BridgeHostIp = $values["CALC2KEY_PRESET${index}_BRIDGE_HOST_IP"]
        }
    }

    return $presets
}

$relayPresets = Get-LocalRelayPresets -Path $localPresetPath

function Test-TcpPort {
    param(
        [string]$HostName,
        [int]$Port,
        [int]$TimeoutMs = 1200
    )

    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $iar = $client.BeginConnect($HostName, $Port, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne($TimeoutMs, $false)) {
            return $false
        }
        $client.EndConnect($iar) | Out-Null
        return $true
    } catch {
        return $false
    } finally {
        $client.Close()
    }
}

function Wait-ForHostReady {
    param(
        [int]$Port,
        [int]$TimeoutSeconds = 10
    )

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    while ($stopwatch.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $proc = Get-Process Calc2KeyCEBridgeHostWin -ErrorAction SilentlyContinue
        if (-not $proc) {
            throw 'Calc2KeyCE Bridge Host exited before it started listening.'
        }

        if (Test-TcpPort -HostName '127.0.0.1' -Port $Port -TimeoutMs 400) {
            return
        }

        if (Test-Path $hostLog) {
            $lines = Get-Content $hostLog -ErrorAction SilentlyContinue
            if ($lines -match 'listening on port') {
                return
            }
        }

        Start-Sleep -Milliseconds 300
    }

    throw "Calc2KeyCE Bridge Host did not begin listening on port $Port in time."
}

function New-Label {
    param([string]$Text, [int]$X, [int]$Y, [int]$Width = 120)
    $label = New-Object System.Windows.Forms.Label
    $label.Text = $Text
    $label.Location = New-Object System.Drawing.Point($X, $Y)
    $label.Size = New-Object System.Drawing.Size($Width, 20)
    return $label
}

function New-TextBox {
    param([string]$Text, [int]$X, [int]$Y, [int]$Width = 280)
    $box = New-Object System.Windows.Forms.TextBox
    $box.Text = $Text
    $box.Location = New-Object System.Drawing.Point($X, $Y)
    $box.Size = New-Object System.Drawing.Size($Width, 23)
    return $box
}

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Calc2Key Bridge Launcher'
$form.Size = New-Object System.Drawing.Size(540, 420)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false

$title = New-Object System.Windows.Forms.Label
$title.Text = 'Start the Windows host and Pi relay from one place'
$title.Font = New-Object System.Drawing.Font('Segoe UI', 11, [System.Drawing.FontStyle]::Bold)
$title.Location = New-Object System.Drawing.Point(16, 14)
$title.Size = New-Object System.Drawing.Size(480, 24)
$form.Controls.Add($title)

$form.Controls.Add((New-Label 'Pi SSH Host' 16 54))
$piHostBox = New-TextBox '' 150 52 340
$form.Controls.Add($piHostBox)

$form.Controls.Add((New-Label 'SSH User' 16 86))
$defaultPiUser = if ($env:CALC2KEY_PI_USER) { $env:CALC2KEY_PI_USER } elseif ($env:USERNAME) { $env:USERNAME } else { '' }
$userBox = New-TextBox $defaultPiUser 150 84 180
$form.Controls.Add($userBox)

$form.Controls.Add((New-Label 'SSH Key Path' 16 118))
$keyPathBox = New-TextBox $defaultKeyPath 150 116 340
$form.Controls.Add($keyPathBox)

$form.Controls.Add((New-Label 'Bridge Host IP' 16 150))
$bridgeHostBox = New-TextBox '' 150 148 180
$form.Controls.Add($bridgeHostBox)

$presetX = 340
foreach ($preset in $relayPresets) {
    $presetButton = New-Object System.Windows.Forms.Button
    $presetButton.Text = $preset.Label
    $presetButton.Tag = $preset
    $presetButton.Location = New-Object System.Drawing.Point($presetX, 146)
    $presetButton.Size = New-Object System.Drawing.Size(72, 28)
    $presetButton.Add_Click({
        $selectedPreset = $this.Tag
        if ($selectedPreset.PiHost) { $piHostBox.Text = $selectedPreset.PiHost }
        if ($selectedPreset.PiUser) { $userBox.Text = $selectedPreset.PiUser }
        if ($selectedPreset.SshKeyPath) { $keyPathBox.Text = $selectedPreset.SshKeyPath }
        if ($selectedPreset.BridgeHostIp) { $bridgeHostBox.Text = $selectedPreset.BridgeHostIp }
    })
    $form.Controls.Add($presetButton)
    $presetX += 78
}

$form.Controls.Add((New-Label 'Bridge Port' 16 182))
$portBox = New-TextBox '28400' 150 180 100
$form.Controls.Add($portBox)

$statusBox = New-Object System.Windows.Forms.TextBox
$statusBox.Location = New-Object System.Drawing.Point(16, 220)
$statusBox.Size = New-Object System.Drawing.Size(474, 132)
$statusBox.Multiline = $true
$statusBox.ReadOnly = $true
$statusBox.ScrollBars = 'Vertical'
$statusBox.Text = "Ready.`r`n"
$form.Controls.Add($statusBox)

function Set-Status {
    param([string]$Message)
    $statusBox.AppendText($Message + [Environment]::NewLine)
}

$startButton = New-Object System.Windows.Forms.Button
$startButton.Text = 'Start Bridge'
$startButton.Location = New-Object System.Drawing.Point(16, 360)
$startButton.Size = New-Object System.Drawing.Size(120, 28)
$form.Controls.Add($startButton)

$stopButton = New-Object System.Windows.Forms.Button
$stopButton.Text = 'Stop Bridge'
$stopButton.Location = New-Object System.Drawing.Point(148, 360)
$stopButton.Size = New-Object System.Drawing.Size(120, 28)
$form.Controls.Add($stopButton)

$shellButton = New-Object System.Windows.Forms.Button
$shellButton.Text = 'Open SSH Shell'
$shellButton.Location = New-Object System.Drawing.Point(280, 360)
$shellButton.Size = New-Object System.Drawing.Size(120, 28)
$form.Controls.Add($shellButton)

$logButton = New-Object System.Windows.Forms.Button
$logButton.Text = 'Open Host Log'
$logButton.Location = New-Object System.Drawing.Point(412, 360)
$logButton.Size = New-Object System.Drawing.Size(110, 28)
$form.Controls.Add($logButton)

$shellButton.Add_Click({
    $sshArgs = @()
    if ($keyPathBox.Text.Trim()) {
        $sshArgs += '-i'
        $sshArgs += $keyPathBox.Text.Trim()
    }
    $sshArgs += "$($userBox.Text.Trim())@$($piHostBox.Text.Trim())"
    Start-Process powershell -ArgumentList @('-NoExit', '-Command', ('ssh ' + ($sshArgs | ForEach-Object {
        if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
    }) -join ' '))
})

$logButton.Add_Click({
    if (Test-Path $hostLog) {
        Start-Process notepad.exe $hostLog
    } else {
        [System.Windows.Forms.MessageBox]::Show('Host log not found yet.', 'Calc2Key Bridge Launcher')
    }
})

$stopButton.Add_Click({
    try {
        Get-Process Calc2KeyCEBridgeHostWin -ErrorAction SilentlyContinue | Stop-Process -Force
        $sshArgs = @()
        if ($keyPathBox.Text.Trim()) {
            $sshArgs += '-i'
            $sshArgs += $keyPathBox.Text.Trim()
        }
        $sshArgs += "$($userBox.Text.Trim())@$($piHostBox.Text.Trim())"
        $sshArgs += 'sudo pkill -f Calc2KeyPiRelay >/dev/null 2>&1 || true'
        & ssh @sshArgs | Out-Null
        Set-Status 'Stopped Windows host and requested Pi relay shutdown.'
    } catch {
        Set-Status "Stop failed: $($_.Exception.Message)"
    }
})

$startButton.Add_Click({
    try {
        if (-not (Test-Path $hostExe)) {
            throw "Windows host not found at $hostExe"
        }
        if ($keyPathBox.Text.Trim() -and -not (Test-Path $keyPathBox.Text.Trim())) {
            throw "SSH key not found at $($keyPathBox.Text.Trim())"
        }

        $port = [int]$portBox.Text.Trim()
        $piHost = $piHostBox.Text.Trim()
        $sshUser = $userBox.Text.Trim()
        $bridgeHost = $bridgeHostBox.Text.Trim()
        $keyPath = $keyPathBox.Text.Trim()

        Get-Process Calc2KeyCEBridgeHostWin -ErrorAction SilentlyContinue | Stop-Process -Force
        Set-Status "Starting Windows host on port $port..."

        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $hostExe
        $psi.WorkingDirectory = $hostWorkingDir
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $false
        if ($leptonicaBin) {
            $psi.EnvironmentVariables['PATH'] = $leptonicaBin + ';' + [System.Environment]::GetEnvironmentVariable('PATH')
        }
        [System.Diagnostics.Process]::Start($psi) | Out-Null

        Wait-ForHostReady -Port $port
        Set-Status "Windows host is listening. Starting Pi relay to ${bridgeHost}:$port..."

        $sshArgs = @()
        if ($keyPath) {
            $sshArgs += '-i'
            $sshArgs += $keyPath
        }
        $sshArgs += "$sshUser@$piHost"
        $remoteCommand = "bash -lc ""sudo pkill -f Calc2KeyPiRelay >/dev/null 2>&1; setsid sudo $piRelayPath --bridge ${bridgeHost}:$port > $piRelayLog 2>&1 < /dev/null & disown"""
        $sshArgs += $remoteCommand

        $startOutput = & ssh @sshArgs 2>&1
        if ($startOutput) {
            Set-Status ($startOutput -join [Environment]::NewLine)
        } else {
            Set-Status 'Pi relay start command sent.'
        }
        Start-Sleep -Seconds 2

        $verifyArgs = @()
        if ($keyPath) {
            $verifyArgs += '-i'
            $verifyArgs += $keyPath
        }
        $verifyArgs += "$sshUser@$piHost"
        $verifyArgs += "pgrep -af Calc2KeyPiRelay || true; tail -n 5 $piRelayLog || true"
        $verifyOutput = & ssh @verifyArgs
        if ($verifyOutput) {
            Set-Status ($verifyOutput -join [Environment]::NewLine)
        } else {
            Set-Status 'Bridge verification returned no output.'
        }
    } catch {
        Set-Status "Start failed: $($_.Exception.Message)"
    }
})

[void]$form.ShowDialog()
