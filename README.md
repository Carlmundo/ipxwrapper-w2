# IPXWrapper Modified for PGY (蒲公英 VPN）

## Usage

This intends to enable `ipxwrapper` in VPN where UDP broadcast / multicast is not supported.

You may either directly go to `Releases` at the right sidebar and download the prebuilt binaries, or
you can follow the steps below to build by yourself.

After you get the binaries, copy them to the targeted application, e.g. `Atomic Bomberman`, thus
replace the original files shipped by official `ipxwrapper`.

It's better to run `ipxconfig.exe` to change the configuration as below:

- Choose the `Primary interface` to the VPN interface you are using. e.g. `OrayBoxVPN ...` in case
  of PGY.
- Click each item in `Network adapters`, uncheck `Enable interface` for it except for the VPN
  adapter, e.g. `OrayBoxVPN ...`

These above settings will reduce the burdern also makes it less error-prone for `ipxwrapper`.

**NOTE:** You may need to disable or correctly configure your `Windows Firewall` to avoid block of
packets.

## Motivation

To play `Atomic Bomberman`, we tried use PGY and then `ipxwrapper` over it. However, `ipxwrapper`
fails in VPN created by PGY because of two fundenmatal issues.

First, the root cause is that PGY VPN doesn't support UDP broadcast / multicast. `ipxwrapper` relies
on this feature, i.e. it sends UDP packet to the broadcast address of LAN, so that every device in
the LAN could find each other and eventually form a simulated IPX network.

This issue might be mitigated by deploying some UDP proxy or write another program, which manually
captures such UDP broadcast packets sent by `ipxwrapper` and then dispatches them to all known (or
configured) IPs in the LAN. Thus in fact mimic the "broadcast" even though PGY VPN doesn't support
it.

Then it comes the second issue. The netmask of PGY VPN is automatically set to `255.255.255.255`.
For certain incoming UDP packets, `ipxwrapper` filters out these sent from other subnets.
Unfortunately, netmask `255.255.255.255` treats UDP packet from any other IP as from another subnet.
Therefore `ipxwrapper` denies to handle any external UDP packets and breaks the functionality, even
if we managed to resolve the first issue.

Consequently, instead of continously finding various tricky solutions to workaround above mentioned
broadcasting and subnet issues, directly modifying related logic of `ipxwrapper` seems to be the
most effective and neat path to success.

## Solution

Basically this fork makes the following enhancements / workarounds:

- when `ipxwrapper` iterateingF the network interfaces, if netmask of an inteface is
  `255.255.255.255`, we will enumerate the local route table. Usually it (e.g. PGY) adds rules in
  table for all remote IPs in this VLAN (virtual LAN). We can find out these remote IPs by checking
  whether they and local VLAN IP are in the same subnet masked by `255.255.0.0` (rather than by
  `255.255.255.255`). For example, given local VLAN ip as `172.16.1.16`, remote ip `172.16.2.3` is
  considered in same VLAN. We cache all of such identifed VLAN ips somewhere.

- later when `ipxwrapper` wants to send a UDP broadcast packet, instead of broadcasting to a
  broadcast address (it doesn't work since this VPN doesn't support it), it unicasts to all of the
  above identified VLAN ips one by one. It simulates the broadcast behaviour.

- on receiver side, we also specially handle the netmask `255.255.255.255` by using `255.255.0.0`
  instead, so that packets from IP in this VLAN will NOT be falsely rejected.

## Building Instructions

`ipxwrapper` uses a fairly uncommon building toolchain which is called `win-builds`. It claims to be
functional on both Windows and Linux. I didn't try with Linux but at least my following steps on
Windows 10 worked pretty well.

BTW: `ipxwrapper` compilation also depends on header files of `WinPcap`. However you don't need to
download and setup it by yourself, since this fork has already put related files into the repository
so no additional manual steps are needed.

### Setup Build Environment

- Install `MSYS2` from [here](https://www.msys2.org/). Then update and install some required
  packages with following commands, in `MSYS2`'s console after it is installed.

```sh
pacman -Syu
pacman -Sy --needed base-devel mingw-w64-x86_64-toolchain
pacman -Sy nasm
```

- Get the `win-builds` from
  [here](http://win-builds.org/doku.php/download_and_installation_from_windows). In case you're
  behind proxy, you can follow the instructions inside it to pre-download the packages to setup an
  offline mirror.

  - Put downloaded files to somewhere. This is my example
    - `C:\software\win-builds-setup\win-builds.exe` -- my renamed executable
    - `C:\software\win-builds-setup\packages\` -- the pakcages I downloaded to setup mirro
  - Run `win-builds.exe` to setup it
    - select install to `MSYS2"` and specify its path
    - select `i686` rather than `x86_64`
    - You may speficify the mirror directory during setup, e.g. `c:\software\win-builds-setup`

- Enter `win-builds`
  - Launch `MSYS2`
  - Run command `source /opt/windows_32/bin/win-builds-switch` to switch to it

### Build `ipxwrapper`

- Get my forked project. You may try either of the following ways:

  - Navigate to `https://github.com/jonathanding/ipxwrapper` and use `Code -> Download Zip`
  - Or clone it `git clone https://github.com/jonathanding/ipxwrapper.git`

- In `win-builds` Build Environment
  - Change directory to where you downloaded the source code. There should be a `Makefile` under it
  - Run `make` and cross your fingers
  - You should see these four files in current directory if compilation succeeds
    - `ipxwrapper.dll`
    - `dpwsockx.dll`
    - `mswsock.dll`
    - `wsock32.dll`
  - Copy them to where you want to use `ipxwrapper` to replace the old files
  - **NOTE:** Do not copy `ipxconfig.exe` generated here. It seems it doesn't has some lib
    statically linked so may have issues on deployed machine. Simply use the unchanged one shipped
    by original `ipxwrapper`.
