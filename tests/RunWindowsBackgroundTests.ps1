# run-windows.ps1 -BackgroundPrecompile starts the background precompile detached: a caller that reads
# run-windows.ps1's output (a launcher, a pipeline) must not wait for that process, which runs as long as the game.
# Runs the script's own Start-Detached and ConvertTo-Literal and its own background start statement in a child
# PowerShell (Windows PowerShell and pwsh, where present) whose output is piped. The child lives in a folder whose name
# has a space, brackets and every apostrophe PowerShell ends a single-quoted literal at (U+0027, U+2018, U+2019, U+201A, U+201B), beside a stand-in
# precompile-windows.ps1 that records its arguments. Checks that the pipe ends at once while the stand-in still runs
# to its end, gets every argument back unchanged and writes the log the statement names. No game, GPU or precompiler.
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot '..\run-windows.ps1'
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'run-windows.ps1 does not parse' }
$failures = 0
function Check([bool]$condition, [string]$message) {
    if (!$condition) { ++$script:failures; Write-Host "FAIL: $message" }
}
$functions = @{}
foreach ($name in 'Start-Detached', 'ConvertTo-Literal') {
    $functions[$name] = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }, $true))[0]
    Check ($null -ne $functions[$name]) "run-windows.ps1 has no $name"
}
# The background precompile is started through Start-Detached.
$calls = @($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Start-Detached' }, $true))
$call = $calls | Where-Object { $_.Extent.Text -match 'precompile-windows\.ps1' -and $_.Extent.Text -match '-Background' } |
    Select-Object -First 1
Check ($null -ne $call) 'the background precompile is not started with Start-Detached'

$quotes = [string]::new([char[]](0x27, 0x2019, 0x2018, 0x201A, 0x201B))
# The values the child gives run-windows.ps1's variables, relative to its folder.
$setup = @'
$quotes = [string]::new([char[]](0x27, 0x2019, 0x2018, 0x201A, 0x201B))
$Game = Join-Path $PSScriptRoot "game $quotes [1]"
$tool = Join-Path $PSScriptRoot "tool $quotes.exe"
$seeds = Join-Path $PSScriptRoot "seeds $quotes"
$CacheRoot = Join-Path $PSScriptRoot "cache $quotes"
$logDir = $PSScriptRoot
$stamp = 'stamp'
$mask = [int64]0xF0
$process = [pscustomobject]@{ Id = 4242 }
'@
# The stand-in: prints a line (for the log), waits, then records what it was given.
$standIn = @'
param([string]$Game, [int64]$Affinity, [string]$Exe, [string]$Seeds, [string]$CacheRoot, [switch]$Background, [int]$WhileProcess)
'stand-in running'
Start-Sleep -Seconds 20
$record = [ordered]@{ Game = $Game; Affinity = $Affinity; Exe = $Exe; Seeds = $Seeds; CacheRoot = $CacheRoot
    Background = [bool]$Background; WhileProcess = $WhileProcess }
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'done.json'), ($record | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
'@

$shells = @((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'), (Get-Command pwsh -ErrorAction SilentlyContinue).Source) |
    Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique
if ($functions['Start-Detached'] -and $functions['ConvertTo-Literal'] -and $call) {
    foreach ($shell in $shells) {
        $host_name = Split-Path $shell -Leaf
        $folder = Join-Path ([IO.Path]::GetTempPath()) "kyty run-windows [bg] it's $quotes $PID $host_name"
        New-Item -ItemType Directory -Force -Path $folder | Out-Null
        try {
            $utf8 = [Text.UTF8Encoding]::new($true)
            [IO.File]::WriteAllText((Join-Path $folder 'precompile-windows.ps1'), $standIn, $utf8)
            $child = $functions['Start-Detached'].Extent.Text + "`n" + $functions['ConvertTo-Literal'].Extent.Text + "`n" +
                $setup + "`n`$background = " + $call.Extent.Text + "`n'started ' + `$background.Id"
            $script = Join-Path $folder 'child.ps1'
            [IO.File]::WriteAllText($script, $child, $utf8)
            $watch = [Diagnostics.Stopwatch]::StartNew()
            $output = & $shell -NoProfile -ExecutionPolicy Bypass -File $script | Out-String
            $elapsed = $watch.Elapsed.TotalSeconds
            Check ($output -match 'started (\d+)') "${host_name}: the child did not start the command: $output"
            # Wait for the detached process itself, so its output and done.json are complete.
            if ($output -match 'started (\d+)') { Wait-Process -Id ([int]$Matches[1]) -Timeout 60 -ErrorAction SilentlyContinue }
            Check ($elapsed -lt 12) ("{0}: a caller reading the output waited {1:N1} s for the detached command" -f $host_name, $elapsed)
            $done = Join-Path $folder 'done.json'
            Check (Test-Path -LiteralPath $done) "${host_name}: the detached command did not run to its end"
            if (Test-Path -LiteralPath $done) {
                $got = [IO.File]::ReadAllText($done) | ConvertFrom-Json
                $expected = [ordered]@{ Game = Join-Path $folder "game $quotes [1]"; Affinity = 0xF0; Exe = Join-Path $folder "tool $quotes.exe"
                    Seeds = Join-Path $folder "seeds $quotes"; CacheRoot = Join-Path $folder "cache $quotes"; Background = $true; WhileProcess = 4242 }
                foreach ($key in $expected.Keys) {
                    Check ("$($got.$key)" -ceq "$($expected[$key])") "${host_name}: -$key arrived as '$($got.$key)', not '$($expected[$key])'"
                }
            }
            $log = Join-Path $folder 'stamp.precompile.log'
            Check ((Test-Path -LiteralPath $log) -and ((Get-Content -LiteralPath $log -Raw) -match 'stand-in running')) (
                "${host_name}: the background output did not go to the named log")
        } finally {
            Start-Sleep -Milliseconds 200
            Remove-Item -Recurse -Force -LiteralPath $folder -ErrorAction SilentlyContinue
        }
    }
}
Check ($shells.Count -gt 0) 'no PowerShell host found'
if ($failures) { Write-Host "RunWindowsBackgroundTests: $failures failures"; exit 1 }
Write-Host "RunWindowsBackgroundTests: all passed ($($shells.Count) PowerShell hosts)"
exit 0
