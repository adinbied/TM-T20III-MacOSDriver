import AppKit
import SwiftUI
import TMCore
import UniformTypeIdentifiers

// Main window = Epson "TM-T20III Utility" (TMUtility.exe): menu tree from
// MENUITEM.DAT, page layouts/labels from the TMU*.dll dialogs and
// Lang/0409.DAT.  Printing Preferences and the APD Utility are separate
// windows (see PreferencesViews.swift), as on Windows.

@main
struct TMUtilityApp: App {
    @StateObject private var model = PrinterModel()
    @StateObject private var queue = QueueModel()

    init() {
        // Running as a bare SwiftPM executable: make it a regular app.
        NSApplication.shared.setActivationPolicy(.regular)
    }

    var body: some Scene {
        WindowGroup("TM-T20III Utility", id: "utility") {
            ContentView()
                .environmentObject(model)
                .environmentObject(queue)
                .frame(minWidth: 940, minHeight: 640)
                .onAppear { NSApplication.shared.activate(ignoringOtherApps: true) }
        }
        .commands {
            CommandGroup(after: .appSettings) {
                OpenWindowButton(title: "Printing Preferences…", id: "prefs")
                    .keyboardShortcut(",", modifiers: .command)
                OpenWindowButton(title: "APD Utility (Advanced Settings)…", id: "apd")
            }
            CommandMenu("Printer") {
                Button("Read Settings") { model.refreshAll() }.keyboardShortcut("r")
                Menu("Load Preset") {
                    ForEach(model.presets) { p in Button(p.title) { model.load(preset: p) } }
                }
            }
        }

        Window("Printing Preferences", id: "prefs") {
            PrintingPreferencesView()
                .environmentObject(queue)
                .environmentObject(model)
        }
        .windowResizability(.contentSize)

        Window("APD Utility", id: "apd") {
            APDUtilityView()
                .environmentObject(queue)
                .environmentObject(model)
        }
    }
}

struct OpenWindowButton: View {
    @Environment(\.openWindow) private var openWindow
    let title: String, id: String
    var body: some View { Button(title) { openWindow(id: id) } }
}

// MARK: - tree (MENUITEM.DAT)

enum Page: String, Hashable, CaseIterable {
    case currentSettings = "Current Settings"
    case logos = "Storing Logos"
    case paperReduction = "Paper Reduction"
    case autoCut = "Automatic Paper Cut"
    case printingControl = "Printing Control"
    case font = "Font"
    case buzzer = "Buzzer"
    case interface = "Communication I/F"
    case operationCheck = "Operation Check"
    case backup = "Backup/Restore"

    var icon: String {
        switch self {
        case .currentSettings: return "list.bullet.rectangle.fill"
        case .logos: return "photo.fill"
        case .paperReduction: return "arrow.down.right.and.arrow.up.left"
        case .autoCut: return "scissors"
        case .printingControl: return "slider.horizontal.3"
        case .font: return "textformat"
        case .buzzer: return "speaker.wave.2.fill"
        case .interface: return "cable.connector"
        case .operationCheck: return "checkmark.seal.fill"
        case .backup: return "externaldrive.fill"
        }
    }

    var color: Color {
        switch self {
        case .currentSettings: return .blue
        case .logos: return .purple
        case .paperReduction: return .teal
        case .autoCut: return .orange
        case .printingControl: return .indigo
        case .font: return .pink
        case .buzzer: return .red
        case .interface: return .green
        case .operationCheck: return .mint
        case .backup: return .brown
        }
    }

    var subtitle: String {
        switch self {
        case .currentSettings: return "Printer information and the values stored in the printer."
        case .logos: return "Logos stored in the printer's memory."
        case .paperReduction: return "Save paper by shrinking blank space and line spacing."
        case .autoCut: return "Cut the paper automatically when the cover closes."
        case .printingControl: return "Print density, speed and paper handling."
        case .font: return "Default font and code page."
        case .buzzer: return "Optional buzzer events."
        case .interface: return "How the printer is connected."
        case .operationCheck: return "Test printing, the cash drawer and the buzzer."
        case .backup: return "Save the printer settings to a file, or load them back."
        }
    }

    /// Pages built around a Table fill the window instead of scrolling.
    var usesTable: Bool { self == .currentSettings || self == .printingControl }
}

struct ContentView: View {
    @EnvironmentObject var m: PrinterModel
    @Environment(\.openWindow) private var openWindow
    @State private var page: Page? = .currentSettings
    @State private var confirmApply = false

    var body: some View {
        NavigationSplitView {
            List(selection: $page) {
                Section("Main") { row(.currentSettings) }
                Section("Basic Settings") {
                    row(.logos); row(.paperReduction); row(.autoCut); row(.printingControl)
                }
                Section("Detailed Settings") { row(.font); row(.buzzer); row(.interface) }
                Section("Tools") { row(.operationCheck); row(.backup) }
            }
            .listStyle(.sidebar)
            .scrollContentBackground(.hidden)
            .background(Theme.surface)
            .navigationSplitViewColumnWidth(min: 210, ideal: 230)
        } detail: {
            let current = page ?? .currentSettings
            VStack(spacing: 0) {
                StatusHeader()
                Divider()
                Group {
                    if current.usesTable {
                        pageBody(current).padding(20)
                    } else {
                        ScrollView { pageBody(current).padding(20) }
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                Divider()
                footer
            }
            .background(Theme.window)
        }
        .toolbar {
            ToolbarItemGroup {
                Toggle(isOn: $m.useTCP) { Label("Network", systemImage: "network") }
                    .toggleStyle(.switch)
                if m.useTCP { TextField("IP[:port]", text: $m.host).frame(width: 150) }
                Button { m.refreshAll() } label: { Label("Read", systemImage: "arrow.down.circle.fill") }
                    .disabled(m.busy)
                    .help("Read all settings from the printer")
                Button { openWindow(id: "prefs") } label: {
                    Label("Printing Preferences", systemImage: "printer.dotmatrix.fill")
                }
                .help("Printing Preferences")
            }
        }
        .opaqueWindow()
        .sheet(isPresented: $confirmApply) { ApplySheet() }
        .onAppear { m.refreshAll() }
    }

    @ViewBuilder private func pageBody(_ p: Page) -> some View {
        VStack(alignment: .leading, spacing: 16) {
            PageHeader(title: p.rawValue, subtitle: p.subtitle, icon: p.icon, color: p.color)
            switch p {
            case .currentSettings: CurrentSettingsPage()
            case .logos: NotTracedPage(page: .logos, controls: ["Select", "Edit", "Clear", "Test Print"],
                                       note: "Uploading NV logos (GS ( L) is not traced yet.")
            case .paperReduction: PaperReductionPage(set: { confirmApply = true })
            case .autoCut: AutoCutPage()
            case .printingControl: PrintingControlPage(set: { confirmApply = true })
            case .font: FontPage(set: { confirmApply = true })
            case .buzzer: BuzzerPage()
            case .interface: InterfacePage()
            case .operationCheck: OperationCheckPage()
            case .backup: BackupRestorePage()
            }
        }
        .frame(maxWidth: .infinity, maxHeight: p.usesTable ? CGFloat.infinity : nil, alignment: .topLeading)
    }

    private func row(_ p: Page) -> some View {
        TileLabel(title: p.rawValue, icon: p.icon, color: p.color).tag(p)
    }

    private var footer: some View {
        HStack(spacing: 10) {
            if m.busy { ProgressView().controlSize(.small) }
            if let e = m.lastError {
                Label(e, systemImage: "exclamationmark.triangle.fill").foregroundColor(.red)
            } else if let msg = m.message {
                Label(msg, systemImage: "checkmark.circle.fill").foregroundColor(.secondary)
                    .symbolRenderingMode(.multicolor)
            }
            Spacer()
            if !m.changes.isEmpty {
                Chip(icon: "pencil", text: "\(m.changes.count) unsaved change(s)", color: .orange)
                Button("Review…") { confirmApply = true }
            }
        }
        .lineLimit(1)
        .padding(.horizontal, 16).padding(.vertical, 8)
        .background(Theme.surface)
    }
}

/// "Printer Name: / Port: / Status:" bar ([PRT_STATUS_STR]).
struct StatusHeader: View {
    @EnvironmentObject var m: PrinterModel

    private var status: (String, Color) {
        guard let s = m.status else { return ("Not connected", .gray) }
        if s.unrecoverable != 0 { return ("Fatal Error", .red) }
        if s.cutter_error != 0 { return ("Auto cutter error", .red) }
        if s.cover_open != 0 { return ("Cover is open", .orange) }
        if s.paper_end != 0 { return ("Out of paper", .red) }
        if s.auto_recoverable != 0 { return ("Auto recoverable error", .orange) }
        if s.near_end != 0 { return ("Paper near-end", .yellow) }
        return s.online != 0 ? ("Online", .green) : ("Offline", .orange)
    }

    var body: some View {
        HStack(spacing: 14) {
            IconTile(symbol: "printer.fill", color: .blue, size: 46)
            VStack(alignment: .leading, spacing: 5) {
                Text(m.info.model.isEmpty ? "TM-T20III" : m.info.model).font(.title2.weight(.semibold))
                HStack(spacing: 6) {
                    Chip(icon: m.useTCP ? "network" : "cable.connector",
                         text: m.useTCP ? "Ethernet \(m.host)" : "USB", color: .blue)
                    if !m.info.serial.isEmpty { Chip(icon: "number", text: m.info.serial, color: .purple) }
                    if !m.info.firmware.isEmpty { Chip(icon: "cpu", text: m.info.firmware, color: .orange) }
                }
            }
            Spacer()
            StatusPill(text: status.0, color: status.1)
            Button { m.refreshStatus() } label: { Label("Refresh", systemImage: "arrow.clockwise") }
                .disabled(m.busy)
        }
        .padding(.horizontal, 20).padding(.vertical, 14)
        .background(Theme.surface)
    }
}

/// Bottom-right "Set" button used by every settings page.
struct SetButton: View {
    @EnvironmentObject var m: PrinterModel
    let action: () -> Void
    var body: some View {
        HStack {
            Spacer()
            Button(action: action) { Label("Set", systemImage: "square.and.arrow.down.fill").frame(minWidth: 60) }
                .buttonStyle(.borderedProminent).controlSize(.large)
                .disabled(m.busy || m.changes.isEmpty).keyboardShortcut(.defaultAction)
        }
    }
}

// MARK: - pages

/// One "Printer Information" value as a small card.
struct InfoTile: View {
    let title: String, value: String, icon: String, color: Color
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                IconTile(symbol: icon, color: color, size: 20)
                Text(title).font(.caption).foregroundColor(.secondary)
            }
            Text(value.isEmpty ? "—" : value).font(.headline).lineLimit(1).truncationMode(.middle)
                .textSelection(.enabled)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .cardSurface(padding: 12)
    }
}

/// TMUPRNSTG: Printer Information + Current Settings (Menu | Item | Value).
struct CurrentSettingsPage: View {
    @EnvironmentObject var m: PrinterModel

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(spacing: 12) {
                InfoTile(title: "Printer Name", value: m.info.model, icon: "printer.fill", color: .blue)
                InfoTile(title: "Serial No", value: m.info.serial, icon: "number", color: .purple)
                InfoTile(title: "Firmware Version", value: m.info.firmware, icon: "cpu", color: .orange)
                InfoTile(title: "Fonts Provided", value: m.info.fonts, icon: "textformat", color: .pink)
            }
            Card("Current Settings", icon: "list.bullet.rectangle.fill", color: .blue) {
                Table(m.items) {
                    TableColumn("Menu") { Text($0.group).foregroundColor(.secondary) }.width(140)
                    TableColumn("Item") { Text($0.title.trimmingCharacters(in: .whitespaces)) }
                    TableColumn("Value") { Text($0.label(for: $0.current)).fontWeight(.medium) }
                }
                .frame(minHeight: 220, maxHeight: .infinity)
            }
            .frame(maxHeight: .infinity)
        }
    }
}

/// One setting as a Windows combo box (with an "unverified" marker).
struct SettingPicker: View {
    @EnvironmentObject var m: PrinterModel
    let item: SettingItem
    var width: CGFloat = 300

    var body: some View {
        HStack(spacing: 6) {
            if item.choices.isEmpty, let r = item.range {
                TextField("", value: Binding(get: { item.effective ?? r.lowerBound },
                                             set: { m.setPending(item.key, min(max($0, r.lowerBound), r.upperBound)) }),
                          format: .number)
                    .frame(width: 60)
                Text("%")
            } else {
                Picker("", selection: Binding(get: { item.effective ?? Int.min },
                                              set: { m.setPending(item.key, $0) })) {
                    if item.effective == nil { Text("").tag(Int.min) }
                    ForEach(item.choices, id: \.value) { c in Text(c.label).tag(c.value) }
                }
                .labelsHidden()
                .frame(width: width)
            }
            if !item.verified {
                Text("unverified").font(.caption2.weight(.medium)).foregroundColor(.orange)
                    .padding(.horizontal, 6).padding(.vertical, 1)
                    .background(Capsule().fill(Color.orange.opacity(0.15)))
                    .help("Mapping inferred from the Windows utility; read it back after writing.")
            }
            if item.isChanged { Image(systemName: "pencil.circle.fill").foregroundColor(.blue) }
        }
    }
}

/// TMUPAPERSTG.
struct PaperReductionPage: View {
    @EnvironmentObject var m: PrinterModel
    let set: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Card("Settings", icon: "slider.horizontal.3", color: .teal) {
                Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 10) {
                    ForEach(m.items.filter { $0.group == "Paper Reduction" }) { item in
                        GridRow {
                            Text(item.title + ":")
                            SettingPicker(item: item, width: 260)
                        }
                    }
                }
            }
            Card("Preview", icon: "eye.fill", color: .teal) {
                HStack(spacing: 40) {
                    ReductionPreview(title: "Normal", reduced: false)
                    ReductionPreview(title: "Paper Reduction", reduced: true)
                }
            }
            SetButton(action: set)
        }
    }
}

struct ReductionPreview: View {
    let title: String, reduced: Bool
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.caption).foregroundColor(.secondary)
            VStack(spacing: reduced ? 2 : 6) {
                Rectangle().fill(.clear).frame(height: reduced ? 4 : 14)
                ForEach(0..<4) { _ in
                    Capsule().fill(reduced ? Color.teal : Color.gray).frame(width: 90, height: reduced ? 4 : 5)
                }
                Rectangle().fill(.clear).frame(height: reduced ? 4 : 14)
            }
            .frame(width: 120)
            .background(RoundedRectangle(cornerRadius: 3).fill(Color(nsColor: .textBackgroundColor)))
            .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(Theme.border))
            .shadow(color: .black.opacity(0.08), radius: 2, y: 1)
        }
    }
}

/// TMUPrintingControl: list view with Function | Setting columns.
struct PrintingControlPage: View {
    @EnvironmentObject var m: PrinterModel
    let set: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Card("Functions", icon: "slider.horizontal.3", color: .indigo) {
                Table(m.items.filter { $0.group == "Printing Control" }) {
                    TableColumn("Function") { Text($0.title) }.width(min: 260, ideal: 330)
                    TableColumn("Setting") { SettingPicker(item: $0, width: 240) }.width(min: 260, ideal: 340)
                }
                .frame(minHeight: 200, maxHeight: .infinity)
            }
            .frame(maxHeight: .infinity)
            HStack(alignment: .bottom) {
                Text("Not available on macOS yet: Multi-language font (20010), Receive buffer 128 KB.")
                    .font(.caption).foregroundColor(.secondary)
                SetButton(action: set)
            }
        }
    }
}

/// TMUFont.
struct FontPage: View {
    @EnvironmentObject var m: PrinterModel
    let set: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Card("Font", icon: "textformat", color: .pink) {
                VStack(alignment: .leading, spacing: 14) {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 10) {
                        ForEach(m.items.filter { $0.group == "Font" }) { item in
                            GridRow { Text(item.title + ":"); SettingPicker(item: item, width: 280) }
                        }
                    }
                    Button { m.testPrint() } label: { Label("Test Print", systemImage: "printer.fill") }
                        .disabled(m.busy)
                }
            }
            InfoBanner(text: "If you are using UPOS, you need to set the font on the UPOS.")
            SetButton(action: set)
        }
    }
}

/// Pages whose printer commands are not traced: same layout, controls
/// disabled, with the reason.
struct NotTracedPage: View {
    let page: Page
    let controls: [String]
    let note: String

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            InfoBanner(text: note, icon: "exclamationmark.triangle.fill", color: .orange)
            Card(page.rawValue, icon: page.icon, color: page.color) {
                HStack { ForEach(controls, id: \.self) { Button($0) {}.disabled(true) } }
            }
        }
    }
}

/// TMUATCUTSTG ([TMUATCUTSTG_DLL_CAP]).
struct AutoCutPage: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            InfoBanner(text: "The automatic cut / top-bottom logo settings are not traced yet; shown for reference. "
                       + "\"Method for canceling auto cutter error\" is under Printing Control.",
                       icon: "exclamationmark.triangle.fill", color: .orange)
            Card("Function", icon: "scissors", color: .orange) {
                Picker("", selection: .constant(0)) {
                    Text("Not use this function").tag(0)
                    Text("Cut paper when the cover is closed.").tag(1)
                    Text("Print logo when paper is cut.").tag(2)
                }
                .pickerStyle(.radioGroup).labelsHidden().disabled(true)
            }
            Card("Condition", icon: "list.bullet", color: .orange) {
                VStack(alignment: .leading) {
                    Toggle("When paper is cut by command", isOn: .constant(false))
                    Toggle("When cover is closed", isOn: .constant(false))
                    Toggle("When printer is powered on", isOn: .constant(false))
                }
                .disabled(true)
            }
        }
    }
}

/// TMUBuzzer ([TMUBUZZERSTG_DLL_CAP]).
struct BuzzerPage: View {
    private let events = ["When an error occurs:", "When the roll paper ends:",
                          "When automatic paper cut activates:", "When specified pulse 1 (2 pin) occurs:",
                          "When specified pulse 2 (5 pin) occurs:", "When printer is powered on:"]

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            InfoBanner(text: "Optional buzzer event settings are not traced yet; shown for reference. "
                       + "Use Operation Check > Test Buzzer to sound it.",
                       icon: "exclamationmark.triangle.fill", color: .orange)
            Card("Buzzer", icon: "speaker.wave.2.fill", color: .red) {
                Picker("", selection: .constant(0)) {
                    Text("Disabled").tag(0)
                    Text("Optional Buzzer").tag(1)
                }
                .pickerStyle(.radioGroup).labelsHidden().disabled(true)
            }
            Card("Events", icon: "bell.fill", color: .red) {
                Grid(alignment: .leading) {
                    ForEach(events, id: \.self) { e in
                        GridRow {
                            Text(e)
                            Picker("", selection: .constant(0)) { Text("Off").tag(0) }.labelsHidden().frame(width: 120)
                            Picker("", selection: .constant(0)) { Text("Default").tag(0) }.labelsHidden().frame(width: 120)
                        }
                    }
                }
                .disabled(true)
            }
        }
    }
}

/// TMUIFSTG (USB form).
struct InterfacePage: View {
    @EnvironmentObject var m: PrinterModel

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Card("Information", icon: "info.circle.fill", color: .green) {
                Grid(alignment: .leading, horizontalSpacing: 20, verticalSpacing: 6) {
                    GridRow { Text("Interface type:").foregroundColor(.secondary); Text(m.useTCP ? "Ethernet" : "USB") }
                    GridRow { Text("USB Device:").foregroundColor(.secondary); Text(m.info.model) }
                    GridRow { Text("Firmware:").foregroundColor(.secondary); Text(m.info.firmware) }
                }
            }
            Card("USB Class Setting", icon: "cable.connector", color: .green) {
                VStack(alignment: .leading, spacing: 8) {
                    Picker("", selection: .constant(0)) {
                        Text("Printer Class").tag(0)
                        Text("Vender Class").tag(1)
                    }
                    .pickerStyle(.radioGroup).labelsHidden().disabled(true)
                    Text("macOS needs Printer Class (the default). Changing the class is not traced; "
                         + "use Epson's utility on Windows if the printer was switched to Vendor Class.")
                        .font(.caption).foregroundColor(.secondary)
                }
            }
        }
    }
}

/// TMUOperationCheck ([TMURUNTST_DLL_CMB]).
struct OperationCheckPage: View {
    @EnvironmentObject var m: PrinterModel
    @State private var printTest = 1
    @State private var deviceTest = 1
    @State private var drawerPin = 2
    @State private var pattern = 1
    @State private var askDrawer = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Card("Print Test", icon: "printer.fill", color: .blue) {
                HStack {
                    Picker("", selection: $printTest) {
                        Text("Test Printing").tag(1)
                        Text("Print Logo").tag(2)
                        Text("Print Font").tag(3)
                    }
                    .labelsHidden().frame(width: 220)
                    Spacer()
                    Button {
                        switch printTest {
                        case 3: m.testPrint(text: "ABCDEFGHIJKLMNOPQRSTUVWXYZ\nabcdefghijklmnopqrstuvwxyz\n0123456789 !\"#$%&'()*+,-./\n")
                        default: m.testPrint()
                        }
                    } label: { Label("Execute", systemImage: "play.fill") }
                    .buttonStyle(.borderedProminent)
                    .disabled(m.busy || printTest == 2)
                }
            }
            Card("Device Test", icon: "wrench.and.screwdriver.fill", color: .mint) {
                HStack {
                    Picker("", selection: $deviceTest) {
                        Text("Communication Test").tag(1)
                        Text("Open Drawer").tag(2)
                        Text("Test Buzzer").tag(3)
                    }
                    .labelsHidden().frame(width: 220)
                    if deviceTest == 3 {
                        Picker("", selection: $pattern) {
                            ForEach(1...5, id: \.self) { Text("Pattern \(["A", "B", "C", "D", "E"][$0 - 1])").tag($0) }
                        }
                        .labelsHidden().frame(width: 120)
                    }
                    Spacer()
                    Button {
                        switch deviceTest {
                        case 1: m.refreshStatus(); m.message = "Communication test completed successfully."
                        case 2: askDrawer = true
                        default: m.buzz(Int32(pattern))
                        }
                    } label: { Label("Execute", systemImage: "play.fill") }
                    .buttonStyle(.borderedProminent)
                    .disabled(m.busy)
                }
            }
        }
        .sheet(isPresented: $askDrawer) {
            VStack(alignment: .leading, spacing: 14) {
                HStack(spacing: 10) {
                    IconTile(symbol: "tray.fill", color: .mint, size: 30)
                    Text("Open Drawer").font(.headline)
                }
                Text("Select the drawer to open.")
                Picker("Connector Pin Number", selection: $drawerPin) {
                    Text("Drawer 1 (2 pin)").tag(2)
                    Text("Drawer 2 (5 pin)").tag(5)
                }
                .pickerStyle(.radioGroup)
                HStack {
                    Spacer()
                    Button("Cancel") { askDrawer = false }.keyboardShortcut(.cancelAction)
                    Button("OK") { m.drawer(Int32(drawerPin)); askDrawer = false }
                        .keyboardShortcut(.defaultAction).buttonStyle(.borderedProminent)
                }
            }
            .padding(20).frame(width: 340)
        }
    }
}

/// TMUBackupRestore ([TMUSTGSAV_RST_DLL_CAP]).
struct BackupRestorePage: View {
    @EnvironmentObject var m: PrinterModel
    @State private var backupPath = ""
    @State private var restorePath = ""
    @State private var confirm: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Card("Backup", icon: "tray.and.arrow.down.fill", color: .blue) {
                VStack(alignment: .leading, spacing: 8) {
                    Toggle("Printer settings", isOn: .constant(true)).disabled(true)
                    Toggle("Stored logo data", isOn: .constant(false)).disabled(true)
                    HStack {
                        Text("File name:")
                        TextField("", text: $backupPath)
                        Button("Browse…") { browseSave() }
                    }
                    HStack {
                        Spacer()
                        Button { confirm = "backup" } label: { Label("Backup", systemImage: "tray.and.arrow.down.fill") }
                            .buttonStyle(.borderedProminent).disabled(backupPath.isEmpty)
                    }
                }
            }
            Card("Restore", icon: "tray.and.arrow.up.fill", color: .orange) {
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Text("File name:")
                        TextField("", text: $restorePath)
                        Button("Browse…") { browseOpen() }
                    }
                    HStack {
                        Spacer()
                        Button { confirm = "restore" } label: { Label("Restore", systemImage: "tray.and.arrow.up.fill") }
                            .buttonStyle(.borderedProminent).tint(.orange).disabled(restorePath.isEmpty)
                    }
                }
            }
            InfoBanner(text: "Same key=value format as the tmutil command-line tool. Restoring loads the values; "
                       + "review them and press Set on the Printing Control page.")
        }
        .alert(confirm == "backup" ? "This will backup your printer settings. Continue?"
                                   : "This will restore your printer settings. Continue?",
               isPresented: Binding(get: { confirm != nil }, set: { if !$0 { confirm = nil } })) {
            Button("OK") {
                if confirm == "backup" {
                    do {
                        try m.backupText().write(toFile: backupPath, atomically: true, encoding: .utf8)
                        m.message = "Backup completed successfully."
                    } catch { m.lastError = "Backup failed." }
                } else if let t = try? String(contentsOfFile: restorePath, encoding: .utf8) {
                    m.restore(text: t)
                } else {
                    m.lastError = "This file is not supported."
                }
                confirm = nil
            }
            Button("Cancel", role: .cancel) { confirm = nil }
        }
    }

    private func browseSave() {
        let p = NSSavePanel()
        p.nameFieldStringValue = "TM-T20III-settings.txt"
        p.allowedContentTypes = [.plainText]
        if p.runModal() == .OK, let url = p.url { backupPath = url.path }
    }

    private func browseOpen() {
        let p = NSOpenPanel()
        p.allowedContentTypes = [.plainText]
        if p.runModal() == .OK, let url = p.url { restorePath = url.path }
    }
}

/// "Do you want to save your changes to the settings to the printer?"
struct ApplySheet: View {
    @EnvironmentObject var m: PrinterModel
    @Environment(\.dismiss) var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 12) {
                IconTile(symbol: "square.and.arrow.down.fill", color: .blue, size: 34)
                Text("Do you want to save your changes to the settings to the printer?").font(.headline)
            }
            Table(m.changes) {
                TableColumn("Setting") { Text($0.title.trimmingCharacters(in: .whitespaces)) }
                TableColumn("Printer") { Text($0.label(for: $0.current)).foregroundColor(.secondary) }
                TableColumn("New") { Text($0.label(for: $0.pending)).fontWeight(.semibold).foregroundColor(.blue) }
                TableColumn("") { Text($0.verified ? "" : "unverified").foregroundColor(.orange) }
            }
            .frame(minHeight: 200)
            InfoBanner(text: "The printer resets after writing. Don't print until it is ready again.",
                       icon: "exclamationmark.triangle.fill", color: .orange)
            HStack {
                Button("Discard changes", role: .destructive) { m.discard(); dismiss() }
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Set") { m.apply(); dismiss() }
                    .keyboardShortcut(.defaultAction).buttonStyle(.borderedProminent)
            }
        }
        .padding(20)
        .frame(width: 660)
    }
}
