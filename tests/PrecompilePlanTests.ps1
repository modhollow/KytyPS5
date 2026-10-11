# precompile-windows.ps1 -PlanOnly: by default the precompile leaves one core free, does not count SMT
# siblings and runs its processes at reduced power; -FullSpeed keeps every allowed CPU at full power as before.
# Explicit -Jobs/-Threads stay as given.
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot '..\precompile-windows.ps1'
$failures = 0

function Get-Plan([hashtable]$arguments) {
	$text = & $script -PlanOnly @arguments
	return "$text" | ConvertFrom-Json
}

function Test-Plan([string]$name, [hashtable]$arguments, [hashtable]$expected) {
	$plan = Get-Plan $arguments
	foreach ($key in $expected.Keys) {
		if ("$($plan.$key)" -ne "$($expected[$key])") {
			Write-Host "FAIL: $name`: $key is $($plan.$key), expected $($expected[$key])"
			$script:failures++
		}
	}
}

$all16 = [int64]0xFFFF
Test-Plan '16 threads on 8 cores (SMT): cores less one, reduced power' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = $all16 } `
	@{ Cpus = 16; Budget = 7; Threads = 3; Jobs = 2; Efficient = $true }
Test-Plan '16 threads on 8 cores, -FullSpeed: every CPU, full power' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = $all16; FullSpeed = $true } `
	@{ Cpus = 16; Budget = 16; Threads = 3; Jobs = 6; Efficient = $false }
Test-Plan '8 cores without SMT' @{ LogicalCpus = 8; PhysicalCores = 8; Affinity = [int64]0xFF } `
	@{ Cpus = 8; Budget = 7; Threads = 3; Jobs = 2; Efficient = $true }
Test-Plan '32 threads on 16 cores' @{ LogicalCpus = 32; PhysicalCores = 16; Affinity = [int64]0xFFFFFFFF } `
	@{ Cpus = 32; Budget = 15; Threads = 3; Jobs = 5; Efficient = $true }
Test-Plan '64 threads on 32 cores' @{ LogicalCpus = 64; PhysicalCores = 32; Affinity = [int64]-1 } `
	@{ Cpus = 64; Budget = 31; Threads = 3; Jobs = 10; Efficient = $true }
Test-Plan '4 threads on 2 cores: one thread' @{ LogicalCpus = 4; PhysicalCores = 2; Affinity = [int64]0xF } `
	@{ Cpus = 4; Budget = 1; Threads = 1; Jobs = 1; Efficient = $true }
Test-Plan '2 threads on 1 core: never below one' @{ LogicalCpus = 2; PhysicalCores = 1; Affinity = [int64]0x3 } `
	@{ Cpus = 2; Budget = 1; Threads = 1; Jobs = 1; Efficient = $true }
Test-Plan 'launch affinity of 8 of 16 threads: its share of the cores' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = [int64]0xFF } `
	@{ Cpus = 8; Budget = 3; Threads = 3; Jobs = 1; Efficient = $true }
Test-Plan 'explicit -Jobs stays' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = $all16; Jobs = 4 } `
	@{ Budget = 7; Threads = 3; Jobs = 4; Efficient = $true }
Test-Plan 'explicit -Threads 2' @{ LogicalCpus = 16; PhysicalCores = 8; Affinity = $all16; Threads = 2 } `
	@{ Budget = 7; Threads = 2; Jobs = 3; Efficient = $true }

if ($failures) {
	Write-Host "PrecompilePlanTests: $failures failures"
	exit 1
}
Write-Host 'PrecompilePlanTests: all passed'
exit 0
