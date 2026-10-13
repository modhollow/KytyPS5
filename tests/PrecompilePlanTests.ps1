# precompile-windows.ps1 -PlanOnly: by default the precompile leaves one core free, does not count SMT
# siblings, fills the budget's cores and their SMT siblings with two-thread processes (a process keeps far fewer
# CPUs busy than it has threads), at most one per 1.5 GB of memory. EcoQoS is explicit; -FullSpeed keeps every allowed CPU at full power and overrides EcoQoS.
# Explicit -Jobs/-Threads stay as given.
param([string]$ScriptPath = (Join-Path $PSScriptRoot '..\precompile-windows.ps1'))
$ErrorActionPreference = 'Stop'
$script = $ScriptPath
$failures = 0

function Get-Plan([hashtable]$arguments) {
	if (!$arguments.ContainsKey('MemoryGB')) { $arguments.MemoryGB = 64 }
	$text = & $script -PlanOnly @arguments
	return "$text" | ConvertFrom-Json
}

function Test-Plan([string]$name, [hashtable]$arguments, [hashtable]$expected) {
	try { $plan = Get-Plan $arguments } catch {
		Write-Host "FAIL: $name`: $($_.Exception.Message)"
		$script:failures++
		return
	}
	foreach ($key in $expected.Keys) {
		if ("$($plan.$key)" -ne "$($expected[$key])") {
			Write-Host "FAIL: $name`: $key is $($plan.$key), expected $($expected[$key])"
			$script:failures++
		}
	}
}

$all16 = [int64]0xFFFF
Test-Plan '16 threads on 8 cores (SMT): cores less one, EcoQoS off' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = $all16 } `
	@{ Cpus = 16; Budget = 7; Threads = 2; Jobs = 7; Efficient = $false }
Test-Plan '16 threads on 8 cores, -FullSpeed: every CPU, full power' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = $all16; FullSpeed = $true } `
	@{ Cpus = 16; Budget = 16; Threads = 2; Jobs = 8; Efficient = $false }
Test-Plan '8 cores without SMT' @{ LogicalCpus = 8; PhysicalCores = 8; Affinity = [int64]0xFF } `
	@{ Cpus = 8; Budget = 7; Threads = 2; Jobs = 3; Efficient = $false }
Test-Plan '32 threads on 16 cores' @{ LogicalCpus = 32; PhysicalCores = 16; Affinity = [int64]0xFFFFFFFF } `
	@{ Cpus = 32; Budget = 15; Threads = 2; Jobs = 15; Efficient = $false }
Test-Plan '64 threads on 32 cores' @{ LogicalCpus = 64; PhysicalCores = 32; Affinity = [int64]-1 } `
	@{ Cpus = 64; Budget = 31; Threads = 2; Jobs = 31; Efficient = $false }
Test-Plan '4 threads on 2 cores: one thread' @{ LogicalCpus = 4; PhysicalCores = 2; Affinity = [int64]0xF } `
	@{ Cpus = 4; Budget = 1; Threads = 2; Jobs = 1; Efficient = $false }
Test-Plan '2 threads on 1 core: never below one' @{ LogicalCpus = 2; PhysicalCores = 1; Affinity = [int64]0x3 } `
	@{ Cpus = 2; Budget = 1; Threads = 2; Jobs = 1; Efficient = $false }
Test-Plan 'launch affinity of 8 of 16 threads: its share of the cores' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = [int64]0xFF } `
	@{ Cpus = 8; Budget = 3; Threads = 2; Jobs = 3; Efficient = $false }
Test-Plan 'explicit -Jobs stays' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = $all16; Jobs = 4 } `
	@{ Budget = 7; Threads = 2; Jobs = 4; Efficient = $false }
Test-Plan 'explicit -Threads 2' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = $all16; Threads = 2 } `
	@{ Budget = 7; Threads = 2; Jobs = 7; Efficient = $false }

Test-Plan 'explicit EcoQoS: same bounded budget' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = $all16; EcoQoS = $true } `
	@{ Cpus = 16; Budget = 7; Threads = 2; Jobs = 7; Efficient = $true }
Test-Plan 'explicit EcoQoS with jobs/threads: same concurrency' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = $all16; Jobs = 4; Threads = 2; EcoQoS = $true } `
	@{ Budget = 7; Threads = 2; Jobs = 4; Efficient = $true }
Test-Plan 'FullSpeed overrides explicit EcoQoS' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = $all16; FullSpeed = $true; EcoQoS = $true } `
	@{ Cpus = 16; Budget = 16; Threads = 2; Jobs = 8; Efficient = $false }
Test-Plan 'explicit -Threads 4: the budget threads in 4s' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = $all16; Threads = 4 } `
	@{ Budget = 7; Threads = 4; Jobs = 3; Efficient = $false }
Test-Plan '4 cores without SMT, 8 GB' @{ LogicalCpus = 4; PhysicalCores = 4; Affinity = [int64]0xF; MemoryGB = 8 } `
	@{ Cpus = 4; Budget = 3; Threads = 2; Jobs = 1; Efficient = $false }
Test-Plan 'memory caps the processes: 8 GB' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = $all16; MemoryGB = 8 } `
	@{ Budget = 7; Threads = 2; Jobs = 5; Efficient = $false }
Test-Plan 'memory caps -FullSpeed too: 6 GB' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = $all16; MemoryGB = 6; FullSpeed = $true } `
	@{ Budget = 16; Threads = 2; Jobs = 4; Efficient = $false }

if ($failures) {
	Write-Host "PrecompilePlanTests: $failures failures"
	exit 1
}
Write-Host 'PrecompilePlanTests: all passed'
exit 0
