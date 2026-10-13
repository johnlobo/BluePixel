import AppKit
import CPCCore

// MARK: - Watch window

/// Expressions evaluated whenever the machine stops (and a few times a second while it runs).
/// Values that changed since the previous stop are shown in red, like the registers.
final class WatchController: NSObject, NSTableViewDataSource, NSTableViewDelegate, NSTextFieldDelegate {
    enum Format: Int, CaseIterable {
        case hex, decimal, binary, byteAt, wordAt, bytesAt, textAt
        var title: String { ["Hex", "Decimal", "Binary", "Byte at", "Word at", "8 bytes at", "Text at (16)"][rawValue] }
    }
    struct Watch { var expr: String; var format: Format; var value = ""; var atStop = ""; var error = false }

    unowned let debugger: DebuggerController
    let panel = makePanel("Watch", width: 520, height: 300)
    private let table = NSTableView()
    private var watches: [Watch] = []
    private var timer: Timer?

    init(debugger: DebuggerController) {
        self.debugger = debugger
        super.init()
        panel.styleMask.insert(.resizable)
        for (id, title, w) in [("expr", "Expression (double-click to edit)", 220.0), ("fmt", "Format", 110.0), ("value", "Value", 160.0)] {
            let c = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id)); c.title = title; c.width = w; table.addTableColumn(c)
        }
        table.dataSource = self; table.delegate = self; table.usesAlternatingRowBackgroundColors = true
        table.rowHeight = 22
        let scroll = NSScrollView(); scroll.documentView = table; scroll.hasVerticalScroller = true
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 180).isActive = true
        let help = makeLabel("e.g. (_snd_music_active) as Byte at, HL, IX+3, peek(#B8B4)"); help.textColor = .secondaryLabelColor; help.font = .systemFont(ofSize: 11)
        let buttons = stack([makeButton("Add…", self, #selector(add(_:))), makeButton("Remove", self, #selector(remove(_:))), NSView(), help], axis: .horizontal)
        let all = stack([scroll, buttons]); all.alignment = .width
        fill(panel, with: all, inset: 12)
        load()
        timer = Timer.scheduledTimer(withTimeInterval: 0.3, repeats: true) { [weak self] _ in
            guard let self = self, self.panel.isVisible, !debugger.emu.paused else { return }
            self.refresh()
        }
    }

    func show() { refresh(); panel.makeKeyAndOrderFront(nil) }

    // MARK: evaluation
    private func evaluate(_ w: Watch) -> (String, Bool) {
        let m = debugger.model
        let (v0, err) = m.eval(w.expr)
        guard let v = v0 else { return (err.isEmpty ? "?" : err, true) }
        let a = Int(v) & 0xFFFF
        switch w.format {
        case .hex: return (v & ~0xFF != 0 ? String(format: "#%04X", Int(v) & 0xFFFF) : String(format: "#%02X", Int(v) & 0xFF), false)
        case .decimal: return (String(v), false)
        case .binary: return (String(Int(v) & 0xFFFF, radix: 2).leftPad(v & ~0xFF != 0 ? 16 : 8), false)
        case .byteAt: let b = m.read(a); return (String(format: "#%02X  %d", b, b), false)
        case .wordAt: let wv = Int(m.read(a)) | Int(m.read(a + 1)) << 8; return (String(format: "#%04X  %d", wv, wv), false)
        case .bytesAt: return (m.readBlock(a, 8).map { String(format: "%02X", $0) }.joined(separator: " "), false)
        case .textAt: return (String(m.readBlock(a, 16).map { $0 >= 32 && $0 < 127 ? Character(UnicodeScalar($0)) : "." }), false)
        }
    }
    func refresh() {
        for i in watches.indices { let (s, e) = evaluate(watches[i]); watches[i].value = s; watches[i].error = e }
        table.reloadData()
    }
    /// Called when the machine stops: remember values so the next stop can show what changed.
    func noteStop() {
        refresh()
        for i in watches.indices { if watches[i].atStop.isEmpty { watches[i].atStop = watches[i].value } }
        table.reloadData()
        for i in watches.indices { watches[i].atStop = watches[i].value }
    }

    // MARK: table
    func numberOfRows(in t: NSTableView) -> Int { watches.count }
    func tableView(_ t: NSTableView, viewFor col: NSTableColumn?, row: Int) -> NSView? {
        let w = watches[row]
        switch col?.identifier.rawValue {
        case "expr":
            let f = NSTextField(string: w.expr); f.isBordered = false; f.drawsBackground = false; f.isEditable = true
            f.font = .monospacedSystemFont(ofSize: 11, weight: .regular); f.tag = row; f.delegate = self; return f
        case "fmt":
            let p = NSPopUpButton(frame: .zero, pullsDown: false); p.addItems(withTitles: Format.allCases.map(\.title))
            p.selectItem(at: w.format.rawValue); p.controlSize = .small; p.font = .systemFont(ofSize: 11); p.tag = row
            p.target = self; p.action = #selector(formatChanged(_:)); return p
        default:
            let f = makeLabel(w.value); f.font = .monospacedSystemFont(ofSize: 11, weight: .regular); f.lineBreakMode = .byTruncatingTail
            let changed = debugger.emu.paused && !w.atStop.isEmpty && w.atStop != w.value
            f.textColor = w.error ? .secondaryLabelColor : (changed ? .systemRed : .labelColor)
            return f
        }
    }
    func controlTextDidEndEditing(_ n: Notification) {
        guard let f = n.object as? NSTextField, f.tag < watches.count else { return }
        watches[f.tag].expr = f.stringValue; watches[f.tag].atStop = ""; save(); refresh()
    }
    @objc func formatChanged(_ p: NSPopUpButton) {
        guard p.tag < watches.count else { return }
        watches[p.tag].format = Format(rawValue: p.indexOfSelectedItem) ?? .hex; watches[p.tag].atStop = ""; save(); refresh()
    }
    @objc func add(_ s: Any?) {
        guard let t = promptText("Add Watch", "Expression (registers, symbols, (address), peek(x), arithmetic):", ""), !t.isEmpty else { return }
        // a bare symbol usually names a variable: show the byte stored there
        let fmt: Format = debugger.model.symbols().contains { $0.0.caseInsensitiveCompare(t) == .orderedSame } ? .byteAt : .hex
        watches.append(Watch(expr: t, format: fmt)); save(); refresh()
    }
    @objc func remove(_ s: Any?) {
        let r = table.selectedRow; guard r >= 0, r < watches.count else { return }
        watches.remove(at: r); save(); refresh()
    }

    // MARK: persistence (settings: [Debugger] Watch N = format|expression)
    private func save() {
        let st = debugger.emu.settings
        for k in st.keys("Debugger") where k.hasPrefix("Watch ") { st["Debugger", k] = nil }
        for (i, w) in watches.enumerated() { st["Debugger", "Watch \(i)"] = "\(w.format.rawValue)|\(w.expr)" }
    }
    private func load() {
        let st = debugger.emu.settings
        watches = st.keys("Debugger").filter { $0.hasPrefix("Watch ") }.sorted { (Int($0.dropFirst(6)) ?? 0) < (Int($1.dropFirst(6)) ?? 0) }.compactMap { k in
            guard let v = st["Debugger", k], let bar = v.firstIndex(of: "|") else { return nil }
            return Watch(expr: String(v[v.index(after: bar)...]), format: Format(rawValue: Int(v[..<bar]) ?? 0) ?? .hex)
        }
    }
}

private extension String {
    func leftPad(_ n: Int) -> String { count >= n ? self : String(repeating: "0", count: n - count) + self }
}

// MARK: - Log window (logpoint output)

final class LogController: NSObject {
    unowned let debugger: DebuggerController
    let panel = makePanel("Log", width: 600, height: 320)
    private let text = NSTextView()
    private var shown: UInt32 = 0
    private var timer: Timer?
    private let info = makeLabel("")

    init(debugger: DebuggerController) {
        self.debugger = debugger
        super.init()
        panel.styleMask.insert(.resizable)
        text.isEditable = false; text.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        let scroll = NSScrollView(); scroll.documentView = text; scroll.hasVerticalScroller = true
        text.autoresizingMask = [.width]; text.isVerticallyResizable = true
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 220).isActive = true
        info.font = .systemFont(ofSize: 11); info.textColor = .secondaryLabelColor
        info.stringValue = "Logpoints: right-click a line ▸ Add Logpoint…, or a message in the Breakpoints window. {expr} = hex, {expr:d} = decimal."
        let row = stack([info, NSView(), makeButton("Clear", self, #selector(clear(_:)))], axis: .horizontal)
        let all = stack([scroll, row]); all.alignment = .width
        fill(panel, with: all, inset: 12)
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in self?.poll() }
    }

    func show() { poll(); panel.makeKeyAndOrderFront(nil) }

    /// Appends lines logged since the last poll (lines that fell out of the ring are reported as skipped).
    func poll() {
        guard panel.isVisible else { return }
        let emu = debugger.emu
        emu.lock.lock()
        guard let d = emu.cpc.pointee.dbg else { emu.lock.unlock(); return }
        let total = dbg_log_total(d)
        if total < shown { shown = 0; text.string = "" }
        var add = ""
        if total - shown > UInt32(DBG_LOG_LINES) { add += "… \(total - shown - UInt32(DBG_LOG_LINES)) lines skipped …\n"; shown = total - UInt32(DBG_LOG_LINES) }
        while shown < total { if let l = dbg_log_line(d, shown) { add += String(cString: l) + "\n" }; shown += 1 }
        emu.lock.unlock()
        guard !add.isEmpty else { return }
        let atEnd = text.visibleRect.maxY >= text.bounds.maxY - 20
        text.textStorage?.append(NSAttributedString(string: add, attributes: [.font: NSFont.monospacedSystemFont(ofSize: 11, weight: .regular), .foregroundColor: NSColor.labelColor]))
        if (text.textStorage?.length ?? 0) > 400_000 { text.textStorage?.deleteCharacters(in: NSRange(location: 0, length: 100_000)) }
        if atEnd { text.scrollToEndOfDocument(nil) }
    }
    @objc func clear(_ s: Any?) {
        let emu = debugger.emu
        emu.lock.lock(); if let d = emu.cpc.pointee.dbg { dbg_log_clear(d) }; emu.lock.unlock()
        shown = 0; text.string = ""
    }
}
