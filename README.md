Nix flake providing **evdev-tools**, three first-party C++ binaries for a TV-attached input box: `evdev-interposer`, which presents virtual keyboard and pointer devices over grabbed evdev devices with all local input gated on the TV's CEC state; `evdev-bridge`, which holds a bridge session that switches the TV to another input and forwards local input to it over CEC; and `evdev-chords`, a passive chord observer that fires configured commands. The C++ source is carried in this repository and built against nixpkgs; there is no upstream project.

## Outputs

- `packages.<system>.evdev-tools` — the package, installing `bin/evdev-interposer`, `bin/evdev-chords`, and `bin/evdev-bridge`.
- `packages.<system>.default` — the same package.
- `formatter.<system>` and `checks.<system>.formatting` — the repository's own source formatting (`nixfmt` and `clang-format`).

The packages are defined for `aarch64-linux` and `x86_64-linux`, which is also what the package's `meta.platforms` declares. All three binaries ship in one derivation, and `meta.mainProgram` is `evdev-interposer`: `lib.getExe` names that binary, so the other two need `lib.getExe'`.

```nix
evdev-tools.packages.${system}.evdev-tools
```

Or build it directly:

```bash
nix build "github:jgus-org/evdev-tools-flake#default"
```

The package builds and tests with no keyboard, gamepad, or TV attached; its test suite exercises the routing, chord, and CEC-state logic directly. `evdev-interposer --help` and `evdev-bridge --help` print the full option list.

## Runtime requirements

Linux only: the tools use evdev, uinput, and the kernel CEC device interface, so they need no display server, desktop session, or D-Bus, but they do need the devices.

- `evdev-interposer` opens `/dev/uinput` read-write to create its sink devices (on NixOS, `hardware.uinput.enable = true`), and opens the evdev nodes under `/dev/input` read-only — the grab it takes on an interposed device (`EVIOCGRAB`) is a read-side claim, so read access to those nodes is enough.
- The TV gate needs a CEC adapter node. `--cec-adapter` takes the adapter **name the kernel reports** (`HDMI-A-1`, `DP-2`), not a device path: the daemon scans `/dev` for CEC nodes and matches that name, so the name is what survives a reboot that renumbers the nodes. That port must be wired to the TV and the kernel must provide the CEC driver.
- `evdev-chords` only reads `/dev/input`, and `evdev-bridge` only connects to a socket; neither needs CEC or uinput of its own.
- Run as a system service (root) or with equivalent access to those nodes. `evdev-interposer` reports readiness with `sd_notify` (`Type=notify`), and runs unchanged where no `NOTIFY_SOCKET` is set.

## Behaviour worth knowing

Without `--cec-adapter` the gate is never engaged: input is never withheld waiting for the TV's state, and nothing wakes the TV. With one, gating is derived only from CEC bus traffic — the TV known-on and not showing this machine, or its active source unknown, gates; no CEC evidence at all leaves input live. While gated, the pop gesture is the only input acted on, and the rest is dropped rather than queued. A false gate costs one pop gesture, because popping while gated wakes the TV and re-announces this machine as the active source.

An interposed device is held by an exclusive grab for as long as the daemon runs, so other readers of that node (including `evdev-chords`) see nothing from it. A gamepad is never interposed even when a matcher covers its vendor:product, and `--remote` sources are read-only by design: never grabbed, never waking the TV.

`--pop-cmd` is invoked with the pop trigger (`key`, `remote`, or `pad chord`) as its argument. Exit status 10 asks the daemon to emit `--pop-forward-key` as a press and release on the keyboard sink; any other status emits nothing.

`evdev-chords` takes one argument, a JSON file of `[{"buttons": [<evdev key codes>], "holdMs": <int>, "command": [<argv>]}, ...]`, where `holdMs` defaults to 0 (fire on press). It rescans `/dev/input` on a two-second timer rather than watching udev, and reads devices without grabbing them.

`evdev-bridge` requires both `--socket <interposer control socket>` and `--target <x.x.x.x>` (a CEC physical address). It exits 0 when the interposer reports the bridge `done`, which is when the TV is back on this machine; `SIGUSR1` only asks the interposer to switch the TV back and the process still waits for that report, while `SIGTERM`, `SIGINT`, or `SIGHUP` closes the socket and ends the session without the return.

Logical-address claiming (`--cec-osd-name`) keeps whatever address the bus yields, since other playback devices legitimately hold addresses. The input switch `evdev-bridge` uses broadcasts `ACTIVE_SOURCE` naming another device's physical address, which is nonstandard — a device announces only itself — and is the only device-initiated input switch a TV honors; a TV that refuses it leaves the switch a no-op. Volume follows System Audio Control: with `--volume-socket` set, `up`/`down`/`mute` datagrams are delivered to that socket only while no audio system is on the CEC bus, and otherwise volume goes over CEC toward the TV.
