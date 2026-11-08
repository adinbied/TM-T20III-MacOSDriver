# TM-T20III — reverse-engineering notes

Source: Epson APD6 6.12.0.0 (DriverVer 10/29/2025) + TM-T20III Utility 1.00, in this folder.
All binaries are native C/C++ (no .NET). The driver binaries ship x86, x64 *and* ARM64 builds.

## 1. Architecture of the Windows stack

| Piece | File | Role | Needed on macOS? |
|---|---|---|---|
| Unidrv minidriver | `EA6MDLTMT203.GPD` | Static command strings + callback IDs | Port to a PPD |
| Render plugin | `amd64/EA6RDPTM.DLL` (IPrintOemUni) | Builds ESC/POS from Unidrv bands | Port to a CUPS raster filter |
| UI plugin | `amd64/EA6UIPTM.DLL` | Printer properties UI | PPD options cover it |
| Command table | `DriverPack/Setup/TM-T20III.ini` -> `[DRV_PRINTER_SPEC] PrinterSpec=` XML | Model-specific commands (cut, drawer, buzzer, sizes) | Copy values |
| Port monitor | `PortMonitor/*/EA6PM.dll` | Spooler port + status | No (CUPS backend) |
| USB kernel driver | `TMUSB64.SYS` | Only for **vendor-class** USB mode (VID 04B8, PID 0202) | No. Userspace libusb/IOKit |
| Utility | `TM-T20IIIUTL/TMUtility.exe` + `TMU*.dll`/`TMN*.dll` engines, `TMComm.dll` transport | Printer settings | Rewrite as Swift app |

COM entry: `DllGetClassObject` @0x180025050 -> factory vtable 0x180142fc0 -> object vtable 0x180142c88.
Implemented IPrintOemUni methods (string list @0x180142f30): CommandCallback, FilterGraphics,
HalftonePattern, OutputCharStr, SendFontCmd.

CommandCallback dispatcher: `FUN_180036f00` (via thunk 0x18005daa0). GPD CallbackIDs:
1/2 = SendBlockData/EndBlockData, 3 = StartJob, 4 = StartPage, 5 = EndPage, 6 = EndJob,
7 = YMoveRelDown, 8 = XMoveAbsolute, 9 = CR.
Job/page operations (cut, drawer, buzzer, feed) are emitted by `FUN_180015920` ("DoOperation").

## 2. Byte stream the driver sends (observed in code + GPD + PrinterSpec)

Job setup (GPD JobSetup1, default ANK case):
```
1B 3D 01                 ESC = 1        select peripheral (printer)
1D 28 4A 02 00 01 00     GS ( J  (x3, fn 1/2/3 = 0)   (paper-layout/label-related reset)
1D 28 4A 02 00 02 00
1D 28 4A 02 00 03 00
1B 63 30 02              ESC c 0 2      paper type
1B 63 31 02              ESC c 1 2
1B 63 33 00              ESC c 3 0      disable paper-sensor-triggered paper-end
1B 32                    ESC 2          default line spacing
1B 52 00                 ESC R 0        international charset USA
1B 74 10                 ESC t 16       codepage WPC1252
1D 62 01                 GS b 1         smoothing on
```
Optional per job (FUN_180057160/57230): `1D 28 4B 02 00 30 n` print-control/density,
`1D 28 4B 02 00 32 n` print speed (command_Speed=3, max 51 -> range 1..?), `1B 55 n` unidirectional.

Page start/end: `1B 4A 00` (ESC J 0).

Raster, per band. Builder `FUN_1800537d0` switches on `command_Image` (TM-T20III = 14 = 0x0E).
1 bpp, MSB = leftmost dot, 1 = black:
```
1D 38 4C p1 p2 p3 p4  30 70 a 01 01 31  xL xH yL yH  <data>
   GS 8 L, p = len(data)+10 (32-bit LE), m=48, fn=112 store raster, bx=by=1, c=49 ('1')
   a = 0x31 -> data is a zlib stream (compress2, Z_DEFAULT_COMPRESSION; zlib 1.2.6 statically linked)
   a = 0x30 -> raw uncompressed rows (fallback, see below)
1D 28 4C 02 00 30 32     GS ( L fn=50: print buffered graphics
```
Algorithm (type 14, 1 bpp):
1. `buf = compress2(rows)` (bound = n + n>>12 + n>>14 + n>>25 + 13).
2. `FUN_180057bf0`: if the *compressed* bytes contain a real-time `DLE DC4` command pattern
   (`10 14 01 m t` with m<2, t 1..8 / `10 14 02 01 08` / `10 14 03 ...` / `10 14 08 01 03 14 01 06 02 08`),
   discard compression and send raw rows with a=0x30.
3. Raw path is sanitised by `FUN_180057ba0`: every `10 04|05|14` (DLE EOT/ENQ/DC4) and `1B 3D` (ESC =)
   inside the pixel data gets its first byte replaced by 0x30 (adds a stray dot but stops the printer
   from executing a real-time command mid-image).
Multitone (8 bpp gray, `ColorMode=GrayScale`): image split into 4 planes, one GS 8 L per plane with
c = '1'..'4', a = 0x35 (zlib) or 0x34 (raw), then one GS ( L fn 50.
Other command_Image types (0x10, 0x66, 0xCA, 2, 5) exist for other models (ESC * column mode etc.) - not needed.
Band height: `command_Image_height` = 256 rows, first band 620 rows. Width = 576 dots (80 mm) / 420 (58 mm).
Print-control helpers: `FUN_180057160` (type 3 on this model): `1D 28 4B 02 00 30 n` with n = 0x31..0x33
(command_Speed=3, command_Speed_max=51=0x33); `FUN_180057230`: same + optional `1B 55 n`; `FUN_1800575d0`: `1B 72 n`.

Vertical move: `ESC J n` repeated in chunks of 0xFF (`FUN_18004bc60`). Y unit = 406/inch (`command_Move_y_unit`).

Finish operations (PrinterSpec):
```
cut_0   1B 4A 00                       feed only, no cut
cut_1   1B 4A 00 1D 56 00              full cut
cut_16  1B 4A FF 1B 4A C1              feed to cutter, no cut
cut_17  1B 4A 00 1D 56 41 00           feed-and-cut (GS V A 0)
dk_1    1B 70 00 32 C8                 drawer pin 2 (also hard-coded in DoOperation)
dk_2    1B 70 01 32 C8                 drawer pin 5
dk_3    both
dk_256..258  ESC p with 0xFA/0x64 timings
dk_512..516  1B 28 41 03 00 61 0n 01   buzzer pattern n (ESC ( A fn 'a')
```
DoOperation also emits `1B 28 41 03 00 61 n m` (buzzer, n=1..7, m = count ≤ 255).

Paper (GPD, MasterUnits 73080/in): 80 mm roll = 230173 wide, printable 207872 from x=10962 -> 576 dots @203 dpi.
58 mm = printable 151200 from x=7200 -> 420 dots.

## 3. Utility protocol (TMUtility)

Uses documented ESC/POS "user setting mode" `GS ( E`, found as literal templates in
`CommunicationInterfaceSettingEngine.dll` / `IMGENGINE.dll`:
```
1D 28 45 03 00 01 49 4E        fn 1  "IN"   enter user setting mode
1D 28 45 02 00 06 a            fn 6  get customize value a  (a = 03..0C, 61..7F, BE..C2 used)
1D 28 45 nL nH 05 a vL vH ...  fn 5  set customize value
1D 28 45 03 00 0B 02 00        fn 11 serial/interface settings
1D 28 45 04 00 02 4F 55 54     fn 2  "OUT"  end (printer resets)
```
Status: `DLE EOT n` (n=1..4 observed in engines) / `GS r n` / `GS a n` (ASB).
Model info (`PrtInfMgrDLL.dll`): `GS I 01`, `GS I 42`, `GS I 43`, `GS I 44`, `GS I 45`
(maker / model name / serial / font info - standard GS I n=65..69 block replies `5F ... 00`).

### Customize-value IDs read by the backup engine (`IMGENGINE.dll` @0x269e0 literal table)
`GS ( E 02 00 06 a` for a = 03 04 05 06 08 09 0A 0B 0C 61 62 64-69 6F 70 74-7F BE-C2,
plus `fn 0E` (memory-switch read, a = '0'..'X'), `fn 0B`/`0C` (serial / USB interface read),
`fn 0F 01`, `fn 10 01`, and `GS ( F 02 00 61 02` (paper-layout read).

Meaning (Epson public ESC/POS reference, generic GS ( E fn 5 table):
| a | dec | setting | utility UI (Lang/0409.DAT) |
|---|---|---|---|
| 03 | 3 | Paper width | 13110 "Paper width - Number of columns" |
| 05 | 5 | Print density | 10050, values 1..7 (UI "Level 1..7") |
| 06 | 6 | Print speed | 10060, values 1..13 |
| 08 | 8 | Default code table | |
| 09 | 9 | Default international charset | |
| 0A | 10 | Interface selection | |
| 0B | 11 | Column emulation mode | 13110 (42-col variants) |
| 0C | 12 | Command execution while offline | |
| 61 | 97 | Thermal-head energizing divisions | 10980 "Power Supply Unit Capacity" (0..2) |
| 62 | 98 | Power supply output | |
| 64 | 100 | Auto-cut after cover close | 20020 "Method for canceling auto cutter error" (0/1) |
| 65-69 | 101-105 | Paper reduction: top margin, bottom margin, line spacing %, LF %, barcode height % | TMUPAPERSTG 5001/5002/5005-5007 |
| 6F/70 | 111/112 | Auto replacement of Font A / Font B | TMUFont |
| 74-7F, BE-C2 | 116-127, 190-194 | Model-specific (buzzer, ARP char height, etc.) | TMUBuzzer / TMUATCUTSTG |
Model-specific rows (116-195) need the TM-T20III page of the reference (behind Epson's terms banner; not read).
Note fn 5 range: k triples `a nL nH`, pL+pH*256 = 3k+1; changes take effect only after fn 2 "OUT"
(printer resets); a=1/2 changes wipe NV memory - never write those blindly.

Utility internals: per-setting C++ objects with string keys (`DensityLevel`=`Level1..7`, `Speed`=`Level1..14`,
`RollPaperWidth`, `ReceiveBufferSize`, `BusyCondition`, ...) in `TMUPrintingControl.dll`; those names are only
used for the save-to-file format. Bytes are produced by the host engine (`CEngineCommCtrl`, handed to plugins via
`FORMVIEWDLL_InitializeEngine`) through `CustomizeValueEngine.dll` (exports ModeIn/ModeOut/Set/Get/SetSP/GetSP/
IsSupport/GetCommand).

Logo: `ModelInfo` says `Logo.BitimageMode=1` and NV graphics = 384 KB. `TMULOGOSTG.dll` contains `FS p 01 00`
(print NV bit image) and key-code logo dialogs (GS ( L fn 67/69 key-code NV graphics). Upload bytes are built
at runtime; not yet traced.
Transport `TMComm.dll`: CreateFileW + DeviceIoControl on TMUSB (vendor class) or USBPRINT; distinguishes
"USB Printer Class" vs "USB Vender Class" (strings @0x30c18).
Model metadata: `ModelInfo_TM-T20III.json` (203 dpi, NV graphics 393216 B, 58/80 mm column counts).

## 4. macOS port plan

1. **CUPS raster filter** `rastertotmt20` (C, universal arm64/x86_64): read CUPS raster 1-bit,
   emit job setup -> GS 8 L / GS ( L bands (<=256 rows) -> cut/drawer/buzzer per PPD options.
2. **PPD**: PageSize 80x297/80x3276/58x297/58x3276 + custom; options CutMode, Drawer, Buzzer, Speed, Density.
3. **Transport**: stock CUPS `usb` backend works when the printer is in USB *Printer Class* mode
   (default for T20III; switchable in utility / memory switch). Vendor-class mode needs a libusb backend.
4. **Utility** (Swift/SwiftUI): IOUSBHost or libusb bulk OUT/IN; implement GS ( E fn 1/5/6/2, DLE EOT status,
   GS I info, logo upload (GS ( L / GS 8 L to NV), test print.

## 5. Target use case: Paperwork.show truss tapes
Source: https://paperwork.show/help/manual/projects_truss%20tapes/ (text + screenshots).
Windows setup they require, and what the Mac stack must reproduce:

| Their Windows setting | Where it lives | Mac equivalent | RE status |
|---|---|---|---|
| Feed&Cut preset "Document[Feed, Cut]": cut only at End of Document, feed 0.00 in, "feed to cut position" on, method Cut | Driver DEVMODE -> DoOperation structs (start-doc @+0x10, end-page, end-doc @+0x1a0/+0x268 in FUN_180036f00 case 5/6) | PPD `CutMode` = None / EndOfPage / **EndOfJob** (default), `FeedToCut` bool, `FeedAmount` | Commands known (cut_1/cut_17, ESC J). DEVMODE field layout **not traced** |
| Margins all 0 | GPD PrintableOrigin + driver `layout.conservation.top/bottom` | PPD ImageableArea = full 576 dots, no top/bottom trim | Partially (GPD values known; conservation logic not traced) |
| User Defined Paper "TRUSS TAPE" 3.18 x 12.00 in, margins 0 | APD utility (`APDUTL.exe`), `[UTL_USERDEFINEDPAPER]`: width 52.6-210.0 mm, height 25.4-3276.0 mm (0.1 mm units) | PPD custom page size (`CustomPageSize`, `cupsMaxSize 80x3276mm`) + preset `TrussTape 3.18x12in` | Limits known |
| Pages must butt together (22 x 12 in sheets = one tape) | end-of-page op = `ESC J 0` only | Filter: no feed/cut between pages, no top-of-page whitespace | Known |
| Utility Paper Reduction: all Disable / Not reduced | GS ( E fn 5 a=101..106 = 0 | Utility (or one-shot `tmsetup` CLI) writes 101-106 = 0 | Known |
| Printing Control: 80 mm/48 col, Speed Level 10, Density Level 4, Resizing None/Nearest/100 %, PSU Level 3 (High), Auto-cutter cancel = Command and Cover Close, Rx buffer 4 KB, Busy = Offline or buffer full, Auto LF off, USB power save on, Busy-cancel 256 B, paper-end sensors on, error signal on, pre-feed off, near-end off | GS ( E fn 5: a=3/11 (width/cols, UI value 0x00010006 = cols<<16 / width), 6 (speed), 5 (density), 70/71/73 (resize), 97 (PSU), 100 (cutter); the rest (31200-38600 group) are **memory switches** via GS ( E fn 3/4 | Utility "Truss-tape profile" button that writes all of these | IDs for 3,5,6,11,70,71,73,97,100-106 known; memory-switch bit positions **not mapped** |
| "Close the utility before printing or prints corrupt" | Utility polls status on the same USB pipe | Mac utility must not hold the USB interface while CUPS prints (open-on-demand, release after) | Design rule |
| Length runs ~1.5 % short (they scale 1.015 in the browser) | Mechanical feed tolerance; GPD Y unit 406/in vs 203 dpi raster | Filter option `LengthCalibration` (vertical resample factor, default 1.000) so the tape is right regardless of app | New feature, not in Epson driver |

## 6. Driver settings model (3rd pass)

### Private DEVMODE (`FUN_18005bf90` = defaults, `FUN_180008220` = name -> offset getter)
Size 0x274, signature 0x41424344 ('DCBA'), version 1. Settings are addressed by name:
| key | offset |
|---|---|
| cutter.DocBegin.{Mode,FeedExtra,FeedToStartPos,EjectCutSheet} | 0x1C 0x20 0x24 0x28 |
| cutter.DocEnd.{...} | 0x2C 0x30 0x34 0x38 |
| cutter.PageBegin.{...} | 0x3C 0x40 0x44 0x48 |
| cutter.PageEnd.{...} | 0x4C 0x50 0x54 0x58 |
| dk.{DocBegin,DocEnd,PageBegin,PageEnd}.{Mode,Cycle,CycleTime,Pattern} | 0x5C ... 0x98 |
| EjectCutSheetDirectionParam / (13-char key) | 0x160 / 0x164 |
Defaults: `cutter.DocEnd.Mode = 17` if `[DeviceSupportInformation] AutoCut` != "No", else 16. Everything else 0.
User-defined paper: `udp.Options` printer-data blob, 0xC0-byte records (UTF-16 name first); the record
whose name matches the selected form is copied into the DEVMODE. Pure page geometry - nothing printer-side.

### Feed & Cut semantics (traced through EnablePDEV `FUN_18002c3e0` -> op structs -> DoOperation `FUN_180015920`)
- 4 op structs per job: DocBegin, PageBegin, PageEnd, DocEnd. Each op: +0x38 = FeedExtra in dots
  (`FeedExtra * dpi / 25400`, so **FeedExtra is stored in µm**; UI shows inch/mm), +0x3C = cut Mode.
- Mode is the key into the PrinterSpec map `command_cut_<Mode>` (`FUN_1800574c0`, std::map lookup;
  Mode 16 is remapped to 48 for label/black-mark paper). For TM-T20III only 0/1/16/17 exist:
  | Mode | UI preset | bytes |
  |---|---|---|
  | 0 | No feed, No cut | `1B 4A 00` |
  | 1 | No feed, Cut | `1B 4A 00 1D 56 00` |
  | 16 | Feed, No cut | `1B 4A FF 1B 4A C1` (feed head->cutter distance) |
  | 17 | Feed, Cut | `1B 4A 00 1D 56 41 00` (GS V A 0 = feed to cutter + cut) |
- Emission order in DoOperation: layout cmd -> user command -> drawer (`ESC p`) -> buzzer (`ESC ( A`) ->
  `ESC J` FeedExtra (255-unit chunks) -> logo -> cut command.
- Paperwork.show "Document[Feed, Cut]" = DocEnd.Mode 17, PageEnd.Mode 0, FeedExtra 0 = driver default.

### Paper conservation (= the margin trimming) - IMPORTANT for truss tapes
- Keys `layout.conservation.top` (default **off**) and `layout.conservation.bottom` (default **ON** when absent);
  `inputbin.lock.reducedPaper.Type` 0x101/0x102/0x103 can force top/bottom/both.
- Flags go to the output queue (`FUN_18004b5f0`, bytes +0x0C top / +0x0D bottom). Vertical moves are queued
  as type-0 entries, raster as type-1. At end of page (`FUN_18005ef50`), if bottom conservation is set,
  **every queued type-0 (feed) entry after the last raster is deleted** -> trailing blank space of each page
  is not fed.
- Consequence: a multi-page tape whose pages end in whitespace comes out shorter by that whitespace per page.
  This is a plausible cause of Paperwork.show's ~1.5 % short tapes (inference - verify by printing a
  page with a ruler line at the very bottom with conservation on/off). Mac filter: **never trim**; always
  feed the full page height (`ESC J` for blank rows) unless the user opts in.
- `layout.margin.left/top` (+`.enable`) are also in µm (÷ 25400 × dpi). Top margin stored at PDEV+0x310.

### Utility setting IDs decoded (TMUPrintingControl UI ids -> printer)
Rule (verified against Epson's public MSW1 table and the GS ( E fn 5 list):
- `1aaa0` -> customize value a = aaa: 10050 a=5 density, 10060 a=6 speed, 10220 a=22 "display when interface
  activating" (power LED), 10700/10710/10730 a=70/71/73 graphics resize method/algorithm/ratio, 10980 a=98 power supply.
- `13110` -> a=3 (paper width, low word: 6=80 mm, 2=58 mm) + a=11 (column emulation, high word: 1=42 col).
- `3MB00` -> memory switch Msw M bit B, UI value = bit value:
  31200 Msw1-2 rx buffer (0=4 KB/large, 1=45 B), 31300 Msw1-3 BUSY (0=offline or full, 1=full),
  31400 Msw1-4 rx error (0='?', 1=ignore), 31500 Msw1-5 auto LF, 35100 Msw5-1 USB power save,
  35200 Msw5-2 BUSY release (0=256 B, 1=138 B), 35300 Msw5-3 paper-end signal sensors, 35400 Msw5-4 error signal,
  35500 Msw5-5 pre-feed, 36100 Msw6-1 near-end sensor, 38600 Msw8-6 error signal (Msw5/6/8 are model-specific;
  mapping inferred from the id rule, confirm by read-back).
- `2xxx0` (20010 multi-language font, 20020 auto-cutter-error cancel) -> model-specific, likely customize 116-195; unconfirmed.

Wire formats (Epson public reference):
- Memory switch write `GS ( E pL pH 03 [a b8..b1]...` with b = '0' off / '1' on / '2' unchanged, len = 9k+1.
  Read `GS ( E 02 00 04 a` -> `37 21 <8 chars '0'/'1', bit8..bit1> 00`.
- Customize read `GS ( E 02 00 06 a` -> `37 27 <a as ASCII decimal> 1F <value as ASCII decimal> 00`.
- All writes only inside user-setting mode (fn 1 "IN" ... fn 2 "OUT"; OUT resets the printer).

### Paperwork.show profile as bytes (for a one-click utility preset)
```
1D 28 45 03 00 01 49 4E                         ; IN
1D 28 45 2E 00 05  03 06 00  0B 00 00  05 04 00  06 0A 00  46 00 00  47 01 00  49 64 00  62 02 00
                   65 00 00  66 00 00  67 00 00  68 00 00  69 00 00  6A 00 00  64 01 00
   ; width 80mm, 48 col, density 4, speed 10, resize none/nearest/100%, PSU level3(=2),
   ; paper reduction 101-106 off, auto-cut cancel = command+cover close
1D 28 45 0A 00 03 01 32 32 32 30 30 30 30 32     ; Msw1: bits5..2 = 0 (auto LF off, '?', busy=offline|full, 4KB)
1D 28 45 04 00 02 4F 55 54                      ; OUT (printer resets)
```
(15 triples -> pL = 3*15+1 = 0x2E; Msw5/6/8 bits from their screenshot left for read-back confirmation before writing.)

## 7. macOS driver (built from these notes)
`macos/` - CUPS raster filter `rastertotmt20` + PPD + tests. Encoder (`macos/src/tmt20.c`) implements
sections 2 and 6: job setup string, GS 8 L / GS ( L bands with zlib + DC4 check + raw sanitising,
cut modes 0/1/16/17, drawer, buzzer, no trimming by default, length calibration. Tested on Linux
(`make test`); CUPS glue not yet compiled on macOS. See `macos/README.md`.

`macos/utility/` - TMUtility.app (TM-T20III Utility + Printing Preferences + APD Utility windows),
`tmutil` / `tmqueue` CLIs, IOUSBHost USB transport. Section 8 lists the Windows screens it copies.
Implements section 6's GS ( E fn1-6, DLE EOT and GS I; catalog of 26 settings; truss preset.
Core/CLI tested on Linux against a simulator; Swift + USB parts not yet compiled.

## 8. Windows UI inventory (4th pass) -> macOS windows

Where the screens come from:
- **Driver property sheet** (`EA6UIPTM.dll`, RT_DIALOG; English text in the 1033 dialogs and string table):
  - 1000 Feed and Cut: Preset option list (strings 112-119), Illustration, Detail option tabs
    (strings 96-99: Start of Document / Start of Page / End of page / End of Document), Eject direction.
  - 1100 per-timing detail:
    - "Feed to cut position before cutting" = Mode bit 16
    - "Cutting method" = Mode bit 0 (strings 120-123)
    - "Feeding amount" + mm/inch
    - "Feed paper to the print starting position" = FeedToStartPos; the TM-T20III spec has no
      command for it, so it prints nothing
    - "Eject single-cut sheets"
  - 3000 Peripherals: device combo (strings 160-164), timing tabs, "Current settings" list
    (Timing | Settings, strings 176-188).
  - 3200 Cash Drawer #1 (2 Pins) / #2 (5 Pins). 3300 Built-in Buzzer (pattern strings 197-207,
    count, cycle). 3402 External Buzzer (Pattern A-E, count).
  - 2000 Language, 4000 Version, 6101 Advanced Settings (launches the APD Utility).
- **Layout and Paper/Quality tabs** are Unidrv's own, driven by the GPD:
  - Paper Source = `InputBin` None/Top/Bottom/Both (EA6RESTM 2001-2004). This is paper
    conservation; the GPD default is Bottom.
  - ColorMode (Monochrome/Grayscale) and Halftone (Dither 8x8).
  - PRINTER_PROPERTY features: Roll paper width (4001-4003) and Reduced size print
    (`layout_ScalingMode` 100%/70%).
  - DOCUMENT_PROPERTY: Upside-down (8001-8003).
- **APD Utility** (`DriverPack/Utility/APDUTL.exe` + `locale/en/*Plugin.xml`; limits in
  `Setup/TM-T20III.ini`):
  - Layout (`[UTL_LAYOUT]`):
    - Adjust Print Position: top/left margin 0-110 in 0.1 mm, i.e. 0-11 mm
      (`layout.margin.*`, µm in the DEVMODE)
    - Reduced Size Print: Do not reduce / Reduce from 80 mm to 58 mm
    - Unsupported Paper: Automatically adjust and print, default on
  - User Defined Paper (`[UTL_USERDEFINEDPAPER]`):
    - name of up to 31 characters
    - width 52.6-210.0 mm, height 25.4-3276.0 mm, margins left/top/right/bottom, unit mm or inch
    - paper type: roll / BM / label
    - stored in the `udp.Options` printer data
  - Send Command: Start of Document / End of Document / Start of Page / End of Page byte strings
    (`command.startdoc` ...; emitted first in DoOperation).
  - The menu also lists Barcode / 2D Symbol / Font Replace, which are device-font features and
    irrelevant for a raster driver. The TM-T20III has no logo-setting entry (`UTL_LOGOSETTING` is
    empty).
- **TM-T20III Utility** (`TMUtility.exe`, tree in `MENUITEM.DAT`, text in `Lang/0409.DAT`):
  - Main > Current Settings (TMUPRNSTG)
  - Basic Settings: Storing Logos (TMULOGOSTG), Paper Reduction (TMUPAPERSTG), Automatic Paper Cut
    (TMUATCUTSTG), Printing Control (TMUPrintingControl list with Function | Setting columns)
  - Detailed Settings: Font (TMUFont: code page + international charset combos), Buzzer
    (TMUBuzzer: Optional Buzzer events error / paper end / auto cut / pulse 1 / pulse 2 / power on,
    Off/Once/Continuous + pattern), Communication I/F (TMUIFSTG: USB Printer/Vender Class)
  - Network tree (TMN*)
  - Operation Check, Backup/Restore
  - `ModelInfo_TM-T20III.json`: OptionalBuzzer events supported, NV graphics 384 KB, column counts.

PrinterSpec peripheral table (`[DRV_PRINTER_SPEC]`, `command_dk_<mode>`):
```
1   1B 70 30 32 C8            drawer #1 (pin 2)
2   1B 70 31 32 C8            drawer #2 (pin 5)
3   both
256 1B 70 31 FA 64            built-in buzzer (long pulse on pin 5)
257 1B 70 31 FA 64 1B 70 30 0A 64     buzzer + drawer #1
258 1B 70 30 FA 64 1B 70 31 0A 64     buzzer + drawer #2
512-516 1B 28 41 03 00 61 0n 01        external buzzer pattern A-E
```
`dk_timing=SSD`, `dk_support=I0` (meaning not traced). Feed and Cut presets map to cut modes per
timing (DocBegin, PageBegin, PageEnd, DocEnd):
- Document[Feed, Cut] = 0,0,0,17
- Document[Feed, No cut] = ..16; Document[No feed, Cut] = ..1; Document[No feed, No cut] = all 0
- Page[...] put 17/16/1 at PageEnd

macOS mapping: `macos/README.md` "Settings" (PPD options + `*TmSetting` keys) and
`macos/utility/README.md`.

## Open questions
- Model-specific customize IDs 116-127 / 190-194 (buzzer, etc.) and utility ids 20010/20020 - read TM-T20III
  page of Epson reference, or diff `GS ( E fn 4/6` dumps on the real printer while toggling settings in the Windows utility.
- Confirm Msw5/6/8 bit mapping by read-back (fn 4) before the Mac utility writes them.
- Confirm the conservation-bottom theory for short truss tapes with a physical test print.
- Exact logo upload sequence (GS ( L fn 67 vs FS q) - trace TMULOGOSTG / EPSLOGOEX if logos matter.
- Not traced, shown read-only in the Mac utility:
  - Automatic Paper Cut / top-bottom logo (TMUATCUTSTG)
  - Optional Buzzer events (TMUBuzzer)
  - USB class switch (TMUIFSTG, `GS ( E` fn 11/12?)
  - Multi-language font (20010)
  - 128 KB receive buffer
- Font page customize ids 8/9 (code page / intl charset) are from Epson's generic table, not confirmed.
- APD "Unsupported Paper" exact rule (Mac: shrink pages more than 5 mm wider than the roll).
- Verify everything against a real capture: print to FILE: port on Windows (or USBPcap) and diff with the Mac filter output.

## Resolved (2nd pass)
- Raster compression: zlib, a=0x31; fallback raw a=0x30 + DLE/ESC= sanitising (see section 2).
- Multitone: 4 planes, a=0x35/0x34, c='1'..'4'.
