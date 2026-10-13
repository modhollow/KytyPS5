# Execute the production scheduler with deterministic CPU-only worker outcomes.
# No precompiler, GPU, game, shader, source save or existing cache is opened.
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot '..\precompile-windows.ps1'
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Precompile source does not parse' }
$assignments = @($ast.FindAll({
    param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst]
}, $true))
$begin = @($assignments | Where-Object { $_.Left.Extent.Text -eq '$pending' })[0]
$end = @($assignments | Where-Object {
    $_.Left.Extent.Text -eq '$mergeArguments' -and $_.Extent.StartOffset -gt $begin.Extent.StartOffset
})[0]
if (!$begin -or !$end) { throw 'Production scheduler boundaries not found' }
$text = [IO.File]::ReadAllText($source)
# The scheduler through the final merge's arguments, without the -ShardLimit early return between them.
$region = $text.Substring($begin.Extent.StartOffset, $end.Extent.EndOffset - $begin.Extent.StartOffset)
$region = $region -replace '(?ms)^if \(\$ShardLimit -gt 0\) \{\r?\n.*?^\s*return\r?\n\}', ''
$scheduler = [scriptblock]::Create($region)
$failures = 0

function Start-Sleep { param([int]$Milliseconds) }
function Remove-NvidiaCache { param($process) }

function Start-Precompile([string]$name, [string[]]$arguments) {
    ++$script:fixture.Starts
    $script:fixture.PeakPending = [math]::Max($script:fixture.PeakPending, $pending.Count + 1)
    if ($script:fixture.Starts -gt $script:fixture.Budget) {
        throw 'Fixture launch budget exhausted before a retry descendant terminated'
    }
    $position = [Array]::IndexOf($arguments, '--shard')
    if ($position -lt 0) { throw 'Worker was launched without its shard identity' }
    $i, $n = [int[]]($arguments[$position + 1] -split '/')
    if ($i -lt 0 -or $i -ge $n) { throw 'Invalid worker shard' }
    $items = @($script:fixture.Items | Where-Object { $_ % $n -eq $i })
    $script:fixture.Launched.Add([pscustomobject]@{ I = $i; N = $n; Name = $name })
    $exit = 0
    if ($script:fixture.Kind -eq 'always-partial' -or
        ($script:fixture.Kind -eq 'partial-success' -and $items.Count -gt 4)) {
        $exit = 3
    } elseif ($script:fixture.Kind -eq 'fatal' -and $i -eq 0) {
        $exit = 1
    }
    if ($exit -eq 0) {
        foreach ($item in $items) { $script:fixture.Completed.Add([int]$item) }
    }
    return [pscustomobject]@{ HasExited = $true; ExitCode = $exit }
}

function Invoke-SchedulerCase([string]$kind, [int]$shards, [int]$jobs,
                              [int[]]$items, [int]$budget, [string]$recorded = '',
                              [bool]$recordedOnly = $false, [int]$recordedPipelines = 0) {
    $script:fixture = [pscustomobject]@{
        Kind = $kind; Starts = 0; PeakPending = $shards; Budget = $budget
        Items = $items; RecordedPipelines = $recordedPipelines
        Launched = [Collections.Generic.List[object]]::new()
        Completed = [Collections.Generic.List[int]]::new()
    }
    $Seeds = 'fixture.seeds'
    $Recorded = $recorded
    $RecordedOnly = $recordedOnly
    $Shards = $shards
    $ShardLimit = 0
    $Jobs = $jobs
    $Threads = 3
    $nvidiaCaches = 'fixture-cache'
    $logName = 'fixture-logs'
    $begin = Get-Date
    $errorText = ''
    # Fixtures for the recorded shards, in this scope only.
    function Get-SeedPipelines([string]$file) { return $script:fixture.RecordedPipelines }
    function Test-Path([string]$path) { return $path -eq 'fixture-recorded.seeds' }
    try { . $scheduler } catch { $errorText = $_.Exception.Message }
    return [pscustomobject]@{
        Error = $errorText; Finished = $finished; Split = $split; Merge = ($mergeArguments -join ' ')
        Starts = $script:fixture.Starts; PeakPending = $script:fixture.PeakPending
        Completed = @($script:fixture.Completed.ToArray())
        Launched = @($script:fixture.Launched.ToArray())
    }
}

function Check([bool]$condition, [string]$message) {
    if (!$condition) {
        ++$script:failures
        Write-Host "FAIL: $message"
    }
}

$oldClaims = [Environment]::GetEnvironmentVariable('KYTY_PRECOMPILE_CLAIMS', 'Process')
try {
    # Many parents that all return partial must reach the existing terminal
    # split limit promptly rather than endlessly expanding the breadth-first frontier.
    $r = Invoke-SchedulerCase 'always-partial' 512 5 @(0) 80
    Check ($r.Error -match 'cannot be split further') (
        "Partial retries did not reach a terminal descendant within 80 launches: $($r.Error)")
    Check ($r.PeakPending -le 558) (
        "Partial retry frontier grew beyond roots plus bounded worker/depth allowance: $($r.PeakPending)")
    Check ($r.Finished -eq 0) 'A partial shard was incorrectly counted as complete'

    # Actual modulo halves must eventually cover every input exactly once.
    $r = Invoke-SchedulerCase 'partial-success' 4 3 @(0..63) 100
    Check (!$r.Error) "Legitimate split/success workload failed: $($r.Error)"
    Check ((($r.Completed | Sort-Object) -join ',') -eq ((0..63) -join ',')) (
        'Split descendants lost or duplicated work')
    Check ($r.Split -gt 0 -and $r.Finished -eq 16) (
        'Partial shards and completed leaves were counted incorrectly')

    $r = Invoke-SchedulerCase 'success' 4 3 @(0..12) 10
    Check (!$r.Error -and $r.Finished -eq 4 -and $r.Split -eq 0) (
        'Already-successful roots changed behavior')
    Check ((($r.Completed | Sort-Object) -join ',') -eq ((0..12) -join ',')) (
        'Successful roots lost or duplicated work')

    $r = Invoke-SchedulerCase 'success' 8 3 @(0..2) 12
    Check (!$r.Error -and $r.Finished -eq 8) 'Empty shards no longer finish normally'
    Check ((($r.Completed | Sort-Object) -join ',') -eq '0,1,2') (
        'Empty-shard handling changed real work coverage')

    # Recorded play's shards are launched before the seeds' (a run stopped early has them).
    $r = Invoke-SchedulerCase 'success' 4 1 @(0..7) 20 'fixture-recorded.seeds' $false 250
    $names = @($r.Launched | ForEach-Object Name)
    Check (!$r.Error -and $r.Finished -eq 7) "Recorded plus seed shards did not all finish: $($r.Error)"
    Check (($names[0..2] | Where-Object { $_ -like '*fixture-recorded' }).Count -eq 3) (
        "Recorded shards were not launched first: $($names -join ', ')")
    Check (($names[3..6] | Where-Object { $_ -like '*-fixture' }).Count -eq 4) (
        "Seed shards did not follow the recorded ones: $($names -join ', ')")

    # -RecordedOnly: only recorded play's shards.
    $r = Invoke-SchedulerCase 'success' 4 2 @(0..7) 20 'fixture-recorded.seeds' $true 250
    $names = @($r.Launched | ForEach-Object Name)
    Check (!$r.Error -and $r.Finished -eq 3 -and ($names | Where-Object { $_ -like '*-fixture' }).Count -eq 0) (
        "-RecordedOnly launched seed shards or missed recorded ones: $($names -join ', ')")
    Check ($r.Merge -eq '--merge') "-RecordedOnly pruned what an earlier full run made: $($r.Merge)"

    # -RecordedOnly without a recording: every shader instead.
    $r = Invoke-SchedulerCase 'success' 4 2 @(0..7) 20 'missing-recorded.seeds' $true 250
    Check (!$r.Error -and $r.Finished -eq 4) '-RecordedOnly without a recording did not fall back to the seeds'
    Check ($r.Merge -eq '--merge --prune') "A full fallback run did not prune: $($r.Merge)"

    $r = Invoke-SchedulerCase 'fatal' 8 5 @(0..7) 12
    Check ($r.Error -match 'failed; logs: fixture-logs') (
        'A non-retry failure was not reported')
    Check ($r.Starts -eq 5 -and $r.Split -eq 0) (
        'A fatal worker was retried or more roots started after failure')
} finally {
    [Environment]::SetEnvironmentVariable('KYTY_PRECOMPILE_CLAIMS', $oldClaims, 'Process')
}
if ($failures) {
    Write-Host "PrecompileShardSchedulerTests: $failures failures"
    exit 1
}
Write-Host 'PrecompileShardSchedulerTests: all passed'
exit 0
