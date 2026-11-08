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
                .frame(minWidth: 900, minHeight: 600)
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
}

struct ContentView: View {
    @EnvironmentObject var m: PrinterModel
    @Environment(\.openWindow) private var openWindow
    @State private var page: Page? = .currentSettings
    @State private var confirmApply = false

    var body: some View {
        NavigationSplitView {
            List(selection: $page) {
                Section("Main") {
                    item(.currentSettings, "list.bullet.rectangle")
                    DisclosureGroup("Basic Settings") {
                        item(.logos, "photo")
                        item(.paperReduction, "arrow.down.right.and.arrow.up.left")
                        item(.autoCut, "scissors")
                        item(.printingControl, "slider.horizontal.3")
                    }
                    DisclosureGroup("Detailed Settings") {
                        item(.font, "textformat")
                        item(.buzzer, "speaker.wave.2")
                        item(.interface, "cable.connector")
                    }
                }
                item(.operationCheck, "checkmark.circle")
                item(.backup, "externaldrive")
            }
            .navigationSplitViewColumnWidth(220)
        } detail: {
            VStack(spacing: 0) {
                StatusHeader()
                Divider()
                Group {
                    switch page ?? .currentSettings {
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
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                Divider()
                footer
            }
        }
        .toolbar {
            ToolbarItemGroup {
                Toggle("Network", isOn: $m.useTCP).toggleStyle(.switch)
                if m.useTCP { TextField("IP[:port]", text: $m.host).frame(width: 150) }
                Button { m.refreshAll() } label: { Label("Read", systemImage: "arrow.down.circle") }
                    .disabled(m.busy)
                Button { openWindow(id: "prefs") } label: {
                    Label("Printing Preferences", systemImage: "printer.dotmatrix")
                }
            }
        }
        .sheet(isPresented: $confirmApply) { ApplySheet() }
        .onAppear { m.refreshAll() }
    }

    private func item(_ p: Page, _ icon: String) -> some View {
        Label(p.rawValue, systemImage: icon).tag(p)
    }

    private var footer: some View {
        HStack {
            if m.busy { ProgressView().controlSize(.small) }
            if let e = m.lastError {
                Label(e, systemImage: "exclamationmark.triangle").foregroundStyle(.red)
            } else if let msg = m.message {
                Text(msg).foregroundStyle(.secondary)
            }
            Spacer()
            if !m.changes.isEmpty {
                Text("\(m.changes.count) unsaved change(s)").foregroundStyle(.orange)
            }
        }
        .padding(8)
    }
}

/// "Printer Name: / Port: / Status:" bar ([PRT_STATUS_STR]).
struct StatusHeader: View {
    @EnvironmentObject var m: PrinterModel

    private var statusText: String {
        guard let s = m.status else { return "—" }
        if s.unrecoverable != 0 { return "Fatal Error" }
        if s.cutter_error != 0 { return "Auto cutter error" }
        if s.cover_open != 0 { return "Cover is open" }
        if s.paper_end != 0 { return "Out of paper" }
        if s.auto_recoverable != 0 { return "Auto recoverable error" }
        if s.near_end != 0 { return "Paper near-end" }
        return s.online != 0 ? "Online" : "Offline"
    }

    var body: some View {
        HStack(spacing: 24) {
            LabeledContent("Printer Name:", value: m.info.model.isEmpty ? "TM-T20III" : m.info.model)
            LabeledContent("Port:", value: m.useTCP ? "Ethernet \(m.host)" : "USB")
            LabeledContent("Status:") {
                Text(statusText).foregroundStyle(statusText == "Online" ? Color.green : Color.orange)
            }
            Spacer()
            Button("Refresh") { m.refreshStatus() }.disabled(m.busy)
        }
        .padding(10)
    }
}

/// Bottom-right "Set" button used by every settings page.
struct SetButton: View {
    @EnvironmentObject var m: PrinterModel
    let action: () -> Void
    var body: some View {
        HStack {
            Spacer()
            Button("Set", action: action).disabled(m.busy || m.changes.isEmpty).keyboardShortcut(.defaultAction)
        }
    }
}

// MARK: - pages

/// TMUPRNSTG: Printer Information + Current Settings (Menu | Item | Value).
struct CurrentSettingsPage: View {
    @EnvironmentObject var m: PrinterModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            GroupBox("Printer Information") {
                Grid(alignment: .leading, horizontalSpacing: 20, verticalSpacing: 4) {
                    GridRow { Text("Printer Name"); Text(m.info.model) }
                    GridRow { Text("Fonts Provided"); Text(m.info.fonts) }
                    GridRow { Text("Serial No"); Text(m.info.serial) }
                    GridRow { Text("Firmware Version"); Text(m.info.firmware) }
                }
                .frame(maxWidth: .infinity, alignment: .leading).padding(4)
            }
            Text("Current Settings")
            Table(m.items) {
                TableColumn("Menu") { Text($0.group) }.width(130)
                TableColumn("Item") { Text($0.title.trimmingCharacters(in: .whitespaces)) }
                TableColumn("Value") { Text($0.label(for: $0.current)) }
            }
        }
        .padding()
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
                Text("unverified").font(.caption2).padding(.horizontal, 4)
                    .background(Color.orange.opacity(0.2), in: Capsule())
                    .help("Mapping inferred from the Windows utility; read it back after writing.")
            }
            if item.isChanged { Image(systemName: "pencil.circle.fill").foregroundStyle(.blue) }
        }
    }
}

/// TMUPAPERSTG.
struct PaperReductionPage: View {
    @EnvironmentObject var m: PrinterModel
    let set: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 10) {
                ForEach(m.items.filter { $0.group == "Paper Reduction" }) { item in
                    GridRow {
                        Text(item.title + ":")
                        SettingPicker(item: item, width: 260)
                    }
                }
            }
            HStack(spacing: 40) {
                ReductionPreview(title: "Normal:", reduced: false)
                ReductionPreview(title: "Paper Reduction:", reduced: true)
            }
            Spacer()
            SetButton(action: set)
        }
        .padding()
    }
}

struct ReductionPreview: View {
    let title: String, reduced: Bool
    var body: some View {
        VStack(alignment: .leading) {
            Text(title)
            VStack(spacing: reduced ? 2 : 6) {
                Rectangle().fill(.clear).frame(height: reduced ? 4 : 14)
                ForEach(0..<4) { _ in Capsule().fill(.secondary).frame(width: 90, height: reduced ? 4 : 5) }
                Rectangle().fill(.clear).frame(height: reduced ? 4 : 14)
            }
            .frame(width: 120)
            .background(Color(nsColor: .textBackgroundColor))
            .border(.separator)
        }
    }
}

/// TMUPrintingControl: list view with Function | Setting columns.
struct PrintingControlPage: View {
    @EnvironmentObject var m: PrinterModel
    let set: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Table(m.items.filter { $0.group == "Printing Control" }) {
                TableColumn("Function") { Text($0.title) }.width(min: 260, ideal: 330)
                TableColumn("Setting") { SettingPicker(item: $0, width: 240) }.width(min: 260, ideal: 340)
            }
            Text("Not available on macOS yet: Multi-language font (20010), Receive buffer 128 KB.")
                .font(.caption).foregroundStyle(.secondary)
            SetButton(action: set)
        }
        .padding()
    }
}

/// TMUFont.
struct FontPage: View {
    @EnvironmentObject var m: PrinterModel
    let set: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 10) {
                ForEach(m.items.filter { $0.group == "Font" }) { item in
                    GridRow { Text(item.title + ":"); SettingPicker(item: item, width: 280) }
                }
            }
            Button("Test Print") { m.testPrint() }.disabled(m.busy)
            Text("If you are using UPOS, you need to set the font on the UPOS.")
                .font(.caption).foregroundStyle(.secondary)
            Spacer()
            SetButton(action: set)
        }
        .padding()
    }
}

/// Pages whose printer commands are not traced: same layout, controls
/// disabled, with the reason.
struct NotTracedPage: View {
    let page: Page
    let controls: [String]
    let note: String

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Label(note, systemImage: "info.circle").foregroundStyle(.secondary)
            HStack { ForEach(controls, id: \.self) { Button($0) {}.disabled(true) } }
            Spacer()
        }
        .padding()
    }
}

/// TMUATCUTSTG ([TMUATCUTSTG_DLL_CAP]).
struct AutoCutPage: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label("The automatic cut / top-bottom logo settings are not traced yet; shown for reference. "
                  + "\"Method for canceling auto cutter error\" is under Printing Control.",
                  systemImage: "info.circle").foregroundStyle(.secondary)
            Picker("", selection: .constant(0)) {
                Text("Not use this function").tag(0)
                Text("Cut paper when the cover is closed.").tag(1)
                Text("Print logo when paper is cut.").tag(2)
            }
            .pickerStyle(.radioGroup).labelsHidden().disabled(true)
            GroupBox("Condition") {
                VStack(alignment: .leading) {
                    Toggle("When paper is cut by command", isOn: .constant(false))
                    Toggle("When cover is closed", isOn: .constant(false))
                    Toggle("When printer is powered on", isOn: .constant(false))
                }
                .disabled(true)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            Spacer()
        }
        .padding()
    }
}

/// TMUBuzzer ([TMUBUZZERSTG_DLL_CAP]).
struct BuzzerPage: View {
    private let events = ["When an error occurs:", "When the roll paper ends:",
                          "When automatic paper cut activates:", "When specified pulse 1 (2 pin) occurs:",
                          "When specified pulse 2 (5 pin) occurs:", "When printer is powered on:"]

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label("Optional buzzer event settings are not traced yet; shown for reference. "
                  + "Use Operation Check > Test Buzzer to sound it.", systemImage: "info.circle")
                .foregroundStyle(.secondary)
            Picker("", selection: .constant(0)) {
                Text("Disabled").tag(0)
                Text("Optional Buzzer").tag(1)
            }
            .pickerStyle(.radioGroup).labelsHidden().disabled(true)
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
            Spacer()
        }
        .padding()
    }
}

/// TMUIFSTG (USB form).
struct InterfacePage: View {
    @EnvironmentObject var m: PrinterModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            LabeledContent("Interface type", value: m.useTCP ? "Ethernet" : "USB")
            GroupBox("Information") {
                Grid(alignment: .leading, horizontalSpacing: 20) {
                    GridRow { Text("USB Device:"); Text(m.info.model) }
                    GridRow { Text("Firmware:"); Text(m.info.firmware) }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            GroupBox("USB Class Setting") {
                VStack(alignment: .leading) {
                    Picker("", selection: .constant(0)) {
                        Text("Printer Class").tag(0)
                        Text("Vender Class").tag(1)
                    }
                    .pickerStyle(.radioGroup).labelsHidden().disabled(true)
                    Text("macOS needs Printer Class (the default). Changing the class is not traced; "
                         + "use Epson's utility on Windows if the printer was switched to Vendor Class.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            Spacer()
        }
        .padding()
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
            GroupBox("Print Test") {
                HStack {
                    Picker("", selection: $printTest) {
                        Text("Test Printing").tag(1)
                        Text("Print Logo").tag(2)
                        Text("Print Font").tag(3)
                    }
                    .labelsHidden().frame(width: 220)
                    Spacer()
                    Button("Execute") {
                        switch printTest {
                        case 3: m.testPrint(text: "ABCDEFGHIJKLMNOPQRSTUVWXYZ\nabcdefghijklmnopqrstuvwxyz\n0123456789 !\"#$%&'()*+,-./\n")
                        default: m.testPrint()
                        }
                    }
                    .disabled(m.busy || printTest == 2)
                }
            }
            GroupBox("Device Test") {
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
                    Button("Execute") {
                        switch deviceTest {
                        case 1: m.refreshStatus(); m.message = "Communication test completed successfully."
                        case 2: askDrawer = true
                        default: m.buzz(Int32(pattern))
                        }
                    }
                    .disabled(m.busy)
                }
            }
            Spacer()
        }
        .padding()
        .sheet(isPresented: $askDrawer) {
            VStack(alignment: .leading, spacing: 12) {
                Text("Open Drawer").font(.headline)
                Text("Select the drawer to open.")
                Picker("Connector Pin Number", selection: $drawerPin) {
                    Text("Drawer 1 (2 pin)").tag(2)
                    Text("Drawer 2 (5 pin)").tag(5)
                }
                .pickerStyle(.radioGroup)
                HStack {
                    Spacer()
                    Button("Cancel") { askDrawer = false }.keyboardShortcut(.cancelAction)
                    Button("OK") { m.drawer(Int32(drawerPin)); askDrawer = false }.keyboardShortcut(.defaultAction)
                }
            }
            .padding().frame(width: 320)
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
            GroupBox("Backup") {
                VStack(alignment: .leading, spacing: 8) {
                    Toggle("Printer settings", isOn: .constant(true)).disabled(true)
                    Toggle("Stored logo data", isOn: .constant(false)).disabled(true)
                    HStack {
                        Text("File name:")
                        TextField("", text: $backupPath)
                        Button("Browse") { browseSave() }
                    }
                    HStack { Spacer(); Button("Backup") { confirm = "backup" }.disabled(backupPath.isEmpty) }
                }
                .padding(4)
            }
            GroupBox("Restore") {
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Text("File name:")
                        TextField("", text: $restorePath)
                        Button("Browse") { browseOpen() }
                    }
                    HStack { Spacer(); Button("Restore") { confirm = "restore" }.disabled(restorePath.isEmpty) }
                }
                .padding(4)
            }
            Text("Same key=value format as the tmutil command-line tool. Restoring loads the values; "
                 + "review them and press Set on the Printing Control page.")
                .font(.caption).foregroundStyle(.secondary)
            Spacer()
        }
        .padding()
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
        VStack(alignment: .leading, spacing: 12) {
            Text("Do you want to save your changes to the settings to the printer?").font(.headline)
            Table(m.changes) {
                TableColumn("Setting") { Text($0.title.trimmingCharacters(in: .whitespaces)) }
                TableColumn("Printer") { Text($0.label(for: $0.current)) }
                TableColumn("New") { Text($0.label(for: $0.pending)) }
                TableColumn("") { Text($0.verified ? "" : "unverified") }
            }
            .frame(minHeight: 200)
            Text("The printer resets after writing. Don't print until it is ready again.")
                .font(.caption).foregroundStyle(.secondary)
            HStack {
                Button("Discard changes", role: .destructive) { m.discard(); dismiss() }
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Set") { m.apply(); dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding()
        .frame(width: 640)
    }
}
