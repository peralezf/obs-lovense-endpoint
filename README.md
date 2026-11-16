# OBS Lovense SmartCam Endpoint

An OBS plugin for Linux that hosts the local SmartCam WebSocket endpoint used by Lovense Cam's Video Feedback browser source. It connects the browser extension directly to OBS on loopback and relays SmartCam state to the Video Feedback renderer.

The plugin does not include or replace Lovense's browser-source interface. It does not create OBS scenes or sources, connect to Lovense cloud services, or run a separate bridge process. Keep the existing Lovense Video Feedback browser source and extension; its local page URL must use port `11000`.

> [!tldr]
> In human terms: the local endpoint Lovense could've provided years ago, with an installer that mostly works.
> If it doesn't, ask your *slop generator of choice*. I'm sure it'll figure something out.

## Install

The installer builds from source and installs to the current user's OBS plugin directory. It does not use `sudo`, change scenes or browser-source settings, or restart OBS.

On Arch Linux, install the build and development packages:

```sh
sudo pacman -S --needed base-devel cmake obs-studio openssl boost websocketpp nlohmann-json
```

> [!info]
> These packages provide the compiler, OBS development files, and protocol libraries needed to build the plugin.

Then, from this directory:

```sh
./install.sh
```

If you downloaded the source as an archive and `install.sh` is not executable, run `chmod +x install.sh` first.

The installer uses a temporary build directory and installs these files under `~/.config/obs-studio/plugins/obs-lovense-endpoint/`:

```text
obs.json
bin/64bit/obs-lovense-endpoint.so
data/locale/en-US.ini
```

Restart OBS to load the plugin. To use a different OBS plugin directory, set `OBS_PLUGIN_DIR` to the full path of the plugin directory before running the installer. Set `BUILD_JOBS` to change the parallel build count.

## Connect

The endpoint serves `ws://127.0.0.1:11000/smartcam/ws`. The Lovense Cam extension and its Video Feedback browser source connect to this endpoint.

If you need to add the browser source manually, use the `index.html` supplied with Lovense's local Video Feedback files and set its URL to include `?port=11000`. On the tested setup, the page is at `~/.config/obs-studio/dist/index.html`; in OBS, enter its absolute file URL, for example:

```text
file:///home/<your-username>/.config/obs-studio/dist/index.html?port=11000
```

Replace `<your-username>` with your Linux username. The installer does not ship `index.html` or change scene settings. Name the Browser Source `Lovense Video Feedback` to match the documented setup; the endpoint identifies its connection by URL, not source name.

In the OBS log, look for:

```text
[Lovense endpoint] listening on 127.0.0.1:11000/smartcam/ws
[Lovense endpoint] video-feedback renderer connected
[Lovense endpoint] cam extension connected
```

The renderer and extension may connect in either order. Port `11000` must be available to OBS; stop any other SmartCam listener using it. This plugin binds only to `127.0.0.1`, does not open a LAN listener, and stops when OBS unloads the module.

## Build requirements

- Linux x86_64 or ARM64
- OBS Studio development files, including the `libobs` CMake package
- CMake 3.16 or newer, a C++17 compiler, and Make
- OpenSSL, Boost.Asio headers, websocketpp headers, and nlohmann-json headers

The plugin and installer were tested with OBS Studio 33.0.0 on Arch Linux. Other distributions need the same development headers and an OBS plugin ABI compatible with their installed runtime. Flatpak OBS is not covered by the default install path or this build.

## Credentials and network scope

The repository contains no account tokens, OBS WebSocket passwords, API credentials, or user configuration. The fixed SmartCam AES key and IV in `src/lovense_websocket_common.hpp` are protocol-compatibility constants required to decode SmartCam frames; they are not personal account credentials. The endpoint itself makes no cloud requests and logs connection status without logging message payloads.

## Uninstall

Close OBS, then remove the plugin directory:

```sh
rm -r -- "$HOME/.config/obs-studio/plugins/obs-lovense-endpoint"
```

If you installed to a custom `OBS_PLUGIN_DIR`, remove that directory instead.

## Repository contents

This repository contains only the OBS endpoint module, the minimal SmartCam protocol sources it needs, locale metadata, CMake configuration, and the installer. Build output, OBS profiles, browser-source assets, bridge services, and user configuration are intentionally excluded.
