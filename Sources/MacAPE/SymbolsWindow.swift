import AppKit
import CPCCore

/// Symbol browser: filter by name, double-click to show the code, toggle a breakpoint on a symbol.
final class SymbolsController: NSObject, NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate {
    unowned let debugger: DebuggerController
    let panel = makePanel("Symbols", width: 380, height: 480)
    private let table = NSTableView()
    private let search = NSSearchField()
    private let countLabel = makeLabel("")
    private var all: [(name: String, addr: Int)] = []
    private var rows: [(name: String, addr: Int)] = []
    private var byAddress = false

    init(debugger: DebuggerController) {
        self.debugger = debugger
        super.init()
        panel.styleMask.insert(.resizable)
        for (id, title, w) in [("name", "Symbol", 250.0), ("addr", "Address", 80.0)] {
            let c = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id)); c.title = title; c.width = w
            c.sortDescriptorPrototype = NSSortDescriptor(key: id, ascending: true); table.addTableColumn(c)
        }
        table.dataSource = self; table.delegate = self; table.usesAlternatingRowBackgroundColors = true
        table.target = self; table.doubleAction = #selector(goTo(_:))
        let scroll = NSScrollView(); scroll.documentView = table; scroll.hasVerticalScroller = true
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 300).isActive = true
        search.placeholderString = "Filter (? and * wildcards)"; search.delegate = self
        search.target = self; search.action = #selector(filterChanged(_:))
        let buttons = stack([makeButton("Load…", self, #selector(load(_:))), makeButton("Clear", self, #selector(clear(_:))), NSView(),
                             makeButton("Breakpoint", self, #selector(toggleBreakpoint(_:))), makeButton("Go To", self, #selector(goTo(_:)))], axis: .horizontal)
        let all = stack([search, scroll, countLabel, buttons]); all.alignment = .width
        fill(panel, with: all, inset: 12)
    }

    func show() { reload(); panel.makeKeyAndOrderFront(nil); panel.makeFirstResponder(search) }

    func reload() {
        all = debugger.model.symbols().map { (name: $0.0, addr: $0.1) }
        applyFilter()
    }
    private func applyFilter() {
        let f = search.stringValue
        let pat = f.isEmpty ? "*" : (f.contains("*") || f.contains("?") ? f : "*" + f + "*")
        rows = all.filter { FindQuery.asmMatch($0.name, pat) }
        rows.sort { byAddress ? ($0.addr, $0.name.lowercased()) < ($1.addr, $1.name.lowercased()) : $0.name.lowercased() < $1.name.lowercased() }
        countLabel.stringValue = all.isEmpty ? "No symbols loaded" : "\(rows.count) of \(all.count) symbols"
        table.reloadData()
    }
    @objc func filterChanged(_ s: Any?) { applyFilter() }
    func controlTextDidChange(_ obj: Notification) { applyFilter() }

    func numberOfRows(in tableView: NSTableView) -> Int { rows.count }
    func tableView(_ t: NSTableView, viewFor col: NSTableColumn?, row: Int) -> NSView? {
        let r = rows[row]
        let tf = makeLabel(col?.identifier.rawValue == "addr" ? String(format: "#%04X", r.addr) : r.name)
        tf.font = .monospacedSystemFont(ofSize: 11, weight: .regular); tf.lineBreakMode = .byTruncatingTail
        if col?.identifier.rawValue == "name", debugger.model.hasBreakpoint(at: r.addr) { tf.textColor = .systemRed }
        return tf
    }
    func tableView(_ t: NSTableView, sortDescriptorsDidChange old: [NSSortDescriptor]) {
        byAddress = t.sortDescriptors.first?.key == "addr"; applyFilter()
    }

    private var selectedRow: (name: String, addr: Int)? { let i = table.clickedRow >= 0 ? table.clickedRow : table.selectedRow; return i >= 0 && i < rows.count ? rows[i] : nil }
    @objc func goTo(_ s: Any?) {
        guard let r = selectedRow else { return }
        debugger.show(); debugger.showInDisassembler(r.addr)
    }
    @objc func toggleBreakpoint(_ s: Any?) {
        guard let r = selectedRow else { return }
        debugger.model.toggleBreakpoint(at: r.addr); debugger.refreshAll(); table.reloadData()
    }
    @objc func load(_ s: Any?) { debugger.loadSymbols(s) }
    @objc func clear(_ s: Any?) { debugger.clearSymbols(s) }
}
