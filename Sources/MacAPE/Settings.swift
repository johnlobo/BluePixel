import Foundation

/// WinAPE-compatible INI storage (WinAPE.ini / .wpf profiles). Keeps section/key order.
final class Settings {
    private(set) var order: [String] = []
    private var data: [String: [(key: String, value: String)]] = [:]

    subscript(section: String, key: String) -> String? {
        get { data[section]?.first { $0.key.lowercased() == key.lowercased() }?.value }
        set {
            if data[section] == nil { data[section] = []; order.append(section) }
            if let i = data[section]!.firstIndex(where: { $0.key.lowercased() == key.lowercased() }) {
                if let v = newValue { data[section]![i].value = v } else { data[section]!.remove(at: i) }
            } else if let v = newValue { data[section]!.append((key, v)) }
        }
    }

    func bool(_ s: String, _ k: String, _ def: Bool) -> Bool {
        guard let v = self[s, k]?.lowercased() else { return def }
        return v == "true" || v == "1" || v == "yes"
    }
    func int(_ s: String, _ k: String, _ def: Int) -> Int { Int(self[s, k] ?? "") ?? def }
    func string(_ s: String, _ k: String, _ def: String = "") -> String { self[s, k] ?? def }
    func set(_ s: String, _ k: String, _ v: Bool) { self[s, k] = v ? "true" : "false" }
    func set(_ s: String, _ k: String, _ v: Int) { self[s, k] = String(v) }
    func set(_ s: String, _ k: String, _ v: String) { self[s, k] = v }

    func keys(_ section: String) -> [String] { data[section]?.map { $0.key } ?? [] }

    func merge(_ other: Settings) {
        for s in other.order { for (k, v) in other.data[s] ?? [] { self[s, k] = v } }
    }

    static func parse(_ text: String) -> Settings {
        let st = Settings()
        var section = ""
        for raw in text.components(separatedBy: .newlines) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix(";") { continue }
            if line.hasPrefix("[") && line.hasSuffix("]") { section = String(line.dropFirst().dropLast()); if st.data[section] == nil { st.data[section] = []; st.order.append(section) }; continue }
            if let eq = line.firstIndex(of: "=") {
                let k = String(line[..<eq]).trimmingCharacters(in: .whitespaces)
                let v = String(line[line.index(after: eq)...]).trimmingCharacters(in: .whitespaces)
                st[section, k] = v
            }
        }
        return st
    }

    static func load(_ url: URL) -> Settings? {
        guard let d = try? Data(contentsOf: url) else { return nil }
        return parse(String(decoding: d, as: UTF8.self))
    }

    func serialize() -> String {
        var out = ""
        for s in order {
            out += "[\(s)]\n"
            for (k, v) in data[s] ?? [] { out += "\(k)=\(v)\n" }
        }
        return out
    }

    func save(_ url: URL) {
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? serialize().write(to: url, atomically: true, encoding: .utf8)
    }
}

enum AppPaths {
    static let support: URL = {
        let u = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("MacAPE")
        try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }()
    static var ini: URL { support.appendingPathComponent("MacAPE.ini") }
    static var profiles: URL { support.appendingPathComponent("Profile") }
    static var roms: URL { support.appendingPathComponent("ROM") }
}
