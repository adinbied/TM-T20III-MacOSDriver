import AppKit
import SwiftUI

// Printing Preferences + APD Utility, laid out like Epson's Windows
// "EPSON TM-T20III Receipt6 Printing Preferences" property sheet (EA6UIPTM.dll
// dialogs 1000/1100/3000/3200/3300/3402/4000/6101 + Unidrv Layout and
// Paper/Quality tabs) and the APD 6 Utility pages (Layout, User Defined Paper,
// Send Command).  Labels are the English strings from those resources.

// MARK: - shared tables

enum Timing: Int, CaseIterable, Identifiable {
    case docBegin, pageBegin, pageEnd, docEnd
    var id: Int { rawValue }
    var key: String { ["docbegin", "pagebegin", "pageend", "docend"][rawValue] }
    /// EA6UIPTM string table 96-99.
    var title: String { ["Start of Document", "Start of Page", "End of page", "End of Document"][rawValue] }
}

struct FeedCutPreset: Identifiable, Hashable {
    let key: String, title: String
    let modes: [Int]              // docbegin, pagebegin, pageend, docend
    var id: String { key }

    /// EA6UIPTM strings 112-119; modes = PrinterSpec command_cut_<n>.
    static let all: [FeedCutPreset] = [
        .init(key: "DocFeedCut", title: "Document[Feed, Cut]", modes: [0, 0, 0, 17]),
        .init(key: "DocFeedNoCut", title: "Document[Feed, No cut]", modes: [0, 0, 0, 16]),
        .init(key: "DocNoFeedCut", title: "Document[No feed, Cut]", modes: [0, 0, 0, 1]),
        .init(key: "DocNoFeedNoCut", title: "Document[No feed, No cut]", modes: [0, 0, 0, 0]),
        .init(key: "PageFeedCut", title: "Page[Feed, Cut]", modes: [0, 0, 17, 0]),
        .init(key: "PageFeedNoCut", title: "Page[Feed, No cut]", modes: [0, 0, 16, 0]),
        .init(key: "PageNoFeedCut", title: "Page[No feed, Cut]", modes: [0, 0, 1, 0]),
        .init(key: "Detail", title: "Custom", modes: []),
    ]
}

/// Peripherals "device" combo (EA6UIPTM strings 160-164).
enum PeripheralDevice: Int, CaseIterable, Identifiable {
    case nothing, drawer, drawerBuzzer, buzzer, external
    var id: Int { rawValue }
    var title: String {
        ["Nothing", "Cash Drawer", "Cash Drawer & Built-in Buzzer", "Built-in Buzzer", "External Buzzer"][rawValue]
    }
    var hasDrawer: Bool { self == .drawer || self == .drawerBuzzer }
    var hasBuzzer: Bool { self == .drawerBuzzer || self == .buzzer || self == .external }

    static func from(modes: [Int]) -> PeripheralDevice? {
        if modes.contains(where: { $0 >= 512 }) { return .external }
        if modes.contains(where: { (257...258).contains($0) }) { return .drawerBuzzer }
        if modes.contains(256) { return modes.contains(where: { (1...3).contains($0) }) ? .drawerBuzzer : .buzzer }
        if modes.contains(where: { (1...3).contains($0) }) { return .drawer }
        return nil
    }
}

/// "Current settings" text for a dk mode (EA6UIPTM strings 176-188).
func peripheralLabel(_ mode: Int) -> String {
    switch mode {
    case 0: return "None"
    case 1: return "Cash Drawer #1"
    case 2: return "Cash Drawer #2"
    case 3: return "Cash Drawer #1 & Cash Drawer #2"
    case 256: return "Built-in Buzzer"
    case 257: return "Built-in Buzzer & Cash Drawer #1"
    case 258: return "Built-in Buzzer & Cash Drawer #2"
    case 512...516: return "External Buzzer(Pattern \(["A", "B", "C", "D", "E"][mode - 512]))"
    default: return "Bad setting"
    }
}

/// Paper sizes of the PPD (EA6RESTM strings 1001-1007) + TRUSS TAPE.
let basePaperSizes: [(String, String)] = [
    ("Letter", "LT 8.5 x 11 in"), ("A4", "A4 210 x 297 mm"), ("Postcard", "Postcard 100 x 148 mm"),
    ("RP58x297", "Roll Paper 58 x 297 mm"), ("RP58x3276", "Roll Paper 58 x 3276 mm"),
    ("RP80x297", "Roll Paper 80 x 297 mm"), ("RP80x3276", "Roll Paper 80 x 3276 mm"),
    ("TrussTape", "TRUSS TAPE 3.18 x 12 in"),
]

private func closeKeyWindow() { NSApp.keyWindow?.close() }

struct QueuePicker: View {
    @EnvironmentObject var q: QueueModel

    var body: some View {
        HStack {
            Picker("Printer:", selection: $q.queue) {
                if q.queues.isEmpty { Text("No TM-T20III queue found").tag("") }
                ForEach(q.queues, id: \.self) { Text($0).tag($0) }
            }
            .frame(maxWidth: 360)
            Button("Refresh") { q.refreshQueues() }
            Spacer()
            if q.busy { ProgressView().controlSize(.small) }
        }
    }
}

struct QueueStatusLine: View {
    @EnvironmentObject var q: QueueModel

    var body: some View {
        if let e = q.lastError {
            Label(e, systemImage: "exclamationmark.triangle").foregroundStyle(.red).lineLimit(2)
        } else if q.dirty {
            Text("Settings have been changed.").foregroundStyle(.secondary)
        } else if let m = q.message {
            Text(m).foregroundStyle(.secondary)
        }
    }
}

// MARK: - Printing Preferences window

struct PrintingPreferencesView: View {
    @EnvironmentObject var q: QueueModel
    @State private var tab = 2                 // Windows opens on Feed and Cut

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            QueuePicker()
            TabView(selection: $tab) {
                LayoutTab().tabItem { Text("Layout") }.tag(0)
                PaperQualityTab().tabItem { Text("Paper/Quality") }.tag(1)
                FeedCutTab().tabItem { Text("Feed and Cut") }.tag(2)
                PeripheralsTab().tabItem { Text("Peripherals") }.tag(3)
                VersionTab().tabItem { Text("Version") }.tag(4)
                AdvancedSettingsTab().tabItem { Text("Advanced Settings") }.tag(5)
            }
            HStack {
                QueueStatusLine()
                Spacer()
                Button("OK") { q.apply { closeKeyWindow() } }
                    .keyboardShortcut(.defaultAction)
                Button("Cancel") { q.revert(); closeKeyWindow() }
                    .keyboardShortcut(.cancelAction)
                Button("Apply") { q.apply() }.disabled(!q.dirty || q.busy)
            }
        }
        .padding(12)
        .frame(minWidth: 640, minHeight: 640)
        .disabled(q.queue.isEmpty && !q.queues.isEmpty)
        .onAppear { if q.queues.isEmpty { q.refreshQueues() } }
    }
}

// MARK: Layout (Unidrv)

struct LayoutTab: View {
    @EnvironmentObject var q: QueueModel
    @State private var advanced = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            GroupBox("Orientation") {
                Picker("", selection: $q.orientation) {
                    Label("Portrait", systemImage: "doc").tag("portrait")
                    Label("Landscape", systemImage: "doc.richtext").tag("landscape")
                }
                .pickerStyle(.radioGroup).labelsHidden()
                .onChange(of: q.orientation) { _ in q.dirty = true }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            GroupBox("Page Order") {
                Picker("", selection: $q.pageOrder) {
                    Text("Front to Back").tag("normal")
                    Text("Back to Front").tag("reverse")
                }
                .pickerStyle(.radioGroup).labelsHidden()
                .onChange(of: q.pageOrder) { _ in q.dirty = true }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            GroupBox("Page Format") {
                Picker("Pages per Sheet", selection: $q.numberUp) {
                    ForEach([1, 2, 4, 6, 9, 16], id: \.self) { Text("\($0)").tag($0) }
                }
                .frame(width: 220)
                .onChange(of: q.numberUp) { _ in q.dirty = true }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            Spacer()
            HStack { Spacer(); Button("Advanced...") { advanced = true } }
        }
        .padding()
        .sheet(isPresented: $advanced) { AdvancedOptionsSheet() }
    }
}

/// Unidrv "Advanced Options" tree.
struct AdvancedOptionsSheet: View {
    @EnvironmentObject var q: QueueModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading) {
            Text("EPSON TM-T20III Advanced Document Settings").font(.headline)
            Form {
                Section("Paper/Output") {
                    PPDPicker(key: "PageSize", title: "Paper Size",
                              choices: basePaperSizes + q.papers.map { ($0.keyword, $0.name) })
                }
                Section("Graphic") {
                    PPDPicker(key: "Resolution", title: "Print Quality", choices: [("203dpi", "203 x 203 dots per inch")])
                    PPDPicker(key: "TmHalftone", title: "Halftoning", choices: [("Dither8x8", "Dither 8x8")])
                }
                Section("Printer Features") {
                    PPDPicker(key: "TmPaperWidth", title: "Roll paper width", choices: [("80mm", "80 mm"), ("58mm", "58 mm")])
                    PPDPicker(key: "TmScaling", title: "Reduced size print", choices: [("100", "100%"), ("70", "70%")])
                    PPDPicker(key: "TmUpsideDown", title: "Upside-down", choices: [("False", "Disabled"), ("True", "Enabled")])
                }
                Section("macOS additions") {
                    PPDPicker(key: "TmLengthScale", title: "Length calibration", choices: [
                        ("0.990", "-1.0 %"), ("0.995", "-0.5 %"), ("1.000", "None"), ("1.005", "+0.5 %"),
                        ("1.010", "+1.0 %"), ("1.015", "+1.5 %"), ("1.020", "+2.0 %")])
                    PPDPicker(key: "TmBlankFeed", title: "Blank space output",
                              choices: [("Feed", "Paper feed (like Epson driver)"), ("Raster", "Blank raster (exact, slower)")])
                    PPDPicker(key: "TmCompress", title: "Compress graphics", choices: [("True", "Yes"), ("False", "No")])
                    PPDPicker(key: "TmPrintControl", title: "Print control mode",
                              choices: [("Default", "Printer setting"), ("Mode1", "Mode 1"), ("Mode2", "Mode 2"), ("Mode3", "Mode 3")])
                }
            }
            .formStyle(.grouped)
            HStack { Spacer(); Button("OK") { dismiss() }.keyboardShortcut(.defaultAction) }
        }
        .padding()
        .frame(width: 520, height: 560)
    }
}

struct PPDPicker: View {
    @EnvironmentObject var q: QueueModel
    let key: String, title: String
    let choices: [(String, String)]

    var body: some View {
        Picker(title, selection: Binding(get: { q.get(key) }, set: { _ = q.set(key, $0) })) {
            ForEach(choices.indices, id: \.self) { i in Text(choices[i].1).tag(choices[i].0) }
        }
    }
}

// MARK: Paper/Quality (Unidrv)

struct PaperQualityTab: View {
    @EnvironmentObject var q: QueueModel
    @State private var advanced = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            GroupBox("Tray Selection") {
                VStack(alignment: .leading, spacing: 6) {
                    PPDPicker(key: "TmPaperSource", title: "Paper Source:",
                              choices: [("None", "None"), ("Top", "Top"), ("Bottom", "Bottom"), ("Both", "Top & Bottom")])
                        .frame(width: 320)
                    Text("Paper Source is Epson's paper conservation: blank space at the top/bottom of each "
                         + "page is not fed. Windows defaults to Bottom; this driver defaults to None so "
                         + "multi-page tapes and labels keep their exact length.")
                        .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            GroupBox("Color") {
                Picker("", selection: Binding(get: { q.get("ColorModel") }, set: { _ = q.set("ColorModel", $0) })) {
                    Text("Monochrome").tag("Mono")
                    Text("Grayscale").tag("Gray")
                }
                .pickerStyle(.radioGroup).labelsHidden()
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            Spacer()
            HStack { Spacer(); Button("Advanced...") { advanced = true } }
        }
        .padding()
        .sheet(isPresented: $advanced) { AdvancedOptionsSheet() }
    }
}

// MARK: Feed and Cut (dialogs 1000 + 1100)

struct FeedCutTab: View {
    @EnvironmentObject var q: QueueModel
    @State private var detail: Timing = .docEnd

    private var presetKey: String { q.get("TmFeedCut") }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .top, spacing: 16) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Preset option:")
                    List(FeedCutPreset.all, selection: Binding<String?>(
                        get: { presetKey }, set: { if let k = $0 { select(k) } })) { p in
                        Text(p.title).tag(p.key)
                    }
                    .listStyle(.bordered(alternatesRowBackgrounds: false))
                    .frame(width: 280, height: 170)
                }
                VStack(alignment: .leading, spacing: 4) {
                    Text("Illustration:")
                    FeedCutIllustration(modes: Timing.allCases.map { q.int("tm-cut-\($0.key)") })
                        .frame(width: 220, height: 170)
                }
            }
            Text("Detail option:")
            TabView(selection: $detail) {
                ForEach(Timing.allCases) { t in
                    CutDetailView(timing: t).tabItem { Text(t.title) }.tag(t)
                }
            }
            .frame(height: 230)
            GroupBox("Eject direction") {
                Picker("", selection: .constant(0)) { Text("Feed direction").tag(0) }
                    .labelsHidden().frame(width: 220).disabled(true)
                    .frame(maxWidth: .infinity, alignment: .trailing)
            }
        }
        .padding()
    }

    private func select(_ key: String) {
        q.set("TmFeedCut", key)
        guard let p = FeedCutPreset.all.first(where: { $0.key == key }), !p.modes.isEmpty else { return }
        for t in Timing.allCases {
            q.set("tm-cut-\(t.key)", p.modes[t.rawValue])
            q.set("tm-feed-\(t.key)", 0)
        }
    }
}

struct FeedCutIllustration: View {
    let modes: [Int]

    var body: some View {
        let pageCut = modes[2] & 1 != 0, docCut = modes[3] & 1 != 0
        let pageFeed = modes[2] & 16 != 0, docFeed = modes[3] & 16 != 0
        VStack(spacing: 4) {
            ForEach(0..<2) { i in
                RoundedRectangle(cornerRadius: 2).stroke(.secondary)
                    .overlay(Text("Page \(i + 1)").font(.caption2).foregroundStyle(.secondary))
                    .frame(width: 90, height: 44)
                if i == 0 { marker(cut: pageCut, feed: pageFeed) }
            }
            marker(cut: docCut, feed: docFeed)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(RoundedRectangle(cornerRadius: 4).fill(Color(nsColor: .textBackgroundColor)))
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(.separator))
    }

    @ViewBuilder private func marker(cut: Bool, feed: Bool) -> some View {
        HStack(spacing: 4) {
            if feed { Image(systemName: "arrow.down.to.line") }
            if cut {
                Image(systemName: "scissors")
                Rectangle().frame(width: 70, height: 1)
            } else {
                Rectangle().frame(width: 70, height: 1).opacity(0.15)
            }
        }
        .foregroundColor(cut ? Color.red : Color.secondary)
        .frame(height: 12)
    }
}

/// Dialog 1100 for one timing.
struct CutDetailView: View {
    @EnvironmentObject var q: QueueModel
    let timing: Timing

    private var mode: Int { q.int("tm-cut-\(timing.key)") }
    private var inch: Bool { q.get("tm-feed-unit") == "inch" }

    private func setMode(_ m: Int) {
        q.set("tm-cut-\(timing.key)", m)
        q.set("TmFeedCut", "Detail")
    }

    var body: some View {
        GroupBox("Paper Cut") {
            VStack(alignment: .leading, spacing: 8) {
                Toggle("Feed to cut position before cutting", isOn: Binding(
                    get: { mode & 16 != 0 }, set: { setMode(($0 ? 16 : 0) | (mode & 1)) }))
                HStack {
                    Text("Cutting method:")
                    Picker("", selection: Binding(get: { mode & 1 }, set: { setMode((mode & 16) | $0) })) {
                        Text("No cut").tag(0)
                        Text("Cut").tag(1)
                    }
                    .labelsHidden().frame(width: 200)
                }
                HStack {
                    Text("Feeding amount:")
                    TextField("", value: Binding(
                        get: { Double(q.int("tm-feed-\(timing.key)")) / (inch ? 25400 : 1000) },
                        set: { v in
                            let um = Int((max(0, v) * (inch ? 25400 : 1000)).rounded())
                            q.set("tm-feed-\(timing.key)", min(um, 1_000_000))
                            q.set("TmFeedCut", "Detail")
                        }), format: .number.precision(.fractionLength(inch ? 2 : 1)))
                        .frame(width: 70)
                    Stepper("", value: Binding(
                        get: { q.int("tm-feed-\(timing.key)") },
                        set: { q.set("tm-feed-\(timing.key)", min(max($0, 0), 1_000_000)); q.set("TmFeedCut", "Detail") }),
                        step: inch ? 2540 : 1000).labelsHidden()
                    Picker("", selection: Binding(get: { q.get("tm-feed-unit") }, set: { _ = q.set("tm-feed-unit", $0) })) {
                        Text("mm").tag("mm")
                        Text("inch").tag("inch")
                    }
                    .labelsHidden().frame(width: 80)
                }
                Toggle("Feed paper to the print starting position", isOn: Binding(
                    get: { q.int("tm-startpos-\(timing.key)") != 0 },
                    set: { q.set("tm-startpos-\(timing.key)", $0 ? 1 : 0) }))
                Toggle("Eject single-cut sheets", isOn: .constant(false)).disabled(true)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(6)
        }
        .padding(8)
    }
}

// MARK: Peripherals (dialogs 3000 / 3200 / 3300 / 3402)

struct PeripheralsTab: View {
    @EnvironmentObject var q: QueueModel
    @EnvironmentObject var printer: PrinterModel
    @State private var device: PeripheralDevice = .nothing
    @State private var pendingDevice: PeripheralDevice?
    @State private var timing: Timing = .docEnd

    private var modes: [Int] { Timing.allCases.map { q.int("tm-dk-\($0.key)") } }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            GroupBox("Peripherals") {
                VStack(alignment: .leading) {
                    Picker("", selection: Binding(get: { device }, set: { new in
                        if new != device {
                            if modes.contains(where: { $0 != 0 }) { pendingDevice = new } else { device = new }
                        }
                    })) {
                        ForEach(PeripheralDevice.allCases) { Text($0.title).tag($0) }
                    }
                    .labelsHidden().frame(width: 360)
                    TabView(selection: $timing) {
                        ForEach(Timing.allCases) { t in
                            HStack(alignment: .top, spacing: 12) {
                                if device.hasDrawer { DrawerPane(timing: t, device: device) }
                                if device == .external { ExternalBuzzerPane(timing: t) } else if device.hasBuzzer {
                                    BuiltInBuzzerPane(timing: t, device: device)
                                }
                                if device == .nothing {
                                    Text("No peripheral is connected.").foregroundStyle(.secondary)
                                }
                                Spacer(minLength: 0)
                            }
                            .padding(8)
                            .tabItem { Text(t.title) }.tag(t)
                        }
                    }
                    .frame(height: 280)
                }
            }
            Text("Current settings:")
            Table(Timing.allCases) {
                TableColumn("Timing") { Text($0.title) }
                TableColumn("Settings") { t in
                    let m = q.int("tm-dk-\(t.key)")
                    Text(m >= 512 ? "\(peripheralLabel(m)) x\(q.int("tm-dk-count-\(t.key)"))" : peripheralLabel(m))
                }
            }
            .frame(height: 120)
        }
        .padding()
        .onAppear { device = PeripheralDevice.from(modes: modes) ?? device }
        .onChange(of: q.queue) { _ in device = PeripheralDevice.from(modes: modes) ?? .nothing }
        .alert("Part of the current settings will be reset. Do you want to proceed?",
               isPresented: Binding(get: { pendingDevice != nil }, set: { if !$0 { pendingDevice = nil } })) {
            Button("Yes") {
                for t in Timing.allCases { q.set("tm-dk-\(t.key)", 0) }
                device = pendingDevice ?? device
                pendingDevice = nil
            }
            Button("No", role: .cancel) { pendingDevice = nil }
        }
    }
}

/// Decomposed dk mode for the drawer/buzzer panes.
private struct DK {
    var drawer1 = false, drawer2 = false, buzzer = false

    init(_ m: Int) {
        switch m {
        case 1: drawer1 = true
        case 2: drawer2 = true
        case 3: drawer1 = true; drawer2 = true
        case 256: buzzer = true
        case 257: buzzer = true; drawer1 = true
        case 258: buzzer = true; drawer2 = true
        default: break
        }
    }

    var mode: Int {
        if buzzer { return drawer1 ? 257 : drawer2 ? 258 : 256 }
        return (drawer1 ? 1 : 0) | (drawer2 ? 2 : 0)
    }
}

struct DrawerPane: View {
    @EnvironmentObject var q: QueueModel
    @EnvironmentObject var printer: PrinterModel
    let timing: Timing
    let device: PeripheralDevice

    private var dk: DK { DK(q.int("tm-dk-\(timing.key)")) }

    private func update(_ f: (inout DK) -> Void) {
        var d = dk
        f(&d)
        q.set("tm-dk-\(timing.key)", d.mode)
    }

    var body: some View {
        GroupBox("Cash Drawer") {
            VStack(alignment: .leading, spacing: 8) {
                GroupBox("Cash Drawer #1 (2 Pins)") {
                    Picker("", selection: Binding(get: { dk.drawer1 }, set: { v in
                        update { $0.drawer1 = v; if v && $0.buzzer { $0.drawer2 = false } } })) {
                        Text("Do not open").tag(false)
                        Text("Open").tag(true)
                    }
                    .pickerStyle(.radioGroup).labelsHidden()
                }
                GroupBox("Cash Drawer #2 (5 Pins)") {
                    Picker("", selection: Binding(get: { dk.drawer2 }, set: { v in
                        update { $0.drawer2 = v; if v && $0.buzzer { $0.drawer1 = false } } })) {
                        Text("Do not open").tag(false)
                        Text("Open").tag(true)
                    }
                    .pickerStyle(.radioGroup).labelsHidden()
                }
                Button("Operation Check") {
                    if dk.drawer1 { printer.drawer(2) }
                    if dk.drawer2 { printer.drawer(5) }
                }
                .disabled(!dk.drawer1 && !dk.drawer2)
            }
            .frame(width: 200, alignment: .leading)
        }
    }
}

struct BuiltInBuzzerPane: View {
    @EnvironmentObject var q: QueueModel
    @EnvironmentObject var printer: PrinterModel
    let timing: Timing
    let device: PeripheralDevice

    private var dk: DK { DK(q.int("tm-dk-\(timing.key)")) }

    var body: some View {
        GroupBox("Built-in Buzzer") {
            VStack(alignment: .leading, spacing: 8) {
                Picker("", selection: Binding(get: { dk.buzzer }, set: { v in
                    var d = dk
                    d.buzzer = v
                    if v && d.drawer1 && d.drawer2 { d.drawer2 = false }    // one drawer with the buzzer
                    if device == .buzzer { d.drawer1 = false; d.drawer2 = false }
                    q.set("tm-dk-\(timing.key)", d.mode)
                })) {
                    Text("Turn off").tag(false)
                    Text("Turn on").tag(true)
                }
                .pickerStyle(.radioGroup).labelsHidden()
                // The TM-T20III's built-in buzzer is a fixed pulse (PrinterSpec
                // command_dk_256..258); pattern, count and cycle do not apply.
                LabeledContent("Pattern:") { Text("1320Hz:1000ms").foregroundStyle(.secondary) }
                LabeledContent("Count:") { Text("1").foregroundStyle(.secondary) }
                LabeledContent("Cycle time (x100 ms):") { Text("—").foregroundStyle(.secondary) }
                Button("Operation Check") { printer.drawer(5) }.disabled(!dk.buzzer)
            }
            .frame(width: 230, alignment: .leading)
        }
    }
}

struct ExternalBuzzerPane: View {
    @EnvironmentObject var q: QueueModel
    @EnvironmentObject var printer: PrinterModel
    let timing: Timing

    private var mode: Int { q.int("tm-dk-\(timing.key)") }

    var body: some View {
        GroupBox("External Buzzer") {
            VStack(alignment: .leading, spacing: 8) {
                Picker("", selection: Binding(get: { mode >= 512 }, set: {
                    q.set("tm-dk-\(timing.key)", $0 ? 512 : 0) })) {
                    Text("Turn off").tag(false)
                    Text("Turn on").tag(true)
                }
                .pickerStyle(.radioGroup).labelsHidden()
                Picker("Pattern:", selection: Binding(get: { mode >= 512 ? mode : 512 },
                                                      set: { q.set("tm-dk-\(timing.key)", $0) })) {
                    ForEach(0..<5) { i in Text("Pattern \(["A", "B", "C", "D", "E"][i])").tag(512 + i) }
                }
                .disabled(mode < 512)
                Stepper(value: Binding(get: { q.int("tm-dk-count-\(timing.key)") },
                                       set: { q.set("tm-dk-count-\(timing.key)", min(max($0, 1), 255)) }),
                        in: 1...255) {
                    Text("Count: \(q.int("tm-dk-count-\(timing.key)"))")
                }
                .disabled(mode < 512)
                Button("Operation Check") { printer.buzz(Int32(mode - 511)) }.disabled(mode < 512)
            }
            .frame(width: 230, alignment: .leading)
        }
    }
}

// MARK: Version (dialog 4000) / Advanced Settings (dialog 6101)

struct VersionTab: View {
    var body: some View {
        VStack(spacing: 10) {
            Image(systemName: "printer").font(.system(size: 40))
            Text("TM-T20III driver for macOS (rastertotmt20)").font(.headline)
            Text("Rebuilt from the behaviour of EPSON Advanced Printer Driver 6")
            Text("Not affiliated with or supported by Seiko Epson Corporation.")
                .font(.caption).foregroundStyle(.secondary)
            ScrollView {
                Text("""
                Components
                  rastertotmt20      CUPS raster filter (ESC/POS GS 8 L / GS ( L, zlib)
                  EpsonTMT20III.ppd  paper sizes and options of the Windows GPD
                  TMUtility          printer settings + Printing Preferences
                  tmutil / tmqueue   command-line tools

                Settings are stored in the CUPS queue's PPD and apply to every
                application. Language: follows macOS.
                """)
                .font(.system(.body, design: .monospaced))
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(8)
            }
            .background(Color(nsColor: .textBackgroundColor))
            .border(.separator)
        }
        .padding()
    }
}

struct AdvancedSettingsTab: View {
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(spacing: 18) {
            Button("Advanced Settings") { openWindow(id: "apd") }
                .controlSize(.large).frame(width: 260, height: 60)
            Text("Start up the APD Utility to access the Advanced Settings.")
                .multilineTextAlignment(.center).frame(width: 300)
            Divider().frame(width: 300)
            Text("Log Option Settings").font(.headline)
            Text("Filter log: run  cupsctl --debug-logging  and read /var/log/cups/error_log.")
                .font(.caption).foregroundStyle(.secondary).multilineTextAlignment(.center).frame(width: 320)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

// MARK: - APD Utility window

enum APDPage: String, CaseIterable, Identifiable {
    case layout = "Layout", paper = "User Defined Paper", command = "Send Command"
    var id: Self { self }
    var icon: String { ["rectangle.dashed", "doc.badge.plus", "terminal"][APDPage.allCases.firstIndex(of: self)!] }
}

struct APDUtilityView: View {
    @EnvironmentObject var q: QueueModel
    @State private var page: APDPage? = .layout

    var body: some View {
        NavigationSplitView {
            List(APDPage.allCases, selection: $page) { p in
                Label(p.rawValue, systemImage: p.icon).tag(p)
            }
            .navigationSplitViewColumnWidth(190)
        } detail: {
            VStack(alignment: .leading, spacing: 10) {
                QueuePicker()
                Divider()
                switch page ?? .layout {
                case .layout: LayoutPage()
                case .paper: UserDefinedPaperPage()
                case .command: SendCommandPage()
                }
                Spacer(minLength: 0)
                QueueStatusLine()
            }
            .padding()
        }
        .frame(minWidth: 780, minHeight: 560)
        .onAppear { if q.queues.isEmpty { q.refreshQueues() } }
    }
}

struct MMField: View {
    let value: Binding<Int>             // micrometres
    var inch = false
    var width: CGFloat = 70

    var body: some View {
        TextField("", value: Binding(
            get: { Double(value.wrappedValue) / (inch ? 25400 : 1000) },
            set: { value.wrappedValue = Int((max(0, $0) * (inch ? 25400 : 1000)).rounded()) }),
                  format: .number.precision(.fractionLength(inch ? 2 : 1)))
            .frame(width: width)
    }
}

/// APD "Layout" (dialog 200; TM-T20III.ini [UTL_LAYOUT]: margins 0-11 mm).
struct LayoutPage: View {
    @EnvironmentObject var q: QueueModel

    private func um(_ k: String) -> Binding<Int> {
        Binding(get: { q.int(k) }, set: { q.set(k, min(max($0, 0), 11000)) })
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            GroupBox("Adjust Print Position") {
                VStack(alignment: .leading, spacing: 8) {
                    Toggle("Top Margin", isOn: Binding(get: { q.int("tm-margin-top-enable") != 0 },
                                                       set: { q.set("tm-margin-top-enable", $0 ? 1 : 0) }))
                    HStack { MMField(value: um("tm-margin-top")); Text("[0-11]mm") }
                        .padding(.leading, 20).disabled(q.int("tm-margin-top-enable") == 0)
                    Toggle("Left Margin", isOn: Binding(get: { q.int("tm-margin-left-enable") != 0 },
                                                        set: { q.set("tm-margin-left-enable", $0 ? 1 : 0) }))
                    HStack { MMField(value: um("tm-margin-left")); Text("[0-11]mm") }
                        .padding(.leading, 20).disabled(q.int("tm-margin-left-enable") == 0)
                }
                .frame(maxWidth: .infinity, alignment: .leading).padding(6)
            }
            GroupBox("Reduced Size Print") {
                Picker("", selection: Binding(get: { q.int("tm-reduce-80-58") }, set: { q.set("tm-reduce-80-58", $0) })) {
                    Text("Do not reduce").tag(0)
                    Text("Reduce from 80 mm to 58 mm").tag(1)
                }
                .pickerStyle(.radioGroup).labelsHidden()
                .frame(maxWidth: .infinity, alignment: .leading).padding(6)
            }
            GroupBox("Unsupported Paper") {
                Toggle("Automatically adjust and print", isOn: Binding(
                    get: { q.int("tm-unsupported-auto") != 0 }, set: { q.set("tm-unsupported-auto", $0 ? 1 : 0) }))
                    .frame(maxWidth: .infinity, alignment: .leading).padding(6)
            }
            HStack { Spacer(); Button("Set") { q.apply() }.disabled(!q.dirty || q.busy) }
        }
    }
}

/// APD "User Defined Paper" (dialog 25010; limits from [UTL_USERDEFINEDPAPER]).
struct UserDefinedPaperPage: View {
    @EnvironmentObject var q: QueueModel
    @State private var selected: String?
    @State private var name = ""
    @State private var inch = false
    @State private var width = 72100                // defaults 721 / 2969 (0.1 mm)
    @State private var height = 296900
    @State private var left = 0
    @State private var top = 0
    @State private var right = 0
    @State private var bottom = 0
    @State private var confirmUpdate = false
    @State private var error: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            GroupBox("Paper List") {
                HStack(alignment: .top) {
                    List(q.papers, selection: $selected) { p in
                        Text(p.name).tag(p.keyword)
                    }
                    .listStyle(.bordered(alternatesRowBackgrounds: false))
                    .frame(height: 120)
                    Button("Delete") {
                        if let p = q.papers.first(where: { $0.keyword == selected }) {
                            q.deletePaper(p.name)
                            q.apply()
                        }
                    }
                    .disabled(selected == nil)
                }
            }
            .onChange(of: selected) { kw in
                guard let p = q.papers.first(where: { $0.keyword == kw }) else { return }
                name = p.name; inch = p.inch
                width = p.width; height = p.height
                left = p.left; top = p.top; right = p.right; bottom = p.bottom
            }
            GroupBox("Add User Defined Paper") {
                VStack(alignment: .leading, spacing: 10) {
                    HStack {
                        Text("Paper Name").frame(width: 80, alignment: .leading)
                        TextField("", text: $name).frame(width: 260)
                        Spacer()
                        Button("Register") { register(confirmed: false) }.disabled(name.isEmpty || q.busy)
                    }
                    HStack(alignment: .top) {
                        GroupBox("Paper Type") {
                            Picker("", selection: .constant(0)) { Text("Standard roll paper").tag(0) }
                                .labelsHidden().frame(width: 200)
                        }
                        GroupBox("Unit") {
                            Picker("", selection: $inch) { Text("mm").tag(false); Text("inch").tag(true) }
                                .pickerStyle(.radioGroup).horizontalRadioGroupLayout().labelsHidden()
                        }
                    }
                    HStack(alignment: .top) {
                        GroupBox("Paper Size") {
                            Grid(alignment: .leading) {
                                GridRow { Text("Width"); MMField(value: $width, inch: inch); Text(inch ? "inch" : "mm") }
                                GridRow { Text("Height"); MMField(value: $height, inch: inch); Text(inch ? "inch" : "mm") }
                            }
                        }
                        GroupBox("Margin") {
                            Grid(alignment: .leading) {
                                GridRow {
                                    Text("Left"); MMField(value: $left, inch: inch); Text(inch ? "inch" : "mm")
                                    Text("Top"); MMField(value: $top, inch: inch); Text(inch ? "inch" : "mm")
                                }
                                GridRow {
                                    Text("Right"); MMField(value: $right, inch: inch); Text(inch ? "inch" : "mm")
                                    Text("Bottom"); MMField(value: $bottom, inch: inch); Text(inch ? "inch" : "mm")
                                }
                            }
                        }
                    }
                    Text("Width 52.6-210.0 mm, height 25.4-3276.0 mm. For Paperwork.show truss tapes: "
                         + "TRUSS TAPE, inch, 3.18 x 12.00, margins 0.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                .padding(6)
            }
        }
        .alert("This paper size has already been registered. Do you want to update the paper size?",
               isPresented: $confirmUpdate) {
            Button("Yes") { register(confirmed: true) }
            Button("No", role: .cancel) {}
        }
        .alert("Error", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK") {}
        } message: { Text(error ?? "") }
    }

    private func register(confirmed: Bool) {
        let n = name.trimmingCharacters(in: .whitespaces)
        if !confirmed && q.hasPaper(named: n) { confirmUpdate = true; return }
        if left + right >= width || top + bottom >= height { error = "Margin exceeding the paper size."; return }
        if let e = q.addPaper(name: n, width: width, height: height, left: left, top: top,
                              right: right, bottom: bottom, inch: inch) {
            error = e
            return
        }
        q.apply()
    }
}

/// APD "Send Command" (dialog 26010; command.startdoc/enddoc/startpage/endpage).
struct SendCommandPage: View {
    @EnvironmentObject var q: QueueModel
    @State private var text: [Timing: String] = [:]
    @State private var on: [Timing: Bool] = [:]
    @State private var error: String?

    /// Dialog order: Start of Document, End of Document, Start of Page, End of Page.
    private let order: [(Timing, String)] = [(.docBegin, "Start of Document"), (.docEnd, "End of Document"),
                                             (.pageBegin, "Start of Page"), (.pageEnd, "End of Page")]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            GroupBox("Send Command") {
                VStack(alignment: .leading, spacing: 10) {
                    ForEach(order.indices, id: \.self) { i in
                        let t = order[i].0, title = order[i].1
                        Toggle(title, isOn: Binding(get: { on[t] ?? false }, set: { on[t] = $0 }))
                        TextField("hex, e.g. 1B 40   or \"text\"", text: Binding(
                            get: { text[t] ?? "" }, set: { text[t] = $0 }))
                            .font(.system(.body, design: .monospaced))
                            .padding(.leading, 20)
                            .disabled(!(on[t] ?? false))
                    }
                    Text("Bytes are sent at that point of every job, before the cash drawer, feed and cut "
                         + "(same order as the Windows driver).")
                        .font(.caption).foregroundStyle(.secondary)
                }
                .padding(6)
            }
            HStack {
                if let e = error { Label(e, systemImage: "exclamationmark.triangle").foregroundStyle(.red) }
                Spacer()
                Button("Set") { save() }.disabled(q.busy)
            }
        }
        .onAppear(perform: reload)
        .onChange(of: q.queue) { _ in reload() }
        .onChange(of: q.revision) { _ in if !q.dirty { reload() } }
    }

    private func reload() {
        for (t, _) in order {
            let v = q.get("tm-cmd-\(t.key)")
            text[t] = v
            on[t] = !v.isEmpty
        }
    }

    private func save() {
        error = nil
        for (t, title) in order {
            let v = (on[t] ?? false) ? (text[t] ?? "") : ""
            if let e = q.set("tm-cmd-\(t.key)", v) {
                error = "\(title): \(e)"
                return
            }
        }
        reload()
        q.apply()
    }
}
