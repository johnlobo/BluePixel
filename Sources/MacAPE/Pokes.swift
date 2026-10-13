import AppKit
import CPCCore

// MARK: - WINAPE.POK database
//
// File layout (little-endian, strings are length-prefixed Latin-1):
//   "WPOK" u16 header
//   repeated until EOF: game
//     str name, u16 identLen, u16 identAddr, identLen bytes, u8 pokeCount
//     pokeCount x: str description, str comment, u8 valueType, u8 reversed, u8 ramConfig, u8 blockCount
//       blockCount x: u16 count, u16 address, count x u16 value (0xFFFF = entered by the user, else the byte)

struct PokeBlock { var address: UInt16; var values: [UInt16] }

struct Poke {
    enum ValueType: Int, CaseIterable { case decimal, hex, bcd, numeric, decimalASCII, long, string
        var title: String { ["Decimal", "Hex", "BCD", "Numeric (Zero based)", "Decimal (ASCII)", "Long", "String"][rawValue] }
    }
    var description = "", comment = ""
    var type = ValueType.decimal
    var reversed = false
    var ramConfig: UInt8 = 0xC0
    var blocks: [PokeBlock] = []

    /// Number of bytes the user has to supply.
    var inputCount: Int { blocks.reduce(0) { $0 + $1.values.filter { $0 == 0xFFFF }.count } }

    /// Converts the text entered by the user into `count` bytes, or nil if it is not valid for the type.
    func encode(_ text: String, count n: Int) -> [UInt8]? {
        let t = text.trimmingCharacters(in: .whitespaces)
        func digits(_ width: Int) -> [UInt8]? {
            guard !t.isEmpty, t.count <= width, t.allSatisfy({ $0.isASCII && $0.isNumber }) else { return nil }
            return (String(repeating: "0", count: width - t.count) + t).map { UInt8($0.asciiValue! - 48) }
        }
        func littleEndian(_ v: UInt64?) -> [UInt8]? {
            guard let v = v, n <= 8, n == 8 || v < (UInt64(1) << (8 * UInt64(n))) else { return nil }
            return (0..<n).map { UInt8((v >> (8 * UInt64($0))) & 0xFF) }
        }
        var out: [UInt8]?
        switch type {
        case .decimal, .long: out = littleEndian(UInt64(t))
        case .hex: out = littleEndian(UInt64(t, radix: 16))
        case .bcd: out = digits(2 * n).map { d in (0..<n).map { d[2 * $0] << 4 | d[2 * $0 + 1] } }
        case .numeric: out = digits(n)
        case .decimalASCII: out = digits(n)?.map { $0 + 48 }
        case .string:
            let b = Array(t.unicodeScalars.map { UInt8(truncatingIfNeeded: $0.value) }.prefix(n))
            out = b + Array(repeating: 0x20, count: n - b.count)
        }
        return reversed ? out?.reversed() : out
    }

    /// Data as editable text: one block per line, "ADDR: XX XX ?? …".
    var dataText: String {
        blocks.map { b in String(format: "%04X:", b.address) + b.values.map { $0 == 0xFFFF ? " ??" : String(format: " %02X", $0 & 0xFF) }.joined() }.joined(separator: "\n")
    }
    static func parseData(_ text: String) -> [PokeBlock]? {
        var out: [PokeBlock] = []
        for line in text.split(whereSeparator: \.isNewline) {
            let parts = line.split(separator: ":", maxSplits: 1)
            guard parts.count == 2, let a = UInt16(parts[0].trimmingCharacters(in: .whitespaces), radix: 16) else { return nil }
            var vals: [UInt16] = []
            for tok in parts[1].split(whereSeparator: { $0 == " " || $0 == "," }) {
                if tok.allSatisfy({ $0 == "?" }) { vals += Array(repeating: 0xFFFF, count: max(1, tok.count / 2)) }
                else if let v = UInt8(tok, radix: 16) { vals.append(UInt16(v)) } else { return nil }
            }
            if vals.isEmpty { return nil }
            out.append(PokeBlock(address: a, values: vals))
        }
        return out.isEmpty ? nil : out
    }
}

struct PokeGame {
    var name = ""
    var identAddress: UInt16 = 0
    var ident: [UInt8] = []
    var pokes: [Poke] = []
}

final class PokeDatabase {
    var games: [PokeGame] = []
    private var header: UInt16 = 0
    let url: URL

    init(url: URL) { self.url = url; if let d = try? Data(contentsOf: url) { _ = parse([UInt8](d)) } }

    private func parse(_ d: [UInt8]) -> Bool {
        guard d.count >= 6, d[0..<4].elementsEqual("WPOK".utf8) else { return false }
        var p = 4
        func u8() throws -> UInt8 { guard p < d.count else { throw CocoaError(.fileReadCorruptFile) }; p += 1; return d[p - 1] }
        func u16() throws -> UInt16 { let lo = try u8(); return UInt16(lo) | UInt16(try u8()) << 8 }
        func bytes(_ n: Int) throws -> [UInt8] { guard p + n <= d.count else { throw CocoaError(.fileReadCorruptFile) }; p += n; return Array(d[p - n..<p]) }
        func latin1() throws -> String { let b = try bytes(Int(try u8())); return String(bytes: b, encoding: .isoLatin1) ?? "" }
        do {
            header = try u16()
            var out: [PokeGame] = []
            while p < d.count {
                var g = PokeGame(); g.name = try latin1()
                let n = Int(try u16()); g.identAddress = try u16(); g.ident = try bytes(n)
                for _ in 0..<Int(try u8()) {
                    var k = Poke(); k.description = try latin1(); k.comment = try latin1()
                    k.type = Poke.ValueType(rawValue: Int(try u8())) ?? .decimal
                    k.reversed = try u8() != 0; k.ramConfig = try u8()
                    for _ in 0..<Int(try u8()) {
                        let c = Int(try u16()); let a = try u16()
                        k.blocks.append(PokeBlock(address: a, values: try (0..<c).map { _ in try u16() }))
                    }
                    g.pokes.append(k)
                }
                out.append(g)
            }
            games = out; return true
        } catch { return false }
    }

    func save() {
        var o: [UInt8] = Array("WPOK".utf8)
        func u16(_ v: UInt16) { o += [UInt8(v & 0xFF), UInt8(v >> 8)] }
        func str(_ s: String) { let b = Array((s.data(using: .isoLatin1, allowLossyConversion: true) ?? Data()).prefix(255)); o.append(UInt8(b.count)); o += b }
        u16(header)
        for g in games {
            str(g.name); u16(UInt16(g.ident.count)); u16(g.identAddress); o += g.ident
            o.append(UInt8(min(255, g.pokes.count)))
            for k in g.pokes.prefix(255) {
                str(k.description); str(k.comment); o += [UInt8(k.type.rawValue), k.reversed ? 1 : 0, k.ramConfig, UInt8(min(255, k.blocks.count))]
                for b in k.blocks.prefix(255) { u16(UInt16(b.values.count)); u16(b.address); b.values.forEach(u16) }
            }
        }
        try? Data(o).write(to: url)
    }

    /// The user's copy lives in Application Support; seeded from the WinAPE folder on first use.
    static func open(romFolder: URL) -> PokeDatabase {
        let dest = AppPaths.support.appendingPathComponent("WINAPE.POK")
        if !FileManager.default.fileExists(atPath: dest.path) {
            for c in [romFolder.deletingLastPathComponent().appendingPathComponent("WINAPE.POK"), URL(fileURLWithPath: NSHomeDirectory() + "/Documents/WinAPE20B2/WINAPE.POK")]
            where FileManager.default.fileExists(atPath: c.path) { try? FileManager.default.copyItem(at: c, to: dest); break }
        }
        return PokeDatabase(url: dest)
    }
}

// MARK: - Emulator access

extension Emulator {
    func peekRAM(_ cfg: UInt8, _ a: UInt16) -> UInt8 { lock.lock(); defer { lock.unlock() }; return cpc_bank_peek(cpc, cfg, a) }
    /// Index of the first game whose identifier matches the running program's RAM.
    func runningGame(in db: PokeDatabase) -> Int? {
        lock.lock(); defer { lock.unlock() }
        return db.games.firstIndex { g in
            !g.ident.isEmpty && g.ident.enumerated().allSatisfy { cpc_bank_peek(cpc, 0xC0, g.identAddress &+ UInt16($0.offset)) == $0.element }
        }
    }
    /// Applies a poke; `input` supplies the bytes for the "??" positions in order.
    func apply(_ k: Poke, input: [UInt8]) {
        lock.lock(); defer { lock.unlock() }
        var next = input.makeIterator()
        for b in k.blocks {
            for (i, v) in b.values.enumerated() {
                let byte = v == 0xFFFF ? (next.next() ?? 0) : UInt8(v & 0xFF)
                cpc_bank_poke(cpc, k.ramConfig, b.address &+ UInt16(i), byte)
            }
        }
    }
}

// MARK: - Pokes window

final class PokesController: NSObject, NSTableViewDataSource, NSTableViewDelegate {
    let emu: Emulator
    let db: PokeDatabase
    let panel = makePanel("Pokes", width: 560, height: 560)
    private let gamesTable = NSTableView(), pokesTable = NSTableView()
    private var order: [Int] = []          // games sorted by name
    private var running: Int?
    private let runningLabel = makeLabel("Running game: (unknown)")
    private var editGameButton: NSButton!, delGameButton: NSButton!, pokeButton: NSButton!, editPokeButton: NSButton!, delPokeButton: NSButton!, addPokeButton: NSButton!

    init(emu: Emulator) {
        self.emu = emu
        db = PokeDatabase.open(romFolder: emu.romFolder)
        super.init()
        panel.styleMask.insert(.resizable)
        func table(_ t: NSTableView, _ cols: [(String, String, CGFloat)], height: CGFloat) -> NSScrollView {
            for c in cols { let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(c.0)); col.title = c.1; col.width = c.2; t.addTableColumn(col) }
            t.dataSource = self; t.delegate = self; t.usesAlternatingRowBackgroundColors = true; t.target = self
            let s = NSScrollView(); s.documentView = t; s.hasVerticalScroller = true; s.translatesAutoresizingMaskIntoConstraints = false
            s.heightAnchor.constraint(greaterThanOrEqualToConstant: height).isActive = true; return s
        }
        let gs = table(gamesTable, [("name", "Game", 360), ("count", "Pokes", 60)], height: 200)
        let ps = table(pokesTable, [("desc", "Poke", 300), ("comment", "Comment", 180)], height: 160)
        gamesTable.doubleAction = #selector(editGame(_:)); pokesTable.doubleAction = #selector(doPoke(_:))
        editGameButton = makeButton("Edit…", self, #selector(editGame(_:))); delGameButton = makeButton("Delete", self, #selector(deleteGame(_:)))
        pokeButton = makeButton("Poke", self, #selector(doPoke(_:))); pokeButton.keyEquivalent = "\r"
        addPokeButton = makeButton("Add…", self, #selector(addPoke(_:)))
        editPokeButton = makeButton("Edit…", self, #selector(editPoke(_:))); delPokeButton = makeButton("Delete", self, #selector(deletePoke(_:)))
        let gameButtons = stack([makeButton("Running Game", self, #selector(selectRunning(_:))), runningLabel, NSView(), makeButton("Add…", self, #selector(addGame(_:))), editGameButton, delGameButton], axis: .horizontal)
        let pokeButtons = stack([pokeButton, NSView(), addPokeButton, editPokeButton, delPokeButton], axis: .horizontal)
        let all = stack([gs, gameButtons, ps, pokeButtons]); all.alignment = .width
        fill(panel, with: all, inset: 12)
        resort()
    }

    func show() { resort(); selectRunning(nil); panel.makeKeyAndOrderFront(nil) }

    private var selectedGame: Int? { let r = gamesTable.selectedRow; return r >= 0 && r < order.count ? order[r] : nil }
    private var selectedPoke: Int? { guard let g = selectedGame else { return nil }; let r = pokesTable.selectedRow; return r >= 0 && r < db.games[g].pokes.count ? r : nil }

    private func resort(select game: Int? = nil) {
        let keep = game ?? selectedGame
        order = db.games.indices.sorted { db.games[$0].name.localizedCaseInsensitiveCompare(db.games[$1].name) == .orderedAscending }
        gamesTable.reloadData()
        if let k = keep, let r = order.firstIndex(of: k) { gamesTable.selectRowIndexes([r], byExtendingSelection: false); gamesTable.scrollRowToVisible(r) }
        pokesTable.reloadData(); updateButtons()
    }
    private func updateButtons() {
        let g = selectedGame != nil, p = selectedPoke != nil
        editGameButton.isEnabled = g; delGameButton.isEnabled = g; addPokeButton.isEnabled = g
        pokeButton.isEnabled = p; editPokeButton.isEnabled = p; delPokeButton.isEnabled = p
    }

    // MARK: table
    func numberOfRows(in t: NSTableView) -> Int { t === gamesTable ? order.count : selectedGame.map { db.games[$0].pokes.count } ?? 0 }
    func tableView(_ t: NSTableView, viewFor col: NSTableColumn?, row: Int) -> NSView? {
        let id = col?.identifier.rawValue ?? ""
        let text: String
        var bold = false
        if t === gamesTable {
            let g = db.games[order[row]]; bold = order[row] == running
            text = id == "name" ? g.name : "\(g.pokes.count)"
        } else {
            guard let gi = selectedGame else { return nil }
            let k = db.games[gi].pokes[row]; text = id == "desc" ? k.description : k.comment
        }
        let tf = makeLabel(text, bold: bold); tf.lineBreakMode = .byTruncatingTail; return tf
    }
    func tableViewSelectionDidChange(_ n: Notification) {
        if (n.object as? NSTableView) === gamesTable { pokesTable.reloadData() }
        updateButtons()
    }

    // MARK: actions
    @objc func selectRunning(_ s: Any?) {
        running = emu.runningGame(in: db)
        runningLabel.stringValue = "Running game: " + (running.map { db.games[$0].name } ?? "(not recognised)")
        resort(select: running)
    }
    @objc func doPoke(_ s: Any?) {
        guard let gi = selectedGame, let pi = selectedPoke else { return }
        let k = db.games[gi].pokes[pi]
        let n = k.inputCount
        var input: [UInt8] = []
        if n > 0 {
            while true {
                let hint = k.comment.isEmpty ? k.type.title : "\(k.comment)  (\(k.type.title))"
                guard let t = promptText(k.description, hint) else { return }
                if let b = k.encode(t, count: n) { input = b; break }
                alertSheet("Invalid value", "\"\(t)\" is not a valid \(k.type.title) value for \(n) byte(s).")
            }
        }
        emu.apply(k, input: input)
    }
    private func alertSheet(_ title: String, _ msg: String) { let a = NSAlert(); a.messageText = title; a.informativeText = msg; a.runModal() }

    @objc func addGame(_ s: Any?) {
        guard let g = GameEditor(emu: emu, game: PokeGame()).run() else { return }
        db.games.append(g); db.save(); resort(select: db.games.count - 1)
    }
    @objc func editGame(_ s: Any?) {
        guard let gi = selectedGame, let g = GameEditor(emu: emu, game: db.games[gi]).run() else { return }
        db.games[gi] = g; db.save(); resort(select: gi)
    }
    @objc func deleteGame(_ s: Any?) {
        guard let gi = selectedGame, confirm("Delete \(db.games[gi].name) and its pokes?") else { return }
        db.games.remove(at: gi); db.save(); running = nil; resort()
    }
    @objc func addPoke(_ s: Any?) {
        guard let gi = selectedGame, let k = PokeEditor(poke: Poke()).run() else { return }
        db.games[gi].pokes.append(k); db.save(); pokesTable.reloadData(); gamesTable.reloadData(); updateButtons()
    }
    @objc func editPoke(_ s: Any?) {
        guard let gi = selectedGame, let pi = selectedPoke, let k = PokeEditor(poke: db.games[gi].pokes[pi]).run() else { return }
        db.games[gi].pokes[pi] = k; db.save(); pokesTable.reloadData()
    }
    @objc func deletePoke(_ s: Any?) {
        guard let gi = selectedGame, let pi = selectedPoke, confirm("Delete poke \"\(db.games[gi].pokes[pi].description)\"?") else { return }
        db.games[gi].pokes.remove(at: pi); db.save(); pokesTable.reloadData(); gamesTable.reloadData(); updateButtons()
    }
    private func confirm(_ q: String) -> Bool {
        let a = NSAlert(); a.messageText = q; a.addButton(withTitle: "Delete"); a.addButton(withTitle: "Cancel"); return a.runModal() == .alertFirstButtonReturn
    }
}

private func formRow(_ label: String, _ v: NSView) -> NSStackView {
    let l = makeLabel(label); l.alignment = .right; l.widthAnchor.constraint(equalToConstant: 120).isActive = true
    return stack([l, v], axis: .horizontal)
}
private func hexBytes(_ b: [UInt8]) -> String { b.map { String(format: "%02X", $0) }.joined(separator: " ") }

/// Name + identifier (RAM area that recognises the running game).
private final class GameEditor: NSObject {
    let emu: Emulator
    var game: PokeGame
    let name = NSTextField(), address = NSTextField(), length = NSTextField(), ident = NSTextField()
    init(emu: Emulator, game: PokeGame) { self.emu = emu; self.game = game }

    func run() -> PokeGame? {
        name.stringValue = game.name; address.stringValue = String(format: "%04X", game.identAddress)
        length.stringValue = "\(game.ident.isEmpty ? 8 : game.ident.count)"; ident.stringValue = hexBytes(game.ident)
        for f in [name, ident] { f.widthAnchor.constraint(equalToConstant: 300).isActive = true }
        for f in [address, length] { f.widthAnchor.constraint(equalToConstant: 60).isActive = true }
        let read = makeButton("Read from Memory", self, #selector(readMemory(_:)))
        let clear = makeButton("Clear", self, #selector(clear(_:)))
        let v = stack([formRow("Name:", name), formRow("Identifier Address:", stack([address, makeLabel("Length:"), length, read, clear], axis: .horizontal)), formRow("Identifier:", ident)])
        v.frame = NSRect(x: 0, y: 0, width: 480, height: v.fittingSize.height); v.translatesAutoresizingMaskIntoConstraints = true
        let a = NSAlert(); a.messageText = game.name.isEmpty ? "Add Game" : "Edit Game"
        a.informativeText = "Pick 6–20 bytes of RAM (bank C0) that only this game contains, such as its title text."
        a.accessoryView = v; a.addButton(withTitle: "OK"); a.addButton(withTitle: "Cancel"); a.window.initialFirstResponder = name
        while a.runModal() == .alertFirstButtonReturn {
            let bytes = ident.stringValue.split(separator: " ").compactMap { UInt8($0, radix: 16) }
            guard !name.stringValue.isEmpty, let addr = UInt16(address.stringValue, radix: 16),
                  bytes.count == ident.stringValue.split(separator: " ").count else { NSSound.beep(); continue }
            game.name = name.stringValue; game.identAddress = addr; game.ident = bytes
            return game
        }
        return nil
    }
    @objc func readMemory(_ s: Any?) {
        guard let a = UInt16(address.stringValue, radix: 16), let n = Int(length.stringValue), n > 0, n <= 255 else { NSSound.beep(); return }
        ident.stringValue = hexBytes((0..<n).map { emu.peekRAM(0xC0, a &+ UInt16($0)) })
    }
    @objc func clear(_ s: Any?) { ident.stringValue = "" }
}

/// Description, comment, value type, RAM bank and data lines.
private final class PokeEditor {
    var poke: Poke
    init(poke: Poke) { self.poke = poke }

    func run() -> Poke? {
        let desc = NSTextField(string: poke.description), comment = NSTextField(string: poke.comment)
        for f in [desc, comment] { f.widthAnchor.constraint(equalToConstant: 300).isActive = true }
        let type = NSPopUpButton(frame: .zero, pullsDown: false); type.addItems(withTitles: Poke.ValueType.allCases.map(\.title)); type.selectItem(at: poke.type.rawValue)
        let reversed = makeCheck("Reversed"); reversed.state = poke.reversed ? .on : .off
        let ram = NSPopUpButton(frame: .zero, pullsDown: false)
        ram.addItems(withTitles: (0xC0...0xFF).map { String(format: "%02X", $0) }); ram.selectItem(at: Int(poke.ramConfig) - 0xC0)
        let data = NSTextView(); data.font = .monospacedSystemFont(ofSize: 12, weight: .regular); data.string = poke.dataText
        data.isAutomaticQuoteSubstitutionEnabled = false; data.isRichText = false
        let scroll = NSScrollView(); scroll.documentView = data; scroll.hasVerticalScroller = true; scroll.borderType = .bezelBorder
        data.autoresizingMask = [.width]; data.isVerticallyResizable = true
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.widthAnchor.constraint(equalToConstant: 300).isActive = true; scroll.heightAnchor.constraint(equalToConstant: 110).isActive = true
        let v = stack([formRow("Description:", desc), formRow("Comment:", comment), formRow("Value Type:", stack([type, reversed], axis: .horizontal)),
                       formRow("RAM Bank:", ram), formRow("Data:", scroll)])
        v.frame = NSRect(x: 0, y: 0, width: 440, height: v.fittingSize.height); v.translatesAutoresizingMaskIntoConstraints = true
        data.frame = NSRect(x: 0, y: 0, width: 296, height: 110)
        let a = NSAlert(); a.messageText = poke.description.isEmpty ? "Add Poke" : "Edit Poke"
        a.informativeText = "One line per address: \"ADDR: XX XX …\" in hex. Use ?? for each byte the user enters."
        a.accessoryView = v; a.addButton(withTitle: "OK"); a.addButton(withTitle: "Cancel"); a.window.initialFirstResponder = desc
        while a.runModal() == .alertFirstButtonReturn {
            guard !desc.stringValue.isEmpty, let blocks = Poke.parseData(data.string) else { NSSound.beep(); continue }
            poke.description = desc.stringValue; poke.comment = comment.stringValue
            poke.type = Poke.ValueType(rawValue: type.indexOfSelectedItem) ?? .decimal
            poke.reversed = reversed.state == .on; poke.ramConfig = UInt8(0xC0 + ram.indexOfSelectedItem); poke.blocks = blocks
            return poke
        }
        return nil
    }
}
