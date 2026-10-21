import AppKit
import CPCCore

// MARK: - Profiler

/// Where the emulated time goes: per routine (self and inclusive, from a shadow call stack) and per instruction address.
final class ProfilerController: NSObject, NSTableViewDataSource, NSTableViewDelegate {
    unowned let debugger: DebuggerController
    let panel = makePanel("Profiler", width: 760, height: 480)
    private let table = NSTableView()
    private let tabs = NSSegmentedControl(labels: ["Routines", "Hot spots"], trackingMode: .selectOne, target: nil, action: nil)
    private let sortPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let info = makeLabel("")
    private let startButton = makeButton("Start", nil, #selector(toggle(_:)))
    private var rows: [[String]] = []
    private var addresses: [Int] = []
    private var timer: Timer?
    static let frameBudget = 19968.0            // 312 lines of 64 µs

    init(debugger: DebuggerController) {
        self.debugger = debugger
        super.init()
        panel.styleMask.insert(.resizable)
        startButton.target = self
        tabs.selectedSegment = 0; tabs.target = self; tabs.action = #selector(tabChanged(_:))
        sortPopup.addItems(withTitles: ["Sort by self time", "Sort by inclusive time", "Sort by calls"]); sortPopup.target = self; sortPopup.action = #selector(tabChanged(_:))
        table.dataSource = self; table.delegate = self; table.usesAlternatingRowBackgroundColors = true
        table.target = self; table.doubleAction = #selector(goTo(_:))
        let scroll = NSScrollView(); scroll.documentView = table; scroll.hasVerticalScroller = true
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 300).isActive = true
        info.font = .systemFont(ofSize: 11); info.textColor = .secondaryLabelColor
        let top = stack([tabs, sortPopup, NSView(), startButton, makeButton("Clear", self, #selector(clear(_:)))], axis: .horizontal)
        let bottom = stack([info, NSView(), makeButton("Copy", self, #selector(copyText(_:))), makeButton("Go To", self, #selector(goTo(_:)))], axis: .horizontal)
        let all = stack([top, scroll, bottom]); all.alignment = .width
        fill(panel, with: all, inset: 12)
        configureColumns()
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self = self, self.panel.isVisible else { return }
            self.reload()
        }
    }

    private func configureColumns() {
        for c in table.tableColumns { table.removeTableColumn(c) }
        let cols: [(String, String, CGFloat)] = tabs.selectedSegment == 0
            ? [("name", "Routine", 220), ("addr", "Address", 56), ("calls", "Calls", 60), ("self", "Self µs/frame", 96), ("selfp", "Self %", 60), ("incl", "Incl. µs/frame", 100), ("inclp", "Incl. %", 60), ("percall", "µs/call", 64)]
            : [("addr", "Address", 56), ("name", "Symbol", 190), ("ins", "Instruction", 210), ("self", "µs/frame", 80), ("selfp", "%", 60)]
        for (id, title, w) in cols { let c = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id)); c.title = title; c.width = w; table.addTableColumn(c) }
        sortPopup.isHidden = tabs.selectedSegment != 0
    }

    func show() { reload(); panel.makeKeyAndOrderFront(nil) }
    @objc func tabChanged(_ s: Any?) { configureColumns(); reload() }

    @objc func toggle(_ s: Any?) {
        let emu = debugger.emu
        emu.lock.lock()
        let d = dbg_attach(emu.cpc)!
        dbg_prof_enable(emu.cpc, !dbg_prof_running(d))
        emu.lock.unlock()
        reload()
    }
    @objc func clear(_ s: Any?) { let emu = debugger.emu; emu.lock.lock(); dbg_prof_clear(emu.cpc); emu.lock.unlock(); reload() }

    private func name(_ addr: Int) -> String {
        if addr == Int(DBG_PROF_TOP) { return "(top level)" }
        if addr == Int(DBG_PROF_HALT) { return "(waiting in HALT)" }
        return debugger.model.nearestSymbol(addr) ?? String(format: "#%04X", addr)
    }

    func reload() {
        let emu = debugger.emu, m = debugger.model
        var entries = [DbgProfEntry](repeating: DbgProfEntry(), count: 4096), n = 0
        var hot = [UInt16](repeating: 0, count: 300), hotUs = [UInt32](repeating: 0, count: 300), nh = 0
        var total: UInt64 = 0, frames: UInt64 = 0, running = false
        emu.lock.lock()
        if let d = emu.cpc.pointee.dbg {
            running = dbg_prof_running(d); total = dbg_prof_total(d); frames = dbg_prof_frames(d)
            if tabs.selectedSegment == 0 { n = Int(dbg_prof_entries(d, &entries, 4096)) } else { nh = Int(dbg_prof_hotspots(d, &hot, &hotUs, 300)) }
        }
        emu.lock.unlock()
        startButton.title = running ? "Stop" : "Start"
        let fr = Double(max(frames, 1)), tot = Double(max(total, 1))
        var halt = 0.0
        if tabs.selectedSegment == 0 {
            var list = Array(entries[0..<n])
            halt = Double(list.first { $0.addr == DBG_PROF_HALT }?.own ?? 0)
            switch sortPopup.indexOfSelectedItem {
            case 1: list.sort { $0.incl > $1.incl }
            case 2: list.sort { $0.calls > $1.calls }
            default: list.sort { $0.own > $1.own }
            }
            addresses = list.map { Int($0.addr) }
            rows = list.map { e in
                let special = e.addr >= 0x10000
                return [name(Int(e.addr)), special ? "" : String(format: "%04X", e.addr), special ? "" : String(e.calls), String(format: "%.0f", Double(e.own) / fr), String(format: "%.1f", Double(e.own) / tot * 100),
                        special ? "" : String(format: "%.0f", Double(e.incl) / fr), special ? "" : String(format: "%.1f", Double(e.incl) / tot * 100),
                        e.calls > 0 && !special ? String(format: "%.1f", Double(e.own) / Double(e.calls)) : ""]
            }
        } else {
            addresses = (0..<nh).map { Int(hot[$0]) }
            rows = (0..<nh).map { i in
                let a = Int(hot[i])
                return [String(format: "%04X", a), m.nearestSymbol(a) ?? "", m.symbolise(m.disasm(a).0), String(format: "%.0f", Double(hotUs[i]) / fr), String(format: "%.1f", Double(hotUs[i]) / tot * 100)]
            }
        }
        let secs = Double(total) / 1_000_000
        if total == 0 { info.stringValue = running ? "Measuring… run the machine." : "Press Start, then let the program run." }
        else {
            let busy = (Double(total) - halt) / fr
            info.stringValue = String(format: "%@ %.1f s (%d frames). Busy %.0f µs per frame of %.0f (%.0f%%) · waiting in HALT %.0f%%.",
                                      running ? "Measuring" : "Measured", secs, Int(frames), busy, ProfilerController.frameBudget, busy / ProfilerController.frameBudget * 100, halt / tot * 100)
        }
        table.reloadData()
    }

    func numberOfRows(in t: NSTableView) -> Int { rows.count }
    func tableView(_ t: NSTableView, viewFor col: NSTableColumn?, row: Int) -> NSView? {
        guard let id = col?.identifier.rawValue, let ci = t.tableColumns.firstIndex(where: { $0.identifier.rawValue == id }), row < rows.count, ci < rows[row].count else { return nil }
        let tf = makeLabel(rows[row][ci]); tf.font = .monospacedSystemFont(ofSize: 11, weight: .regular); tf.lineBreakMode = .byTruncatingTail
        if id == "name" && tabs.selectedSegment == 1 { tf.textColor = .systemOrange }
        if ["calls", "self", "selfp", "incl", "inclp", "percall"].contains(id) { tf.alignment = .right }
        return tf
    }

    @objc func goTo(_ s: Any?) {
        let i = table.clickedRow >= 0 ? table.clickedRow : table.selectedRow
        guard i >= 0, i < addresses.count, addresses[i] < 0x10000 else { return }
        debugger.show(); debugger.showInDisassembler(addresses[i])
    }
    @objc func copyText(_ s: Any?) {
        let head = table.tableColumns.map { $0.title }.joined(separator: "\t")
        let text = ([head] + rows.map { $0.joined(separator: "\t") }).joined(separator: "\n")
        NSPasteboard.general.clearContents(); NSPasteboard.general.setString(text, forType: .string)
    }
}

// MARK: - Memory access map

/// 64K as a 256×256 picture (one pixel per byte, a row per high byte): red = executed, green = written, blue = read.
final class AccessMapView: NSView {
    var image: CGImage? { didSet { needsDisplay = true } }
    var onHover: ((Int?) -> Void)?, onClick: ((Int, Bool) -> Void)?
    static let margin: CGFloat = 40
    override var isFlipped: Bool { true }
    private var scale: CGFloat { max(1, min((bounds.width - AccessMapView.margin) / 256, bounds.height / 256).rounded(.down)) }
    private var mapRect: NSRect { NSRect(x: AccessMapView.margin, y: 0, width: 256 * scale, height: 256 * scale) }

    override func draw(_ dirtyRect: NSRect) {
        NSColor(white: 0.08, alpha: 1).setFill(); mapRect.fill()
        if let img = image, let ctx = NSGraphicsContext.current?.cgContext {
            ctx.saveGState(); ctx.interpolationQuality = .none
            ctx.translateBy(x: mapRect.minX, y: mapRect.maxY); ctx.scaleBy(x: 1, y: -1)         // the view is flipped, CGImage is not
            ctx.draw(img, in: CGRect(x: 0, y: 0, width: mapRect.width, height: mapRect.height)); ctx.restoreGState()
        }
        let attrs: [NSAttributedString.Key: Any] = [.font: NSFont.monospacedSystemFont(ofSize: 9, weight: .regular), .foregroundColor: NSColor.secondaryLabelColor]
        for k in 0..<16 {
            let y = mapRect.minY + CGFloat(k) * 16 * scale
            NSAttributedString(string: String(format: "%04X", k * 0x1000), attributes: attrs).draw(at: NSPoint(x: 4, y: y))
            NSColor(white: 1, alpha: k % 4 == 0 ? 0.25 : 0.08).setFill(); NSRect(x: mapRect.minX, y: y, width: mapRect.width, height: 1).fill()
        }
    }

    private func address(_ e: NSEvent) -> Int? {
        let p = convert(e.locationInWindow, from: nil)
        guard mapRect.contains(p) else { return nil }
        return Int((p.y - mapRect.minY) / scale) * 256 + Int((p.x - mapRect.minX) / scale)
    }
    override func updateTrackingAreas() {
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: bounds, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect], owner: self, userInfo: nil))
    }
    override func mouseMoved(with event: NSEvent) { onHover?(address(event)) }
    override func mouseExited(with event: NSEvent) { onHover?(nil) }
    override func mouseDown(with event: NSEvent) { if let a = address(event) { onClick?(a, event.clickCount > 1) } }
}

final class AccessMapController: NSObject {
    unowned let debugger: DebuggerController
    let panel = makePanel("Memory Access Map", width: 640, height: 640)
    private let view = AccessMapView()
    private let info = makeLabel("Hover a pixel for its address. Click: show in memory. Double-click: show in the disassembler.")
    private let startButton = makeButton("Start", nil, #selector(toggle(_:)))
    private let checks = [makeCheck("Executed"), makeCheck("Written"), makeCheck("Read")]
    private var r = [UInt32](repeating: 0, count: 65536), w = [UInt32](repeating: 0, count: 65536), x = [UInt32](repeating: 0, count: 65536)
    private var timer: Timer?
    private var hover: Int?

    init(debugger: DebuggerController) {
        self.debugger = debugger
        super.init()
        panel.styleMask.insert(.resizable)
        startButton.target = self
        for c in checks { c.state = .on; c.target = self; c.action = #selector(redraw(_:)) }
        checks[0].contentTintColor = .systemRed; checks[1].contentTintColor = .systemGreen; checks[2].contentTintColor = .systemBlue
        view.translatesAutoresizingMaskIntoConstraints = false
        view.widthAnchor.constraint(greaterThanOrEqualToConstant: 296).isActive = true
        view.heightAnchor.constraint(greaterThanOrEqualToConstant: 256).isActive = true
        info.font = .monospacedSystemFont(ofSize: 11, weight: .regular); info.lineBreakMode = .byTruncatingTail
        let top = stack(checks as [NSView] + [NSView(), startButton, makeButton("Clear", self, #selector(clear(_:)))], axis: .horizontal)
        let all = stack([top, view, info]); all.alignment = .width
        fill(panel, with: all, inset: 12)
        view.onHover = { [weak self] a in self?.hover = a; self?.updateInfo() }
        view.onClick = { [weak self] a, dbl in
            guard let self = self else { return }
            self.debugger.show()
            if dbl { self.debugger.showInDisassembler(a) } else { self.debugger.memory.show(a); self.debugger.memory.reload() }
        }
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            guard let self = self, self.panel.isVisible else { return }
            self.reload()
        }
    }

    func show() { reload(); panel.makeKeyAndOrderFront(nil) }
    @objc func redraw(_ s: Any?) { render() }

    @objc func toggle(_ s: Any?) {
        let emu = debugger.emu
        emu.lock.lock()
        let d = dbg_attach(emu.cpc)!
        dbg_access_enable(emu.cpc, !dbg_access_running(d))
        emu.lock.unlock()
        reload()
    }
    @objc func clear(_ s: Any?) { let emu = debugger.emu; emu.lock.lock(); dbg_access_clear(emu.cpc); emu.lock.unlock(); reload() }

    func reload() {
        let emu = debugger.emu
        var running = false
        emu.lock.lock()
        if let d = emu.cpc.pointee.dbg {
            running = dbg_access_running(d)
            dbg_access_copy(d, 0, &r); dbg_access_copy(d, 1, &w); dbg_access_copy(d, 2, &x)
        }
        emu.lock.unlock()
        startButton.title = running ? "Stop" : "Start"
        render(); updateInfo()
    }

    private func level(_ n: UInt32) -> UInt32 { n == 0 ? 0 : UInt32(min(255, 70 + 11 * (32 - n.leadingZeroBitCount))) }    // log scale: 1 access is visible, 2^16 is full

    private func render() {
        var px = [UInt32](repeating: 0xFF14_1414, count: 65536)                    // ABGR as little-endian RGBA bytes
        let showX = checks[0].state == .on, showW = checks[1].state == .on, showR = checks[2].state == .on
        for a in 0..<65536 {
            let cx = showX ? level(x[a]) : 0, cw = showW ? level(w[a]) : 0, cr = showR ? level(r[a]) : 0
            if cx | cw | cr != 0 { px[a] = 0xFF00_0000 | cr << 16 | cw << 8 | cx }
        }
        let img: CGImage? = px.withUnsafeMutableBytes { p in
            CGContext(data: p.baseAddress, width: 256, height: 256, bitsPerComponent: 8, bytesPerRow: 1024, space: CGColorSpaceCreateDeviceRGB(),
                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)?.makeImage()
        }
        view.image = img
    }

    private func updateInfo() {
        guard let a = hover else {
            let used = (0..<65536).reduce((0, 0, 0)) { ($0.0 + (x[$1] > 0 ? 1 : 0), $0.1 + (w[$1] > 0 ? 1 : 0), $0.2 + (r[$1] > 0 ? 1 : 0)) }
            info.stringValue = "\(used.0) bytes executed · \(used.1) written · \(used.2) read. Hover a pixel for details."
            return
        }
        let sym = debugger.model.nearestSymbol(a).map { "  " + $0 } ?? ""
        info.stringValue = String(format: "#%04X%@   executed %u · written %u · read %u", a, sym, x[a], w[a], r[a])
    }
}
