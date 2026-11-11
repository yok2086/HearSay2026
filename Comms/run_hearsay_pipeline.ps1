# Windows (PowerShell) version of run_hearsay_pipeline.sh.
#
# Run from the repo root in PowerShell:
#   powershell -ExecutionPolicy Bypass -File .\Comms\run_hearsay_pipeline.ps1
#
# Needs: Windows OpenSSH client (built into Windows 10/11), Python 3 with
# paho-mqtt (py -m pip install paho-mqtt), and SSH keys set up for stujump and
# the Ultra96. Windows OpenSSH has no connection sharing, so every step opens
# its own SSH connection; without keys you are asked for passwords each time.
#
# Differences from the .sh version:
#   - coordinator / dashboard / stats open in three separate console windows
#     instead of a tmux session.
#   - The tunnel runs in its own minimised window instead of ssh -f.
#   - Press Enter in this window (or Ctrl+C) to stop everything, instead of
#     detaching from tmux.

$ErrorActionPreference = "Stop"

$JumpHost  = "nathan@stujump.comp.nus.edu.sg"
$UltraHost = "xilinx@makerslab-fpga-36.ddns.comp.nus.edu.sg"

$RemoteMqttDir   = "/home/xilinx/mqtt"
$RemoteConf      = "$RemoteMqttDir/mosquitto.conf"
$RemoteLogDir    = "/home/xilinx/logs"
$RemoteBrokerLog = "$RemoteLogDir/mosquitto.log"

# Dummy Inference
# $RemoteInferenceScript = "/home/xilinx/dummy_inference.py"; $InferenceMatch = "[d]ummy_inference.py"

# TCN inference on the DPU (two gloves)
$RemoteInferenceScript = "/home/xilinx/jupyter_notebooks/tcn/tcn_handler.py"; $InferenceMatch = "[t]cn_handler.py"

# Dummy Phone sender
# $RemoteInferenceScript = "/home/xilinx/dummy_phone_sender.py"; $InferenceMatch = "[d]ummy_phone_sender.py"

# Sensor Inference
# $RemoteInferenceScript = "/home/xilinx/ultra96_inference.py"; $InferenceMatch = "[u]ltra96_inference.py"

# Keyboard pipeline test
# $RemoteInferenceScript = "/home/xilinx/keyboard_ultra96.py"; $InferenceMatch = "[k]eyboard_ultra96.py"

$RemoteInferenceLog = "$RemoteLogDir/ultra96_inference.log"

$RemotePython = "sudo -n /usr/local/share/pynq-venv/bin/python3 -u"
# $RemotePython = "python3 -u"

$LocalBind  = "0.0.0.0"
$LocalPort  = 8883
$RemotePort = 8883
# Hotspot backup: gloves/headset on the same hotspot send plain MQTT here
$RelayPort  = 1883

$LocalDir = $PSScriptRoot   # the Comms folder this script lives in

# Leave empty to auto-detect ("py -3" if the Python launcher exists, else "python").
$PythonCmd = ""

$ToolScripts = @("coordinator.py", "dashboard.py", "stats.py")
# Keyboard pipeline test: also run the relay
# $ToolScripts += "keyboard_relay.py"


function Invoke-Ultra([string]$Command) {
    # Single quotes only inside $Command: Windows PowerShell mangles embedded double quotes.
    $output = & ssh -J $JumpHost $UltraHost $Command
    return $output
}

function Stop-PortListener([int]$Port) {
    $owners = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty OwningProcess -Unique
    foreach ($procId in $owners) {
        if ($procId -gt 0) { Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue }
    }
}

$script:TunnelProcess = $null
$script:ToolProcesses = @()

function Invoke-Cleanup {
    Write-Host ""
    Write-Host "=== Shutting everything down ==="

    Write-Host "Killing broker + inference script on the Ultra96..."
    try {
        Invoke-Ultra ("pkill -f '[m]osquitto -c $RemoteConf' || true; " +
                      "pkill -f '[u]ltra96_inference.py' || true; " +
                      "pkill -f '[d]ummy_inference.py' || true; " +
                      "pkill -f '[d]ummy_phone_sender.py' || true; " +
                      "pkill -f '[k]eyboard_ultra96.py' || true; " +
                      "sudo -n pkill -f '[t]cn_handler.py' || true") 2>$null | Out-Null
    } catch { }

    Write-Host "Closing local relay tunnel..."
    if ($script:TunnelProcess -and -not $script:TunnelProcess.HasExited) {
        Stop-Process -Id $script:TunnelProcess.Id -Force -ErrorAction SilentlyContinue
    }
    Stop-PortListener $LocalPort
    Stop-PortListener $RelayPort

    Write-Host "Closing coordinator / dashboard / stats windows..."
    foreach ($p in $script:ToolProcesses) {
        if (-not $p.HasExited) { & taskkill /T /F /PID $p.Id 2>$null | Out-Null }
    }

    Write-Host "Done. Everything stopped (Ultra96 included)."
}


try {
    if (-not (Get-Command ssh -ErrorAction SilentlyContinue)) {
        throw "ssh not found. Install it in Settings > System > Optional features > OpenSSH Client."
    }
    if (-not $PythonCmd) {
        if (Get-Command py -ErrorAction SilentlyContinue) { $PythonCmd = "py -3" }
        elseif (Get-Command python -ErrorAction SilentlyContinue) { $PythonCmd = "python" }
        else { throw "Python 3 not found. Install it from python.org, then run: py -m pip install paho-mqtt" }
    }

    Write-Host "=== 1. Checking/starting broker on Ultra96 ==="
    $alreadyRunning = Invoke-Ultra "pgrep -f '[m]osquitto -c $RemoteConf' || true"

    if ($alreadyRunning) {
        Write-Host "Broker already running (pid $alreadyRunning) - skipping start, going straight to local scripts."
    } else {
        Write-Host "Starting broker..."
        Invoke-Ultra "mkdir -p $RemoteLogDir; setsid nohup mosquitto -c $RemoteConf > $RemoteBrokerLog 2>&1 < /dev/null &" | Out-Null
        Start-Sleep -Seconds 2
    }

    Write-Host ""
    Write-Host "=== 2. Starting DPU inference script on Ultra96 ==="
    $inferenceRunning = Invoke-Ultra "pgrep -f '$InferenceMatch' || true"

    if ($inferenceRunning) {
        Write-Host "Inference script already running (pid $inferenceRunning) - skipping start, going straight to local scripts."
    } else {
        Write-Host "Starting inference script..."
        Invoke-Ultra "mkdir -p $RemoteLogDir; setsid nohup $RemotePython $RemoteInferenceScript > $RemoteInferenceLog 2>&1 < /dev/null &" | Out-Null
        Start-Sleep -Seconds 2
    }

    Write-Host ""
    Write-Host "=== 3. Opening local relay tunnel (background) ==="
    # Kill any stale tunnel already holding this port before opening a fresh one
    Stop-PortListener $LocalPort
    Stop-PortListener $RelayPort
    Start-Sleep -Seconds 1

    $tunnelArgs = "-N -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 " +
                  "-J $JumpHost -L ${LocalBind}:${LocalPort}:localhost:${RemotePort} " +
                  "-L ${LocalBind}:${RelayPort}:localhost:1883 $UltraHost"
    $script:TunnelProcess = Start-Process -FilePath "ssh" -ArgumentList $tunnelArgs `
        -WindowStyle Minimized -PassThru
    Start-Sleep -Seconds 3
    if ($script:TunnelProcess.HasExited) {
        throw "Tunnel failed to start (port $LocalPort in use, or SSH login failed)."
    }
    Write-Host "Tunnel opened in background (this PC ${LocalPort} -> Ultra96 broker)."
    Write-Host "Hotspot relay open on port ${RelayPort} for devices on the same network."

    Write-Host ""
    Write-Host "=== 4. Launching $($ToolScripts -join ', ') in separate windows ==="
    foreach ($tool in $ToolScripts) {
        $title = [System.IO.Path]::GetFileNameWithoutExtension($tool)
        # cmd /k keeps the window open after the script exits, like the tmux "Press enter" prompt.
        $script:ToolProcesses += Start-Process -FilePath "cmd.exe" `
            -ArgumentList "/k title $title & $PythonCmd $tool" `
            -WorkingDirectory $LocalDir -PassThru
    }

    Write-Host ""
    Write-Host "=== All set ==="
    Write-Host "Broker + inference: running on the Ultra96"
    Write-Host "Tunnel running in a minimised window"
    Write-Host "$($ToolScripts -join ' / ') running in their own windows"
    Write-Host ""
    Write-Host "Now power on the FireBeetle."
    Write-Host ""
    Write-Host "IMPORTANT - how shutdown works:"
    Write-Host "  Closing one of the tool windows only stops that program."
    Write-Host "  To fully stop EVERYTHING (including the Ultra96's broker and inference script),"
    Write-Host "  come back to this window and press Enter (or Ctrl+C)."
    Write-Host ""
    Read-Host "Press Enter to stop everything" | Out-Null
}
finally {
    # Runs on Enter, on Ctrl+C, and if any step above fails.
    Invoke-Cleanup
}
