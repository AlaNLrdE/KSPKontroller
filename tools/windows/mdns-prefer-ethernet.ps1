<#
.SYNOPSIS
    Announce this PC's .local name (mDNS/LLMNR) only over the wired adapter while it is connected.

.DESCRIPTION
    Windows' own responder (Dnscache) answers <hostname>.local on every connected adapter. With Ethernet and
    Wi-Fi on the same LAN, peers (the Raspberry Pi HMI) receive two addresses and flip between them.

    While a wired (802.3) adapter is Up with a LAN IPv4 and no wireless adapter sits on a different subnet,
    four block rules in the firewall group "KSPKontroller mDNS" drop UDP 5353 (mDNS) and 5355 (LLMNR), inbound
    and outbound, on wireless interfaces only (rule InterfaceType Wireless). Otherwise they are disabled and
    Wi-Fi answers again. Adapters are chosen by physical media type (NdisPhysicalMedium), never by alias.

    -Install   copy this script to "%ProgramFiles%\KSPKontroller", (re)create the four rules and the
               scheduled task "\KSPKontroller\mDNS prefer Ethernet" (SYSTEM: at startup, on
               NetworkProfile/Operational 10000/10001 and every 15 minutes), then run -Apply. Idempotent.
    -Apply     recompute and enable/disable the rules (what the task runs).
    -Status    read-only: adapters, decision, rule and task state. Needs no elevation.
    -Uninstall remove the rules of this group, the task and the installed copy.

    Only rules named KSPKontroller-mDNS-* in group "KSPKontroller mDNS" are touched. Log:
    %ProgramData%\KSPKontroller\mdns-prefer-ethernet.log.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\windows\mdns-prefer-ethernet.ps1 -Install
#>
[CmdletBinding()]
param(
    [switch]$Install,
    [switch]$Apply,
    [switch]$Status,
    [switch]$Uninstall
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$Group = 'KSPKontroller mDNS'
$RulePrefix = 'KSPKontroller-mDNS-'
$TaskPath = '\KSPKontroller\'
$TaskName = 'mDNS prefer Ethernet'
$InstallDir = Join-Path $env:ProgramFiles 'KSPKontroller'
$InstalledScript = Join-Path $InstallDir 'mdns-prefer-ethernet.ps1'
$LogDir = Join-Path $env:ProgramData 'KSPKontroller'
$LogFile = Join-Path $LogDir 'mdns-prefer-ethernet.log'

# NdisPhysicalMedium values (ntddndis.h)
$MediumWireless = 9    # NdisPhysicalMediumNative802_11
$MediumWired = 14      # NdisPhysicalMedium802_3

$RuleSpecs = @(
    @{ Name = $RulePrefix + 'In-UDP5353-Wireless';  Direction = 'Inbound';  Port = 5353; What = 'mDNS' },
    @{ Name = $RulePrefix + 'Out-UDP5353-Wireless'; Direction = 'Outbound'; Port = 5353; What = 'mDNS' },
    @{ Name = $RulePrefix + 'In-UDP5355-Wireless';  Direction = 'Inbound';  Port = 5355; What = 'LLMNR' },
    @{ Name = $RulePrefix + 'Out-UDP5355-Wireless'; Direction = 'Outbound'; Port = 5355; What = 'LLMNR' }
)

function Write-Log([string]$Message) {
    $line = '{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Message
    Write-Output $line
    try {
        if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir | Out-Null }
        $old = @()
        if (Test-Path $LogFile) { $old = @(Get-Content -Path $LogFile -Tail 199) }
        Set-Content -Path $LogFile -Value ($old + $line) -Encoding ASCII
    } catch { }
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function ConvertTo-UInt32([string]$Address) {
    $b = ([Net.IPAddress]::Parse($Address)).GetAddressBytes()
    return ([uint32]$b[0] -shl 24) -bor ([uint32]$b[1] -shl 16) -bor ([uint32]$b[2] -shl 8) -bor [uint32]$b[3]
}

function Get-LanIPv4([int]$InterfaceIndex) {
    $result = @()
    $addresses = @(Get-NetIPAddress -InterfaceIndex $InterfaceIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue)
    foreach ($a in $addresses) {
        if ($a.AddressState -ne 'Preferred') { continue }
        if ($a.IPAddress -like '169.254.*' -or $a.IPAddress -like '127.*') { continue }
        $bits = [int]$a.PrefixLength
        $mask = [uint32]0
        if ($bits -gt 0) { $mask = [uint32]([uint64]4294967295 -shl (32 - $bits) -band 4294967295) }
        $result += New-Object PSObject -Property @{
            Address = $a.IPAddress; Prefix = $bits; Network = ((ConvertTo-UInt32 $a.IPAddress) -band $mask); Mask = $mask
        }
    }
    return ,$result
}

function Get-Adapters([int]$Medium) {
    $list = @()
    foreach ($n in @(Get-NetAdapter -Physical -ErrorAction SilentlyContinue)) {
        if ([int]$n.NdisPhysicalMedium -ne $Medium) { continue }
        $ips = @()
        if ($n.Status -eq 'Up') { $ips = Get-LanIPv4 $n.ifIndex }
        $list += New-Object PSObject -Property @{
            Alias = $n.Name; Description = $n.InterfaceDescription; Status = [string]$n.Status; IPv4 = $ips
        }
    }
    return ,$list
}

function Get-Decision {
    $wired = Get-Adapters $MediumWired
    $wireless = Get-Adapters $MediumWireless
    $wiredNets = @($wired | ForEach-Object { $_.IPv4 } | Where-Object { $_ })
    $wirelessNets = @($wireless | ForEach-Object { $_.IPv4 } | Where-Object { $_ })
    $shared = $false
    foreach ($w in $wiredNets) {
        foreach ($x in $wirelessNets) {
            $mask = [uint32]([Math]::Min([double]$w.Mask, [double]$x.Mask))
            if ((($w.Network -band $mask) -eq ($x.Network -band $mask))) { $shared = $true }
        }
    }
    $block = $false
    if ($wiredNets.Count -eq 0) {
        $reason = 'no wired adapter Up with a LAN IPv4: Wi-Fi may announce'
    } elseif ($wirelessNets.Count -eq 0) {
        $block = $true
        $reason = 'wired LAN up, no wireless IPv4: block wireless pre-emptively'
    } elseif ($shared) {
        $block = $true
        $reason = 'wired and wireless share a subnet: announce on wired only'
    } else {
        $reason = 'wireless is on a different subnet than wired: both announce on their own LAN'
    }
    return New-Object PSObject -Property @{ Block = $block; Reason = $reason; Wired = $wired; Wireless = $wireless }
}

function Format-Adapter($a) {
    $ips = @($a.IPv4 | ForEach-Object { '{0}/{1}' -f $_.Address, $_.Prefix })
    if ($ips.Count -eq 0) { $ips = @('no LAN IPv4') }
    return '  {0} [{1}] {2}: {3}' -f $a.Alias, $a.Description, $a.Status, ($ips -join ', ')
}

function Get-OurRules {
    return @(Get-NetFirewallRule -Group $Group -ErrorAction SilentlyContinue |
        Where-Object { $_.Name.StartsWith($RulePrefix) })
}

function Invoke-Apply {
    $d = Get-Decision
    $rules = @(Get-OurRules)
    if ($rules.Count -ne $RuleSpecs.Count) {
        Write-Log ('apply: {0} of {1} rules present; run -Install' -f $rules.Count, $RuleSpecs.Count)
        if ($rules.Count -eq 0) { exit 2 }
    }
    $want = 'False'
    if ($d.Block) { $want = 'True' }
    $changed = 0
    foreach ($r in $rules) {
        if ([string]$r.Enabled -ne $want) {
            Set-NetFirewallRule -Name $r.Name -Enabled $want
            $changed++
        }
    }
    $state = 'disabled'
    if ($d.Block) { $state = 'enabled' }
    Write-Log ('apply: wireless mDNS/LLMNR block {0} ({1} changed): {2}' -f $state, $changed, $d.Reason)
}

function Get-TaskXml {
    $script = [Security.SecurityElement]::Escape($InstalledScript)
    $query = [Security.SecurityElement]::Escape(
        '<QueryList><Query Id="0" Path="Microsoft-Windows-NetworkProfile/Operational">' +
        '<Select Path="Microsoft-Windows-NetworkProfile/Operational">*[System[(EventID=10000 or EventID=10001)]]</Select>' +
        '</Query></QueryList>')
    $start = (Get-Date).ToString('yyyy-MM-ddTHH:mm:ss')
    $xml = @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>KSPKontroller: announce this PC's .local name only over Ethernet while it is up (tools/windows/mdns-prefer-ethernet.ps1).</Description>
  </RegistrationInfo>
  <Triggers>
    <BootTrigger><Enabled>true</Enabled><Delay>PT30S</Delay></BootTrigger>
    <EventTrigger><Enabled>true</Enabled><Delay>PT5S</Delay><Subscription>$query</Subscription></EventTrigger>
    <TimeTrigger><Enabled>true</Enabled><StartBoundary>$start</StartBoundary><Repetition><Interval>PT15M</Interval><StopAtDurationEnd>false</StopAtDurationEnd></Repetition></TimeTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author"><UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel></Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>Queue</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <StartWhenAvailable>true</StartWhenAvailable>
    <ExecutionTimeLimit>PT2M</ExecutionTimeLimit>
    <Enabled>true</Enabled>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>powershell.exe</Command>
      <Arguments>-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File "$script" -Apply</Arguments>
    </Exec>
  </Actions>
</Task>
"@
    return $xml
}

function Invoke-Install {
    if (-not (Test-Path $InstallDir)) { New-Item -ItemType Directory -Path $InstallDir | Out-Null }
    $self = $PSCommandPath
    if ((Resolve-Path $self).Path -ne $InstalledScript) { Copy-Item -Path $self -Destination $InstalledScript -Force }

    foreach ($r in $RuleSpecs) {
        Get-NetFirewallRule -Name $r.Name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
        $params = @{
            Name = $r.Name; DisplayName = ('KSPKontroller: block {0} (UDP {1}) {2} on Wi-Fi while Ethernet is up' -f $r.What, $r.Port, $r.Direction.ToLower())
            Group = $Group; Direction = $r.Direction; Action = 'Block'; Protocol = 'UDP'; LocalPort = $r.Port
            InterfaceType = 'Wireless'; Profile = 'Any'; Enabled = 'False'
            Description = 'Managed by tools/windows/mdns-prefer-ethernet.ps1 (KSPKontroller). Enabled only while a wired adapter is on the same LAN.'
        }
        New-NetFirewallRule @params | Out-Null
    }

    Register-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Xml (Get-TaskXml) -Force | Out-Null
    Write-Log ('install: {0} rules in group "{1}", task {2}{3}, script {4}' -f $RuleSpecs.Count, $Group, $TaskPath, $TaskName, $InstalledScript)
    Invoke-Apply
}

function Invoke-Uninstall {
    foreach ($r in (Get-OurRules)) { Remove-NetFirewallRule -Name $r.Name }
    if (Get-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Confirm:$false
    }
    if (Test-Path $InstalledScript) { Remove-Item -Path $InstalledScript -Force -Confirm:$false }
    if ((Test-Path $InstallDir) -and -not (Get-ChildItem -Path $InstallDir -Force)) {
        Remove-Item -Path $InstallDir -Force -Confirm:$false
    }
    Write-Log 'uninstall: rules, task and installed script removed; Wi-Fi announces again'
}

function Show-Status {
    $d = Get-Decision
    Write-Output 'Wired (802.3) adapters:'
    foreach ($a in $d.Wired) { Write-Output (Format-Adapter $a) }
    Write-Output 'Wireless (802.11) adapters:'
    foreach ($a in $d.Wireless) { Write-Output (Format-Adapter $a) }
    $want = 'disabled'
    if ($d.Block) { $want = 'enabled' }
    Write-Output ('Decision: block {0} ({1})' -f $want, $d.Reason)
    $rules = @(Get-OurRules)
    Write-Output ('Firewall group "{0}": {1} of {2} rules' -f $Group, $rules.Count, $RuleSpecs.Count)
    foreach ($r in $rules) { Write-Output ('  {0} Enabled={1}' -f $r.Name, $r.Enabled) }
    $task = Get-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -ErrorAction SilentlyContinue
    if ($task) {
        $info = Get-ScheduledTaskInfo -TaskPath $TaskPath -TaskName $TaskName -ErrorAction SilentlyContinue
        $last = 'never'
        if ($info) { $last = '{0} (result {1})' -f $info.LastRunTime, $info.LastTaskResult }
        Write-Output ('Task {0}{1}: {2}, last run {3}' -f $TaskPath, $TaskName, $task.State, $last)
    } else {
        Write-Output ('Task {0}{1}: not installed' -f $TaskPath, $TaskName)
    }
    if (Test-Path $InstalledScript) {
        $same = (Get-FileHash $InstalledScript).Hash -eq (Get-FileHash $PSCommandPath).Hash
        Write-Output ('Installed copy {0}: {1}' -f $InstalledScript, $(if ($same) { 'matches this script' } else { 'differs (rerun -Install)' }))
    }
    $inSync = ($rules.Count -eq $RuleSpecs.Count) -and -not ($rules | Where-Object { ([string]$_.Enabled -eq 'True') -ne $d.Block })
    Write-Output ('In sync: {0}' -f $inSync)
}

$chosen = @($Install, $Apply, $Status, $Uninstall | Where-Object { $_ }).Count
if ($chosen -ne 1) {
    Write-Output 'Usage: mdns-prefer-ethernet.ps1 -Install | -Apply | -Status | -Uninstall'
    exit 64
}
if ($Status) { Show-Status; exit 0 }
if (-not (Test-Admin)) {
    Write-Output 'This action changes firewall rules and scheduled tasks: run it from an elevated PowerShell.'
    exit 5
}
if ($Install) { Invoke-Install }
elseif ($Apply) { Invoke-Apply }
elseif ($Uninstall) { Invoke-Uninstall }
