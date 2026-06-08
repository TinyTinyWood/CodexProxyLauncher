# Codex Proxy Launcher

A small native Windows launcher for starting the Codex desktop app with proxy
environment variables.

## Features

- Starts Codex with `HTTP_PROXY`, `HTTPS_PROXY`, `ALL_PROXY`, `NO_PROXY`, and
  related proxy environment variables.
- Supports HTTP and SOCKS proxy URLs such as `http://127.0.0.1:10808` and
  `socks5://127.0.0.1:10808`.
- Detects Codex's WSL backend setting and injects proxy variables into WSL when
  Codex is configured to run there.

## Requirements

- Windows 10 or later.
- Codex desktop app installed on the machine.

## Quick Start

1. Download or build `CodexProxyLauncher.exe`.
2. Put `config.txt` next to the executable, or let the launcher create it on
   first run.
3. Run `CodexProxyLauncher.exe`.
4. Enter your proxy address, for example `http://127.0.0.1:10808` or
   `socks5://127.0.0.1:10808`.
5. Click the start button to launch Codex.

If Codex is already running, the launcher asks you to close it first. It does
not kill existing Codex processes automatically.

## Configuration

Settings are stored in `config.txt` next to the executable. The GUI edits only
`proxy_address`; advanced options can be changed by editing the file directly.

```ini
proxy_address=http://127.0.0.1:10808
chromium_proxy=
no_proxy=localhost,127.0.0.1,::1
codex_exe_path=
startup_wait_seconds=20
temporarily_set_user_proxy_environment=false
```

### Options

- `proxy_address`: Main proxy URL passed to Codex. Include the scheme, such as
  `http://` or `socks5://`.
- `chromium_proxy`: Optional Chromium-specific proxy override. Leave empty to
  reuse `proxy_address`.
- `no_proxy`: Comma-separated hosts that should bypass the proxy.
- `codex_exe_path`: Optional explicit path to `Codex.exe`. Leave empty to let
  the launcher auto-detect Codex.
- `startup_wait_seconds`: How long to wait for Codex to appear after launch.
- `temporarily_set_user_proxy_environment`: Temporarily writes proxy variables
  to the current user's environment when launching packaged Windows app entries.

## WSL Proxy Support

The launcher checks:

```text
%USERPROFILE%\.codex\config.toml
```

If it finds:

```toml
runCodexInWindowsSubsystemForLinux = true
```

the launcher adds proxy variables to `WSLENV` and attempts to configure proxy
settings inside WSL. If the WSL proxy endpoint cannot be reached, the launcher
continues starting Codex and shows a warning because network requests may hang
inside the WSL backend.

## Build

Open a Developer PowerShell for Visual Studio, then run:

```powershell
msbuild .\CodexProxyLauncherWin32.vcxproj /p:Configuration=Release /p:Platform=x64
```

The executable is generated at:

```text
bin\Release\CodexProxyLauncher.exe
```

## Repository Layout

```text
.
|-- CodexProxyLauncherWin32.vcxproj
|-- app.manifest
|-- assets/
|   |-- app-icon.ico
|   `-- app-icon.png
|-- config.txt
`-- src/
    |-- main.cpp
    |-- resource.h
    `-- resource.rc
```

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
