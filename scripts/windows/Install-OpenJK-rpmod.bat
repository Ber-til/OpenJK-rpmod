@echo off
rem Installs OpenJK-rpmod: copies the files next to this script into Jedi Academy's GameData folder.
rem Double-click it after extracting the release zip, or drag the GameData folder onto it.
rem The work is done by the PowerShell after the #!PS line below.
setlocal
set "RPMOD_BAT=%~f0"
set "RPMOD_TARGET=%~1"
set "RPMOD_ELEVATED="
if /i "%~2"=="elevated" set "RPMOD_ELEVATED=1"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s = Get-Content -LiteralPath $env:RPMOD_BAT -Raw; $i = $s.IndexOf([string][char]10 + '#!PS'); Invoke-Expression $s.Substring($i + 5)"
set "RC=%ERRORLEVEL%"
echo.
pause
exit /b %RC%
#!PS
$ErrorActionPreference = 'Stop'
try { $Host.UI.RawUI.WindowTitle = 'OpenJK-rpmod installer' } catch {}
$src = Split-Path -Parent $env:RPMOD_BAT
$me = Split-Path -Leaf $env:RPMOD_BAT

function Say($text, $color) {
	if ($color) { Write-Host $text -ForegroundColor $color } else { Write-Host $text }
}

function Fail($text) {
	Say ''
	Say $text 'Red'
	exit 1
}

# GameData is the folder with the game's base\assets0.pk3 in it
function Test-GameData($p) {
	if (-not $p) { return $false }
	try { return [IO.File]::Exists([IO.Path]::Combine($p, 'base', 'assets0.pk3')) } catch { return $false }
}

# the folder picked, as GameData: the game's folder, GameData itself, or a folder in it
function Resolve-GameData($p) {
	if (-not $p) { return $null }
	$p = "$p".Trim().Trim('"').TrimEnd('\', '/')
	if (-not $p) { return $null }
	try { $p = [IO.Path]::GetFullPath($p) } catch { return $null }
	foreach ($c in @($p, [IO.Path]::Combine($p, 'GameData'), [IO.Path]::GetDirectoryName($p))) {
		if ($c -and (Test-GameData $c)) { return [IO.Path]::GetFullPath($c).TrimEnd('\') }
	}
	return $null
}

$found = New-Object System.Collections.Generic.List[string]
function Add-Candidate($p) {
	try {
		$g = Resolve-GameData $p
		if ($g -and -not ($found -contains $g)) { $found.Add($g) }
	} catch {}
}

function Get-RegValue($key, $name) {
	try { return (Get-ItemProperty -LiteralPath $key -ErrorAction Stop).$name } catch { return $null }
}

function Pick-Folder {
	Say ''
	Say "Pick Jedi Academy's GameData folder (the one with the base folder in it)..."
	try {
		Add-Type -AssemblyName System.Windows.Forms
		$owner = New-Object System.Windows.Forms.Form
		$owner.TopMost = $true
		$dialog = New-Object System.Windows.Forms.FolderBrowserDialog
		$dialog.Description = "Pick Jedi Academy's GameData folder"
		$dialog.ShowNewFolderButton = $false
		$picked = $dialog.ShowDialog($owner)
		$owner.Dispose()
		if ($picked -ne [System.Windows.Forms.DialogResult]::OK) { return $null }
		return $dialog.SelectedPath
	} catch {
		return Read-Host 'Type or paste the path of the GameData folder'
	}
}

Say 'OpenJK-rpmod installer' 'Cyan'
Say ''

# the release's files have to be next to this script
if (-not [IO.File]::Exists([IO.Path]::Combine($src, 'openjk.x86.exe'))) {
	Fail "The game files aren't next to this script. Extract the whole zip first (right-click it, Extract All...), then run $me from the folder that makes."
}
if (Test-GameData $src) {
	Say "These files are already in Jedi Academy's GameData folder: nothing to copy." 'Green'
	Say 'Start openjk.x86.exe there, or tick "Use OpenJK if available" in the RPMod launcher.'
	exit 0
}

$target = $null
if ($env:RPMOD_TARGET) {
	# dragged onto the script, or back as administrator
	$target = Resolve-GameData $env:RPMOD_TARGET
	if (-not $target) { Fail "$($env:RPMOD_TARGET) isn't Jedi Academy's GameData folder: there's no base\assets0.pk3 in it." }
} else {
	Say 'Looking for Jedi Academy...'

	# Steam, in each of its libraries
	$steam = @()
	foreach ($k in @('HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam')) {
		foreach ($n in @('SteamPath', 'InstallPath')) {
			$v = Get-RegValue $k $n
			if ($v) { $steam += $v }
		}
	}
	if (${env:ProgramFiles(x86)}) { $steam += [IO.Path]::Combine(${env:ProgramFiles(x86)}, 'Steam') }
	$libraries = @()
	foreach ($r in $steam) {
		$libraries += $r
		try {
			$vdf = [IO.Path]::Combine($r, 'steamapps', 'libraryfolders.vdf')
			if ([IO.File]::Exists($vdf)) {
				foreach ($m in [regex]::Matches([IO.File]::ReadAllText($vdf), '"path"\s+"([^"]+)"')) {
					$libraries += $m.Groups[1].Value.Replace('\\', '\')
				}
			}
		} catch {}
	}
	foreach ($l in $libraries) {
		try { Add-Candidate ([IO.Path]::Combine($l, 'steamapps', 'common', 'Jedi Academy', 'GameData')) } catch {}
	}
	foreach ($k in @('HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 6020', 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 6020')) {
		Add-Candidate (Get-RegValue $k 'InstallLocation')
	}

	# GOG
	foreach ($k in @('HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games', 'HKLM:\SOFTWARE\GOG.com\Games')) {
		try {
			foreach ($g in (Get-ChildItem -LiteralPath $k -ErrorAction Stop)) {
				$v = Get-ItemProperty -LiteralPath $g.PSPath
				if ("$($v.gameName)" -like '*Jedi Academy*') { Add-Candidate $v.path }
			}
		} catch {}
	}

	# the CD version
	foreach ($k in @('HKLM:\SOFTWARE\WOW6432Node\LucasArts\Star Wars Jedi Knight Jedi Academy\1.0', 'HKLM:\SOFTWARE\LucasArts\Star Wars Jedi Knight Jedi Academy\1.0')) {
		Add-Candidate (Get-RegValue $k 'Install Path')
		Add-Candidate (Get-RegValue $k 'InstallPath')
	}

	# where it often is
	$common = @('C:\GOG Games\Star Wars Jedi Knight - Jedi Academy', 'C:\Games\Jedi Academy', 'C:\JKA', 'C:\Jedi Academy')
	foreach ($pf in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
		if ($pf) {
			$common += [IO.Path]::Combine($pf, 'GOG Galaxy', 'Games', 'Star Wars Jedi Knight - Jedi Academy')
			$common += [IO.Path]::Combine($pf, 'LucasArts', 'Star Wars Jedi Knight Jedi Academy')
		}
	}
	foreach ($p in $common) { Add-Candidate $p }

	if ($found.Count -eq 1) {
		Say ''
		Say 'Found Jedi Academy here:'
		Say "  $($found[0])" 'Yellow'
		$a = Read-Host 'Install OpenJK-rpmod there? Y yes, N no, O pick another folder [Y]'
		if ($a -match '^\s*(y|yes)?\s*$') { $target = $found[0] }
		elseif ($a -notmatch '^\s*o') { Fail 'Nothing was installed.' }
	} elseif ($found.Count -gt 1) {
		Say ''
		Say 'Found Jedi Academy in more than one place:'
		for ($i = 0; $i -lt $found.Count; $i++) { Say ('  {0}  {1}' -f ($i + 1), $found[$i]) }
		Say '  O  another folder'
		$a = Read-Host 'Install into which one? (its number, or O)'
		$k = 0
		if ([int]::TryParse("$a".Trim(), [ref]$k) -and $k -ge 1 -and $k -le $found.Count) { $target = $found[$k - 1] }
		elseif ($a -notmatch '^\s*o') { Fail 'Nothing was installed.' }
	} else {
		Say "Couldn't find Jedi Academy by itself." 'Yellow'
	}
	while (-not $target) {
		$p = Pick-Folder
		if (-not $p) { Fail 'No folder picked: nothing was installed.' }
		$target = Resolve-GameData $p
		if (-not $target) { Say "$p isn't Jedi Academy's GameData folder: there's no base\assets0.pk3 in it. Try again." 'Yellow' }
	}
}

Say ''
Say "Installing into $target" 'Cyan'

# not over a game that's running
while (Get-Process -Name 'openjk.x86', 'openjk_sp.x86', 'openjkded.x86', 'jamp', 'jasp' -ErrorAction SilentlyContinue) {
	Say 'Jedi Academy is running. Close it, then press Enter (or type Q to stop).' 'Yellow'
	if ((Read-Host) -match '^\s*q') { Fail 'Nothing was installed.' }
}

# Program Files needs an administrator: ask, and go on in a new window
$probe = [IO.Path]::Combine($target, '.openjk-rpmod-write-test')
$canWrite = $true
try { [IO.File]::WriteAllText($probe, 'x'); [IO.File]::Delete($probe) } catch { $canWrite = $false }
if (-not $canWrite) {
	if ($env:RPMOD_ELEVATED) { Fail "Can't write to $target, even as administrator." }
	Say 'Windows only lets an administrator change that folder. Asking for permission...' 'Yellow'
	try {
		Start-Process -FilePath $env:RPMOD_BAT -ArgumentList @(('"' + $target + '"'), 'elevated') -Verb RunAs
		Say 'The installer goes on in the new window.'
		exit 0
	} catch {
		Fail "No permission given: nothing was installed. You can also right-click $me and pick Run as administrator."
	}
}

# copy everything but this script, keeping what it replaces
$files = @(Get-ChildItem -LiteralPath $src -Recurse -File | Where-Object { $_.FullName -ne $env:RPMOD_BAT })
$backup = [IO.Path]::Combine($target, 'OpenJK-rpmod-backup', (Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'))
$copied = 0
$kept = 0
try {
	foreach ($f in $files) {
		$rel = $f.FullName.Substring($src.Length).TrimStart('\')
		$to = [IO.Path]::Combine($target, $rel)
		if ([IO.File]::Exists($to)) {
			$b = [IO.Path]::Combine($backup, $rel)
			[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($b))
			[IO.File]::Copy($to, $b, $true)
			$kept++
		}
		[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($to))
		[IO.File]::Copy($f.FullName, $to, $true)
		# from a downloaded zip: so Windows doesn't warn each time the game starts
		try { Unblock-File -LiteralPath $to } catch {}
		$copied++
	}
} catch {
	Fail ("Copying stopped at $rel`: " + $_.Exception.Message + $(if ($kept) { "`nThe files it had replaced are kept in $backup" } else { '' }))
}

Say ''
Say "OpenJK-rpmod is installed ($copied files)." 'Green'
if ($kept) { Say "The files it replaced are kept in $backup" }
Say 'Start openjk.x86.exe in that folder, or tick "Use OpenJK if available" in the RPMod launcher.'
exit 0
