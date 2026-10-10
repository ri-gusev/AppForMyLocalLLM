param([Parameter(Mandatory=$true)][string]$Executable,
      [Parameter(Mandatory=$true)][string]$Report,
      [string]$Temperature)
$ErrorActionPreference = 'Stop'
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$reportPath = [System.IO.Path]::GetFullPath($Report)
$arguments = @('.', $reportPath)
if ($Temperature) { $arguments += $Temperature }
$process = Start-Process -FilePath $executablePath -ArgumentList $arguments `
    -WindowStyle Hidden -PassThru -RedirectStandardOutput "$reportPath.stdout.txt" `
    -RedirectStandardError "$reportPath.stderr.txt"
$gpuTool = Get-Command nvidia-smi -ErrorAction SilentlyContinue
$samples = @()
do {
    $ollamaProcesses = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match '^(ollama|llama)' }
    $ram = ($ollamaProcesses | Measure-Object -Property WorkingSet64 -Sum).Sum
    $gpuMemory = $null
    if ($gpuTool) {
        $gpuValues = & $gpuTool.Source --query-gpu=memory.used --format=csv,noheader,nounits 2>$null
        if ($LASTEXITCODE -eq 0) { $gpuMemory = ($gpuValues | ForEach-Object { [double]$_ } | Measure-Object -Sum).Sum }
    }
    $samples += [pscustomobject]@{ utc = [DateTime]::UtcNow.ToString('o'); ollama_working_set_mb = [math]::Round($ram / 1MB, 2); gpu_total_used_mb = $gpuMemory }
    Start-Sleep -Milliseconds 500
    $process.Refresh()
} while (-not $process.HasExited)
$process.WaitForExit()
[pscustomobject]@{
    scope = 'RAM: all ollama/llama processes; GPU: total device usage including other applications; not isolated per request'
    exit_code = $process.ExitCode
    peak_ollama_working_set_mb = ($samples | Measure-Object ollama_working_set_mb -Maximum).Maximum
    peak_gpu_total_used_mb = ($samples | Measure-Object gpu_total_used_mb -Maximum).Maximum
    samples = $samples
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$reportPath.resources.json" -Encoding UTF8
Get-Content -LiteralPath "$reportPath.stdout.txt"
if ($process.ExitCode -ne 0) { exit $process.ExitCode }
