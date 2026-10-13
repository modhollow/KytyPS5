# Exercise launcher.cmd -> Windows PowerShell -> the production Play/precompile command ->
# Windows PowerShell, using isolated probe scripts instead of a window, a game or a GPU.
$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixture = Join-Path $temporaryRoot ('kyty-launcher-module-test-' + [guid]::NewGuid().ToString('N'))
$windowsPowerShell = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$originalModules = $env:PSModulePath
$originalPath = $env:PATH
$failures = 0

function Write-Utf8([string]$Path, [string]$Text) {
	[IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}
function Run-Cmd([string]$Arguments) {
	$info = [Diagnostics.ProcessStartInfo]::new()
	$info.FileName = $env:ComSpec
	$info.Arguments = $Arguments
	$info.UseShellExecute = $false
	$info.CreateNoWindow = $true
	$info.RedirectStandardInput = $true
	$info.RedirectStandardOutput = $true
	$info.RedirectStandardError = $true
	$process = [Diagnostics.Process]::Start($info)
	try {
		$process.StandardInput.Close() # The production precompile command ends with pause.
		$output = $process.StandardOutput.ReadToEndAsync()
		$errors = $process.StandardError.ReadToEndAsync()
		if (!$process.WaitForExit(15000)) {
			$process.Kill($true)
			throw 'isolated command chain timed out'
		}
		if ($process.ExitCode) { throw "command exited $($process.ExitCode): $($errors.Result)" }
	} finally { $process.Dispose() }
}

try {
	[IO.Directory]::CreateDirectory($fixture) | Out-Null
	$poison = Join-Path $fixture 'incompatible-modules\Microsoft.PowerShell.Utility'
	[IO.Directory]::CreateDirectory($poison) | Out-Null
	# Model the incompatible first Utility module: hashing fails when this module is selected.
	Write-Utf8 "$poison/Microsoft.PowerShell.Utility.psd1" "@{RootModule='Utility.psm1';ModuleVersion='7.6.0';FunctionsToExport=@('Get-FileHash');CmdletsToExport=@()}"
	Write-Utf8 "$poison/Utility.psm1" "function Get-FileHash { throw 'Get-FileHash unavailable from inherited incompatible Utility module' }"
	$probe = @'
$ErrorActionPreference = 'Stop'
try {
 Import-Module Microsoft.PowerShell.Utility -ErrorAction Stop
 $hash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
 if ($PSVersionTable.PSVersion.Major -ne 5 -or $hash.Length -ne 64) { throw 'wrong runtime or hash' }
 [IO.File]::WriteAllText($PSCommandPath + '.result', 'PASS|' + $PSVersionTable.PSVersion.ToString() + '|' + $hash)
} catch {
 [IO.File]::WriteAllText($PSCommandPath + '.result', 'FAIL|' + $_.Exception.Message)
}
exit 0
'@
	Copy-Item -LiteralPath "$source/launcher.cmd" -Destination "$fixture/launcher.cmd"
	Write-Utf8 "$fixture/run-windows.ps1" $probe
	Write-Utf8 "$fixture/precompile-windows.ps1" $probe
	# Extract the actual command builders; the fixture does not recreate their argument strings.
	$tokens = $null; $parseErrors = $null
	$ast = [Management.Automation.Language.Parser]::ParseFile("$source/launcher.ps1", [ref]$tokens, [ref]$parseErrors)
	if ($parseErrors.Count) { throw 'production launcher syntax errors' }
	$builders = @($ast.FindAll({ param($node)
		$node -is [Management.Automation.Language.FunctionDefinitionAst] -and
		$node.Name -in @('Get-PlayCommand', 'Get-PrecompileCommand')
	}, $false))
	if ($builders.Count -ne 2) { throw 'production command builders missing' }
	$launcherProbe = @'
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$settings = @{resolution='1280x720';language=1;fullscreen=$false;redzone=$true;fps120=$false;ecores=$false;game=''}
$efficiencyMask = 0
'@ + "`n" + (($builders | ForEach-Object { $_.Extent.Text }) -join "`n") + @'

function Invoke-ProbeCommand([string]$Command) {
 $info = New-Object Diagnostics.ProcessStartInfo
 $info.FileName = $env:ComSpec
 $info.Arguments = '/d /v:on /s /c "' + $Command + '"'
 $info.UseShellExecute = $false
 $info.CreateNoWindow = $true
 $info.RedirectStandardInput = $true
 $info.RedirectStandardOutput = $true
 $info.RedirectStandardError = $true
 $p = [Diagnostics.Process]::Start($info)
 try {
  $p.StandardInput.Close()
  $output = $p.StandardOutput.ReadToEndAsync()
  $errors = $p.StandardError.ReadToEndAsync()
  if (!$p.WaitForExit(15000)) { $p.Kill(); throw 'probe command timeout' }
  if ($p.ExitCode) { throw ('probe command exited ' + $p.ExitCode) }
 } finally { $p.Dispose() }
}
try {
 Invoke-ProbeCommand (Get-PlayCommand)
 Invoke-ProbeCommand (Get-PrecompileCommand)
} catch {
 [IO.File]::WriteAllText((Join-Path $root 'launcher-error.txt'), $_.Exception.Message)
}
'@
	Write-Utf8 "$fixture/launcher.ps1" $launcherProbe
	$caller = "@echo off`r`ncall `"$fixture\launcher.cmd`"`r`necho %PSModulePath%> `"$fixture\caller-modules.txt`"`r`n"
	[IO.File]::WriteAllText("$fixture/caller.cmd", $caller, [Text.Encoding]::ASCII)
	$env:PATH = (Split-Path $windowsPowerShell -Parent) + ';' + $originalPath
	foreach ($case in @('incompatible-parent', 'default-parent')) {
		$env:PSModulePath = if ($case -eq 'incompatible-parent') { Split-Path $poison -Parent } else { Join-Path (Split-Path $windowsPowerShell -Parent) 'Modules' }
		$expectedParent = $env:PSModulePath
		foreach ($name in @('run-windows.ps1.result', 'precompile-windows.ps1.result', 'caller-modules.txt', 'launcher-error.txt')) {
			Remove-Item -LiteralPath "$fixture/$name" -ErrorAction SilentlyContinue
		}
		Run-Cmd ('/d /s /c ""' + $fixture + '\caller.cmd""')
		$deadline = [datetime]::UtcNow.AddSeconds(20)
		while (!(Test-Path "$fixture/precompile-windows.ps1.result") -and !(Test-Path "$fixture/launcher-error.txt") -and [datetime]::UtcNow -lt $deadline) {
			Start-Sleep -Milliseconds 100
		}
		foreach ($route in @('run-windows.ps1', 'precompile-windows.ps1')) {
			$result = if (Test-Path "$fixture/$route.result") { [IO.File]::ReadAllText("$fixture/$route.result") } else { 'FAIL|no child result' }
			if ($result -notmatch '^PASS\|5\.1(?:\.[0-9]+){2}\|[A-F0-9]{64}$') {
				Write-Host "FAIL: $case/$route`: $result"
				++$failures
			} else { Write-Host "PASS: $case/$route" }
		}
		$callerModules = [IO.File]::ReadAllText("$fixture/caller-modules.txt").TrimEnd("`r", "`n")
		# cmd's echo prints its current status for an empty expansion.
		if ($expectedParent -and $callerModules -ne $expectedParent) {
			Write-Host "FAIL: $case altered the caller module path"
			++$failures
		}
	}
} finally {
	$env:PSModulePath = $originalModules
	$env:PATH = $originalPath
	$resolvedFixture = [IO.Path]::GetFullPath($fixture)
	if (!$resolvedFixture.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -or
	    (Split-Path $resolvedFixture -Leaf) -notmatch '^kyty-launcher-module-test-[a-f0-9]{32}$') { throw 'unsafe fixture cleanup path' }
	Remove-Item -LiteralPath $resolvedFixture -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failures) { Write-Host "LauncherModulePathTests: $failures failures"; exit 1 }
Write-Host 'LauncherModulePathTests: all passed'
