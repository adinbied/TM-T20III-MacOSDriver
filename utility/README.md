# TM-T20III Utility for macOS

Replacement for Epson's Windows TM-T20III software. One app, three windows, each laid out
like the Windows original with its labels:

| Window | Windows original | Stored in |
|---|---|---|
| **TM-T20III Utility** | TMUtility.exe: Current Settings, Basic Settings (Storing Logos, Paper Reduction, Automatic Paper Cut, Printing Control), Detailed Settings (Font, Buzzer, Communication I/F), Operation Check, Backup/Restore | the printer (USB via IOUSBHost, or TCP 9100) |
| **Printing Preferences** (⌘,) | driver property sheet: Layout, Paper/Quality, Feed and Cut, Peripherals, Version, Advanced Settings | the CUPS queue's PPD |
| **APD Utility** | APD 6 Utility: Layout, User Defined Paper, Send Command | the CUPS queue's PPD |

Command-line equivalents: **tmutil** (printer settings) and **tmqueue** (queue settings).

## Build (on the Mac)

```sh
xcode-select --install             # once
make test                          # protocol tests vs. simulated printer + queue model tests
make tmutil tmqueue                # build/tmutil, build/tmqueue (universal)
make app                           # "TM-T20III Utility.app" (universal, ad-hoc signed)
                                   # SIGN_ID="Developer ID Application: ..." make app  -> hardened + timestamped
```

Try without a printer: `make sim` in one terminal, then
`build/tmutil --tcp 127.0.0.1:19100 get`.

## Using it

```sh
tmutil info                        # model, serial, firmware
tmutil status
tmutil get                         # all settings
tmutil dump                        # raw customize values + memory switches
tmutil set density=4 speed=10      # shows current -> new, asks, writes
tmutil preset truss                # Paperwork.show settings
tmutil backup before.txt           # key=value file (app uses the same format)
tmutil restore before.txt
tmutil test-print | drawer 2 | buzz 1
tmutil --tcp 192.168.1.50 get      # Ethernet models
```

Queue settings (Printing Preferences from the shell):

```sh
tmqueue keys                                       # every setting, default, allowed values
tmqueue show TM_T20III
sudo tmqueue set TM_T20III TmFeedCut=Detail tm-cut-pageend=1 tm-feed-docend=10000
sudo tmqueue paper-add TM_T20III "TRUSS TAPE" 3.18 12 --inch    # User Defined Paper
sudo tmqueue set TM_T20III 'tm-cmd-docbegin=1B 40'                # Send Command
```

Queues created before driver 0.2 have an old PPD: re-run `../scripts/add-printer.sh` once
(the .pkg installer upgrades them automatically and keeps their settings).

Writes enter Epson's user-setting mode, send all changes, and leave it; **the printer resets**
afterwards (USB disconnects and reconnects for a few seconds).

**Close the app / don't run tmutil while printing.** Each operation opens the printer
exclusively and releases it immediately. If CUPS is printing, the open fails rather than
corrupting the job (the Windows utility's known problem).

## Printer settings (TM-T20III Utility window)

| key | Windows label | printer command | verified |
|---|---|---|---|
| paper_width_columns | Paper width - Number of columns | `GS ( E` fn5 a=3 (width) + a=11 (columns) | yes |
| density, speed | Print density, Print speed | a=5, a=6 | id yes, *value scale assumed = level* |
| power_led, resize_*, power_supply, autocut_cover | Power LED ..., Resizing method ..., Power Supply Unit Capacity, Method for canceling auto cutter error | a=22, 70, 71, 73, 98, 100 | yes |
| reduce_* | Paper Reduction page | a=101-106 | yes |
| rx_buffer, busy_condition, rx_error, auto_lf | Receive buffer capacity ... Auto line feed | Msw1 bits 2-5 (`GS ( E` fn3/fn4) | yes |
| usb_power_save ... near_end_sensor, error_signal_8 | Power saving function for USB ... | Msw5-1..5, Msw6-1, Msw8-6 | **inferred** |
| code_page, intl_charset | Font page | a=8, a=9 | **inferred** |

Shown but not writable yet (commands not traced): Storing Logos, Automatic Paper Cut, Buzzer
events, USB class, Multi-language font, the 128 KB receive buffer.

Before trusting the unverified ones: configure the printer with Epson's Windows utility
(e.g. Paperwork.show's settings: speed 10, density 4), then on the Mac run
`tmutil dump` and check the values match. `tmutil` and the app never write customize values
1 and 2 (they wipe NV memory).

Not yet implemented: network (TMN*) pages.

## Layout

```
Sources/TMCore/          C core: protocol, catalog, presets, transports
  tmcore.c               GS ( E fn1-6, DLE EOT, GS I, catalog, presets
  tmdriver.c             Printing Preferences model <-> queue PPD (+ user defined paper)
  tmlink_posix.c         fd + TCP transports
  tmlink_usb_darwin.m    USB via IOUSBHost (macOS)
Resources/               AppIcon.png/.icns + make_icon.py (draws them)
Sources/TMUtility/       SwiftUI app (TMUtilityApp, PreferencesViews, QueueModel, PrinterModel)
cli/tmutil.c             printer settings CLI
cli/tmqueue.c            queue settings CLI (libcups)
tests/                   simulator, protocol tests, TCP sim server
```

## Status

- Core + both CLIs: built and tested on Linux. The tests cover:
  - simulator protocol tests
  - generating and parsing the queue PPD
  - `tmqueue` -> `cupstestppd` -> filter, end to end
- USB transport: compiles on macOS (tmutil, tmcore_test), not yet run against a printer.
- SwiftUI app: written for the macOS 13 SDK, **not yet compiled**.
- No real-printer test yet.
