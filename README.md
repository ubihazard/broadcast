<p align="center"><img alt="BROADcast" src="/icon/icon256.png"/></p>
<h1 align="center">BROADcast</h1>

<!--
![BROADcast](/icon/icon256.png)

# BROADcast
-->

Force IPv4 UDP broadcast on all network interfaces in Windows 7 and later.

Download the [latest release](https://github.com/ubihazard/broadcast/releases).

## Description

If you work with Windows it might come to you as a surprise that IPv4 UDP broadcast packets sent to the global broadcast address `255.255.255.255` (all available network interfaces), well, won’t actually be delivered to *all* available network interfaces present on the system.

This is because Windows networking implementation of BSD sockets considers such packets to be deliverable only to the interface of highest priority (the “default” one). The priority of network interface is determined by its metric value which is (usually) automatically assigned by the system. The interface with the lowest metric value would be the interface of highest priority. Normally this is the interface through which your PC is connected to the internet (or your home router). You can check metric values assigned by Windows to all network interfaces by running this command in PowerShell:

```sh
Get-NetIPInterface
```

Whatever the rationale for this behavior is, it has a serious implication that all other network interfaces won’t receive global UDP broadcast packets, and a certain application will experience connectivity problems if it depends on such packets to be delivered properly.

How does this matter? Consider a PC connected to the local area network with a standard subnet of 192.168.179.0/24 and a VPN with a subnet of 100.100.100.0/24. Now there’s a service running on this PC which VPN clients also have to access and this service relies on global UDP broadcast packets for communication.

Naturally, the only way to solve this problem in Windows is to bridge both networks. But bridging is highly undesirable because doing so would expose your entire LAN to a party connected via VPN.

BROADcast solves this problem nicely by capturing global UDP broadcast packets delivered to the primary (also called “preferred” route) and simply relaying them to all other suitable network interfaces which Windows decided to just pass by.

![BROADcast relays UDP packets](/screenshot.png)

On the screenshot above BROADcast is capturing global UDP broadcast packets delivered to `10.10.10.100` real LAN address (being also the preferred route) and is relaying them to VPN address `100.100.100.1`. Without BROADcast running, packets would’ve remained delivered only to `10.10.10.100`, ignoring completely the VPN segment of the network.

Examples of software that vitally depend on this functionality include: LAN chat applications, some multiplayer video games, and other decentralized applications used for collaboration.

*Note: BROADcast requires administrator privileges in order to run. This is because it has to capture UDP packets using raw sockets, and this is allowed only for elevated accounts.*

## How to Use

BROADcast is a console application. It can also run in background as a Windows service.

Start relaying global UDP broadcast packets:

```sh
broadcast.exe -b
```

Add `-d` to display verbose diagnostic messages (useful for troubleshooting):

```
broadcast.exe -b -d
```

Broadcast packets would be delivered to all network interfaces except the default one. Use <kbd>Ctrl+C</kbd> to exit BROADcast cleanly.

As a bonus feature BROADcast allows to make any interface the default (or preferred). It does so by taking the current metric value of the interface you desire to turn into default and adding it to each other interface metric value, making it the lowest metric value of all:

```sh
BROADcast.exe -i "Interface" -m
```

Run the command again without the `-m` argument to undo all metric value changes and restore the automatic system-managed values:

```sh
BROADcast.exe -i "Interface"
```

“Interface” is the network interface name which can be looked up (and changed) in the “Network and Sharing Center” section of the Windows Control Panel. You can access it quickly by pressing <kbd>Win+R</kbd> and opening:

```
%windir%\explorer.exe shell:::{992CFFA0-F557-101A-88EC-00DD010CCC48}
```

To install (or uninstall) BROADcast as a Windows service invoke it with the appropriate argument:

```sh
broadcast.exe install | uninstall
```

### OpenVPN

BROADcast repository contains an example OpenVPN configuration and scripts for running BROADcast after starting a OpenVPN server using a TAP device.

*Note that if you intend to start BROADcast from OpenVPN using its start/stop script functionality, you must also run OpenVPN with administrator privileges, just like BROADcast.*

Because of this it is highly recommended to start BROADcast separately as a Windows service instead.

### ForceBindIP

While not directly related to BROADcast or UDP broadcast at all, another useful Windows networking crutch is [ForceBindIP](http://r1ch.net/).

Unlike BROADcast, which provides a fix for UDP protocol, ForceBindIP provides a similar fix for TCP protocol by forcing an application to bind (or listen) on a particular network interface instead of the one it automatically chooses (which is often not the one that you want).

## Support

If you find [BROADcast](https://github.com/ubihazard/broadcast) useful, you can [buy me a ☕](https://www.buymeacoffee.com/ubihazard "Show support")!
