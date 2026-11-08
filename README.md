<img src="utility/Resources/AppIcon.png" width="128" align="right" alt="TM-T20III Utility icon">

# Epson TM-T20III driver and utility for macOS (Apple Silicon / Intel)

Open CUPS raster driver and settings utility rebuilt from Epson's end-of-life Windows APD6
driver and TM-T20III Utility. Prints anything macOS can print (PDF, browsers, truss tapes) on 80 mm or
58 mm roll paper. Not made or supported by Epson.


## Prebuilt install

1. Download **`TM-T20III-Driver-<version>.pkg`** from the [latest release](../../releases/latest).
   It is signed with a Developer ID and notarized by Apple, so it opens without Gatekeeper warnings.
2. Connect the printer over USB and switch it on. <img width="640" height="386" align="right" alt="Screenshot 2026-11-07 at 5 19 12 PM" src="https://github.com/user-attachments/assets/474bd73e-488c-4243-8eb4-12fd05742dd0" />

3. Double-click the `.pkg` and follow the installer. It installs:
   - the driver: `/Library/Printers/Epson/TMT20III/rastertotmt20` and the PPD
   - **TM-T20III Utility** in Applications
   - the `tmutil` and `tmqueue` command-line tools in `/usr/local/bin`
4. A queue named **TM_T20III** is created if the printer was connected. Existing TM-T20III
   queues are upgraded and keep their settings. Printer connected later, or on Ethernet:

   ```sh
   /Library/Printers/Epson/TMT20III/add-printer.sh
   /Library/Printers/Epson/TMT20III/add-printer.sh TM_T20III socket://192.168.1.50:9100
   ```


Optionally check the download against the `.sha256` file on the release:
`shasum -a 256 -c TM-T20III-Driver-<version>.pkg.sha256`.

Uninstall: `sudo /Library/Printers/Epson/TMT20III/uninstall.sh` (add `--remove-queues` to also
delete the print queues).

Requires macOS 13 or later. The printer must be in **USB Printer Class** mode (the T20III
default). In Vendor Class mode (USB PID 0x0202, used with Epson's TMUSB.sys) macOS's USB
backend will not see it.

## Build from source

Needs the Xcode command line tools (`xcode-select --install`); the app needs Swift 5.9+
(Xcode 15 or later).

```sh
git clone <this repo> && cd <repo>
make                            # universal arm64 + x86_64 build/rastertotmt20
make test                       # encoder self-test (no printer needed)
make filter-test                # real filter + PPD on generated CUPS raster
sudo make install               # filter -> /Library/Printers/Epson/TMT20III, PPD -> /Library/Printers/PPDs
./scripts/add-printer.sh        # finds usb://EPSON/TM-T20III... and creates queue TM_T20III

cd utility
make test                       # protocol tests vs. simulated printer + queue model tests
make tmutil tmqueue             # build/tmutil, build/tmqueue (universal)
make app                        # "TM-T20III Utility.app" (universal, ad-hoc signed)
```

Ethernet models: `./scripts/add-printer.sh TM_T20III socket://<ip>:9100`.

## Making the signed installer

`scripts/make-installer.sh` (or `make installer`) builds everything from clean, runs the tests,
signs every binary and the app with the hardened runtime, builds a signed product archive,
notarizes and staples it, and writes `dist/TM-T20III-Driver-<VERSION>.pkg` plus a `.sha256`.

One-time setup on the Mac that builds releases:

1. In Xcode > Settings > Accounts > your team > **Manage Certificates**, add a
   **Developer ID Application** and a **Developer ID Installer** certificate (or create them at
   developer.apple.com and import them). Check with `security find-identity -v`.
2. Create an app-specific password at account.apple.com, then store notarization credentials:

   ```sh
   xcrun notarytool store-credentials tmt20iii-notary        --apple-id you@example.com --team-id ABCDE12345
   ```

Release steps:

```sh
echo 0.2.1 > VERSION                  # also bump *FileVersion in ppd/EpsonTMT20III.ppd
./scripts/make-installer.sh           # -> dist/TM-T20III-Driver-0.2.1.pkg (+ .sha256)
./scripts/make-installer.sh --release # same, then uploads it to GitHub release v0.2.1 (needs gh)
```

Without `--release`, upload `dist/*.pkg` and `dist/*.pkg.sha256` to a new GitHub release by
hand; the Prebuilt install link above always points at the latest one.

| Option / variable | Meaning |
|---|---|
| `--unsigned` | ad-hoc binaries, unsigned pkg, no notarization (local testing) |
| `--no-notarize` | signed but not notarized |
| `--skip-tests` | skip the test suites |
| `DEV_ID_APP`, `DEV_ID_INSTALLER` | certificate names, if more than one is in the keychain |
| `NOTARY_PROFILE` | notarytool profile name (default `tmt20iii-notary`) |
| `ID_PREFIX` | bundle / package identifier prefix (default `local.tmt20iii`, e.g. set `com.yourcompany.tmt20iii`) |

The app icon is drawn by `utility/Resources/make_icon.py` (Pillow + numpy); its PNG and .icns
outputs are committed, so building needs no Python packages.

## Settings (same as the Windows driver)

Every Windows driver setting has a Mac counterpart. The windows match Epson's layout and labels.

| Windows | macOS |
|---|---|
| Printing Preferences > Layout / Paper/Quality / Feed and Cut / Peripherals / Version / Advanced Settings | TM-T20III Utility > **Printing Preferences…** (same tabs) |
| APD Utility > Layout / User Defined Paper / Send Command | TM-T20III Utility > **APD Utility** (Advanced Settings tab) |
| Print dialog | macOS print dialog > Printer Options (Paper/Quality, Advanced, Feed and Cut, Peripherals) |

Printing Preferences writes the queue's PPD (`lpadmin -P`, one admin prompt), so the settings
apply to every app. The same from the shell: `utility/build/tmqueue` (see `utility/README.md`).

Print-dialog / `lp -o` options:

| Option | Values | Windows equivalent |
|---|---|---|
| `PageSize` | Letter, A4, Postcard, RP58x297, RP58x3276, **RP80x297**, RP80x3276, TrussTape, UDP1..n (user defined), Custom.WxH | Paper Size |
| `TmPaperSource` | **None**, Top, Bottom, Both | Paper Source (= paper conservation; Windows default Bottom) |
| `ColorModel` | **Mono**, Gray | Color |
| `TmPaperWidth` | **80mm**, 58mm | Roll paper width |
| `TmScaling` | **100**, 70 | Reduced size print |
| `TmUpsideDown` | **False**, True | Upside-down |
| `TmFeedCut` | **DocFeedCut**, DocFeedNoCut, DocNoFeedCut, DocNoFeedNoCut, PageFeedCut, PageFeedNoCut, PageNoFeedCut, Detail ("Custom") | Feed and Cut preset |
| `TmPeripherals` | **Default** (as set in Printing Preferences), None | Peripherals |
| `TmLengthScale` | 0.990 ... **1.000** ... 1.020, Custom.x | (macOS only) vertical calibration |
| `TmBlankFeed` | **Feed**, Raster | (macOS only) |
| `TmCompress` | **True**, False | (macOS only) |
| `TmPrintControl` | **Default**, Mode1-3 | (macOS only, experimental) |

Detail settings (written by Printing Preferences as `*TmSetting` lines; can also be given per job
with `-o`). `<t>` is `docbegin`, `pagebegin`, `pageend` or `docend`:

| Setting | Values | Windows |
|---|---|---|
| `tm-cut-<t>` | 0 none, 1 cut, 16 feed to cutter, 17 feed + cut | Feed and Cut > Detail option |
| `tm-feed-<t>` | micrometres | Feeding amount |
| `tm-dk-<t>`, `tm-dk-count-<t>` | 0, 1, 2, 3 (drawers), 256-258 (built-in buzzer +drawer), 512-516 (external buzzer A-E) + count | Peripherals |
| `tm-cmd-<t>` | hex bytes, e.g. `1B 40` | APD Utility > Send Command |
| `tm-margin-top`, `tm-margin-left` (+`-enable`) | micrometres, 0-11000 | APD Utility > Layout > Adjust Print Position |
| `tm-reduce-80-58` | 0/1 | APD Utility > Layout > Reduce from 80 mm to 58 mm |
| `tm-unsupported-auto` | **1**/0 | APD Utility > Layout > Unsupported Paper |

Example: `lp -d TM_T20III -o PageSize=TrussTape -o TmLengthScale=1.005 tape.pdf`

## Truss tapes (Paperwork.show)

1. Print with **TRUSS TAPE** (built in, or add it under APD Utility > User Defined Paper:
   inch, 3.18 x 12.00, margins 0, exactly as Paperwork.show's Windows guide), Scale 100 %,
   headers/footers off.
2. Paper Source = None (default) - every blank row is fed, so 12 in pages add up exactly.
3. Feed and Cut = Document[Feed, Cut] (default).
4. Leave Paperwork.show's own scale at **1.000** first and measure a 10 ft calibration tape. If it is
   still off, set Length calibration once in Printing Preferences > Layout > Advanced... instead of in
   the browser - then every app gets the correction.
5. Printer-side settings (paper reduction off, speed, density): TM-T20III Utility > Printer > Load Preset >
   Truss tapes, then Set.

## Debugging

- Decode any stream (ours or a Windows `FILE:` capture) back into paper:
  `python3 tests/escpos_decode.py job.bin --pbm job.pbm -v`
- `sh tests/queue_test.sh`: tmqueue writes a queue PPD, cupstestppd checks it, the filter prints with it.
- Capture what CUPS sends: `lpadmin -p TM_TEST -v file:///tmp/tm.bin -P ppd/EpsonTMT20III.ppd`
  (requires `FileDevice Yes` in `/etc/cups/cups-files.conf`), then decode `/tmp/tm.bin`.
- Filter log: `cupsctl --debug-logging`, then `/var/log/cups/error_log` (lines `DEBUG: width=...`).

## Settings utility

`utility/` holds TM-T20III Utility.app (SwiftUI) with three windows that copy the Windows ones:
TM-T20III Utility (printer-side settings), Printing Preferences and APD Utility. It also builds
the `tmutil` (printer) and `tmqueue` (queue) command-line tools.

## Layout

```
utility/                TM-T20III Utility.app, tmutil, tmqueue
src/tmt20.[ch]          ESC/POS encoder (portable C99 + zlib, unit-tested)
src/rastertotmt20.c     CUPS raster filter glue (options, margins, scaling, upside-down, dither)
ppd/EpsonTMT20III.ppd   PPD (page sizes, options, filter path)
tests/                  encoder self-test, filter end-to-end test, stream decoder
scripts/                add-printer.sh, uninstall.sh, make-installer.sh
pkg/                    installer postinstall script, distribution + welcome pages
VERSION                 release version (app, pkg)
```

## Status

- Encoder + filter + PPD: built and tested on Linux with libcups. The tests cover:
  - unit tests and a pixel-exact round trip
  - every setting above through the real filter and PPD
  - `cupstestppd`, which flags only the missing filter path (expected off-Mac)
- On a Mac: the utility protocol and queue-model tests pass. The driver tests, `tmqueue` and the
  app build still need confirming there. No real-printer test yet.
- Unverified assumptions:
  - The ESC J unit is 1/406 in, i.e. 2 units per row. If blank areas come out the wrong length,
    switch to `TmBlankFeed=Raster`.
  - The `1284DeviceID` string.
  - `TmPrintControl` semantics.
  - "Unsupported Paper": pages wider than the roll by more than 5 mm are shrunk. This is our
    reading of the APD option; Epson's rule is not traced.
