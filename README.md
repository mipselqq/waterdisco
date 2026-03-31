# What's new in this fork

This fork adds several quality-of-life features and improvements to the original Throne app:

### 🚀 Smarter Speed Tests
* **New Metrics:** See more valuable details for each server, including **Connection Time**, **Rx Speed**, and overall **Site Score**.
* **Ping removal**: Ping is just a bad metric for measuring server quality. Removed in favour of connection time, which basically shows HTTP connection establishment time via a tunnel.
* **Save Time:** The new **"Speed test fall short"** option stops testing slow servers early, prioritizing your fast servers first. If one server's download test time is more than 2x of fastest discovered time, skip.
* **Stop Anytime:** Added a button to easily cancel a speed test while it's running.

### ⚡ Auto-Connect & Startup
* **Connect to Best:** Added a **"Connect with best site score"** action to instantly pick the fastest server.
* **Startup Automation:** The app can now automatically test your servers and connect to the best one every time you open it.
* **Startup Selection:** Choose exactly which servers you want the app to test on startup.

### 🛠️ Profile Management
* **Disable Profiles:** You can now disable specific profiles so they are ignored during tests and auto-connects.
* **Saved Settings:** The app now correctly remembers your selected toggles, test results, and disabled profiles even after a restart.

Btw, the codebase is such a spaghetti that I don't even care about the quality lol.

# Waterdisco

Qt based Desktop cross-platform GUI proxy utility, empowered by [Sing-box](https://github.com/SagerNet/sing-box)

Supports Windows 11/10/8/7/36 / Linux / MacOS out of the box.

<img width="300" height="300" alt="Untitled1" src="https://github.com/user-attachments/assets/272c3ac1-156f-4b04-a1cd-cdd8eebe8c73" />

### Note on MacOS releases
Apple platforms have a very strict security policy and since Throne does not have a signed certificate, you will have to remove the quarantine using `xattr -d com.apple.quarantine /path/to/throne.app`. Also to get the built-in privilege escalation to work, `Terminal` should have the `Full Disk` access.

### GitHub Releases (Portable ZIP)

[![GitHub All Releases](https://img.shields.io/github/downloads/Mahdi-zarei/nekoray/total?label=downloads-total&logo=github&style=flat-square)](https://github.com/throneproj/Throne/releases)

### RPM repository
[Throne RPM repository](https://parhelia512.github.io/) for Fedora/RHEL and openSUSE/SLE.

## Supported protocols

- SOCKS
- HTTP(S)
- Shadowsocks
- Trojan
- VMess
- VLESS
- TUIC
- Hysteria
- Hysteria2
- AnyTLS
- NaïveProxy
- Juicity
- TrustTunnel
- ShadowTLS
- Wireguard
- SSH
- Custom Outbound
- Custom Config
- Chaining outbounds
- Extra Core

## Subscription Formats

Various formats are supported, including share links, JSON array of outbounds and v2rayN link format as well as limited support for Shadowsocks and Clash formats.

## Credits

- [SagerNet/sing-box](https://github.com/SagerNet/sing-box)
- [Qv2ray](https://github.com/Qv2ray/Qv2ray)
- [Qt](https://www.qt.io/)
- [simple-protobuf](https://github.com/tonda-kriz/simple-protobuf)
- [fkYAML](https://github.com/fktn-k/fkYAML)
- [quirc](https://github.com/dlbeer/quirc)
- [QHotkey](https://github.com/Skycoder42/QHotkey)

## FAQ
**How does this project differ from the original Nekoray?** <br/>
Nekoray's developer partially abandoned the project on December of 2023, some minor updates were done recently but the project is now officially archived. This project was meant to continue the way of the original project, with lots of improvements, tons of new features and also, removal of obsolete features and simplifications.

**Why does my Anti-Virus detect Throne and/or its Core as malware?** <br/>
Throne's built-in update functionallity downloads the new release, removes the old files and replaces them with the new ones, which is quite simliar to what malwares do, remove your files and replace them with an encrypted version of your files.
Also the `System DNS` feature will change your system's DNS settings, which is also considered a dangerous action by some Anti-Virus applications.

**Is setting the `SUID` bit really needed on Linux?** <br/>
To create and manage a system TUN interface, root access is required, without it, you will have to grant the Core some `Cap_xxx_admin` and still, need to enter your password 3 to 4 times per TUN activation. You can also opt to disable the automatic privilege escalation in `Basic Settings`->`Security`, but note that features that require root access will stop working unless you manually grant the needed permissions.

**Why does my internet stop working after I force quit Throne?** <br/>
If Throne is force-quit while `System proxy` is enabled, the process ends immediately and Throne cannot reset the proxy. <br/>
Solution:
- Always close Throne normally.
- If you force quit by accident, open Throne again, enable `System proxy`, then disable it- this will reset the settings.

**Where are the downloadable route profiles/rulesets coming from?**<br/>
They are located at the [routeprofiles](https://github.com/throneproj/routeprofiles) repository.

**How does "Throne-\<version\>-debian-system-qt-x64.deb" differ from "Throne-\<version\>-debian-x64.deb" and why is the latter 3 times heavier then the former?**<br/>
The first one does not pack the Qt libraries and relies on those installed on the host. The second one packs everything needed with itself, thus being heavier. The reason the first one exists is that on legacy systems provided Qt libraries use unsupported system features. If a graphical interface fails to load for your system, you may try to download the system-qt version and install fitting Qt libraries from your package manager or compile them from source.
