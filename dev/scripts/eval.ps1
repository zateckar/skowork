# Developer evaluation of a running Skowork client (add-evaluation-export design D1, D2). Needs
# policy.evaluation.export = on and a client whose local API pipe is reachable (see skowork-pipe.ps1).
# Exports hold personal data in plain text; the default folder dev/eval-data is git-ignored.
#
#   ./eval.ps1 -Export -From 2026-09-01 -To 2026-11-27 [-Sources mail,teams,calendar,meetings] [-Audio] [-OutDir <folder>]
#   ./eval.ps1 -Run -Folder <export folder> -Step triage|agenda|knowledge|diarize|asr [-Variant <file.json>] [-MaxItems 200]
#   ./eval.ps1 -Status -Job <job id>
#   ./eval.ps1 -Cancel -Job <job id>
#   ./eval.ps1 -Clean -Folder <export folder>
[CmdletBinding(DefaultParameterSetName = 'Export')]
param(
    [Parameter(ParameterSetName = 'Export', Mandatory)] [switch] $Export,
    [Parameter(ParameterSetName = 'Export', Mandatory)] [string] $From,
    [Parameter(ParameterSetName = 'Export', Mandatory)] [string] $To,
    [Parameter(ParameterSetName = 'Export')] [string[]] $Sources = @('mail', 'teams', 'calendar', 'meetings'),
    [Parameter(ParameterSetName = 'Export')] [switch] $Audio,
    [Parameter(ParameterSetName = 'Export')] [string] $OutDir = (Join-Path $PSScriptRoot '../eval-data'),

    [Parameter(ParameterSetName = 'Run', Mandatory)] [switch] $Run,
    [Parameter(ParameterSetName = 'Run', Mandatory)] [Parameter(ParameterSetName = 'Clean', Mandatory)] [string] $Folder,
    [Parameter(ParameterSetName = 'Run', Mandatory)] [ValidateSet('triage', 'agenda', 'knowledge', 'diarize', 'asr')] [string] $Step,
    [Parameter(ParameterSetName = 'Run')] [string] $Variant,
    [Parameter(ParameterSetName = 'Run')] [ValidateRange(1, 2000)] [int] $MaxItems = 200,

    [Parameter(ParameterSetName = 'Status', Mandatory)] [switch] $Status,
    [Parameter(ParameterSetName = 'Cancel', Mandatory)] [switch] $Cancel,
    [Parameter(ParameterSetName = 'Status', Mandatory)] [Parameter(ParameterSetName = 'Cancel', Mandatory)] [string] $Job,

    [Parameter(ParameterSetName = 'Clean', Mandatory)] [switch] $Clean,
    [switch] $NoWait
)
$ErrorActionPreference = 'Stop'
$pipe = Join-Path $PSScriptRoot 'skowork-pipe.ps1'

function Invoke-Eval([string] $Method, [hashtable] $Params, [int] $TimeoutMs = 30000) {
    $json = ($Params | ConvertTo-Json -Depth 20 -Compress)
    $raw = & $pipe -Method $Method -Params $json -TimeoutMs $TimeoutMs
    $answer = $raw | ConvertFrom-Json
    if ($answer.error) {
        $code = if ($answer.error.data) { $answer.error.data.code } else { $answer.error.code }
        throw "$Method failed ($code): $($answer.error.message)"
    }
    return $answer.result
}

function Wait-Job([string] $Id) {
    while ($true) {
        $s = Invoke-Eval 'eval.status' @{ job = $Id }
        Write-Host ("{0}: {1} {2}/{3}" -f $Id, $s.state, $s.done, $s.total)
        if ($s.state -in @('done', 'failed', 'cancelled')) { return $s }
        Start-Sleep -Seconds 2
    }
}

switch ($PSCmdlet.ParameterSetName) {
    'Export' {
        $out = [System.IO.Path]::GetFullPath($OutDir)
        $params = @{ out_dir = $out; from = $From; to = $To; sources = @($Sources); audio = [bool]$Audio }
        if ($Audio) {
            $dry = Invoke-Eval 'eval.export' ($params + @{ dry_run = $true })
            Write-Host ("Audio will take about {0:N0} MB." -f ($dry.audio_bytes / 1MB))
        }
        $started = Invoke-Eval 'eval.export' $params
        Write-Host "Export $($started.job) into $($started.folder)"
        if (-not $NoWait) { Wait-Job $started.job | ConvertTo-Json -Depth 10 }
    }
    'Run' {
        $params = @{ folder = [System.IO.Path]::GetFullPath($Folder); step = $Step; max_items = $MaxItems }
        if ($Variant) { $params.variant = Get-Content -Raw -Encoding utf8 $Variant | ConvertFrom-Json -AsHashtable }
        $started = Invoke-Eval 'eval.run' $params
        Write-Host "Replay $($started.job)"
        if (-not $NoWait) { Wait-Job $started.job | ConvertTo-Json -Depth 10 }
    }
    'Status' { Invoke-Eval 'eval.status' @{ job = $Job } | ConvertTo-Json -Depth 10 }
    'Cancel' { Invoke-Eval 'eval.cancel' @{ job = $Job } | ConvertTo-Json }
    'Clean' {
        $full = [System.IO.Path]::GetFullPath($Folder)
        if (-not (Test-Path (Join-Path $full 'manifest.json'))) { throw "$full is not an evaluation export (no manifest.json)." }
        $answer = Read-Host "Delete the export $full and everything in it? Type yes"
        if ($answer -ne 'yes') { Write-Host 'Kept.'; return }
        Remove-Item -Recurse -Force $full
        Write-Host "Deleted $full"
    }
}
