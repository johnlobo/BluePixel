import AppKit
import CPCCore

/// Last instructions executed before the machine stopped (newest first), with registers at that point.
final class HistoryController: NSObject, NSTableViewDataSource, NSTableViewDelegate {
    unowned let debugger: DebuggerController
    let panel = makePanel("Execution History", width: 840, height: 460)
    private let table = NSTableView()
    private let info = makeLabel("")
    private var items: [DbgHistory] = []

    init(debugger: DebuggerController) {
        self.debugger = debugger
        super.init()
        panel.styleMask.insert(.resizable)
        let cols: [(String, String, CGFloat)] = [("n", "#", 44), ("pc", "Address", 56), ("sym", "Symbol", 230), ("ins", "Instruction", 200),
                                                 ("af", "AF", 44), ("bc", "BC", 44), ("de", "DE", 44), ("hl", "HL", 44), ("ix", "IX", 44), ("iy", "IY", 44), ("sp", "SP", 44)]
        for (id, title, w) in cols { let c = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id)); c.title = title; c.width = w; table.addTableColumn(c) }
        table.dataSource = self; table.delegate = self; table.usesAlternatingRowBackgroundColors = true
        table.target = self; table.doubleAction = #selector(goTo(_:))
        let scroll = NSScrollView(); scroll.documentView = table; scroll.hasVerticalScroller = true
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 320).isActive = true
        let row = stack([info, NSView(), makeButton("Clear", self, #selector(clear(_:))), makeButton("Go To", self, #selector(goTo(_:)))], axis: .horizontal)
        let all = stack([scroll, row]); all.alignment = .width
        fill(panel, with: all, inset: 12)
    }

    func show() { reload(); panel.makeKeyAndOrderFront(nil) }

    func reload() {
        let emu = debugger.emu
        emu.lock.lock()
        if let d = emu.cpc.pointee.dbg { let n = Int(dbg_history_count(d)); items = (0..<n).map { dbg_history_get(d, Int32($0)) } } else { items = [] }
        emu.lock.unlock()
        info.stringValue = "\(items.count) instructions (newest first). Double-click to show the code."
        table.reloadData()
    }

    func numberOfRows(in t: NSTableView) -> Int { items.count }
    func tableView(_ t: NSTableView, viewFor col: NSTableColumn?, row: Int) -> NSView? {
        let h = items[row], m = debugger.model
        let text: String
        switch col?.identifier.rawValue ?? "" {
        case "n": text = "-\(row + 1)"
        case "pc": text = String(format: "%04X", h.pc)
        case "sym": text = m.nearestSymbol(Int(h.pc)) ?? ""
        case "ins": text = m.symbolise(m.disasm(Int(h.pc)).0)
        case "af": text = String(format: "%04X", h.af)
        case "bc": text = String(format: "%04X", h.bc)
        case "de": text = String(format: "%04X", h.de)
        case "hl": text = String(format: "%04X", h.hl)
        case "ix": text = String(format: "%04X", h.ix)
        case "iy": text = String(format: "%04X", h.iy)
        default: text = String(format: "%04X", h.sp)
        }
        let tf = makeLabel(text); tf.font = .monospacedSystemFont(ofSize: 11, weight: .regular); tf.lineBreakMode = .byTruncatingTail
        if col?.identifier.rawValue == "sym" { tf.textColor = .systemOrange }
        return tf
    }

    @objc func goTo(_ s: Any?) {
        let i = table.clickedRow >= 0 ? table.clickedRow : table.selectedRow
        guard i >= 0, i < items.count else { return }
        debugger.show(); debugger.showInDisassembler(Int(items[i].pc))
    }
    @objc func clear(_ s: Any?) {
        let emu = debugger.emu
        emu.lock.lock(); if let d = emu.cpc.pointee.dbg { d.pointee.hist_count = 0; d.pointee.hist_pos = 0 }; emu.lock.unlock()
        reload()
    }
}
