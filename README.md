# Waterdisco

<img width="128" alt="Waterdisco icon" src="res/public/Throne.png" />

Cross-platform Qt proxy client powered by [sing-box](https://github.com/SagerNet/sing-box). Waterdisco is a fork of Throne; legacy internal names such as `Throne`, `ThroneCore`, `throne.db` and `throne://` are intentionally retained for compatibility.

## Fork Changes

- Profile startup and disable controls with persistent performance metrics.
- Ranked connection/download speed tests with Site Score, cancellation and recovery.
- Remember-last-profile startup, subscription request routing and configurable request identity.
- Parallel fast path for large line-based subscriptions.
- Full application-state export/import, routing-profile tray selection and Improve mood player.

## Installation

Download the matching release artifact from CI.

- Windows: `Waterdisco-<version>-windows-universal-installer.exe`; portable archives are `Waterdisco-<version>-windows64.zip`, `Waterdisco-<version>-windows-arm64.zip`, `Waterdisco-<version>-windows32.zip` and `Waterdisco-<version>-windowslegacy64.zip`.
- Debian, Ubuntu and derivatives: install `Waterdisco-<version>-debian-amd64.deb` (or arm64). Use `Waterdisco-<version>-debian-*-system-qt.deb` only when the system Qt is compatible.
- CachyOS, Arch and other Linux distributions: unpack `Waterdisco-<version>-linux-amd64.zip` or `Waterdisco-<version>-linux-arm64.zip`, then run `./Throne -appdata` inside the unpacked directory.
- macOS: unpack `Waterdisco-<version>-macos-arm64.zip`, `Waterdisco-<version>-macos-amd64.zip` or `Waterdisco-<version>-macoslegacy-amd64.zip`, move `Waterdisco.app` to `/Applications`, then run `xattr -dr com.apple.quarantine /Applications/Waterdisco.app` if macOS blocks it.

The binary remains named `Throne` inside portable archives for compatibility.

## Supported protocols

- SOCKS, HTTP(S), Shadowsocks, Trojan, VMess, VLESS, TUIC, Hysteria, Hysteria2, AnyTLS, Mieru, NaïveProxy, Juicity, TrustTunnel, ShadowTLS, WireGuard, AmneziaWG, SSH, Snell, OpenVPN and OpenConnect.
- Xray VLESS, custom sing-box/Xray outbounds and configs, chaining and extra cores.

## Subscription formats

Share links, sing-box JSON, v2rayN links, limited Shadowsocks and Clash formats, plus legacy `throne://` deeplinks.

## Credits

- [Throne](https://github.com/throneproj/Throne) upstream
- [sing-box](https://github.com/SagerNet/sing-box), [Xray-core](https://github.com/xtls/xray-core), [Qv2ray](https://github.com/Qv2ray/Qv2ray), Qt, simple-protobuf, fkYAML, quirc, QHotkey and SQLiteCpp.
