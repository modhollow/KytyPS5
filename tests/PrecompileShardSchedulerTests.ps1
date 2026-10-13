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
# The fixture's clock: it moves a minute at each worker launch (the scheduler's polls take no time).
function Get-Date { return $script:fixture.Clock }
# The finished-shards list (precompile-windows.ps1 .done), in the fixture.
function Get-ShardInputs { return 'fixture-inputs' }
function Read-DoneShards { return , $script:fixture.Done }
function Add-DoneShard([string]$file, [string]$inputs, [string]$name) { $script:fixture.Recorded.Add($name) }
function Remove-NvidiaCache { param($process) }
# -Background, -MergeOnly and the game the background run watches (it ends after GameEndsAfter launches).
$caseOptions = @{}
function Test-WatchedProcess {
    return !($script:caseOptions.GameEndsAfter -and $script:fixture.Starts -ge $script:caseOptions.GameEndsAfter)
}

function Start-Precompile([string]$name, [string[]]$arguments) {
    ++$script:fixture.Starts
    $script:fixture.Clock = $script:fixture.Clock.AddMinutes(1)
    $script:fixture.PeakPending = [math]::Max($script:fixture.PeakPending, $pending.Count + 1)
    if ($script:fixture.Starts -gt $script:fixture.Budget) {
        throw 'Fixture launch budget exhausted before a retry descendant terminated'
    }
    $position = [Array]::IndexOf($arguments, '--shard')
    if ($position -lt 0) { throw 'Worker was launched without its shard identity' }
    $i, $n = [int[]]($arguments[$position + 1] -split '/')
    if ($i -lt 0 -or $i -ge $n) { throw 'Invalid worker shard' }
    # The production membership rule (precompileSchedule.h InShard): every n-th, or contiguous slices.
    $all = @($script:fixture.Items)
    $items = if ($arguments -contains '--shard-blocks') {
        @(for ($k = 0; $k -lt $all.Count; $k++) { if ([math]::Floor($k * $n / $all.Count) -eq $i) { $all[$k] } })
    } else {
        @($all | Where-Object { $_ % $n -eq $i })
    }
    $script:fixture.Blocks.Add([pscustomobject]@{ Name = $name; Blocks = ($arguments -contains '--shard-blocks') })
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
    # A worker runs for -Lag polls of the scheduler before it has exited.
    $worker = [pscustomobject]@{ ExitCode = $exit; Polls = 0; Lag = $script:fixture.Lag; Killed = $false }
    $worker | Add-Member ScriptProperty HasExited { $this.Polls++; $this.Killed -or $this.Polls -gt $this.Lag }
    $worker | Add-Member ScriptMethod Kill { $this.Killed = $true; $script:fixture.Kills++ }
    $worker | Add-Member ScriptMethod WaitForExit { }
    return $worker
}

function Invoke-SchedulerCase([string]$kind, [int]$shards, [int]$jobs,
                              [int[]]$items, [int]$budget, [string]$recorded = '',
                              [bool]$recordedOnly = $false, [int]$recordedPipelines = 0, [int]$minutes = 0,
                              [int]$lag = 0, [string[]]$done = @()) {
    $script:fixture = [pscustomobject]@{
        Kind = $kind; Starts = 0; PeakPending = $shards; Budget = $budget; Clock = [datetime]'2026-01-01'
        Lag = $lag; Done = [Collections.Generic.HashSet[string]]::new([string[]]$done)
        Recorded = [Collections.Generic.List[string]]::new()
        Items = $items; RecordedPipelines = $recordedPipelines
        Launched = [Collections.Generic.List[object]]::new()
        Blocks = [Collections.Generic.List[object]]::new()
        Completed = [Collections.Generic.List[int]]::new()
        Kills = 0
    }
    $Background = [bool]$script:caseOptions.Background
    $MergeOnly = [bool]$script:caseOptions.MergeOnly
    $Seeds = 'fixture.seeds'
    $Recorded = $recorded
    $RecordedOnly = $recordedOnly
    $Minutes = $minutes
    $CacheRoot = 'fixture-cache-root'
    $gameId = 'FIXTURE_01.000.000'
    $Exe = 'fixture.exe'
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
    function Test-Path([string]$path) { return $path -in @('fixture-recorded.seeds', 'other\fixture.seeds') }
    try { . $scheduler } catch { $errorText = $_.Exception.Message }
    return [pscustomobject]@{
        Error = $errorText; Finished = $finished; Split = $split; Merge = ($mergeArguments -join ' ')
        BudgetStopped = $budgetStopped; Partial = $partialRun; Skipped = $skipped
        Kills = $script:fixture.Kills; GameEnded = $gameEnded
        DoneAdded = @($script:fixture.Recorded.ToArray())
        Starts = $script:fixture.Starts; PeakPending = $script:fixture.PeakPending
        Completed = @($script:fixture.Completed.ToArray())
        Launched = @($script:fixture.Launched.ToArray())
        Blocks = @($script:fixture.Blocks.ToArray())
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

    # Recorded play's shards are contiguous slices of the (ranked) recorded file; the seeds' every n-th.
    Check ((@($r.Blocks | Where-Object { $_.Name -like '*fixture-recorded' -and $_.Blocks }).Count -eq 3) -and
        (@($r.Blocks | Where-Object { $_.Name -like '*-fixture' -and $_.Blocks }).Count -eq 0)) (
        "Recorded shards were not contiguous or seed shards were: $(($r.Blocks | ForEach-Object { "$($_.Name)=$($_.Blocks)" }) -join ', ')")

    # A contiguous slice that left pipelines out runs again as its two halves (2i, 2i + 1 of 2n),
    # which together cover it exactly.
    $r = Invoke-SchedulerCase 'partial-success' 4 3 @(0..63) 100 'fixture-recorded.seeds' $true 350
    Check (!$r.Error) "Contiguous split workload failed: $($r.Error)"
    Check ((($r.Completed | Sort-Object) -join ',') -eq ((0..63) -join ',')) (
        "Contiguous split descendants lost or duplicated work: $(($r.Completed | Sort-Object) -join ',')")
    $names = @($r.Launched | ForEach-Object Name)
    Check ($names -contains 'shard0of8b-fixture-recorded' -and @($names | Where-Object { $_ -notlike 'shard*b-fixture-recorded' }).Count -eq 0) (
        "Contiguous halves were not named as contiguous shards: $($names -join ', ')")

    # A list naming an every-n-th shard never skips the contiguous shard with the same i/n.
    $r = Invoke-SchedulerCase 'success' 4 2 @(0..7) 20 'fixture-recorded.seeds' $true 250 0 0 @('shard0of3-fixture-recorded')
    Check (!$r.Error -and $r.Skipped -eq 0 -and @($r.Launched | ForEach-Object Name) -contains 'shard0of3b-fixture-recorded') (
        "An every-n-th name skipped a contiguous shard (skipped $($r.Skipped))")

    # Tiers that share a file stem are refused (their shard files would collide).
    $r = Invoke-SchedulerCase 'success' 4 2 @(0..7) 20 'other\fixture.seeds' $false 250
    Check ($r.Error -match 'must not be named like the seed file') "Same-stem tiers were not refused: $($r.Error)"

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

    # -Minutes: no shard starts after the budget, recorded play's first; the running ones finish and the
    # merge keeps what earlier runs made.
    $r = Invoke-SchedulerCase 'success' 4 2 @(0..7) 20 'fixture-recorded.seeds' $false 250 3
    $names = @($r.Launched | ForEach-Object Name)
    Check (!$r.Error -and $r.Starts -eq 3 -and $r.Finished -eq 3 -and $r.BudgetStopped) (
        "The budget did not stop new shards after 3 minutes: $($names -join ', ') $($r.Error)")
    Check (($names | Where-Object { $_ -like '*-fixture' }).Count -eq 0) (
        "Seed shards started before recorded play's within the budget: $($names -join ', ')")
    Check ($r.Merge -eq '--merge' -and $r.Partial) "A budget-stopped run pruned or was not partial: $($r.Merge)"

    # A budget the run fits in changes nothing.
    $r = Invoke-SchedulerCase 'success' 4 2 @(0..7) 20 '' $false 0 60
    Check (!$r.Error -and $r.Finished -eq 4 -and !$r.BudgetStopped -and $r.Merge -eq '--merge --prune' -and !$r.Partial) (
        "A run within its budget was cut short or not pruned: $($r.Merge)")

    # Shards still running when the budget ends finish (and count); none starts after it.
    $r = Invoke-SchedulerCase 'success' 4 2 @(0..7) 20 '' $false 0 2 5
    Check (!$r.Error -and $r.BudgetStopped -and $r.Starts -eq 2 -and $r.Finished -eq 2) (
        "Running shards did not finish at the budget's end: starts $($r.Starts), finished $($r.Finished) $($r.Error)")

    # A shard split when the budget has ended: its halves wait for a later run.
    $r = Invoke-SchedulerCase 'always-partial' 4 2 @(0) 20 '' $false 0 2
    Check (!$r.Error -and $r.BudgetStopped -and $r.Starts -eq 2 -and $r.Merge -eq '--merge') (
        "Split halves started after the budget's end: starts $($r.Starts) $($r.Error)")

    # Shards an earlier run finished are not run again, and the merge then keeps the store (no prune).
    $r = Invoke-SchedulerCase 'success' 4 2 @(0..7) 20 'fixture-recorded.seeds' $false 250 0 0 @(
        'shard0of3b-fixture-recorded', 'shard1of3b-fixture-recorded')
    $names = @($r.Launched | ForEach-Object Name)
    Check (!$r.Error -and $r.Skipped -eq 2 -and ($names -notcontains 'shard0of3b-fixture-recorded') -and $names.Count -eq 5) (
        "Finished shards ran again: $($names -join ', ') (skipped $($r.Skipped))")
    Check ($r.Merge -eq '--merge' -and !$r.Partial) "A run that kept earlier shards pruned them or was partial: $($r.Merge)"
    Check ($r.DoneAdded.Count -eq 5 -and $r.DoneAdded -contains 'shard2of3b-fixture-recorded') (
        "Finished shards were not recorded as done: $($r.DoneAdded -join ', ')")

    # A shard that splits again on a later run: the half that run finished is kept.
    $r = Invoke-SchedulerCase 'partial-success' 4 2 @(0..31) 40 '' $false 0 0 0 @('shard0of8-fixture')
    $names = @($r.Launched | ForEach-Object Name)
    Check (!$r.Error -and $r.Skipped -eq 1 -and $names -notcontains 'shard0of8-fixture' -and $names -contains 'shard4of8-fixture') (
        "A finished half ran again: $($names -join ', ') (skipped $($r.Skipped)) $($r.Error)")
    Check ((($r.Completed | Sort-Object) -join ',') -eq ((0..31 | Where-Object { $_ % 8 -ne 0 }) -join ',')) (
        'The other halves lost or duplicated work')

    # -MergeOnly: finished shards merged (merge-before), nothing compiled while shards are left; when every
    # shard is finished, the precompile completes (merged without pruning, not partial).
    $caseOptions = @{ MergeOnly = $true }
    $r = Invoke-SchedulerCase 'success' 4 2 @(0..7) 20 '' $false 0 0 0 @('shard0of4-fixture')
    Check (!$r.Error -and $r.Starts -eq 0 -and !$r.Merge) "-MergeOnly compiled or completed with shards left: starts $($r.Starts), merge '$($r.Merge)'"
    $r = Invoke-SchedulerCase 'success' 4 2 @(0..7) 20 '' $false 0 0 0 @('shard0of4-fixture', 'shard1of4-fixture', 'shard2of4-fixture', 'shard3of4-fixture')
    Check (!$r.Error -and $r.Starts -eq 0 -and $r.Merge -eq '--merge' -and !$r.Partial) (
        "-MergeOnly did not complete a finished precompile: merge '$($r.Merge)', partial $($r.Partial) $($r.Error)")

    # -Background: the shards not finished yet, no merge; it stops when the game ends, ending the shard in progress.
    $caseOptions = @{ Background = $true }
    $r = Invoke-SchedulerCase 'success' 4 1 @(0..7) 20 '' $false 0 0 0 @('shard0of4-fixture')
    Check (!$r.Error -and $r.Starts -eq 3 -and $r.Finished -eq 3 -and !$r.Merge -and $r.DoneAdded.Count -eq 3) (
        "-Background did not run the unfinished shards without a merge: starts $($r.Starts), merge '$($r.Merge)' $($r.Error)")
    $caseOptions = @{ Background = $true; GameEndsAfter = 2 }
    $r = Invoke-SchedulerCase 'success' 4 1 @(0..7) 20 '' $false 0 0 3
    Check (!$r.Error -and $r.GameEnded -and $r.Starts -eq 2 -and $r.Kills -eq 1 -and $r.Finished -eq 1 -and !$r.Merge) (
        "-Background did not stop with the game: starts $($r.Starts), kills $($r.Kills), finished $($r.Finished) $($r.Error)")
    # A shard that finished as the game ended is listed, not compiled again.
    $caseOptions = @{ Background = $true; GameEndsAfter = 2 }
    $r = Invoke-SchedulerCase 'success' 4 2 @(0..7) 20 '' $false 0 0 1
    Check (!$r.Error -and $r.GameEnded -and $r.Kills -eq 0 -and $r.Finished -eq 2 -and $r.DoneAdded.Count -eq 2) (
        "Shards finished as the game ended were not listed: finished $($r.Finished), listed $($r.DoneAdded.Count), kills $($r.Kills)")
    $caseOptions = @{}

    $r = Invoke-SchedulerCase 'fatal' 8 5 @(0..7) 12
    Check ($r.Error -match 'failed; logs: fixture-logs') (
        'A non-retry failure was not reported')
    Check ($r.Starts -eq 5 -and $r.Split -eq 0) (
        'A fatal worker was retried or more roots started after failure')
} finally {
    [Environment]::SetEnvironmentVariable('KYTY_PRECOMPILE_CLAIMS', $oldClaims, 'Process')
}

# The finished-shards list itself (the script's own functions, on a temporary file; the device check stubbed).
foreach ($name in 'Get-ShardInputs', 'Get-StoreOfThisDevice', 'Read-DoneShards', 'Add-DoneShard') {
    $definition = @($ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true))[0]
    if (!$definition) { throw "precompile-windows.ps1 has no $name" }
    . ([scriptblock]::Create($definition.Extent.Text))
}
$doneRoot = Join-Path ([IO.Path]::GetTempPath()) "kyty-done-$PID"
# The real probe, against a stand-in precompiler that prints FAKE_STATUS and exits with FAKE_EXIT.
New-Item -ItemType Directory -Force $doneRoot | Out-Null
$Exe = Join-Path $doneRoot 'fake-precompile.cmd'
Set-Content -Path $Exe -Value "@echo off`r`nif defined FAKE_STATUS echo %FAKE_STATUS%`r`necho noise 1>&2`r`nexit /b %FAKE_EXIT%" -Encoding ascii
$Game = $doneRoot
$Seeds = $Exe
$toolDirectory = $doneRoot
foreach ($case in @(@('static cache current', 0, 'yes'), @('static cache outdated', 0, 'yes'), @('static cache stale', 0, 'no'),
        @('', 1, 'unknown'), @('static cache current', 1, 'unknown'))) {
    $env:FAKE_STATUS = $case[0]
    $env:FAKE_EXIT = "$($case[1])"
    $state = Get-StoreOfThisDevice
    Check ($state -eq $case[2]) "The store probe said '$state' for '$($case[0])', exit $($case[1])"
}
Remove-Item Env:FAKE_STATUS, Env:FAKE_EXIT -ErrorAction SilentlyContinue
$storeOfThisDevice = 'yes'
function Get-StoreOfThisDevice { return $script:storeOfThisDevice }
$doneFile = Join-Path $doneRoot 'static-v1\FIXTURE.done'
$oldRobust = $env:KYTY_ROBUST_BUFFERS
try {
    Add-DoneShard $doneFile 'inputs-a' 'shard0of4-fixture'
    Add-DoneShard $doneFile 'inputs-a' 'shard1of4-fixture'
    $lines = @(Get-Content $doneFile)
    Check ($lines.Count -eq 3 -and $lines[0] -eq 'inputs-a') "The list's header or names were not written: $($lines -join ' | ')"
    $set = Read-DoneShards $doneFile 'inputs-a'
    Check ($set.Count -eq 2 -and $set.Contains('shard1of4-fixture')) 'The list of the same inputs was not read'
    $set = Read-DoneShards $doneFile 'inputs-b'
    Check ($set.Count -eq 0 -and !(Test-Path $doneFile)) 'The list of other inputs was used or kept'
    Add-DoneShard $doneFile 'inputs-a' 'shard0of4-fixture'
    $storeOfThisDevice = 'unknown'
    $set = Read-DoneShards $doneFile 'inputs-a'
    Check ($set.Count -eq 0 -and (Test-Path $doneFile)) 'A failed store probe used or discarded the list'
    $storeOfThisDevice = 'no'
    $set = Read-DoneShards $doneFile 'inputs-a'
    Check ($set.Count -eq 0 -and !(Test-Path $doneFile)) 'The list was used although this GPU and driver have no store'
    $env:KYTY_ROBUST_BUFFERS = '0'
    $a = Get-ShardInputs @($PSCommandPath)
    $env:KYTY_ROBUST_BUFFERS = '1'
    $b = Get-ShardInputs @($PSCommandPath)
    Check ($a -ne $b -and $a -like "$((Get-FileHash $PSCommandPath).Hash)*") 'The inputs ignore a file or the robust-buffers switch'
} finally {
    $env:KYTY_ROBUST_BUFFERS = $oldRobust
    Remove-Item -Recurse -Force $doneRoot -ErrorAction SilentlyContinue
}
if ($failures) {
    Write-Host "PrecompileShardSchedulerTests: $failures failures"
    exit 1
}
Write-Host 'PrecompileShardSchedulerTests: all passed'
exit 0
