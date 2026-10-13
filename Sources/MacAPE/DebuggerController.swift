import AppKit
import CPCCore
import UniformTypeIdentifiers

func promptText(_ title: String, _ message: String, _ initial: String = "") -> String? {
    let a = NSAlert(); a.messageText = title; a.informativeText = message
    let f = NSTextField(frame: NSRect(x: 0, y: 0, width: 300, height: 24)); f.stringValue = initial
    a.accessoryView = f; a.addButton(withTitle: "OK"); a.addButton(withTitle: "Cancel")
    a.window.initialFirstResponder = f
    return a.runModal() == .alertFirstButtonReturn ? f.stringValue : nil
}

final class DebuggerController: NSWindowController, NSWindowDelegate {
    let emu: Emulator
    let model: DebugModel
    let disasm = DisasmView(), memory = MemoryView()
    private var regFields: [Reg: NSTextField] = [:]
    private var flagButtons: [NSButton] = []
    private let tLabel = NSTextField(labelWithString: "0")
    private let intLabel = NSTextField(labelWithString: "DI")
    private let stackView = NSTextView()
    private var lastPane: NSView?
    private var timer: Timer?
    private var checkFollow = NSButton(checkboxWithTitle: "Follow PC", target: nil, action: nil)
    private var checkBreaks = NSButton(checkboxWithTitle: "Breakpoints", target: nil, action: nil)
    private var checkHide = NSButton(checkboxWithTitle: "Hide On Run", target: nil, action: nil)
    private var checkBRK = NSButton(checkboxWithTitle: "Break Instructions", target: nil, action: nil)
    private var modeButtons: [NSButton] = []
    private let lowerCheck = NSButton(checkboxWithTitle: "Lower", target: nil, action: nil)
    private let upperCheck = NSButton(checkboxWithTitle: "Upper ROM:", target: nil, action: nil)
    private let upperPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let ramPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    var breakpointsWindow: BreakpointsController?
    var registersWindow: RegistersController?
    var dataAreasWindow: DataAreasController?
    var timersWindow: TimersController?
    var graphicsWindow: GraphicsController?
    var lastFind: (pattern: [UInt8], wild: [Bool])?

    init(emulator: Emulator) {
        emu = emulator; model = DebugModel(emu: emulator)
        let win = NSWindow(contentRect: NSRect(x: 120, y: 120, width: 940, height: 720), styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        win.title = "WinAPE Debugger"; win.isReleasedWhenClosed = false
        super.init(window: win)
        win.delegate = self
        disasm.model = model; memory.model = model
        disasm.onFocus = { [weak self] in self?.lastPane = self?.disasm }
        memory.onFocus = { [weak self] in self?.lastPane = self?.memory }
        build()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            guard let self = self, self.window?.isVisible == true, !self.emu.paused else { return }
            self.refreshRegisters()
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    // MARK: layout
    private func hexField(_ reg: Reg) -> NSTextField {
        let f = NSTextField(string: ""); f.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        f.alignment = .center; f.target = self; f.action = #selector(registerEdited(_:)); f.identifier = NSUserInterfaceItemIdentifier(reg.rawValue)
        f.translatesAutoresizingMaskIntoConstraints = false
        f.widthAnchor.constraint(equalToConstant: reg.digits == 2 ? 36 : 56).isActive = true
        regFields[reg] = f; return f
    }

    final class Root: NSView {
        var onLayout: (() -> Void)?
        override func layout() { super.layout(); onLayout?(); let h = bounds.height; for v in subviews { v.frame.origin.y = h - v.frame.maxY } }
    }
    private let root = Root()
    private var rowLabels: [NSView] = []

    private func place(_ views: [NSView], x: CGFloat, y: CGFloat, gap: CGFloat = 6) -> CGFloat {
        var cx = x
        for v in views {
            var w = v.fittingSize.width
            let fh = v.fittingSize.height
            if let f = v as? NSTextField, f.isEditable { w = 58 }
            if v === intLabel || v === tLabel { w = max(w, 48) }
            v.frame = NSRect(x: cx, y: y, width: max(w, 16), height: max(fh, 20))
            cx += w + gap
        }
        return cx
    }

    private func build() {
        guard let content = window?.contentView else { return }
        root.frame = content.bounds; root.autoresizingMask = [.width, .height]
        content.addSubview(root)
        let regLayout: [[Reg]] = [[.af, .hl, .de, .bc], [.ix, .iy, .pc], [.af2, .hl2, .de2, .bc2], [.sp, .im, .i, .r]]
        var registerViews: [[NSView]] = []
        for r in regLayout { registerViews.append(r.flatMap { [makeLabel($0.rawValue), hexField($0)] }) }
        let flagNames = ["S", "Z", "-", "H", "-", "V", "N", "C"]
        for (i, n) in flagNames.enumerated() {
            let b = NSButton(checkboxWithTitle: n, target: self, action: #selector(flagToggled(_:))); b.tag = 7 - i
            b.isEnabled = n != "-"; b.font = .systemFont(ofSize: 11); flagButtons.append(b)
        }
        let tReset = makeButton("Reset", self, #selector(resetT(_:))); tReset.controlSize = .small
        let tRow: [NSView] = [makeLabel("T"), tLabel, tReset, makeLabel("Ints"), intLabel]
        stackView.isEditable = false; stackView.font = monoFont()
        let stackScroll = NSScrollView(); stackScroll.documentView = stackView; stackScroll.hasVerticalScroller = true
        stackView.autoresizingMask = [.width]
        let stackTitle = makeLabel("Stack", bold: true)
        func symButton(_ sym: String, _ tip: String, _ sel: Selector) -> NSButton {
            let b = NSButton(image: NSImage(systemSymbolName: sym, accessibilityDescription: tip)!, target: self, action: sel); b.toolTip = tip; b.bezelStyle = .texturedRounded; return b
        }
        let tools: [NSView] = [symButton("arrow.right.to.line", "Go To Address", #selector(gotoAddress(_:))), symButton("magnifyingglass", "Search", #selector(findInPane(_:))),
            symButton("square.and.arrow.down", "Load Data", #selector(loadData(_:))), symButton("square.and.arrow.up", "Save Data", #selector(saveData(_:))),
            symButton("circle.fill", "Edit Breakpoints", #selector(showBreakpoints(_:))), symButton("list.bullet.rectangle", "Edit Data Areas", #selector(showDataAreas(_:))),
            symButton("photo", "Find Graphics", #selector(showGraphics(_:))), symButton("timer", "Show Timers", #selector(showTimers(_:))),
            symButton("cpu", "Registers", #selector(showChipRegisters(_:)))]
        for b in [checkFollow, checkBreaks, checkHide, checkBRK] { b.target = self; b.action = #selector(optionsChanged(_:)) }
        checkFollow.state = .on; checkBreaks.state = .on; checkHide.state = .on
        for (i, t) in ["Read", "Write", "Any"].enumerated() {
            let b = NSButton(radioButtonWithTitle: t, target: self, action: #selector(memorySelectionChanged(_:))); b.tag = i; modeButtons.append(b)
        }
        modeButtons[0].state = .on
        upperPopup.addItems(withTitles: (0..<32).map { String($0) })
        ramPopup.addItems(withTitles: (0..<64).map { String(format: "%02X", 0xC0 + $0) })
        for c in [lowerCheck, upperCheck] { c.target = self; c.action = #selector(memorySelectionChanged(_:)) }
        for p in [upperPopup, ramPopup] { p.target = self; p.action = #selector(memorySelectionChanged(_:)); p.controlSize = .small }
        let current = makeButton("Current", self, #selector(memoryFollowCurrent(_:))); current.controlSize = .small
        let memTitle = makeLabel("Memory:", bold: true), ramLabel = makeLabel("RAM:")
        let memRow: [NSView] = [memTitle] + modeButtons + [lowerCheck, upperCheck, upperPopup, ramLabel, ramPopup, current]
        let optRow: [NSView] = [checkFollow, checkBreaks, checkHide, checkBRK]

        let all: [NSView] = registerViews.flatMap { $0 } + flagButtons + tRow + [stackScroll, stackTitle] + tools + optRow + memRow + [disasm, memory]
        for v in all { root.addSubview(v) }

        root.onLayout = { [weak self] in
            guard let self = self else { return }
            let w = self.root.bounds.width, h = self.root.bounds.height
            var y: CGFloat = 10
            for r in registerViews { _ = self.place(r, x: 12, y: y); y += 26 }
            _ = self.place(self.flagButtons, x: 12, y: y, gap: 2); y += 24
            _ = self.place(tRow, x: 12, y: y); y += 28
            stackTitle.frame = NSRect(x: w - 252, y: 8, width: 100, height: 18)
            stackScroll.frame = NSRect(x: w - 252, y: 28, width: 240, height: 142)
            self.stackView.frame = NSRect(x: 0, y: 0, width: 240, height: 300)
            _ = self.place(tools, x: 12, y: y); _ = self.place(optRow, x: 12 + 9 * 38 + 12, y: y + 2); y += 32
            let remaining = h - y - 40
            self.disasm.frame = NSRect(x: 12, y: y, width: w - 24, height: remaining * 0.58); y += remaining * 0.58 + 6
            _ = self.place(memRow, x: 12, y: y); y += 28
            self.memory.frame = NSRect(x: 12, y: y, width: w - 24, height: h - y - 10)
            self.disasm.reload(); self.memory.reload()
            if ProcessInfo.processInfo.environment["DBGLAYOUT"] != nil { for v in self.root.subviews.prefix(14) { FileHandle.standardError.write("  \(type(of: v)) \(v.frame) hidden=\(v.isHidden) alpha=\(v.alphaValue)\n".data(using: .utf8)!) }; FileHandle.standardError.write("layout w=\(w) h=\(h) disasm=\(self.disasm.frame) memory=\(self.memory.frame) first=\(registerViews[0][0].frame) rows=\(self.disasm.bounds.height)\n".data(using: .utf8)!) }
        }
        root.needsLayout = true
    }
    private func monoFont() -> NSFont { .monospacedSystemFont(ofSize: 11, weight: .regular) }

    // MARK: show / refresh
    func show() { window?.makeKeyAndOrderFront(nil); window?.makeFirstResponder(disasm); refreshAll() }
    func windowDidResize(_ notification: Notification) { refreshAll() }

    func refreshAll() {
        model.sync()
        syncMemoryControls()
        refreshRegisters()
        disasm.followPC = checkFollow.state == .on
        disasm.reload()
        if memory.topAddr == 0 && memory.cursor == 0 { memory.cursor = model.pc; memory.topAddr = model.pc & 0xFFF0 }
        memory.reload()
        breakpointsWindow?.reload(); registersWindow?.reload(); timersWindow?.reload(); dataAreasWindow?.reload(); graphicsWindow?.reload()
    }

    func refreshRegisters() {
        emu.lock.lock()
        let z = emu.cpc.pointee.z80
        let t = emu.cpc.pointee.t_counter
        emu.lock.unlock()
        for (r, f) in regFields where window?.firstResponder !== f.currentEditor() { f.stringValue = String(format: "%0\(r.digits)X", r.get(z)) }
        for b in flagButtons { b.state = (Int(z.af.l) >> b.tag) & 1 == 1 ? .on : .off }
        tLabel.stringValue = String(t)
        intLabel.stringValue = (z.iff1 ? "EI" : "DI")
        var s = ""
        let sp = Int(z.sp.w)
        for i in 0..<16 { let a = (sp + i * 2) & 0xFFFF; let lo = model.read(a), hi = model.read(a + 1); s += String(format: "%04X: %04X\n", a, Int(lo) | Int(hi) << 8) }
        stackView.string = s
    }

    private func syncMemoryControls() {
        let v = model.view
        modeButtons.forEach { $0.state = $0.tag == Int(v.mode) ? .on : .off }
        lowerCheck.state = v.lower ? .on : .off; upperCheck.state = v.upper ? .on : .off
        upperPopup.selectItem(at: Int(min(v.upper_rom, 31)))
        let i = Int(v.ram_cfg) - 0xC0; ramPopup.selectItem(at: max(0, min(63, i)))
    }

    // MARK: actions
    @objc func registerEdited(_ f: NSTextField) {
        guard let id = f.identifier?.rawValue, let r = Reg(rawValue: id) else { return }
        let (v, err) = model.eval(f.stringValue)
        if let v = v { emu.lock.lock(); r.set(&emu.cpc.pointee.z80, Int(v)); emu.lock.unlock() } else { NSSound.beep(); _ = err }
        refreshAll()
    }
    @objc func flagToggled(_ b: NSButton) {
        emu.lock.lock(); var f = emu.cpc.pointee.z80.af.l; if b.state == .on { f |= 1 << UInt8(b.tag) } else { f &= ~(1 << UInt8(b.tag)) }; emu.cpc.pointee.z80.af.l = f; emu.lock.unlock()
    }
    @objc func resetT(_ s: Any?) { emu.lock.lock(); emu.cpc.pointee.t_counter = 0; emu.lock.unlock(); refreshRegisters() }
    @objc func optionsChanged(_ s: Any?) {
        emu.lock.lock()
        let d = dbg_attach(emu.cpc)!
        d.pointee.active = checkBreaks.state == .on
        d.pointee.break_instructions = checkBRK.state == .on
        emu.lock.unlock()
        disasm.followPC = checkFollow.state == .on
        if disasm.followPC { disasm.reload() }
    }
    var hideOnRun: Bool { checkHide.state == .on }

    @objc func memorySelectionChanged(_ s: Any?) {
        model.followMachine = false
        if let b = s as? NSButton, modeButtons.contains(b) { model.view.mode = Int32(b.tag) }
        model.view.lower = lowerCheck.state == .on; model.view.upper = upperCheck.state == .on
        model.view.upper_rom = Int32(upperPopup.indexOfSelectedItem); model.view.ram_cfg = UInt8(0xC0 + ramPopup.indexOfSelectedItem)
        disasm.reload(); memory.reload()
    }
    @objc func memoryFollowCurrent(_ s: Any?) { model.followMachine = true; model.view.mode = 0; refreshAll() }

    @objc func gotoAddress(_ s: Any?) {
        guard let t = promptText("Go To Address", "Address or expression:", ""), !t.isEmpty else { return }
        guard let v = model.eval(t).0 else { NSSound.beep(); return }
        if lastPane === memory { memory.cursor = Int(v) & 0xFFFF; memory.topAddr = Int(v) & 0xFFF0; memory.anchor = nil }
        else { checkFollow.state = .off; disasm.followPC = false; disasm.topAddr = Int(v); disasm.selected = Int(v) & 0xFFFF }
        disasm.reload(); memory.reload()
    }

    @objc func findInPane(_ s: Any?) {
        let a = NSAlert(); a.messageText = "Find"; a.informativeText = "Enter search text (use ? for wildcard), or bytes separated by spaces."
        let f = NSTextField(frame: NSRect(x: 0, y: 0, width: 300, height: 24))
        let kind = NSPopUpButton(frame: NSRect(x: 0, y: 0, width: 300, height: 24)); kind.addItems(withTitles: ["Text", "Bytes (hex)"])
        let stack = NSStackView(views: [kind, f]); stack.orientation = .vertical; stack.frame = NSRect(x: 0, y: 0, width: 300, height: 56)
        a.accessoryView = stack; a.addButton(withTitle: "Find"); a.addButton(withTitle: "Cancel")
        guard a.runModal() == .alertFirstButtonReturn, !f.stringValue.isEmpty else { return }
        var pat: [UInt8] = [], wild: [Bool] = []
        if kind.indexOfSelectedItem == 0 { for ch in f.stringValue.utf8 { pat.append(ch == 63 ? 0 : ch); wild.append(ch == 63) } }
        else { for tok in f.stringValue.split(separator: " ") { if tok == "?" || tok == "??" { pat.append(0); wild.append(true) } else if let v = UInt8(tok, radix: 16) { pat.append(v); wild.append(false) } else { NSSound.beep(); return } } }
        lastFind = (pat, wild); findAgain(nil)
    }
    @objc func findAgain(_ s: Any?) {
        guard let (pat, wild) = lastFind else { findInPane(nil); return }
        let from = lastPane === memory ? memory.cursor : (disasm.selected >= 0 ? disasm.selected : model.pc)
        emu.lock.lock()
        let r = pat.withUnsafeBufferPointer { p in wild.withUnsafeBufferPointer { w in dbg_find(emu.cpc, &model.view, UInt16(from & 0xFFFF), p.baseAddress, w.baseAddress, Int32(pat.count), true) } }
        emu.lock.unlock()
        if r < 0 { NSSound.beep(); return }
        if lastPane === memory { memory.cursor = Int(r); memory.topAddr = Int(r) & 0xFFF0; memory.anchor = nil } else { disasm.followPC = false; checkFollow.state = .off; disasm.topAddr = Int(r); disasm.selected = Int(r) }
        disasm.reload(); memory.reload()
    }

    @objc func loadData(_ s: Any?) {
        let p = NSOpenPanel(); guard p.runModal() == .OK, let u = p.url, let d = try? Data(contentsOf: u) else { return }
        var addr = memory.cursor
        for b in d.prefix(65536) { model.write(addr, b); addr = (addr + 1) & 0xFFFF }
        refreshAll()
    }
    @objc func saveData(_ s: Any?) {
        let r = memory.selection
        let p = NSSavePanel(); p.nameFieldStringValue = String(format: "%04X-%04X.bin", r.lowerBound, r.upperBound)
        guard p.runModal() == .OK, let u = p.url else { return }
        try? Data(model.readBlock(r.lowerBound, r.count)).write(to: u)
    }

    @objc func showBreakpoints(_ s: Any?) { if breakpointsWindow == nil { breakpointsWindow = BreakpointsController(emu: emu, debugger: self) }; breakpointsWindow?.show() }
    @objc func showDataAreas(_ s: Any?) { if dataAreasWindow == nil { dataAreasWindow = DataAreasController(emu: emu, debugger: self) }; dataAreasWindow?.show() }
    @objc func showTimers(_ s: Any?) { if timersWindow == nil { timersWindow = TimersController(emu: emu) }; timersWindow?.show() }
    @objc func showGraphics(_ s: Any?) { if graphicsWindow == nil { graphicsWindow = GraphicsController(model: model) }; graphicsWindow?.show() }
    @objc func showChipRegisters(_ s: Any?) { if registersWindow == nil { registersWindow = RegistersController(emu: emu) }; registersWindow?.show() }

    // MARK: disassembler context menu
    func disasmContextMenu() -> NSMenu {
        let m = NSMenu()
        func add(_ t: String, _ sel: Selector, _ key: String = "") { let i = NSMenuItem(title: t, action: sel, keyEquivalent: key); i.target = self; m.addItem(i) }
        add("Find…", #selector(findInPane(_:))); add("Find Again", #selector(findAgain(_:))); m.addItem(.separator())
        add("Goto…", #selector(gotoAddress(_:))); m.addItem(.separator())
        add("Run to Cursor", #selector(runToCursor(_:))); add("Toggle Breakpoint", #selector(toggleBP(_:))); m.addItem(.separator())
        add("Disassemble…", #selector(disassembleRange(_:))); m.addItem(.separator())
        add("Mark as Data", #selector(markData(_:))); add("Clear Data Area", #selector(clearData(_:)))
        return m
    }
    @objc func runToCursor(_ s: Any?) {
        guard disasm.selected >= 0 else { return }
        emu.debugRunTo(UInt16(disasm.selected)); if hideOnRun { window?.orderOut(nil) }
    }
    @objc func toggleBP(_ s: Any?) { if disasm.selected >= 0 { model.toggleBreakpoint(at: disasm.selected); refreshAll() } }
    @objc func markData(_ s: Any?) {
        let r = lastPane === memory ? memory.selection : (disasm.selected >= 0 ? disasm.selected...disasm.selected : model.pc...model.pc)
        emu.lock.lock(); dbg_area_add(emu.cpc, UInt16(r.lowerBound), UInt16(r.upperBound), false); emu.lock.unlock(); refreshAll()
    }
    @objc func clearData(_ s: Any?) {
        guard disasm.selected >= 0 else { return }
        emu.lock.lock(); dbg_area_remove(emu.cpc, UInt16(disasm.selected)); emu.lock.unlock(); refreshAll()
    }
    @objc func disassembleRange(_ s: Any?) {
        guard let t = promptText("Disassemble", "Range (start,end):", String(format: "#%04X,#%04X", disasm.selected >= 0 ? disasm.selected : model.pc, ((disasm.selected >= 0 ? disasm.selected : model.pc) + 0x100) & 0xFFFF)) else { return }
        let parts = t.split(separator: ",").map { model.eval(String($0)).0 }
        guard parts.count == 2, let s0 = parts[0], let e0 = parts[1] else { NSSound.beep(); return }
        var out = "", a = Int(s0)
        while a <= Int(e0) { let (txt, n) = model.disasm(a); out += "    \(txt)\n"; a += max(n, 1) }
        let p = NSSavePanel(); p.nameFieldStringValue = "disassembly.asm"
        if p.runModal() == .OK, let u = p.url { try? out.write(to: u, atomically: true, encoding: .utf8) }
    }
}
