<#
.SYNOPSIS
    Swaps Community Shaders builds in an MO2 instance, each with its own shader cache, and runs them unattended.

.DESCRIPTION
    Each variant is a mod folder ("CS AB <name>") plus a stash of what the game writes for that build. Only one variant's
    state is live at a time, because each build deletes the other's cache folders when it starts.

      cs-ab.ps1 stage <name> <aio folder> [-Pad n]
                                              Copies a build's AIO folder in as the variant's mod.
      cs-ab.ps1 use <name>                    Makes it the enabled CS mod and swaps its shader cache in.
      cs-ab.ps1 restore                       Puts back the mod and state from before the first "use".
      cs-ab.ps1 status                        What is live, and what is stashed.
      cs-ab.ps1 run <name> <scenario> [-Label x] [-Env A=1,B=2] [-TimeoutSeconds n] [-Until regex]
                                              "use", one game run of the scenario, then collects its files.

    Scenarios are scenarios\*.env beside this script: the CommunityShaders.env for the run. Results land in
    <Root>\runs\<time>-<name>-<scenario>[-<label>]. compare.py reads those folders.
#>
param(
    [Parameter(Position = 0, Mandatory)][ValidateSet('stage', 'use', 'restore', 'status', 'run')][string]$Action,
    [Parameter(Position = 1)][string]$Name,
    [Parameter(Position = 2)][string]$Argument,
    [string]$Label = '',
    [string[]]$Env = @(),
    [int]$TimeoutSeconds = 600,
    [string]$Until = '',
    [int]$Pad = 0,
    [string]$Mo2 = 'C:\Modding\MO2',
    [string]$Root = 'C:\Modding\CS-AB',
    [string]$Profile2 = 'Default'
)
$ErrorActionPreference = 'Stop'

$modList = Join-Path $Mo2 "profiles\$Profile2\modlist.txt"
$overwrite = Join-Path $Mo2 'overwrite'
$skse = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Skyrim Special Edition\SKSE'
$statePath = Join-Path $Root 'state.json'
# What a build writes that the other build must not see: moved, not copied, so a swap is instant. Settings stay shared,
# so every variant runs the same ones (what a build does not know, it ignores).
$liveState = @(
    @{ Live = (Join-Path $overwrite 'ShaderCache'); Stash = 'ShaderCache' }
)

function Get-State {
    if (Test-Path $statePath) { return Get-Content $statePath -Raw | ConvertFrom-Json }
    return [pscustomobject]@{ live = $null; originalMod = $null }
}
function Set-State($state) {
    New-Item -ItemType Directory -Force $Root | Out-Null
    $state | ConvertTo-Json | Set-Content $statePath -Encoding utf8
}
function Assert-NotRunning {
    if (Get-Process SkyrimSE, ModOrganizer -ErrorAction SilentlyContinue) { throw 'The game or MO2 is running. Close them first: MO2 rewrites the mod list.' }
}
function Stash-Live([string]$variant) {
    $folder = Join-Path $Root "variants\$variant"
    foreach ($item in $liveState) {
        $stash = Join-Path $folder $item.Stash
        if (Test-Path $stash) { throw "$stash already exists; refusing to overwrite a stash" }
        if (Test-Path $item.Live) {
            New-Item -ItemType Directory -Force $folder | Out-Null
            Move-Item $item.Live $stash
        }
    }
}
function Unstash([string]$variant) {
    $folder = Join-Path $Root "variants\$variant"
    foreach ($item in $liveState) {
        $stash = Join-Path $folder $item.Stash
        if (Test-Path $stash) {
            New-Item -ItemType Directory -Force (Split-Path $item.Live) | Out-Null
            Move-Item $stash $item.Live
        }
    }
}
function Set-EnabledMod([string]$mod) {
    $lines = Get-Content $modList
    $isCs = { param($l) $l -match '^[+-](CS AB .+|CommunityShaders_AIO|Community Shaders DXVK ORG)$' }
    $slot = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if (& $isCs $lines[$i]) {
            if ($lines[$i][0] -eq '+' -and $slot -lt 0) { $slot = $i }
            $lines[$i] = '-' + $lines[$i].Substring(1)
        }
    }
    $wanted = [array]::IndexOf($lines, "-$mod")
    if ($wanted -ge 0) {
        $lines[$wanted] = "+$mod"
    } elseif ($slot -ge 0) {
        # Takes the enabled mod's place in the load order.
        $lines = @($lines[0..$slot]) + "+$mod" + @($lines[($slot + 1)..($lines.Count - 1)])
    } else { throw "No enabled CS mod in $modList to take the place of" }
    Set-Content $modList $lines -Encoding utf8
}
function Enabled-CsMod {
    Get-Content $modList | Where-Object { $_ -match '^\+(CS AB .+|CommunityShaders_AIO|Community Shaders DXVK ORG)$' } | ForEach-Object { $_.Substring(1) } | Select-Object -First 1
}
function Use-Variant([string]$variant) {
    Assert-NotRunning
    $mod = "CS AB $variant"
    if (-not (Test-Path (Join-Path $Mo2 "mods\$mod"))) { throw "No variant '$variant': stage it first" }
    $state = Get-State
    if (-not $state.originalMod) {
        $state.originalMod = Enabled-CsMod
        # The state from before the first swap is a variant of its own, so restore can bring it back.
        $state.live = '_original'
    }
    if ($state.live -ne $variant) {
        Stash-Live $state.live
        Unstash $variant
        Set-EnabledMod $mod
        $state.live = $variant
    }
    Set-State $state
}

switch ($Action) {
    'stage' {
        Assert-NotRunning
        if (-not $Name -or -not $Argument) { throw 'stage <name> <aio folder>' }
        $mod = Join-Path $Mo2 "mods\CS AB $Name"
        robocopy $Argument $mod /MIR /NFL /NDL /NJH /NJS /NP /XF *.pdb | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "robocopy failed: $LASTEXITCODE" }
        $LASTEXITCODE = 0
        # usvfs crashes the game when its file table grows while the game's threads read it, which a cold V1 shader cache does: it
        # creates thousands of files. Empty files here make MO2 grow the table before the game starts.
        if ($Pad) {
            $padFolder = Join-Path $mod '_cs-ab-pad'
            New-Item -ItemType Directory -Force $padFolder | Out-Null
            foreach ($i in 1..$Pad) { [IO.File]::WriteAllBytes((Join-Path $padFolder "pad$i"), @()) }
        }
        $dll = Get-Item (Join-Path $mod 'SKSE\Plugins\CommunityShaders.dll')
        "Staged $Name from $Argument, DLL of $($dll.LastWriteTime)"
    }
    'use' { Use-Variant $Name; "Live: $Name" }
    'restore' {
        Assert-NotRunning
        $state = Get-State
        if (-not $state.originalMod) { 'Nothing to restore'; return }
        if ($state.live -ne '_original') {
            Stash-Live $state.live
            Unstash '_original'
        }
        Set-EnabledMod $state.originalMod
        Remove-Item $statePath
        "Restored $($state.originalMod)"
    }
    'status' {
        $state = Get-State
        "Live: $($state.live); enabled: $(Enabled-CsMod); original: $($state.originalMod)"
        Get-ChildItem (Join-Path $Root 'variants') -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            "  $($_.Name): stash $((Get-ChildItem $_.FullName -Directory | ForEach-Object Name) -join ', ')"
        }
    }
    'run' {
        Use-Variant $Name
        $scenario = Join-Path $PSScriptRoot "scenarios\$Argument.env"
        if (-not (Test-Path $scenario)) { throw "No scenario $scenario" }
        $envFile = Join-Path $skse 'CommunityShaders.env'
        $lines = @('CS_AB_PROBE=1') + (Get-Content $scenario) + ($Env | ForEach-Object { $_ -split ',(?=[A-Z_0-9]+=)' })
        Set-Content $envFile $lines -Encoding ascii
        $start = Get-Date
        $startUtc = $start.ToUniversalTime()
        Start-Process -FilePath (Join-Path $Mo2 'ModOrganizer.exe') -ArgumentList '-p', $Profile2, '"moshortcut://:SKSE"' -WorkingDirectory $Mo2 | Out-Null
        $seen = $false; $stop = 'timeout'; $focused = $false
        # The displays must stay on: with them off, Present throttles to a few frames a second.
        Add-Type -Namespace CsAb -Name Native -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint f); [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, UIntPtr e); [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr w, bool a);'
        [void][CsAb.Native]::SetThreadExecutionState([uint32]2147483651)
        [CsAb.Native]::mouse_event(1, 0, 0, 0, [UIntPtr]::Zero)
        while (((Get-Date) - $start).TotalSeconds -lt $TimeoutSeconds) {
            Start-Sleep -Seconds 2
            $game = Get-Process SkyrimSE -ErrorAction SilentlyContinue
            if ($game) { $seen = $true }
            if ($game -and -not $focused -and $game.MainWindowHandle -ne [IntPtr]::Zero) { [CsAb.Native]::SwitchToThisWindow($game.MainWindowHandle, $true); $focused = $true }
            if ($seen -and -not $game) { $stop = 'exited'; break }
            if ($Until -and $seen -and (Get-Content (Join-Path $skse 'CommunityShaders.log') -ErrorAction SilentlyContinue | Select-String -Pattern $Until -Quiet)) { $stop = 'until'; Start-Sleep -Seconds 3; break }
        }
        [void][CsAb.Native]::SetThreadExecutionState([uint32]2147483648)
        Get-Process SkyrimSE -ErrorAction SilentlyContinue | Stop-Process -Force
        Start-Sleep -Seconds 2
        $m = Get-Process ModOrganizer -ErrorAction SilentlyContinue
        if ($m) { $m | ForEach-Object { [void]$_.CloseMainWindow() }; Start-Sleep -Seconds 6; Get-Process ModOrganizer -ErrorAction SilentlyContinue | Stop-Process -Force }
        Remove-Item $envFile -ErrorAction SilentlyContinue

        $id = "$($start.ToString('MMdd-HHmmss'))-$Name-$Argument" + $(if ($Label) { "-$Label" } else { '' })
        $out = Join-Path $Root "runs\$id"
        New-Item -ItemType Directory -Force $out | Out-Null
        foreach ($log in 'CommunityShaders.log', 'SkyrimSE_d3d11.log') { Copy-Item (Join-Path $skse $log) $out -ErrorAction SilentlyContinue }
        Get-ChildItem $skse -File | Where-Object { $_.LastWriteTimeUtc -ge $startUtc -and $_.Name -match '^(CS-AB-memory\.csv|CommunityShaders-VRAM-\d+\.json)$' } | ForEach-Object {
            if ($_.Name -like '*.json') { Move-Item $_.FullName $out } else { Copy-Item $_.FullName $out }
        }
        @{ variant = $Name; scenario = $Argument; label = $Label; env = $Env; stop = $stop; seconds = [int]((Get-Date) - $start).TotalSeconds } | ConvertTo-Json | Set-Content (Join-Path $out 'run.json') -Encoding utf8
        "$id stop=$stop"
    }
}
