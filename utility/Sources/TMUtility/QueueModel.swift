import Foundation
import TMCore

/// Printing Preferences for one CUPS queue (= the Windows driver's DEVMODE +
/// APD Utility printer data).  Everything is stored in the queue's PPD (see
/// tmdriver.h); applying means `lpadmin -p QUEUE -P generated.ppd`.
@MainActor
final class QueueModel: ObservableObject {
    static let templatePath = "/Library/Printers/PPDs/Contents/Resources/EpsonTMT20III.ppd"

    @Published var queues: [String] = []
    @Published var queue: String = "" { didSet { if queue != oldValue { load() } } }
    @Published private(set) var revision = 0          // bumps on every edit -> views refresh
    @Published var dirty = false
    @Published var busy = false
    @Published var message: String?
    @Published var lastError: String?

    /// Layout tab job defaults that live outside the PPD (lpadmin -o).
    @Published var orientation = "portrait"           // portrait | landscape
    @Published var pageOrder = "normal"               // normal | reverse
    @Published var numberUp = 1
    private var savedJobDefaults: (String, String, Int) = ("portrait", "normal", 1)

    private var drv: UnsafeMutablePointer<tm_drv> = tm_drv_new()!
    private var saved: UnsafeMutablePointer<tm_drv> = tm_drv_new()!

    deinit {
        tm_drv_free(drv)
        tm_drv_free(saved)
    }

    // MARK: values

    func get(_ key: String) -> String {
        _ = revision
        return tm_drv_get(drv, key).map { String(cString: $0) } ?? ""
    }

    func int(_ key: String) -> Int { Int(get(key)) ?? 0 }

    /// Returns an error text if the value was rejected.
    @discardableResult
    func set(_ key: String, _ value: String) -> String? {
        let rc = tm_drv_set(drv, key, value)
        if rc != 0 { return String(cString: tm_drv_strerror(rc)) }
        touch()
        return nil
    }

    func set(_ key: String, _ value: Int) { set(key, String(value)) }

    private func touch() {
        revision += 1
        dirty = true
    }

    struct Paper: Identifiable, Hashable {
        let keyword: String, name: String
        let width: Int, height: Int, left: Int, top: Int, right: Int, bottom: Int   // micrometres
        let inch: Bool
        var id: String { keyword }
    }

    var papers: [Paper] {
        _ = revision
        return (0..<Int(tm_drv_paper_count(drv))).compactMap { i in
            guard let p = tm_drv_paper_at(drv, Int32(i))?.pointee,
                  let kw = tm_drv_paper_keyword(drv, Int32(i)),
                  let nm = tm_drv_paper_name(drv, Int32(i)) else { return nil }
            return Paper(keyword: String(cString: kw), name: String(cString: nm),
                         width: p.width_um, height: p.height_um, left: p.left_um, top: p.top_um,
                         right: p.right_um, bottom: p.bottom_um, inch: p.unit_inch != 0)
        }
    }

    func hasPaper(named n: String) -> Bool { papers.contains { $0.name.caseInsensitiveCompare(n) == .orderedSame } }

    @discardableResult
    func addPaper(name: String, width: Int, height: Int, left: Int, top: Int, right: Int, bottom: Int,
                  inch: Bool) -> String? {
        let rc = tm_drv_paper_add_simple(drv, name, width, height, left, top, right, bottom, inch ? 1 : 0)
        if rc != 0 { return String(cString: tm_drv_strerror(rc)) }
        touch()
        return nil
    }

    func deletePaper(_ name: String) {
        if tm_drv_paper_del(drv, name) == 0 { touch() }
    }

    // MARK: queues

    func refreshQueues() {
        Task.detached {
            let names = (try? Shell.run("/usr/bin/lpstat", ["-e"]))?.stdout
                .split(whereSeparator: \.isNewline).map(String.init) ?? []
            var ours: [String] = []
            for n in names {
                if let ppd = try? Self.fetchPPD(n), ppd.contains("rastertotmt20") { ours.append(n) }
            }
            await MainActor.run {
                self.queues = ours
                if !ours.contains(self.queue) { self.queue = ours.first ?? "" }
            }
        }
    }

    nonisolated static func fetchPPD(_ queue: String) throws -> String {
        guard let enc = queue.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed),
              let url = URL(string: "http://localhost:631/printers/\(enc).ppd") else {
            throw ShellError(message: "bad queue name")
        }
        if let data = try? Data(contentsOf: url) {
            return String(decoding: data, as: UTF8.self)
        }
        // cupsd not answering on localhost:631 - try the spool copy.
        return try String(contentsOfFile: "/private/etc/cups/ppd/\(queue).ppd", encoding: .isoLatin1)
    }

    func load() {
        guard !queue.isEmpty else { return }
        let q = queue
        busy = true
        Task.detached {
            let ppd = Result { try Self.fetchPPD(q) }
            let opts = (try? Shell.run("/usr/bin/lpoptions", ["-p", q]))?.stdout ?? ""
            await MainActor.run {
                self.busy = false
                switch ppd {
                case .success(let text):
                    tm_drv_parse_ppd(self.drv, text)
                    tm_drv_parse_ppd(self.saved, text)
                    self.parseJobDefaults(opts)
                    self.revision += 1
                    self.dirty = false
                    self.lastError = nil
                case .failure(let e):
                    self.lastError = "Cannot read the settings of \(q): \(e.localizedDescription)"
                }
            }
        }
    }

    private func parseJobDefaults(_ s: String) {
        func opt(_ k: String) -> String? {
            for tok in s.split(separator: " ") where tok.hasPrefix(k + "=") {
                return String(tok.dropFirst(k.count + 1))
            }
            return nil
        }
        orientation = opt("orientation-requested") == "4" ? "landscape" : "portrait"
        pageOrder = opt("outputorder") == "reverse" ? "reverse" : "normal"
        numberUp = Int(opt("number-up") ?? "1") ?? 1
        savedJobDefaults = (orientation, pageOrder, numberUp)
    }

    /// Cancel: back to what the queue holds.
    func revert() {
        tm_drv_free(drv)
        drv = tm_drv_copy(saved)!
        (orientation, pageOrder, numberUp) = savedJobDefaults
        revision += 1
        dirty = false
    }

    /// OK / Apply / Set: write the PPD (one administrator prompt).
    func apply(then: (() -> Void)? = nil) {
        guard !queue.isEmpty else { lastError = "No TM-T20III queue selected."; return }
        guard queue.range(of: "^[A-Za-z0-9_.@-]+$", options: .regularExpression) != nil else {
            lastError = "Unsupported queue name \(queue)"
            return
        }
        guard let tmpl = try? String(contentsOfFile: Self.templatePath, encoding: .isoLatin1) else {
            lastError = "Driver PPD not installed (\(Self.templatePath)). Run 'sudo make install' in macos/."
            return
        }
        guard let cText = tm_drv_generate_ppd(tmpl, drv) else {
            lastError = "The installed PPD is too old for Printing Preferences - reinstall the driver."
            return
        }
        let ppd = String(cString: cText)
        free(cText)
        let q = queue
        let jobArgs = ["-o", "orientation-requested=\(orientation == "landscape" ? 4 : 3)",
                       "-o", "outputorder=\(pageOrder)", "-o", "number-up=\(numberUp)"]
        busy = true
        Task.detached {
            let result = Result<Void, Error> {
                let path = NSTemporaryDirectory() + "tmqueue-\(UUID().uuidString).ppd"
                try ppd.write(toFile: path, atomically: true, encoding: .isoLatin1)
                defer { try? FileManager.default.removeItem(atPath: path) }
                try Shell.admin("/usr/sbin/lpadmin", ["-p", q, "-P", path] + jobArgs)
            }
            await MainActor.run {
                self.busy = false
                switch result {
                case .success:
                    tm_drv_free(self.saved)
                    self.saved = tm_drv_copy(self.drv)!
                    self.savedJobDefaults = (self.orientation, self.pageOrder, self.numberUp)
                    self.dirty = false
                    self.message = "Settings saved successfully."
                    self.lastError = nil
                    then?()
                case .failure(let e):
                    self.lastError = "Failed to save settings: \(e.localizedDescription)"
                }
            }
        }
    }
}

// MARK: - process helpers

struct ShellError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

enum Shell {
    struct Output { let status: Int32; let stdout: String; let stderr: String }

    static func run(_ exe: String, _ args: [String]) throws -> Output {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        let out = Pipe(), err = Pipe()
        p.standardOutput = out
        p.standardError = err
        p.standardInput = FileHandle.nullDevice
        try p.run()
        let o = out.fileHandleForReading.readDataToEndOfFile()
        let e = err.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return Output(status: p.terminationStatus, stdout: String(decoding: o, as: UTF8.self),
                      stderr: String(decoding: e, as: UTF8.self))
    }

    /// Run as the user first (admin users may change their own queues); if
    /// CUPS wants authentication, ask for it once with the system dialog.
    static func admin(_ exe: String, _ args: [String]) throws {
        if let r = try? run(exe, args), r.status == 0 { return }
        let quoted = ([exe] + args).map { "'" + $0.replacingOccurrences(of: "'", with: "'\\''") + "'" }
            .joined(separator: " ")
        let script = "do shell script \"\(quoted.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\""))\" with administrator privileges"
        let r = try run("/usr/bin/osascript", ["-e", script])
        if r.status != 0 {
            throw ShellError(message: r.stderr.isEmpty ? "lpadmin failed" : r.stderr.trimmingCharacters(in: .whitespacesAndNewlines))
        }
    }
}
