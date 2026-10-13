# Static shader and pipeline precompile (tools\local\static-precompile), a program of its own:
#   .\precompile-windows.ps1              every shader and pipeline of the game into the static pipeline
#                                         cache _PipelineCache\static\<title>_<version>.bin, which the emulator
#                                         looks up before compiling (hours the first time: run it overnight); with
#                                         a driver that has VK_KHR_pipeline_binary (NVIDIA 5xx) the pipelines'
#                                         binaries instead, <title>_<version>.binaries, read only when a pipeline is
#                                         needed (the driver copies a whole .bin into memory: GBs)
#   .\precompile-windows.ps1 -Jobs 8      8 processes at a time (default: 2 threads each, as many as the allowed
#                                         cores less one hold with their SMT siblings)
#   .\precompile-windows.ps1 -RecordedOnly  only what recorded play uses (the specializations of recorded play, or the
#                                         release's hints): about a fifth of the time; the rest is compiled when the
#                                         game first needs it, or by a full run later (which keeps these);
#                                         run-windows.ps1 -Prompt still offers that full run (<title>_<version>.partial)
#   .\precompile-windows.ps1 -Minutes 15  a time budget: no shard starts after 15 minutes of compiling (the running
#                                         ones finish: up to the longest shard over, then the merge), and what is
#                                         done is kept (recorded play first); run it again to continue where it stopped
#   .\precompile-windows.ps1 -Coverage    no pipelines, only what the seeds compile to, for
#                                         tools\local\static-precompile\precompile.py coverage
#   .\precompile-windows.ps1 -InputsOnly  only the compiled inputs the emulator's shader prefetch
#                                         translates in the background (_PipelineCache\static\<title>_<version>.shaders;
#                                         every full run writes them too)
# The NVIDIA driver compiles big compute shaders nearly one at a time per process, so the work is split
# into shards, a below-normal-priority process each (kyty_shader_precompile --shard i/n), whose caches
# are merged into the static cache at the end. The shards are small (-Shards, about 100 pipelines each):
# after a few hundred pipelines NVIDIA compresses binaries with a dictionary of its own, which only that
# PC reads (pipelineBinaries.h); a shard that got there anyway left the rest out (exit code 3) and runs
# again as two. recorded-<title>_<version>.seeds next to the seed file (-Recorded) adds the specializations of
# recorded play; a PC without a recording of its own (a release) gets those of the specialization hints the release
# ships (tools\local\static-precompile\hints-<title>_<version>.hints: recordings without the game's code), completed
# with the code of the seed file into hinted-<title>_<version>.seeds (kyty_shader_precompile --apply-hints, a second).
# An interrupted run resumes: finished shards are merged first, and what the static cache
# holds is not compiled again (binaries: the final merge keeps only what this run's shards made, so
# pipelines no seed makes any more are dropped; a .bin left from before is where the first binaries run
# takes them from without compiling). Build the program with build-windows.cmd kyty_shader_precompile; it also
# makes the seed file when there is none (--make-seeds: the game's shaders from its files, with the render-pass
# states of tools\local\static-precompile\pass-states.json). A release package (.github/workflows/build.yml) has
# the program and launch.json next to this script (precompile.cmd) and pass-states.json in that folder below it;
# the seed file is made there. The files are the game version's (seeds-<title>_<version>.seeds): versions need
# not share shaders.
param(
	[string]$Game = '',
	# Default: seeds-<title>_<version>.seeds next to this script (a release) or in _Build\static-precompile.
	[string]$Seeds = '',
	# The shaders as the game specialized them in recorded play (tools\local\static-precompile\precompile.py
	# recorded-seeds): the specializations the seeds' guesses miss (a new PC compiled ~55 compute
	# pipelines, up to 6 s each, before the HUD). '' = none; default: recorded-<title>_<version>.seeds next to the seed file,
	# else the release's hints completed with the seed file's code (hinted-<title>_<version>.seeds).
	[string]$Recorded = '*',
	[int]$Jobs = 0,
	[int]$Threads = 2,
	[int]$Shards = 512,
	[int64]$Affinity = 0, # 0: the launch config's CPUs (this PC's leave out 4 and 5, where the compiler crashes), else all
	[switch]$Coverage,
	[switch]$InputsOnly,
	[switch]$RecordedOnly,
	# A time budget in minutes of compiling (0: none; making seeds and hints and merging earlier shards come first
	# and are not counted): no shard starts after it, the running ones finish, and the cache is merged as partial
	# (no pruning; <title>_<version>.partial). A later run continues: the shards finished for the same inputs
	# (<title>_<version>.done) are not run again.
	[int]$Minutes = 0,
	# Every allowed CPU (the processes still below normal priority), instead of the cores less one.
	# Overrides EcoQoS.
	[switch]$FullSpeed,
	# Opt in to Windows EcoQoS power saving. Lower clocks/efficient cores can lengthen precompilation.
	[switch]$EcoQoS,
	# Print the processes, threads and power the run would use (JSON) and exit; -LogicalCpus/-PhysicalCores stand
	# in for this PC's (tests).
	[switch]$PlanOnly,
	[int]$LogicalCpus = 0,
	[int]$PhysicalCores = 0,
	[int]$MemoryGB = 0,
	# Only the first N shards of the seeds, and no merge after them (measuring the precompile: the static cache stays
	# incomplete); 0: all.
	[int]$ShardLimit = 0,
    [string]$CacheRoot = '',
	[string]$Exe = $(if (Test-Path "$PSScriptRoot\kyty_shader_precompile.exe") { "$PSScriptRoot\kyty_shader_precompile.exe" } else { "$PSScriptRoot\_Build\windows\kyty_shader_precompile.exe" })
)
$ErrorActionPreference = 'Stop'
# Where the precompiler runs (as the emulator does: it reads the launch configuration there).
$toolDirectory = $PSScriptRoot
if (!$CacheRoot) { $CacheRoot = if ($env:KYTY_PIPELINE_CACHE_ROOT) { $env:KYTY_PIPELINE_CACHE_ROOT } else { Join-Path $PSScriptRoot '_PipelineCache' } }
$CacheRoot = [IO.Path]::GetFullPath($CacheRoot)
$env:KYTY_PIPELINE_CACHE_ROOT = $CacheRoot

if ($Affinity -eq 0) {
	$config = @("$PSScriptRoot\launch.json", "$PSScriptRoot\run-windows.json") | Where-Object { Test-Path $_ } |
		Select-Object -First 1
	foreach ($cpu in (Get-Content $config -Raw | ConvertFrom-Json).cpu_affinity) { $Affinity = $Affinity -bor ([int64]1 -shl [int]$cpu) }
}
if ($LogicalCpus -le 0) { $LogicalCpus = [Environment]::ProcessorCount }
if ($PhysicalCores -le 0) {
	$PhysicalCores = try { [math]::Max(1, [int](Get-CimInstance Win32_Processor | Measure-Object NumberOfCores -Sum).Sum) } catch { $LogicalCpus }
}
$all = if ($LogicalCpus -ge 64) { [int64]-1 } else { ([int64]1 -shl $LogicalCpus) - 1 }
$mask = $Affinity -band $all
if ($mask -eq 0) { $mask = $all }
$cpus = 0
for ($bit = 0; $bit -lt 64; $bit++) { if ($mask -band ([int64]1 -shl $bit)) { $cpus++ } }
# The processes (Jobs), threads each and power of the shards, and the threads of the single-process steps (Budget).
# By default the budget is the allowed CPUs' share of the cores less one, so one core stays free for the rest of
# the PC. The shards' threads fill the budget's cores with their SMT siblings: a process compiles a big compute
# pipeline nearly alone (the driver) while its other threads wait, so the CPU stays far below its threads (8 cores,
# 16 threads: 2 processes of 3 threads kept 40 % of it busy; 7 of 2 about 80 %, in about half the time). A process
# takes up to about 1 GB of memory: at most one per 1.5 GB. EcoQoS is opt-in because precompilation blocks game
# launch. -FullSpeed: every allowed CPU, with EcoQoS disabled.
function Get-PrecompilePlan([int]$cpus, [int]$logical, [int]$physical, [int]$threads, [int]$jobs, [bool]$fullSpeed, [bool]$ecoQoS,
	[int]$memoryGB) {
	$memoryJobs = [math]::Max(1, [math]::Floor($memoryGB / 1.5))
	if ($fullSpeed) {
		if ($jobs -le 0) { $jobs = [math]::Min($memoryJobs, [math]::Max(1, [math]::Ceiling($cpus / $threads))) }
		return [pscustomobject]@{ Jobs = [int]$jobs; Threads = [int]$threads; Budget = [int]$cpus; Efficient = $false }
	}
	$cores = [math]::Floor($cpus * [math]::Min($physical, $logical) / $logical)
	$budget = [math]::Max(1, $cores - 1)
	# The budget's threads: its cores with their SMT siblings.
	$budgetThreads = [math]::Max(1, [math]::Floor($budget * $logical / [math]::Min($physical, $logical)))
	$threads = [math]::Min($threads, $budgetThreads)
	if ($jobs -le 0) { $jobs = [math]::Min($memoryJobs, [math]::Max(1, [math]::Floor($budgetThreads / $threads))) }
	[pscustomobject]@{ Jobs = [int]$jobs; Threads = [int]$threads; Budget = [int]$budget; Efficient = $ecoQoS }
}
if ($MemoryGB -le 0) {
	# Without the size (WMI unavailable) the memory does not limit the processes.
	$MemoryGB = try { [int]((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB) } catch { 0 }
	if ($MemoryGB -le 0) { $MemoryGB = 1024 }
}
$plan = Get-PrecompilePlan $cpus $LogicalCpus $PhysicalCores $Threads $Jobs $FullSpeed.IsPresent $EcoQoS.IsPresent $MemoryGB
if ($PlanOnly) {
	[pscustomobject]@{ Cpus = $cpus; Jobs = $plan.Jobs; Threads = $plan.Threads; Budget = $plan.Budget; Efficient = $plan.Efficient } |
		ConvertTo-Json -Compress
	return
}

if (!(Test-Path $Exe)) { throw "missing $Exe; build it with build-windows.cmd kyty_shader_precompile" }
# The game's sce_sys\param.json (its title and version). The game is its folder (eboot.bin and sce_sys) or the
# folder packed into a ZArchive (a .zar file, read without extracting it): a .zar's is copied out by the program.
function Read-GameParam([string]$game) {
	if (!$game) { return $null }
	if (Test-Path -LiteralPath "$game\sce_sys\param.json") { return Get-Content -LiteralPath "$game\sce_sys\param.json" -Raw -Encoding UTF8 | ConvertFrom-Json }
	$tool = $Exe
	if ($game -notmatch '\.zar$' -or !(Test-Path -LiteralPath $game -PathType Leaf) -or !(Test-Path $tool)) { return $null }
	$copy = [IO.Path]::GetTempFileName()
	try {
		& $tool --game $game --param $copy 2>$null | Out-Null
		if ($LASTEXITCODE -eq 0) { return Get-Content -LiteralPath $copy -Raw -Encoding UTF8 | ConvertFrom-Json }
	} catch {
	} finally {
		Remove-Item -LiteralPath $copy -ErrorAction SilentlyContinue
	}
	return $null
}

# Caches made from the game's files are named by its title and version (seeds-<title>_<version>.seeds,
# _PipelineCache\static\<title>_<version>.*, the warmup recordings; versions need not share shaders). Those named
# by the title alone are from before: the game's that was played last (the remembered one), so they take its
# name (run-windows.ps1 has the same).
function Rename-TitleCaches([string]$game) {
	$info = Read-GameParam $game
	if (!$info -or !$info.titleId -or !$info.contentVersion) { return }
	$title = $info.titleId
	$id = "$($title)_$($info.contentVersion)"
	$moves = @()
	foreach ($dir in @($PSScriptRoot, "$PSScriptRoot\_Build\static-precompile")) {
		$moves += , @("$dir\seeds.seeds", "$dir\seeds-$id.seeds")
		$moves += , @("$dir\recorded.seeds", "$dir\recorded-$id.seeds")
	}
	foreach ($file in @(Get-ChildItem "$CacheRoot\static\$title.*" -File -ErrorAction SilentlyContinue)) {
		$moves += , @($file.FullName, (Join-Path $file.DirectoryName ($id + $file.Name.Substring($title.Length))))
	}
	foreach ($file in @(Get-ChildItem "$CacheRoot\warmup-v2\*\$title.shaders" -File -ErrorAction SilentlyContinue)) {
		$moves += , @($file.FullName, (Join-Path $file.DirectoryName "$id.shaders"))
	}
	foreach ($move in $moves) {
		if (!(Test-Path $move[0]) -or (Test-Path $move[1])) { continue }
        $expected = (Get-FileHash -LiteralPath $move[0] -Algorithm SHA256).Hash
        $staging = $move[1] + '.import.' + [Guid]::NewGuid().ToString('N')
        $source = $target = $null
        try {
            $source = [IO.File]::OpenRead($move[0])
            $target = [IO.File]::Open($staging, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            $source.CopyTo($target)
            $target.Flush($true)
        } finally {
            if ($target) { $target.Dispose() }
            if ($source) { $source.Dispose() }
        }
        if ((Get-FileHash -LiteralPath $staging -Algorithm SHA256).Hash -ne $expected) { throw 'cache source changed while copying; original retained' }
        [IO.File]::Move($staging, $move[1])
        Write-Host "caches:   copied legacy data for $id; original retained (the loader still checks compatibility)"
	}
}

# The game run-windows.ps1 was given last, else the default folder.
$lastGame = if (Test-Path "$PSScriptRoot\game-path.txt") { "$(Get-Content "$PSScriptRoot\game-path.txt" -Raw)".Trim() }
if (!$lastGame) { $lastGame = "$env:USERPROFILE\Documents\PPSA01341-app0" }
Rename-TitleCaches $lastGame
if (!$Game) { $Game = $lastGame }
$info = Read-GameParam $Game
if (!$info) { throw "no sce_sys\param.json in $Game (-Game <folder or .zar>, or start the game once to choose it)" }
$gameId = "$($info.titleId)_$($info.contentVersion)"
$defaultSeeds = !$Seeds
if (!$Seeds) {
	$Seeds = if ((Test-Path "$PSScriptRoot\seeds-$gameId.seeds") -or !(Test-Path "$PSScriptRoot\_Build")) { "$PSScriptRoot\seeds-$gameId.seeds" } else { "$PSScriptRoot\_Build\static-precompile\seeds-$gameId.seeds" }
}
# The seed file another program made (<file>.program: its size and time) is made again (a release unpacked over the
# last), and replaced where it lists something else (the vertex shaders' indirect draws' records since 10-10): the
# static cache is then older than it, out of date (--status), and the precompile compiles what is new.
$programStamp = ''
if (Test-Path -LiteralPath $Exe) {
    $producer = (& $Exe --producer 2>$null | Out-String).Trim()
    $programStamp = if ($LASTEXITCODE -eq 0 -and $producer -match '^[0-9a-f]{64}$') { $producer } else { (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash }
}
$producerStates = Join-Path $PSScriptRoot 'tools\local\static-precompile\pass-states.json'
if (Test-Path -LiteralPath $producerStates) { $programStamp += ':' + (Get-FileHash -LiteralPath $producerStates -Algorithm SHA256).Hash }
$madeBy = "$Seeds.program"
$remake = $defaultSeeds -and (Test-Path $Seeds) -and (!(Test-Path $madeBy) -or "$(Get-Content $madeBy -Raw)".Trim() -ne $programStamp)
if (!(Test-Path $Seeds) -or $remake) {
	# Every shader the game ships, with the pipelines it draws them with (from the game files, by the program:
	# precompile.py seeds without Python).
	New-Item -ItemType Directory -Force (Split-Path $Seeds) | Out-Null
	$made = if ($remake) { "$Seeds.new" } else { $Seeds }
	& $Exe --game $Game --make-seeds $made --states "$PSScriptRoot\tools\local\static-precompile\pass-states.json"
	if ($LASTEXITCODE) { throw 'making the seed file failed (kyty_shader_precompile --make-seeds)' }
	if ($remake -and (Get-FileHash $made).Hash -eq (Get-FileHash $Seeds).Hash) { Remove-Item $made } elseif ($remake) { Move-Item -Force $made $Seeds }
	Set-Content $madeBy $programStamp
}
if ($Recorded -eq '*') {
	$Recorded = Join-Path (Split-Path $Seeds) "recorded-$gameId.seeds"
	$hints = "$PSScriptRoot\tools\local\static-precompile\hints-$gameId.hints"
	if (!(Test-Path $Recorded) -and (Test-Path $hints)) {
		# (Made again each run: the hints of this release, the code of this seed file.)
		$Recorded = Join-Path (Split-Path $Seeds) "hinted-$gameId.seeds"
		& $Exe --game $Game --seeds $Seeds --apply-hints $hints --out $Recorded
		if ($LASTEXITCODE) {
			Write-Host 'the specialization hints could not be applied (kyty_shader_precompile --apply-hints): the seeds alone'
			$Recorded = ''
		}
	}
}
# The shards an earlier run finished (<title>_<version>.done: the inputs they were made from, then one shard
# name a line), when its inputs were this run's: the precompiler and the seed files by content, the switches
# that change what they compile to, and the display adapters with their drivers (the store is the GPU's and the
# driver's: static-v1\driver-v1\<device>). And only while this GPU and driver have a store (--status), which
# holds what those shards made.
function Get-ShardInputs([string[]]$files) {
	$hashes = $files | Where-Object { $_ -and (Test-Path $_) } | ForEach-Object { (Get-FileHash $_).Hash }
	# The PCI display adapters (not the remote, virtual or capture ones that come and go with a session).
	$adapters = try {
		Get-CimInstance Win32_VideoController | Where-Object { "$($_.PNPDeviceID)" -like 'PCI\*' } | Sort-Object PNPDeviceID |
			ForEach-Object { "$($_.PNPDeviceID)=$($_.DriverVersion)" }
	} catch { 'adapters unknown' }
	$switches = "KYTY_PORTABLE_SHADERS=$env:KYTY_PORTABLE_SHADERS KYTY_ROBUST_BUFFERS=$env:KYTY_ROBUST_BUFFERS"
	((@($hashes) + @($adapters) + $switches) -join ' ')
}
# Whether this GPU and driver have a store (precompiler --status): 'yes', 'no' (stale), or 'unknown' when the
# probe itself failed (no device, a crash), which must not discard the list.
function Get-StoreOfThisDevice {
	$out = Join-Path ([IO.Path]::GetTempPath()) "kyty-precompile-status-$PID"
	try {
		$probe = Start-Process -FilePath $Exe -ArgumentList @('--game', "`"$Game`"", '--seeds', "`"$Seeds`"", '--status') `
			-WorkingDirectory $toolDirectory -NoNewWindow -Wait -PassThru -RedirectStandardOutput "$out.out" -RedirectStandardError "$out.err"
		$status = @(Get-Content "$out.out" -ErrorAction SilentlyContinue)
		if ($probe.ExitCode -ne 0 -or !($status -match '^static cache ')) { return 'unknown' }
		if ($status -match '^static cache (current|outdated)') { 'yes' } else { 'no' }
	} catch {
		'unknown'
	} finally {
		Remove-Item "$out.out", "$out.err" -ErrorAction SilentlyContinue
	}
}
function Read-DoneShards([string]$file, [string]$inputs) {
	$set = [Collections.Generic.HashSet[string]]::new()
	if (Test-Path $file) {
		$lines = @(Get-Content $file)
		$store = if ($lines.Count -and $lines[0] -eq $inputs) { Get-StoreOfThisDevice } else { 'no' }
		if ($store -eq 'yes') {
			foreach ($line in $lines | Select-Object -Skip 1) { [void]$set.Add($line) }
		} elseif ($store -eq 'no') {
			Remove-Item $file
		} else {
			Write-Host 'precompile: could not tell whether this GPU has a store; the finished-shards list is kept but not used'
		}
	}
	return , $set
}
function Add-DoneShard([string]$file, [string]$inputs, [string]$name) {
	if (!(Test-Path $file)) { New-Item -ItemType Directory -Force (Split-Path $file) | Out-Null; Set-Content $file $inputs }
	Add-Content $file $name
}
# The pipeline recipes of a seed file (its body after the identity line and checksum: the records, each its word
# count and words, then the recipes' count).
function Get-SeedPipelines([string]$file) {
	$stream = [IO.File]::OpenRead($file)
	try {
		$reader = [IO.BinaryReader]::new($stream)
		while ($reader.ReadByte() -ne 10) {}
		$null = $reader.ReadUInt64()
		$records = $reader.ReadUInt32()
		for ($i = 0; $i -lt $records; $i++) { $null = $stream.Seek([int64]$reader.ReadUInt32() * 4, [IO.SeekOrigin]::Current) }
		return $reader.ReadUInt32()
	} finally {
		$stream.Dispose()
	}
}
$Jobs, $Threads = $plan.Jobs, $plan.Threads
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logs = if (Test-Path "$PSScriptRoot\_Build") { "$PSScriptRoot\_Build\run-logs" } else { "$PSScriptRoot\logs" }
$logs = "$logs\$stamp-precompile"
$logName = $logs.Substring($PSScriptRoot.Length + 1)
New-Item -ItemType Directory -Force $logs | Out-Null
$begin = Get-Date

# Opt-in Windows execution-speed throttling for a precompile process.
if ($plan.Efficient -and !('Kyty.ProcessPower' -as [type])) {
	Add-Type -Namespace Kyty -Name ProcessPower -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)]
public struct PowerThrottlingState { public uint Version; public uint ControlMask; public uint StateMask; }
[DllImport("kernel32.dll", SetLastError = true)]
static extern bool SetProcessInformation(IntPtr process, int informationClass, ref PowerThrottlingState information, uint size);
// ProcessPowerThrottling (4), PROCESS_POWER_THROTTLING_CURRENT_VERSION (1), PROCESS_POWER_THROTTLING_EXECUTION_SPEED (1).
public static bool Throttle(IntPtr process) {
	var state = new PowerThrottlingState { Version = 1, ControlMask = 1, StateMask = 1 };
	return SetProcessInformation(process, 4, ref state, 12);
}
'@
}
$reducedPower = 'on'
# Each process gets an empty NVIDIA disk cache of its own (nothing trained yet), removed when it is done.
$nvidiaCaches = Join-Path ([IO.Path]::GetTempPath()) "kyty-precompile-$stamp"
function Start-Precompile([string]$name, [string[]]$arguments) {
	$env:__GL_SHADER_DISK_CACHE_PATH = Join-Path $nvidiaCaches $name
	New-Item -ItemType Directory -Force $env:__GL_SHADER_DISK_CACHE_PATH | Out-Null
	$process = Start-Process -FilePath $Exe -ArgumentList (@('--game', "`"$Game`"") + $arguments) -NoNewWindow -PassThru `
		-WorkingDirectory $PSScriptRoot -RedirectStandardOutput "$logs\$name.out.log" -RedirectStandardError "$logs\$name.err.log"
	$process.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::BelowNormal
	if ($plan.Efficient -and ![Kyty.ProcessPower]::Throttle($process.Handle) -and $script:reducedPower -ne 'unavailable') {
		$script:reducedPower = 'unavailable'
		Write-Host 'precompile: Windows did not enable EcoQoS; continuing with its default power policy'
	}
	$process.ProcessorAffinity = [IntPtr]$mask
	$null = $process.Handle # keeps the exit code readable after the process ends
	$process | Add-Member NoteProperty NvidiaCache $env:__GL_SHADER_DISK_CACHE_PATH -PassThru
}
function Remove-NvidiaCache($process) { Remove-Item -Recurse -Force $process.NvidiaCache -ErrorAction SilentlyContinue }
function Wait-Precompile($process, [string]$what) {
	$process.WaitForExit()
	Remove-NvidiaCache $process
	if ($process.ExitCode -ne 0) { throw "$what failed; logs: $logName" }
}

Write-Host "precompile: $Seeds, $Shards shards, $Jobs at a time of $Threads threads, affinity 0x$('{0:X}' -f $mask), $(if ($plan.Efficient) { 'EcoQoS power saving (-FullSpeed disables it)' } else { 'EcoQoS off' }); logs $logName"
try {
if ($Coverage) {
	$out = [IO.Path]::ChangeExtension($Seeds, '.compiled.shaders')
	Wait-Precompile (Start-Precompile 'coverage' @('--seeds', "`"$Seeds`"", '--no-pipelines', '--threads', "$($plan.Budget)",
		'--out', "`"$out`"")) 'coverage'
	Write-Host "wrote $out"
	return
}
# The compiled inputs for the emulator's shader prefetch: the seeds translated as the full run does,
# without pipelines (half a minute).
$inputs = @('--seeds', "`"$Seeds`"", '--no-pipelines', '--threads', "$($plan.Budget)", '--static-inputs')
if ($InputsOnly) {
	Wait-Precompile (Start-Precompile 'inputs' $inputs) 'inputs'
	Write-Host "inputs saved beneath $CacheRoot (producer and integrity checked on load)"
	return
}
# The shards an interrupted run finished first: what they hold is not compiled again.
Wait-Precompile (Start-Precompile 'merge-before' @('--merge')) 'merge'
$pending = [System.Collections.Generic.LinkedList[object]]::new()
# A shard's name (its logs, its NVIDIA cache, the finished-shards list): contiguous slices (b) and every n-th
# hold different items under the same i/n.
function Get-ShardName([string]$seedFile, [string]$shard, [bool]$blocks) {
	'shard' + ($shard -replace '/', 'of') + $(if ($blocks) { 'b' } else { '' }) + '-' + [IO.Path]::GetFileNameWithoutExtension($seedFile)
}
# What recorded play uses first (exactly the pipelines the game asks for: a fifth of the work), then every
# shader of the game as the seeds guess it: a run stopped part way has the ones that matter most.
$recordedShards = $ShardLimit -le 0 -and $Recorded -and (Test-Path $Recorded)
# The precompiler names a shard's file after its seed file's stem and i/n only: two tiers must not share a stem.
if ($recordedShards -and [IO.Path]::GetFileNameWithoutExtension($Recorded) -eq [IO.Path]::GetFileNameWithoutExtension($Seeds)) {
	throw "the recorded seed file must not be named like the seed file ($([IO.Path]::GetFileName($Recorded)))"
}
$doneFile = Join-Path $CacheRoot "static-v1\$gameId.done"
$doneInputs = Get-ShardInputs @($Exe, $Seeds, $(if ($recordedShards) { $Recorded }))
$done = if ($ShardLimit -le 0) { Read-DoneShards $doneFile $doneInputs } else { [Collections.Generic.HashSet[string]]::new() }
$skipped = 0
$queue = @()
if ($recordedShards) {
	# About as many pipelines a shard as the seeds' (100: a recording of every world holds about 16000).
	# Contiguous slices (--shard-blocks): a recorded file ranked by use has what play needs first in its
	# first shards, which a run stopped early (-Minutes) then holds.
	$count = [math]::Max(1, [math]::Ceiling((Get-SeedPipelines $Recorded) / 100))
	for ($i = 0; $i -lt $count; $i++) { $queue += , @($Recorded, "$i/$count", $true) }
} elseif ($RecordedOnly -and $ShardLimit -le 0) {
	Write-Host 'precompile: no recorded play or hints for this game version: every shader instead (-RecordedOnly ignored)'
}
if (!$RecordedOnly -or !$recordedShards) {
	$count = if ($ShardLimit -gt 0) { [math]::Min($ShardLimit, $Shards) } else { $Shards }
	for ($i = 0; $i -lt $count; $i++) { $queue += , @($Seeds, "$i/$Shards", $false) }
}
foreach ($entry in $queue) {
	if ($done.Contains((Get-ShardName $entry[0] $entry[1] $entry[2]))) { $skipped++ } else { [void]$pending.AddLast($entry) }
}
if ($skipped) { Write-Host "precompile: $skipped shards an earlier run finished are kept, not run again" }
# The shards share the pipelines they hold (PipelineBinaryWriter::Claimed): one that the seeds of several
# shards make (a third of them) is compiled once.
$env:KYTY_PRECOMPILE_CLAIMS = Join-Path $nvidiaCaches 'claims'
$running = @{}
$finished = 0
$split = 0
$shown = Get-Date
# The budget counts from the first shard: making seeds and hints and merging an earlier run's shards come first.
$deadline = if ($Minutes -gt 0) { (Get-Date).AddMinutes($Minutes) } else { [datetime]::MaxValue }
$budgetStopped = $false
while (($pending.Count -and !$budgetStopped) -or $running.Count) {
	while ($pending.Count -and $running.Count -lt $Jobs -and !$budgetStopped) {
		if ((Get-Date) -ge $deadline) {
			$budgetStopped = $true
			Write-Host ("precompile: the {0}-minute budget is used; {1} running shards finish, {2} wait for a later run" -f $Minutes,
				$running.Count, $pending.Count)
			break
		}
		$seedFile, $shard, $blocks = $pending.First.Value
		$pending.RemoveFirst()
		$name = Get-ShardName $seedFile $shard $blocks
		$arguments = @('--seeds', "`"$seedFile`"", '--shard', $shard, '--threads', "$Threads")
		if ($blocks) { $arguments += '--shard-blocks' }
		$running[$name] = @($seedFile, $shard, (Start-Precompile $name $arguments), $blocks)
	}
	Start-Sleep -Milliseconds 250
	foreach ($name in @($running.Keys)) {
		$seedFile, $shard, $process, $blocks = $running[$name]
		if (!$process.HasExited) { continue }
		$running.Remove($name)
		Remove-NvidiaCache $process
		if ($process.ExitCode -eq 3) {
			$i, $n = [int[]]($shard -split '/')
			if ($n -ge 65536) { throw "$name cannot be split further; logs: $logName" }
			# Resolve smaller retries before expanding all remaining parents. A FIFO
			# frontier can grow for hours without reaching a completed or terminal child.
			# Both halves remain scheduled (every n-th: i and i + n of 2n; contiguous slices: 2i and
			# 2i + 1); partial shard data stays available. Halves an earlier run finished are kept
			# (a shard that splits splits again).
			$halves = if ($blocks) { "$(2 * $i + 1)/$(2 * $n)", "$(2 * $i)/$(2 * $n)" } else { "$($i + $n)/$(2 * $n)", "$i/$(2 * $n)" }
			foreach ($half in $halves) {
				if ($done.Contains((Get-ShardName $seedFile $half $blocks))) { $skipped++ } else { [void]$pending.AddFirst(@($seedFile, $half, $blocks)) }
			}
			$split++
		} elseif ($process.ExitCode -ne 0) {
			throw "$name failed; logs: $logName"
		} else {
			$finished++
			if ($ShardLimit -le 0) { Add-DoneShard $doneFile $doneInputs $name }
		}
	}
	if (((Get-Date) - $shown).TotalSeconds -ge 60) {
		$shown = Get-Date
		Write-Host ("  {0:hh\:mm\:ss} shards: {1} done, {2} running, {3} waiting ({4} split)" -f ((Get-Date) - $begin), $finished,
			$running.Count, $pending.Count, $split)
	}
}
$env:KYTY_PRECOMPILE_CLAIMS = $null
if ($ShardLimit -gt 0) {
	Write-Host ("{0} of {1} shards done in {2:hh\:mm\:ss} ({3} split); not merged (-ShardLimit)" -f $finished, $Shards, ((Get-Date) - $begin), $split)
	return
}
# -RecordedOnly and a used -Minutes budget keep what an earlier run made (--prune would keep only this run's),
# and mark the cache as partial: the store is newer than the seeds, so --status calls it current, and
# run-windows.ps1 -Prompt would stop offering the full run. A full run removes the mark. A run that kept
# shards an earlier one finished does not prune either: their pipelines are in no shard file of this run (so
# pipelines that no seed makes any more stay until a full run that starts from no finished shards).
$partialRun = ($RecordedOnly -and $recordedShards) -or $budgetStopped
$mergeArguments = if ($partialRun -or $skipped) { @('--merge') } else { @('--merge', '--prune') }
$partial = Join-Path $CacheRoot "static-v1\$gameId.partial"
Wait-Precompile (Start-Precompile 'merge' $mergeArguments) 'merge'
if ($partialRun) {
	New-Item -ItemType Directory -Force (Split-Path $partial) | Out-Null
	$why = @()
	if ($RecordedOnly -and $recordedShards) { $why += 'only what recorded play uses (-RecordedOnly)' }
	if ($budgetStopped) { $why += "the $Minutes-minute budget ended the run (-Minutes)" }
	Set-Content $partial "precompile-windows.ps1: $($why -join '; '); a full run removes this"
} else {
	Remove-Item $partial, $doneFile -ErrorAction SilentlyContinue
}
Wait-Precompile (Start-Precompile 'inputs' $inputs) 'inputs'
$cache = Get-ChildItem "$CacheRoot\static-v1\driver-v1\*\$gameId.bin", "$CacheRoot\static-v1\driver-v1\*\$gameId.binaries" -ErrorAction SilentlyContinue |
	Sort-Object LastWriteTime | Select-Object -Last 1
Write-Host ("done in {0:hh\:mm\:ss}: {1} ({2:N0} MB; {3} shards split)" -f ((Get-Date) - $begin), $cache.FullName, ($cache.Length / 1MB), $split)
} finally {
	# A failed shard ends the run: the others are stopped too.
	if ($running) { foreach ($entry in $running.Values) { if (!$entry[2].HasExited) { $entry[2].Kill(); $entry[2].WaitForExit() } } }
	$env:__GL_SHADER_DISK_CACHE_PATH = $null
	$env:KYTY_PRECOMPILE_CLAIMS = $null
	Remove-Item -Recurse -Force $nvidiaCaches -ErrorAction SilentlyContinue
}
