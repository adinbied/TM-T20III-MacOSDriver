import Foundation
import TMCore

/// One catalog entry (from tmcore.c) plus the values shown in the UI.
struct SettingItem: Identifiable, Hashable {
    let index: Int32
    let key: String
    let title: String
    let group: String
    let verified: Bool
    let choices: [Choice]
    let range: ClosedRange<Int>?
    var current: Int?          // read from the printer
    var pending: Int?          // edited, not yet written

    var id: String { key }
    var effective: Int? { pending ?? current }
    var isChanged: Bool { pending != nil && pending != current }

    struct Choice: Hashable { let label: String; let value: Int }

    func label(for v: Int?) -> String {
        guard let v else { return "—" }
        return choices.first { $0.value == v }?.label ?? String(v)
    }

    static func loadCatalog() -> [SettingItem] {
        var out: [SettingItem] = []
        var i: Int32 = 0
        while let s = tm_setting_at(i) {
            var choices: [Choice] = []
            var c = s.pointee.choices
            while let p = c, let lbl = p.pointee.label {
                choices.append(Choice(label: String(cString: lbl), value: Int(p.pointee.value)))
                c = p + 1
            }
            let range = choices.isEmpty ? Int(s.pointee.min)...Int(s.pointee.max) : nil
            out.append(SettingItem(index: i,
                                   key: String(cString: s.pointee.key),
                                   title: String(cString: s.pointee.title),
                                   group: String(cString: s.pointee.group),
                                   verified: s.pointee.verified != 0,
                                   choices: choices, range: range,
                                   current: nil, pending: nil))
            i += 1
        }
        return out
    }
}

struct Preset: Identifiable, Hashable {
    struct KV: Hashable { let key: String; let value: String }
    let name: String
    let title: String
    let items: [KV]
    var id: String { name }

    static func load() -> [Preset] {
        var out: [Preset] = []
        var i: Int32 = 0
        while let p = tm_preset_at(i) {
            var items: [KV] = []
            var it = p.pointee.items
            while let cur = it, let s = cur.pointee {
                let kv = String(cString: s).split(separator: "=", maxSplits: 1).map(String.init)
                if kv.count == 2 { items.append(KV(key: kv[0], value: kv[1])) }
                it = cur + 1
            }
            out.append(Preset(name: String(cString: p.pointee.name),
                              title: String(cString: p.pointee.title), items: items))
            i += 1
        }
        return out
    }
}

enum Transport: Hashable {
    case usb
    case tcp(host: String, port: Int)
}

struct PrinterInfo {
    var maker = "", model = "", serial = "", firmware = "", fonts = ""
}

struct TMError: LocalizedError {
    let code: Int32
    let what: String
    var errorDescription: String? { "\(what): \(String(cString: tm_strerror(code)))" }
}

/// Opens a link for each operation and closes it right after, so the USB
/// interface is never held while CUPS wants to print.
final class PrinterConnection {
    let transport: Transport
    init(_ t: Transport) { transport = t }

    func with<T>(_ body: (UnsafeMutablePointer<tm_link>) throws -> T) throws -> T {
        let l = UnsafeMutablePointer<tm_link>.allocate(capacity: 1)
        l.initialize(to: tm_link())
        defer { tm_link_close(l); l.deinitialize(count: 1); l.deallocate() }
        let rc: Int32
        switch transport {
        case .usb: rc = tm_link_usb(l)
        case let .tcp(host, port): rc = tm_link_tcp(l, host, Int32(port))
        }
        if rc != 0 { throw TMError(code: rc, what: "connect") }
        return try body(l)
    }

    func info() throws -> PrinterInfo {
        try with { l in
            func q(_ n: Int32) -> String {
                var buf = [CChar](repeating: 0, count: 128)
                return tm_info(l, n, &buf, buf.count) == 0 ? String(cString: buf) : "?"
            }
            return PrinterInfo(maker: q(0x42), model: q(0x43), serial: q(0x44), firmware: q(0x41),
                               fonts: q(0x45))
        }
    }

    func status() throws -> tm_status_t {
        try with { l in
            var s = tm_status_t()
            let rc = tm_status_all(l, &s)
            if rc != 0 { throw TMError(code: rc, what: "status") }
            return s
        }
    }

    func read(_ items: [SettingItem]) throws -> [String: Int] {
        try with { l in
            var out: [String: Int] = [:]
            for it in items {
                guard let s = tm_setting_at(it.index) else { continue }
                var v = 0
                if tm_setting_read(l, s, &v) == 0 { out[it.key] = v }
            }
            return out
        }
    }

    func apply(_ changes: [(SettingItem, Int)]) throws {
        try with { l in
            var arr = changes.compactMap { (it, v) -> tm_assign? in
                guard let s = tm_setting_at(it.index) else { return nil }
                return tm_assign(s: s, value: v)
            }
            let rc = tm_apply(l, &arr, Int32(arr.count))
            if rc != 0 { throw TMError(code: rc, what: "write settings") }
        }
    }

    func testPrint(_ text: String) throws {
        try with { l in
            let rc = tm_print_test(l, text)
            if rc != 0 { throw TMError(code: rc, what: "test print") }
        }
    }

    func drawer(pin: Int32) throws {
        try with { l in
            let rc = tm_kick_drawer(l, pin)
            if rc != 0 { throw TMError(code: rc, what: "drawer") }
        }
    }

    func buzz(pattern: Int32) throws {
        try with { l in
            let rc = tm_buzz(l, pattern, 1)
            if rc != 0 { throw TMError(code: rc, what: "buzzer") }
        }
    }
}

@MainActor
final class PrinterModel: ObservableObject {
    @Published var items: [SettingItem] = SettingItem.loadCatalog()
    @Published var presets: [Preset] = Preset.load()
    @Published var useTCP = false
    @Published var host = ""
    @Published var info = PrinterInfo()
    @Published var status: tm_status_t?
    @Published var busy = false
    @Published var message: String?
    @Published var lastError: String?

    private let queue = DispatchQueue(label: "tmutility.io")

    var groups: [String] {
        var seen: [String] = []
        for i in items where !seen.contains(i.group) { seen.append(i.group) }
        return seen
    }
    var changes: [SettingItem] { items.filter(\.isChanged) }

    private var connection: PrinterConnection {
        if useTCP {
            let parts = host.split(separator: ":")
            let port = parts.count > 1 ? Int(parts[1]) ?? 9100 : 9100
            return PrinterConnection(.tcp(host: String(parts.first ?? ""), port: port))
        }
        return PrinterConnection(.usb)
    }

    private func run<T>(_ what: String, _ work: @escaping (PrinterConnection) throws -> T,
                        done: @escaping (T) -> Void) {
        busy = true
        lastError = nil
        let conn = connection
        queue.async {
            let result = Result { try work(conn) }
            Task { @MainActor in
                self.busy = false
                switch result {
                case .success(let v): done(v)
                case .failure(let e): self.lastError = "\(what): \(e.localizedDescription)"
                }
            }
        }
    }

    func refreshAll() {
        let snapshot = items
        run("Read", { c -> (PrinterInfo, tm_status_t?, [String: Int]) in
            let info = try c.info()
            let st = try? c.status()
            return (info, st, try c.read(snapshot))
        }, done: { r in
            let (info, st, values) = r
            self.info = info
            self.status = st
            for i in self.items.indices {
                self.items[i].current = values[self.items[i].key]
                self.items[i].pending = nil
            }
            self.message = "Read \(values.count) settings from \(info.model)"
        })
    }

    func refreshStatus() {
        run("Status", { try $0.status() }, done: { self.status = $0 })
    }

    func setPending(_ key: String, _ v: Int?) {
        guard let i = items.firstIndex(where: { $0.key == key }) else { return }
        items[i].pending = (v == items[i].current) ? nil : v
    }

    func discard() {
        for i in items.indices { items[i].pending = nil }
    }

    func load(preset: Preset) {
        for kv in preset.items {
            guard let item = items.first(where: { $0.key == kv.key }),
                  let s = tm_setting_at(item.index) else { continue }
            var val = 0
            if tm_setting_parse(s, kv.value, &val) == 0 { setPending(kv.key, val) }
        }
        message = "Loaded preset '\(preset.title)' - review and Apply"
    }

    func apply() {
        let ch = changes.compactMap { it in it.pending.map { (it, $0) } }
        guard !ch.isEmpty else { return }
        run("Apply", { try $0.apply(ch) }, done: {
            for (it, v) in ch {
                if let i = self.items.firstIndex(where: { $0.key == it.key }) {
                    self.items[i].current = v
                    self.items[i].pending = nil
                }
            }
            self.message = "Written \(ch.count) settings. The printer is resetting."
        })
    }

    func testPrint(text: String = "TM-T20III test print\nTMUtility for macOS\n") {
        run("Test print", { try $0.testPrint(text) },
            done: { self.message = "A test page is being printed. Please check the printout." })
    }

    func drawer(_ pin: Int32) {
        run("Drawer", { try $0.drawer(pin: pin) },
            done: { self.message = "A drawer test has been executed. Please check if it ran properly." })
    }

    func buzz(_ p: Int32) {
        run("Buzzer", { try $0.buzz(pattern: p) },
            done: { self.message = "A Buzzer test has been executed. Please check if it worked properly." })
    }

    // Backup format = tmutil's: key=value lines
    func backupText() -> String {
        "# TM-T20III settings backup (TMUtility)\n" +
            items.compactMap { i in i.current.map { "\(i.key)=\($0)" } }.joined(separator: "\n") + "\n"
    }

    func restore(text: String) {
        var n = 0
        for line in text.split(whereSeparator: \.isNewline) where !line.hasPrefix("#") {
            let kv = line.split(separator: "=", maxSplits: 1).map(String.init)
            guard kv.count == 2, let item = items.first(where: { $0.key == kv[0] }),
                  let s = tm_setting_at(item.index) else { continue }
            var val = 0
            if tm_setting_parse(s, kv[1], &val) == 0 { setPending(kv[0], val); n += 1 }
        }
        message = "Loaded \(n) settings from backup - review and Apply"
    }
}
