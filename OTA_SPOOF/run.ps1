#Requires -Version 5.1
<#
    HP10 OTA spoof - Windows launcher.
    Brings up a hosted-network AP (WPA2-PSK, no ICS), assigns a static IP to
    the virtual adapter, opens the firewall, then holds until Ctrl+C and tears
    everything down. The Python core launch is added in a later batch.
    Must be run from an elevated (Administrator) PowerShell.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptRoot   = Split-Path -Parent $MyInvocation.MyCommand.Path
$SettingsPath = Join-Path $ScriptRoot 'settings.env'
$FirewallGroup = 'HP10-OTA-Spoof'
$HostedAdapterDesc = 'Microsoft Hosted Network Virtual Adapter'

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)) {
        throw 'This script must be run from an elevated (Administrator) PowerShell.'
    }
}

function Read-Settings {
    param([string]$Path)
    if (-not (Test-Path $Path)) { throw "Settings file not found: $Path" }
    $cfg = @{}
    foreach ($line in Get-Content $Path) {
        $trimmed = $line.Trim()
        if ($trimmed -eq '' -or $trimmed.StartsWith('#')) { continue }
        $idx = $trimmed.IndexOf('=')
        if ($idx -lt 1) { continue }
        $key = $trimmed.Substring(0, $idx).Trim()
        $val = $trimmed.Substring($idx + 1).Trim()
        $cfg[$key] = $val
    }
    return $cfg
}

function Assert-Settings {
    param([hashtable]$Cfg)
    foreach ($k in @('SSID','PSK','AP_IP','PREFIX','FW_PATH')) {
        if (-not $Cfg.ContainsKey($k) -or $Cfg[$k] -eq '') {
            throw "Missing required setting: $k"
        }
    }
    if ($Cfg['PSK'].Length -lt 8 -or $Cfg['PSK'].Length -gt 63) {
        throw 'PSK must be 8-63 characters (WPA2 requirement).'
    }
    if ($Cfg['AP_IP'] -notmatch '^\d{1,3}(\.\d{1,3}){3}$') {
        throw "AP_IP is not a valid IPv4 address: $($Cfg['AP_IP'])"
    }
    $fw = $Cfg['FW_PATH']
    if (-not [System.IO.Path]::IsPathRooted($fw)) {
        $fw = Join-Path $ScriptRoot $fw
    }
    if (-not (Test-Path $fw)) {
        Write-Warning "Firmware file not found yet: $fw (place it before triggering an OTA)."
    }
}

function Get-HostedAdapter {
    for ($i = 0; $i -lt 10; $i++) {
        $a = Get-NetAdapter -InterfaceDescription $HostedAdapterDesc -ErrorAction SilentlyContinue |
             Where-Object { $_.Status -ne 'Not Present' } | Select-Object -First 1
        if ($a) { return $a }
        Start-Sleep -Milliseconds 500
    }
    throw "Hosted network virtual adapter did not appear. Is the dongle plugged in and hosted-network capable?"
}

function Start-AP {
    param([hashtable]$Cfg)
    Write-Host "[*] Configuring hosted network SSID '$($Cfg['SSID'])'..."
    netsh wlan set hostednetwork mode=allow ssid="$($Cfg['SSID'])" key="$($Cfg['PSK'])" | Out-Null
    Write-Host '[*] Starting hosted network...'
    $out = netsh wlan start hostednetwork 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "netsh failed to start hosted network: $out"
    }
    $adapter = Get-HostedAdapter
    Write-Host "[*] Hosted adapter is '$($adapter.Name)' (ifIndex $($adapter.ifIndex))."
    return $adapter
}

function Set-APAddress {
    param([hashtable]$Cfg, $Adapter)
    $idx = $Adapter.ifIndex
    Get-NetIPAddress -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -notlike '169.254.*' } |
        Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
    New-NetIPAddress -InterfaceIndex $idx -IPAddress $Cfg['AP_IP'] `
        -PrefixLength ([int]$Cfg['PREFIX']) -ErrorAction Stop | Out-Null
    foreach ($ifIdx in @($idx, (Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty ifIndex))) {
        if ($ifIdx) {
            netsh interface ipv4 set interface $ifIdx weakhostreceive=enabled | Out-Null
            netsh interface ipv4 set interface $ifIdx weakhostsend=enabled | Out-Null
        }
    }
    Write-Host "[*] Assigned $($Cfg['AP_IP'])/$($Cfg['PREFIX']) to the hosted adapter."
}

function Open-Firewall {
    param([hashtable]$Cfg)
    $udpPorts = @()
    foreach ($k in @('DNS_PORT','DHCP_PORT')) {
        if ($Cfg.ContainsKey($k) -and $Cfg[$k] -ne '') { $udpPorts += [int]$Cfg[$k] }
    }
    $tcpPort = if ($Cfg.ContainsKey('HTTP_PORT') -and $Cfg['HTTP_PORT'] -ne '') { [int]$Cfg['HTTP_PORT'] } else { 80 }
    Remove-NetFirewallRule -Group $FirewallGroup -ErrorAction SilentlyContinue
    if ($udpPorts.Count -gt 0) {
        New-NetFirewallRule -DisplayName 'HP10 OTA UDP' -Group $FirewallGroup -Direction Inbound `
            -Action Allow -Protocol UDP -LocalPort $udpPorts -Profile Any | Out-Null
    }
    New-NetFirewallRule -DisplayName 'HP10 OTA HTTP' -Group $FirewallGroup -Direction Inbound `
        -Action Allow -Protocol TCP -LocalPort $tcpPort -Profile Any | Out-Null
    Write-Host "[*] Firewall opened for UDP $($udpPorts -join ',') and TCP $tcpPort."
}

function Invoke-Cleanup {
    param($Adapter, [hashtable]$Cfg)
    Write-Host ''
    Write-Host '[*] Tearing down...'
    Remove-NetFirewallRule -Group $FirewallGroup -ErrorAction SilentlyContinue
    if ($Adapter -and $Cfg) {
        Get-NetIPAddress -InterfaceIndex $Adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            Where-Object { $_.IPAddress -eq $Cfg['AP_IP'] } |
            Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
    }
    netsh wlan stop hostednetwork | Out-Null
    Write-Host '[*] Done.'
}

Assert-Admin
$Cfg = Read-Settings -Path $SettingsPath
Assert-Settings -Cfg $Cfg
$Adapter = $null
try {
    $Adapter = Start-AP -Cfg $Cfg
    Set-APAddress -Cfg $Cfg -Adapter $Adapter
    Open-Firewall -Cfg $Cfg
    Write-Host ''
    Write-Host "[+] AP is up. Point the HP10 at SSID '$($Cfg['SSID'])'."
    Write-Host '[+] (Python core launches here in batch 2.) Press Ctrl+C to stop.'
    while ($true) { Start-Sleep -Seconds 1 }
}
finally {
    Invoke-Cleanup -Adapter $Adapter -Cfg $Cfg
}