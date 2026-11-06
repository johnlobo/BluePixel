import Foundation
import CPCCore

/// WinAPE-compatible command line:
///   BluePixel [file…] [/A[:prog|:|CPM]] [/T:autotype.txt] [/SN:snapshot.sna] [/SYM:symbols]
/// Options may also be written -A, --a, /a=prog …; a leading "/" is only an option when the
/// name is one of the above, so absolute paths still work. Plain files are opened by type
/// (discs go to drive A, a second disc to drive B).
struct LaunchOptions {
    var files: [URL] = []
    var autoRun: String?            // "" = pick from the catalogue, "|CPM" or a program name
    var typeFile: URL?
    var snapshot: URL?
    var symbols: URL?
    // test/automation hooks
    var typed: String?, shot: String?, shotWin: String?, panel: String?
    var after = 4.0, panelAt = 1.0
    var help = false
    var errors: [String] = []

    static let usage = """
    Usage: BluePixel [options] [disc.dsk] [tape.cdt] [snapshot.sna] [cartridge.cpr] [profile.wpf]
      /A              run the first program on the disc in drive A
      /A:name         run "name" from drive A   (/A:|CPM boots CP/M)
      /T:file         type the contents of an Auto-Type text file
      /SN:file        load and start a snapshot
      /SYM:file       load assembler/debugger symbols (.noi, .sym, Maxam/WinAPE lists)
                      (/T, /SN and /SYM also accept the file as the next argument)
      --type text     type text (~ is Return)
      -h, --help      show this help
    """

    init(_ args: [String]) {
        var i = 0
        var missing: [String] = []
        func url(_ p: String) -> URL {
            let u = URL(fileURLWithPath: (p as NSString).expandingTildeInPath)
            if !FileManager.default.fileExists(atPath: u.path) { missing.append(p) }
            return u
        }
        while i < args.count {
            let a = args[i]; i += 1
            // Value-taking automation flags (--flag value).
            if ["--shot", "--after", "--panel", "--panelat", "--shotwin", "--type"].contains(a) {
                guard i < args.count else { errors.append("\(a) needs a value"); continue }
                let v = args[i]; i += 1
                switch a {
                case "--shot": shot = v
                case "--after": after = Double(v) ?? 4
                case "--panel": panel = v
                case "--panelat": panelAt = Double(v) ?? 1
                case "--shotwin": shotWin = v
                default: typed = v
                }
                continue
            }
            if a == "-h" || a == "--help" || a == "/?" { help = true; continue }
            if a.hasPrefix("-psn_") || a.hasPrefix("-NS") || a.hasPrefix("-Apple") { if i < args.count, !args[i].hasPrefix("-") { i += 1 }; continue }
            if let (name, given) = LaunchOptions.option(a) {
                var value = given
                // "/SYM file" (value as the next argument) is accepted as well as "/SYM:file".
                if value == nil, ["T", "SN", "SYM"].contains(name), i < args.count, LaunchOptions.option(args[i]) == nil, !args[i].hasPrefix("--") { value = args[i]; i += 1 }
                switch name {
                case "A": autoRun = value ?? ""
                case "T": if let v = value { typeFile = url(v) } else { errors.append("/T needs a file name") }
                case "SN": if let v = value { snapshot = url(v) } else { errors.append("/SN needs a file name") }
                case "SYM": if let v = value { symbols = url(v) } else { errors.append("/SYM needs a file name") }
                case "SHUTDOWN": break     // Windows-only in WinAPE
                default: break
                }
                continue
            }
            if a.hasPrefix("-") { errors.append("Unknown option \(a)"); continue }
            files.append(url(a))
        }
        errors += missing.map { "No such file: \($0)" }
        let gone = Set(missing.map { URL(fileURLWithPath: ($0 as NSString).expandingTildeInPath).path })
        files.removeAll { gone.contains($0.path) }
        if let u = typeFile, gone.contains(u.path) { typeFile = nil }
        if let u = snapshot, gone.contains(u.path) { snapshot = nil }
        if let u = symbols, gone.contains(u.path) { symbols = nil }
    }

    /// Recognises /NAME, /NAME:value, -NAME=value, --name … for the WinAPE option names.
    private static func option(_ a: String) -> (String, String?)? {
        var body = Substring(a)
        if body.hasPrefix("--") { body = body.dropFirst(2) } else if body.hasPrefix("/") || body.hasPrefix("-") { body = body.dropFirst() } else { return nil }
        let sep = body.firstIndex { $0 == ":" || $0 == "=" }
        let name = (sep.map { body[..<$0] } ?? body).uppercased()
        guard ["A", "T", "SN", "SYM", "SHUTDOWN"].contains(name) else { return nil }
        let value = sep.map { String(body[body.index(after: $0)...]) }
        return (name, value?.isEmpty == true ? nil : value)
    }
}

extension Emulator {
    /// Catalogue of the disc in a drive (user 0 files, first extent), or nil without a readable disc.
    func catalogue(drive: Int) -> [DskFile]? {
        lock.lock(); defer { lock.unlock() }
        guard let d = fdc_disc(cpc.pointee.fdc, Int32(drive)) else { return nil }
        var buf = [DskFile](repeating: DskFile(), count: 64)
        let n = Int(dsk_catalogue(d, &buf, 64))
        return n < 0 ? nil : Array(buf.prefix(n))
    }
    func isCPMBoot(drive: Int) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard let d = fdc_disc(cpc.pointee.fdc, Int32(drive)) else { return false }
        return dsk_is_cpm_boot(d)
    }

    /// The command WinAPE's /A types: RUN"name for a program, |CPM for a CP/M disc.
    func autoRunCommand(_ requested: String) -> String? {
        if requested.uppercased() == "|CPM" { return "|CPM\n" }
        if !requested.isEmpty { return "RUN\"\(requested)\n" }
        let files = (catalogue(drive: 0) ?? []).filter { $0.user == 0 }
        func s(_ t: (CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar)) -> String { withUnsafeBytes(of: t) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) } }
        func e(_ t: (CChar, CChar, CChar, CChar)) -> String { withUnsafeBytes(of: t) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) } }
        let named = files.map { (name: s($0.name), ext: e($0.ext), system: $0.system) }
        // Prefer a visible BASIC program, then a binary, then a file without extension.
        for ext in ["BAS", "BIN", ""] {
            if let f = named.first(where: { $0.ext == ext && !$0.system }) ?? named.first(where: { $0.ext == ext }) {
                return "RUN\"\(f.name)\(ext.isEmpty ? "" : "." + ext)\n"
            }
        }
        if files.isEmpty && isCPMBoot(drive: 0) { return "|CPM\n" }
        return nil
    }
}
