import AppKit
import CPCCore
import UniformTypeIdentifiers

/// Generic window with one table and a row of buttons.
class TableWindowController: NSObject, NSTableViewDataSource, NSTableViewDelegate, NSTextFieldDelegate {
    let panel: NSPanel
    let table = NSTableView()
    var columns: [(id: String, title: String, width: CGFloat, editable: Bool)] = []
    var rows: [[String]] = []
    var onEdit: ((Int, String, String) -> Void)?
    let buttonBar = NSStackView()
    let contentStack = NSStackView()

    init(title: String, width: CGFloat = 560, height: CGFloat = 320) {
        panel = makePanel(title, width: width, height: height)
        super.init()
        let scroll = NSScrollView(); scroll.documentView = table; scroll.hasVerticalScroller = true
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 160).isActive = true
        table.dataSource = self; table.delegate = self; table.usesAlternatingRowBackgroundColors = true
        buttonBar.spacing = 6
        contentStack.orientation = .vertical; contentStack.alignment = .leading; contentStack.spacing = 8
        contentStack.addArrangedSubview(scroll); contentStack.addArrangedSubview(buttonBar)
        scroll.widthAnchor.constraint(equalTo: contentStack.widthAnchor).isActive = true
        fill(panel, with: contentStack)
    }

    func setColumns(_ cols: [(id: String, title: String, width: CGFloat, editable: Bool)]) {
        columns = cols
        for c in table.tableColumns { table.removeTableColumn(c) }
        for c in cols { let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(c.id)); col.title = c.title; col.width = c.width; table.addTableColumn(col) }
    }
    func addButton(_ title: String, _ action: Selector) { buttonBar.addArrangedSubview(makeButton(title, self, action)) }
    func show() { reload(); panel.makeKeyAndOrderFront(nil) }
    func reload() { rows = buildRows(); table.reloadData() }
    func buildRows() -> [[String]] { [] }

    func numberOfRows(in tableView: NSTableView) -> Int { rows.count }
    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard let id = tableColumn?.identifier.rawValue, let ci = columns.firstIndex(where: { $0.id == id }), row < rows.count else { return nil }
        let tf = NSTextField(string: ci < rows[row].count ? rows[row][ci] : "")
        tf.isBordered = false; tf.drawsBackground = false; tf.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        tf.isEditable = columns[ci].editable; tf.delegate = self; tf.identifier = NSUserInterfaceItemIdentifier("\(row):\(id)")
        return tf
    }
    func controlTextDidEndEditing(_ obj: Notification) {
        guard let tf = obj.object as? NSTextField, let ident = tf.identifier?.rawValue else { return }
        let parts = ident.split(separator: ":"); guard parts.count == 2, let row = Int(parts[0]) else { return }
        onEdit?(row, String(parts[1]), tf.stringValue)
    }
    var selectedRow: Int { table.selectedRow }
}

// MARK: - Breakpoints
final class BreakpointsController: TableWindowController {
    let emu: Emulator; weak var debugger: DebuggerController?
    private let segment = NSSegmentedControl(labels: ["Code", "Memory", "Input/Output", "Change"], trackingMode: .selectOne, target: nil, action: nil)
    private var indices: [Int] = []

    init(emu: Emulator, debugger: DebuggerController) {
        self.emu = emu; self.debugger = debugger
        super.init(title: "Breakpoints", width: 780, height: 360)
        segment.selectedSegment = 0; segment.target = self; segment.action = #selector(tabChanged(_:))
        contentStack.insertArrangedSubview(segment, at: 0)
        configureColumns()
        for (t, s) in [("Clear", #selector(clearSelected(_:))), ("Clear All", #selector(clearAll(_:))), ("Add", #selector(add(_:))), ("Reset", #selector(resetSel(_:))), ("Reset All", #selector(resetAll(_:))), ("Load", #selector(load(_:))), ("Save", #selector(save(_:)))] { addButton(t, s) }
        onEdit = { [weak self] row, col, text in self?.edited(row, col, text) }
    }

    private func configureColumns() {
        setColumns([("addr", "Address", 150, false), ("type", "Type", 70, false), ("count", "Count", 60, false), ("cond", "Condition", 170, true), ("pass", "Pass", 44, true), ("log", "Log message (logpoint: don't stop)", 220, true)])
    }
    @objc func tabChanged(_ s: Any?) { reload() }

    private func withBP<T>(_ body: (UnsafeMutablePointer<DbgBreakpoint>) -> T) -> T {
        emu.lock.lock(); defer { emu.lock.unlock() }
        let d = dbg_attach(emu.cpc)!
        return withUnsafeMutablePointer(to: &d.pointee.bp) { $0.withMemoryRebound(to: DbgBreakpoint.self, capacity: Int(DBG_MAX_BP)) { body($0) } }
    }

    override func buildRows() -> [[String]] {
        let tab = segment.selectedSegment
        var out: [[String]] = []; indices = []
        let list: [(Int, DbgBreakpoint)] = withBP { arr in (0..<Int(DBG_MAX_BP)).compactMap { arr[$0].used ? ($0, arr[$0]) : nil } }
        for (i, b) in list {
            let group = b.type == BP_CODE ? 0 : (b.type == BP_IO ? 2 : (b.type == BP_CHANGE ? 3 : 1))
            guard group == tab else { continue }
            indices.append(i)
            var addr: String
            switch b.type {
            case BP_CODE: addr = debugger?.model.describe(Int(b.start)) ?? String(format: "%04X", b.start)
            case BP_IO: addr = String(format: "%04X/%04X", b.port, b.mask)
            case BP_CHANGE: addr = withUnsafePointer(to: b.expr) { $0.withMemoryRebound(to: CChar.self, capacity: 160) { String(cString: $0) } }
            default: addr = b.start == b.end ? String(format: "%04X", b.start) : String(format: "%04X-%04X", b.start, b.end)
            }
            let type = b.type == BP_CHANGE ? "Change" : b.type == BP_CODE ? "Code" : b.type == BP_MEM_READ ? "Read" : b.type == BP_MEM_WRITE ? "Write" : (b.on_in && b.on_out ? "In/Out" : b.on_in ? "In" : "Out")
            let cond = withUnsafePointer(to: b.cond) { $0.withMemoryRebound(to: CChar.self, capacity: 160) { String(cString: $0) } }
            let log = withUnsafePointer(to: b.log) { $0.withMemoryRebound(to: CChar.self, capacity: 160) { String(cString: $0) } }
            out.append([addr, log.isEmpty ? type : type + " (log)", b.pass_count > 0 ? "\(b.hits)/\(b.pass_count)" : "\(b.count)", cond, b.pass_count > 0 ? String(b.pass_count) : "", log])
        }
        return out
    }

    private func edited(_ row: Int, _ col: String, _ text: String) {
        guard row < indices.count else { return }
        let i = indices[row]
        if col == "log" { emu.lock.lock(); dbg_set_log(emu.cpc, Int32(i), text); emu.lock.unlock(); reload(); return }
        let cur: (String, Int) = withBP { arr in (withUnsafePointer(to: arr[i].cond) { $0.withMemoryRebound(to: CChar.self, capacity: 160) { String(cString: $0) } }, Int(arr[i].pass_count)) }
        var cond = cur.0, pass = cur.1
        if col == "cond" { cond = text } else if col == "pass" { pass = Int(text) ?? 0 }
        emu.lock.lock(); let ok = dbg_set_condition(emu.cpc, Int32(i), cond, Int32(pass)); emu.lock.unlock()
        if !ok { NSSound.beep() }
        reload()
    }

    @objc func clearSelected(_ s: Any?) {
        guard selectedRow >= 0, selectedRow < indices.count else { return }
        emu.lock.lock(); dbg_remove_bp(emu.cpc, Int32(indices[selectedRow])); emu.lock.unlock(); reload(); debugger?.refreshAll()
    }
    @objc func clearAll(_ s: Any?) { emu.lock.lock(); dbg_clear_bps(emu.cpc); emu.lock.unlock(); reload(); debugger?.refreshAll() }
    @objc func resetSel(_ s: Any?) { guard selectedRow >= 0, selectedRow < indices.count else { return }; let i = indices[selectedRow]; withBP { $0[i].hits = 0; $0[i].count = 0 }; reload() }
    @objc func resetAll(_ s: Any?) { withBP { a in for i in 0..<Int(DBG_MAX_BP) { a[i].hits = 0; a[i].count = 0 } }; reload() }

    @objc func add(_ s: Any?) {
        switch segment.selectedSegment {
        case 0:
            guard let t = promptText("Add Code Breakpoint", "Address or expression:"), let v = debugger?.model.eval(t).0 else { return }
            emu.lock.lock(); _ = dbg_add_code_bp(emu.cpc, UInt16(truncatingIfNeeded: v)); emu.lock.unlock()
        case 1:
            guard let t = promptText("Add Memory Breakpoint", "Range start,end (add 'r' and/or 'w' after a space, default rw):", "#4000,#4000 rw") else { return }
            let flags = t.split(separator: " ").dropFirst().joined().lowercased()
            let rng = (t.split(separator: " ").first ?? "").split(separator: ",").map { debugger?.model.eval(String($0)).0 }
            guard let s0 = rng.first ?? nil else { return }
            let e0 = rng.count > 1 ? (rng[1] ?? s0) : s0
            emu.lock.lock(); _ = dbg_add_mem_bp(emu.cpc, UInt16(truncatingIfNeeded: s0), UInt16(truncatingIfNeeded: e0), flags.isEmpty || flags.contains("r"), flags.isEmpty || flags.contains("w")); emu.lock.unlock()
        case 2: addIO()
        default:
            guard let t = promptText("Break on Change", "Stop when this changes — a register (A, HL…), peek(addr), word(addr) or any expression. Registers and fixed addresses are cheap; other expressions slow the emulation:", "A"), !t.isEmpty else { return }
            var err = [CChar](repeating: 0, count: 128)
            emu.lock.lock(); let i = dbg_add_change_bp(emu.cpc, t, &err, 128); emu.lock.unlock()
            if i < 0 { NSSound.beep(); let a = NSAlert(); a.messageText = "Cannot watch this expression"; a.informativeText = String(cString: err); a.runModal() }
        }
        reload(); debugger?.refreshAll()
    }

    private func addIO() {
        let a = NSAlert(); a.messageText = "Add Input/Output Breakpoint"
        let dev = NSPopUpButton(frame: .zero, pullsDown: false)
        let devices: [(String, String, String)] = [("User Defined Port", "0000", "FFFF"), ("Gate Array", "7F00", "C000"), ("CRTC", "BC00", "C000"), ("ROM Select", "DF00", "2000"),
                                                    ("PPI", "F400", "0800"), ("Printer", "EF00", "1000"), ("FDC", "FA7E", "0480"), ("Expansion", "F800", "0400")]
        dev.addItems(withTitles: devices.map { $0.0 })
        let port = NSTextField(string: "7F00"), mask = NSTextField(string: "C000")
        let inB = NSButton(checkboxWithTitle: "Input", target: nil, action: nil), outB = NSButton(checkboxWithTitle: "Output", target: nil, action: nil)
        inB.state = .on; outB.state = .on
        let grid = NSGridView(views: [[makeLabel("Type:"), dev], [makeLabel("Port:"), port], [makeLabel("Mask:"), mask], [inB, outB]])
        grid.frame = NSRect(x: 0, y: 0, width: 300, height: 110)
        class H: NSObject { var dev: NSPopUpButton!; var port: NSTextField!; var mask: NSTextField!; let d: [(String, String, String)]
            init(_ d: [(String, String, String)]) { self.d = d }
            @objc func pick(_ s: Any?) { let e = d[dev.indexOfSelectedItem]; port.stringValue = e.1; mask.stringValue = e.2 } }
        let h = H(devices); h.dev = dev; h.port = port; h.mask = mask; dev.target = h; dev.action = #selector(H.pick(_:))
        a.accessoryView = grid; a.addButton(withTitle: "Add"); a.addButton(withTitle: "Cancel")
        guard a.runModal() == .alertFirstButtonReturn else { return }
        var p = port.stringValue.uppercased(), m = UInt16(mask.stringValue, radix: 16) ?? 0xFFFF
        if p.contains("X") { var mm: UInt16 = 0; var pp = ""; for ch in p { mm = mm << 4 | (ch == "X" ? 0 : 0xF); pp.append(ch == "X" ? "0" : ch) }; p = pp; m = mm }
        emu.lock.lock(); _ = dbg_add_io_bp(emu.cpc, UInt16(p, radix: 16) ?? 0, m, inB.state == .on, outB.state == .on); emu.lock.unlock()
    }

    @objc func load(_ s: Any?) {
        let p = NSOpenPanel(); guard p.runModal() == .OK, let u = p.url, let text = try? String(contentsOf: u, encoding: .utf8) else { return }
        emu.lock.lock(); dbg_clear_bps(emu.cpc)
        for line in text.split(separator: "\n") {
            let f = line.split(separator: ";", omittingEmptySubsequences: false).map(String.init); guard f.count >= 9 else { continue }
            let t = f[0], a = UInt16(f[1], radix: 16) ?? 0, b = UInt16(f[2], radix: 16) ?? 0
            var idx: Int32 = -1
            switch t { case "change": idx = dbg_add_change_bp(emu.cpc, f[1], nil, 0); case "code": idx = dbg_add_code_bp(emu.cpc, a); case "read": idx = dbg_add_mem_bp(emu.cpc, a, b, true, false); case "write": idx = dbg_add_mem_bp(emu.cpc, a, b, false, true)
            default: idx = dbg_add_io_bp(emu.cpc, UInt16(f[3], radix: 16) ?? 0, UInt16(f[4], radix: 16) ?? 0xFFFF, f[5] == "1", f[6] == "1") }
            if idx >= 0 {
                _ = dbg_set_condition(emu.cpc, idx, f.count > 9 ? f[9] : "", Int32(f[8]) ?? 0)
                if f.count > 10 { dbg_set_log(emu.cpc, idx, f[10...].joined(separator: ";")) }   // logpoint message (older files have none)
            }
        }
        emu.lock.unlock(); reload(); debugger?.refreshAll()
    }
    @objc func save(_ s: Any?) {
        let p = NSSavePanel(); p.nameFieldStringValue = "breakpoints.txt"; guard p.runModal() == .OK, let u = p.url else { return }
        var out = ""
        let list: [DbgBreakpoint] = withBP { arr in (0..<Int(DBG_MAX_BP)).compactMap { arr[$0].used ? arr[$0] : nil } }
        for b in list {
            let t = b.type == BP_CHANGE ? "change" : b.type == BP_CODE ? "code" : b.type == BP_MEM_READ ? "read" : b.type == BP_MEM_WRITE ? "write" : "io"
            let cond = withUnsafePointer(to: b.cond) { $0.withMemoryRebound(to: CChar.self, capacity: 160) { String(cString: $0) } }
            let log = withUnsafePointer(to: b.log) { $0.withMemoryRebound(to: CChar.self, capacity: 160) { String(cString: $0) } }
            let expr = withUnsafePointer(to: b.expr) { $0.withMemoryRebound(to: CChar.self, capacity: 160) { String(cString: $0) } }
            out += "\(t);\(b.type == BP_CHANGE ? expr : String(b.start, radix: 16));\(String(b.end, radix: 16));\(String(b.port, radix: 16));\(String(b.mask, radix: 16));\(b.on_in ? 1 : 0);\(b.on_out ? 1 : 0);1;\(b.pass_count);\(cond)" + (log.isEmpty ? "" : ";" + log) + "\n"
        }
        try? out.write(to: u, atomically: true, encoding: .utf8)
    }
}

// MARK: - Data areas
final class DataAreasController: TableWindowController {
    let emu: Emulator; weak var debugger: DebuggerController?
    init(emu: Emulator, debugger: DebuggerController) {
        self.emu = emu; self.debugger = debugger
        super.init(title: "Data Areas", width: 420, height: 300)
        setColumns([("start", "Start", 90, true), ("end", "End", 90, true), ("type", "Type", 90, true), ("label", "Label", 60, false)])
        addButton("Clear", #selector(clear(_:))); addButton("Clear All", #selector(clearAll(_:))); addButton("Add", #selector(add(_:)))
        onEdit = { [weak self] row, col, text in self?.edited(row, col, text) }
    }
    private func areas() -> [(Int, DbgDataArea)] {
        emu.lock.lock(); defer { emu.lock.unlock() }
        let d = dbg_attach(emu.cpc)!
        return withUnsafeMutablePointer(to: &d.pointee.areas) { $0.withMemoryRebound(to: DbgDataArea.self, capacity: Int(DBG_MAX_AREAS)) { a in (0..<Int(DBG_MAX_AREAS)).compactMap { a[$0].used ? ($0, a[$0]) : nil } } }
    }
    override func buildRows() -> [[String]] { areas().map { [String(format: "%04X", $0.1.start), String(format: "%04X", $0.1.end), $0.1.words ? "Words" : "Bytes", $0.1.labels ? "✓" : ""] } }
    private func edited(_ row: Int, _ col: String, _ text: String) {
        let list = areas(); guard row < list.count else { return }
        let i = list[row].0
        emu.lock.lock(); let d = dbg_attach(emu.cpc)!
        withUnsafeMutablePointer(to: &d.pointee.areas) { $0.withMemoryRebound(to: DbgDataArea.self, capacity: Int(DBG_MAX_AREAS)) { a in
            if col == "start", let v = UInt16(text, radix: 16) { a[i].start = v } else if col == "end", let v = UInt16(text, radix: 16) { a[i].end = v } else if col == "type" { a[i].words = text.lowercased().hasPrefix("w") } } }
        emu.lock.unlock(); reload(); debugger?.refreshAll()
    }
    @objc func add(_ s: Any?) {
        guard let t = promptText("Add Data Area", "Range start,end:") else { return }
        let r = t.split(separator: ",").compactMap { debugger?.model.eval(String($0)).0 }; guard r.count == 2 else { return }
        emu.lock.lock(); dbg_area_add(emu.cpc, UInt16(truncatingIfNeeded: r[0]), UInt16(truncatingIfNeeded: r[1]), false); emu.lock.unlock(); reload(); debugger?.refreshAll()
    }
    @objc func clear(_ s: Any?) {
        let list = areas(); guard selectedRow >= 0, selectedRow < list.count else { return }
        emu.lock.lock(); dbg_area_remove(emu.cpc, list[selectedRow].1.start); emu.lock.unlock(); reload(); debugger?.refreshAll()
    }
    @objc func clearAll(_ s: Any?) {
        emu.lock.lock(); let d = dbg_attach(emu.cpc)!; withUnsafeMutablePointer(to: &d.pointee.areas) { $0.withMemoryRebound(to: DbgDataArea.self, capacity: Int(DBG_MAX_AREAS)) { a in for i in 0..<Int(DBG_MAX_AREAS) { a[i].used = false } } }; emu.lock.unlock()
        reload(); debugger?.refreshAll()
    }
}

// MARK: - Timers
final class TimersController: TableWindowController {
    let emu: Emulator
    init(emu: Emulator) {
        self.emu = emu
        super.init(title: "Timers", width: 520, height: 280)
        setColumns([("id", "ID", 70, false), ("count", "Count", 70, false), ("last", "Last (µs)", 80, false), ("min", "Min", 70, false), ("max", "Max", 70, false), ("avg", "Average", 80, false)])
        addButton("Clear", #selector(clear(_:))); addButton("Clear All", #selector(clearAll(_:)))
    }
    private func timers() -> [(Int, DbgTimer)] {
        emu.lock.lock(); defer { emu.lock.unlock() }
        let d = dbg_attach(emu.cpc)!
        return withUnsafeMutablePointer(to: &d.pointee.timers) { $0.withMemoryRebound(to: DbgTimer.self, capacity: Int(DBG_MAX_TIMERS)) { a in (0..<Int(DBG_MAX_TIMERS)).compactMap { a[$0].used ? ($0, a[$0]) : nil } } }
    }
    override func buildRows() -> [[String]] {
        timers().map { let t = $0.1; return [String(format: "%04X", t.id), String(t.count), String(t.last), t.count > 0 ? String(t.min) : "-", String(t.max), t.count > 0 ? String(t.total / UInt64(t.count)) : "-"] }
    }
    @objc func clear(_ s: Any?) {
        let l = timers(); guard selectedRow >= 0, selectedRow < l.count else { return }
        emu.lock.lock(); let d = dbg_attach(emu.cpc)!; withUnsafeMutablePointer(to: &d.pointee.timers) { $0.withMemoryRebound(to: DbgTimer.self, capacity: Int(DBG_MAX_TIMERS)) { $0[l[selectedRow].0].used = false } }; emu.lock.unlock(); reload()
    }
    @objc func clearAll(_ s: Any?) {
        emu.lock.lock(); let d = dbg_attach(emu.cpc)!; withUnsafeMutablePointer(to: &d.pointee.timers) { $0.withMemoryRebound(to: DbgTimer.self, capacity: Int(DBG_MAX_TIMERS)) { a in for i in 0..<Int(DBG_MAX_TIMERS) { a[i].used = false } } }; emu.lock.unlock(); reload()
    }
}

// MARK: - Chip registers
final class RegistersController: NSObject {
    let emu: Emulator
    let panel = makePanel("CPC Emulator - Registers", width: 620, height: 560)
    private let text = NSTextView()
    private var timer: Timer?
    init(emu: Emulator) {
        self.emu = emu
        super.init()
        text.isEditable = false; text.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        let scroll = NSScrollView(); scroll.documentView = text; scroll.hasVerticalScroller = true
        fill(panel, with: scroll, inset: 0)
        timer = Timer.scheduledTimer(withTimeInterval: 0.3, repeats: true) { [weak self] _ in if self?.panel.isVisible == true { self?.reload() } }
    }
    func show() { reload(); panel.makeKeyAndOrderFront(nil) }

    private func swatch(_ rgb: UInt32, _ s: NSMutableAttributedString, _ font: NSFont) {
        let c = NSColor(red: CGFloat((rgb >> 16) & 255) / 255, green: CGFloat((rgb >> 8) & 255) / 255, blue: CGFloat(rgb & 255) / 255, alpha: 1)
        s.append(NSAttributedString(string: "██", attributes: [.foregroundColor: c, .font: font]))
    }

    func reload() {
        emu.lock.lock()
        let c = emu.cpc.pointee
        emu.lock.unlock()
        let font = NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)
        let s = NSMutableAttributedString()
        func line(_ t: String) { s.append(NSAttributedString(string: t + "\n", attributes: [.font: font, .foregroundColor: NSColor.labelColor])) }
        line("Palette:")
        for i in 0..<17 {
            let rgb = c.cfg.plus ? c.asic.pal_rgb_get(i) : c.hw_rgb_get(Int(c.ga_palette_get(i)))
            s.append(NSAttributedString(string: String(format: "%2X ", i), attributes: [.font: font, .foregroundColor: NSColor.secondaryLabelColor])); swatch(rgb, s, font)
            s.append(NSAttributedString(string: String(format: " %02X   ", c.ga_palette_get(i)), attributes: [.font: font, .foregroundColor: NSColor.labelColor]))
            if i % 4 == 3 { line("") }
        }
        line("")
        line("PSG:  " + (0..<16).map { String(format: "%02X", c.psg.regs_get($0)) }.joined(separator: " "))
        line("CRTC: " + (0..<18).map { String(format: "%02X", c.crtc.r_get($0)) }.joined(separator: " "))
        line(String(format: "VCC:%02X VLC:%02X VTAC:%02X HCC:%02X HSC:%02X VSC:%02X VMA:%04X  Mode:%d  R52:%02d  ICSR:%02X", c.crtc.vcc, c.crtc.vlc, c.crtc.vtac, c.crtc.hcc, c.crtc.hsc, c.crtc.vsc, c.crtc.ma_row &+ c.crtc.hcc, c.ga_mode, c.ga_irq_counter, c.asic.icsr))
        if c.cfg.plus {
            line("")
            line("ASIC \(c.asic.unlocked ? "unlocked" : "locked")   RAM page \(c.asic.page_visible ? "enabled" : "disabled")   LRB:\(c.asic.lower_base)  Cart bank:\(c.asic.cart_bank)")
            line(String(format: "PRI:%02X SSS:%02X SSSA:%04X SSC:%02X IVR:%02X DCSR:%02X", c.asic.pri, c.asic.splt, c.asic.ssa, c.asic.sscr, c.asic.ivr, c.asic.dcsr))
            for i in 0..<3 { line(String(format: "DMA %d: addr %04X pause %d prescale %d %@", i, c.asic.dma_addr(i), c.asic.dma_pause(i), c.asic.dma_prescale(i), (c.asic.dcsr >> UInt8(i)) & 1 == 1 ? "enabled" : "")) }
        }
        text.textStorage?.setAttributedString(s)
    }
}

// Tuple-array accessors for the imported C arrays
extension CPC {
    func hw_rgb_get(_ i: Int) -> UInt32 { withUnsafePointer(to: hw_rgb) { $0.withMemoryRebound(to: UInt32.self, capacity: 32) { $0[i & 31] } } }
    func ga_palette_get(_ i: Int) -> UInt8 { withUnsafePointer(to: ga_palette) { $0.withMemoryRebound(to: UInt8.self, capacity: 17) { $0[i] } } }
}
extension CPCPsg { func regs_get(_ i: Int) -> UInt8 { withUnsafePointer(to: regs) { $0.withMemoryRebound(to: UInt8.self, capacity: 16) { $0[i] } } } }
extension CPCCrtc { func r_get(_ i: Int) -> UInt8 { withUnsafePointer(to: r) { $0.withMemoryRebound(to: UInt8.self, capacity: 32) { $0[i] } } } }
extension CPCAsic {
    func pal_rgb_get(_ i: Int) -> UInt32 { withUnsafePointer(to: pal_rgb) { $0.withMemoryRebound(to: UInt32.self, capacity: 17) { $0[i] } } }
    func dma_addr(_ i: Int) -> UInt16 { i == 0 ? dma.0.addr : i == 1 ? dma.1.addr : dma.2.addr }
    func dma_pause(_ i: Int) -> Int { Int(i == 0 ? dma.0.pause : i == 1 ? dma.1.pause : dma.2.pause) }
    func dma_prescale(_ i: Int) -> Int { Int(i == 0 ? dma.0.prescale : i == 1 ? dma.1.prescale : dma.2.prescale) }
}

// MARK: - Graphics finder
final class GraphicsController: NSObject {
    let model: DebugModel
    let panel = makePanel("Find Graphics", width: 640, height: 520)
    private let imageView = NSImageView()
    private var address = 0xC000, widthBytes = 16, height = 64, mode = 1, zoom = 4
    private let addrField = NSTextField(string: "C000"), widthField = NSTextField(string: "16"), heightField = NSTextField(string: "64")
    private let modePopup = NSPopUpButton(frame: .zero, pullsDown: false), zoomPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let encPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let info = makeLabel("Address: C000")

    init(model: DebugModel) {
        self.model = model
        super.init()
        modePopup.addItems(withTitles: ["Mode 0", "Mode 1", "Mode 2"]); modePopup.selectItem(at: 1)
        zoomPopup.addItems(withTitles: ["1x", "2x", "4x", "8x"]); zoomPopup.selectItem(at: 2)
        encPopup.addItems(withTitles: ["Linear", "CPC screen (8 lines)"])
        for c in [addrField, widthField, heightField] { c.target = self; c.action = #selector(changed(_:)); c.widthAnchor.constraint(equalToConstant: 60).isActive = true }
        for p in [modePopup, zoomPopup, encPopup] { p.target = self; p.action = #selector(changed(_:)) }
        imageView.imageScaling = .scaleNone; imageView.translatesAutoresizingMaskIntoConstraints = false
        imageView.wantsLayer = true; imageView.layer?.backgroundColor = NSColor.black.cgColor
        let row1 = NSStackView(views: [makeLabel("Mode:"), modePopup, makeLabel("Address:"), addrField, makeLabel("Width:"), widthField, makeLabel("Height:"), heightField])
        let row2 = NSStackView(views: [makeLabel("Zoom:"), zoomPopup, makeLabel("Encoding:"), encPopup, makeButton("Copy", self, #selector(copyImage(_:))), makeButton("Mark as data", self, #selector(markData(_:))), info])
        let scroll = NSScrollView(); scroll.documentView = imageView; scroll.hasVerticalScroller = true; scroll.hasHorizontalScroller = true
        scroll.translatesAutoresizingMaskIntoConstraints = false
        let all = NSStackView(views: [row1, row2, scroll]); all.orientation = .vertical; all.alignment = .leading
        scroll.widthAnchor.constraint(equalTo: all.widthAnchor).isActive = true; scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 300).isActive = true
        fill(panel, with: all)
    }
    func show() { reload(); panel.makeKeyAndOrderFront(nil) }

    @objc func changed(_ s: Any?) {
        address = Int(addrField.stringValue, radix: 16) ?? address; widthBytes = max(1, min(128, Int(widthField.stringValue) ?? widthBytes)); height = max(1, min(512, Int(heightField.stringValue) ?? height))
        mode = modePopup.indexOfSelectedItem; zoom = [1, 2, 4, 8][zoomPopup.indexOfSelectedItem]
        reload()
    }

    private func palette() -> [UInt32] {
        model.emu.lock.lock(); defer { model.emu.lock.unlock() }
        let c = model.cpc.pointee
        return (0..<16).map { c.cfg.plus ? c.asic.pal_rgb_get($0) : c.hw_rgb_get(Int(c.ga_palette_get($0))) }
    }

    func reload() {
        let pal = palette()
        let pixPerByte = mode == 0 ? 2 : mode == 1 ? 4 : 8, w = widthBytes * pixPerByte
        var px = [UInt32](repeating: 0xFF000000, count: w * height)
        let interleaved = encPopup.indexOfSelectedItem == 1
        for y in 0..<height {
            let base = interleaved ? address + (y & 7) * 0x800 + (y >> 3) * 80 : address + y * widthBytes
            let bytes = model.readBlock(base, widthBytes)
            for (bx, b) in bytes.enumerated() {
                for p in 0..<pixPerByte {
                    var pen = 0
                    let bb = Int(b)
                    switch mode {
                    case 0:
                        let sh = p == 0 ? [7, 3, 5, 1] : [6, 2, 4, 0]   // pen bits 0..3
                        for (k, bit) in sh.enumerated() { pen |= ((bb >> bit) & 1) << k }
                    case 1:
                        pen = ((bb >> (7 - p)) & 1) | (((bb >> (3 - p)) & 1) << 1)
                    default:
                        pen = (bb >> (7 - p)) & 1
                    }
                    px[y * w + bx * pixPerByte + p] = pal[pen]
                }
            }
        }
        let data = Data(bytes: px, count: px.count * 4)
        if let prov = CGDataProvider(data: data as CFData), let cg = CGImage(width: w, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue), provider: prov, decode: nil, shouldInterpolate: false, intent: .defaultIntent) {
            let img = NSImage(cgImage: cg, size: NSSize(width: CGFloat(w * zoom * (mode == 2 ? 1 : 1)), height: CGFloat(height * zoom)))
            imageView.image = img; imageView.frame = NSRect(origin: .zero, size: img.size)
        }
        info.stringValue = String(format: "Address: %04X", address)
    }
    @objc func copyImage(_ s: Any?) { if let i = imageView.image { NSPasteboard.general.clearContents(); NSPasteboard.general.writeObjects([i]) } }
    @objc func markData(_ s: Any?) { model.emu.lock.lock(); dbg_area_add(model.cpc, UInt16(address & 0xFFFF), UInt16((address + widthBytes * height - 1) & 0xFFFF), false); model.emu.lock.unlock() }
}
