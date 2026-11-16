#!/usr/bin/env bash
set -Eeuo pipefail

readonly plugin_id="obs-lovense-endpoint"
readonly source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

usage() {
	cat <<'EOF'
Build and install the OBS-local Lovense SmartCam endpoint for Linux.

Usage:
  ./install.sh

Optional environment variables:
  OBS_PLUGIN_DIR  Exact OBS plugin directory (default:
                  ~/.config/obs-studio/plugins/obs-lovense-endpoint)
  BUILD_JOBS      Parallel build jobs (default: detected CPU count)

The installer builds in a temporary directory and does not restart OBS or
change scenes, sources, browser URLs, OBS settings, or credentials.
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
	usage
	exit 0
fi
if [[ $# -ne 0 ]]; then
	usage >&2
	exit 2
fi
if [[ "$(uname -s)" != "Linux" ]]; then
	printf 'Error: this plugin installer supports Linux only.\n' >&2
	exit 1
fi
case "$(uname -m)" in
	x86_64|aarch64|arm64) ;;
	*) printf 'Error: unsupported 64-bit architecture: %s\n' "$(uname -m)" >&2; exit 1 ;;
esac

for command_name in cmake c++ make; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		printf 'Error: required build tool not found: %s\n' "$command_name" >&2
		printf 'Install a C++ compiler, CMake, and Make, then retry.\n' >&2
		exit 1
	fi
done

plugin_dir="${OBS_PLUGIN_DIR:-${HOME:?HOME must be set}/.config/obs-studio/plugins/${plugin_id}}"
jobs="${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')}"
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
	printf 'Error: BUILD_JOBS must be a positive integer.\n' >&2
	exit 2
fi

build_dir="$(mktemp -d "${TMPDIR:-/tmp}/${plugin_id}.build.XXXXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT

printf 'Configuring %s...\n' "$plugin_id"
cmake -S "$source_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
printf 'Building %s...\n' "$plugin_id"
cmake --build "$build_dir" --target obs-lovense-endpoint --parallel "$jobs"

printf 'Installing to %s...\n' "$plugin_dir"
cmake --install "$build_dir" --prefix "$plugin_dir"

if [[ ! -s "$plugin_dir/bin/64bit/obs-lovense-endpoint.so" || ! -s "$plugin_dir/obs.json" ]]; then
	printf 'Error: installation did not produce the expected OBS plugin files.\n' >&2
	exit 1
fi

printf '\nInstalled successfully. Restart OBS to load the endpoint.\n'
printf 'The endpoint listens on 127.0.0.1:11000 and does not alter scenes or sources.\n'
