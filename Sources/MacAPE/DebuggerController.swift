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
    private let flagsField = NSTextField(string: "00000000")
    private let tLabel = NSTextField(string: "0")
    private let intsCheck = NSButton(checkboxWithTitle: "Ints", target: nil, action: nil)
    private let stackView = StackListView()
    private let lowerBasePopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let cartBankPopup = NSPopUpButton(frame: .zero, pullsDown: false)
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
        let win = NSWindow(contentRect: NSRect(x: 120, y: 120, width: 1000, height: 740), styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        win.title = "MacAPE Debugger"; win.isReleasedWhenClosed = false
        super.init(window: win)
        win.delegate = self
        disasm.model = model; memory.model = model; stackView.model = model
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
        regFields[reg] = f; return f
    }

    /// Lays subviews out top-down (frames are given with y measured from the top).
    final class Root: NSView {
        var onLayout: (() -> Void)?
        override var isFlipped: Bool { true }
        override func layout() { super.layout(); onLayout?() }
    }
    private let root = Root()

    private func box(_ title: String, _ views: [NSView]) -> NSBox {
        let b = NSBox(); b.title = title; b.titleFont = .systemFont(ofSize: 11)
        let st = NSStackView(views: views); st.orientation = .vertical; st.alignment = .leading; st.spacing = 4
        st.translatesAutoresizingMaskIntoConstraints = false
        let c = b.contentView!; c.addSubview(st)
        NSLayoutConstraint.activate([st.leadingAnchor.constraint(equalTo: c.leadingAnchor, constant: 6), st.topAnchor.constraint(equalTo: c.topAnchor, constant: 4),
                                     st.trailingAnchor.constraint(lessThanOrEqualTo: c.trailingAnchor, constant: -6)])
        root.addSubview(b); return b
    }
    private func hrow(_ views: [NSView]) -> NSStackView { let s = NSStackView(views: views); s.orientation = .horizontal; s.spacing = 6; return s }

    private func build() {
        guard let content = window?.contentView else { return }
        root.frame = content.bounds; root.autoresizingMask = [.width, .height]
        content.addSubview(root)

        // Right-hand column: flags, register pairs, IM/Ints, T counter, stack.
        let mono = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
        let flagCaption = makeLabel("SZ-H-VNC"); flagCaption.font = .monospacedSystemFont(ofSize: 12, weight: .regular); flagCaption.textColor = .systemBlue
        flagsField.font = mono; flagsField.alignment = .center; flagsField.target = self; flagsField.action = #selector(flagsEdited(_:))
        let flagsLabel = makeLabel("Flags")
        let pairs: [(Reg, Reg)] = [(.af, .af2), (.hl, .hl2), (.de, .de2), (.bc, .bc2), (.ix, .sp), (.iy, .i), (.pc, .r)]
        var regViews: [(NSTextField, NSTextField, NSTextField, NSTextField)] = []
        for (l, r) in pairs {
            let ll = makeLabel(l.rawValue), rl = makeLabel(r.rawValue)
            for x in [ll, rl] { x.font = .monospacedSystemFont(ofSize: 12, weight: .semibold); x.textColor = .systemBlue }
            regViews.append((ll, hexField(l), rl, hexField(r)))
        }
        let imLabel = makeLabel("IM"); imLabel.font = .monospacedSystemFont(ofSize: 12, weight: .semibold); imLabel.textColor = .systemBlue
        let imField = hexField(.im)
        intsCheck.target = self; intsCheck.action = #selector(intsToggled(_:))
        let tCaption = makeLabel("T"); tCaption.font = .monospacedSystemFont(ofSize: 12, weight: .semibold); tCaption.textColor = .systemBlue
        tLabel.isEditable = false; tLabel.font = mono
        let tReset = NSButton(image: NSImage(systemSymbolName: "xmark", accessibilityDescription: "Reset T")!, target: self, action: #selector(resetT(_:)))
        tReset.isBordered = false; tReset.contentTintColor = .systemRed; tReset.toolTip = "Reset T counter"
        let stackTitle = makeLabel("Stack")
        let rightViews: [NSView] = [flagCaption, flagsLabel, flagsField, imLabel, imField, intsCheck, tCaption, tLabel, tReset, stackTitle, stackView] + regViews.flatMap { [$0.0, $0.1, $0.2, $0.3] }
        rightViews.forEach { root.addSubview($0) }

        // Bottom: memory selection groups, file buttons, options and tool buttons.
        for (i, t) in ["Read", "Write", "Any"].enumerated() {
            let b = NSButton(radioButtonWithTitle: t, target: self, action: #selector(memorySelectionChanged(_:))); b.tag = i; modeButtons.append(b)
        }
        modeButtons[0].state = .on
        upperPopup.addItems(withTitles: (0..<32).map { String(format: "%02d", $0) })
        ramPopup.addItems(withTitles: (0..<64).map { String(format: "%02X", 0xC0 + $0) })
        lowerBasePopup.addItems(withTitles: ["#0000 (Default)", "#4000", "#8000"])
        cartBankPopup.addItems(withTitles: (0..<32).map { String($0) })
        for c in [lowerCheck, upperCheck] { c.target = self; c.action = #selector(memorySelectionChanged(_:)) }
        for p in [upperPopup, ramPopup, lowerBasePopup, cartBankPopup] { p.target = self; p.action = #selector(memorySelectionChanged(_:)); p.controlSize = .small; p.font = .systemFont(ofSize: 11) }
        upperCheck.title = "Upper ROM:"; lowerCheck.title = "Lower"
        let current = makeButton("Current", self, #selector(memoryFollowCurrent(_:))); current.controlSize = .small
        let memBox = box("Memory", modeButtons)
        let romBox = box("ROM", [hrow([lowerCheck, upperCheck, upperPopup])])
        let ramBox = box("RAM", [hrow([makeLabel("RAM Bank:"), ramPopup, current])])
        let secBox = box("Secondary ROM Mapping", [hrow([makeLabel("Lower ROM Base:"), lowerBasePopup, makeLabel("Cartridge Bank:"), cartBankPopup])])
        func symButton(_ sym: String, _ tip: String, _ sel: Selector, tint: NSColor? = nil) -> NSButton {
            let b = NSButton(image: NSImage(systemSymbolName: sym, accessibilityDescription: tip)!, target: self, action: sel)
            b.toolTip = tip; b.bezelStyle = .texturedRounded
            if let t = tint { b.image = b.image?.withSymbolConfiguration(NSImage.SymbolConfiguration(paletteColors: [t])) }
            root.addSubview(b); return b
        }
        let fileButtons = [symButton("folder", "Load Data", #selector(loadData(_:))), symButton("square.and.arrow.down", "Save Data", #selector(saveData(_:))),
                           symButton("arrow.right.to.line", "Go To Address", #selector(gotoAddress(_:)))]
        for b in [checkFollow, checkHide, checkBreaks, checkBRK] { b.target = self; b.action = #selector(optionsChanged(_:)); root.addSubview(b) }
        checkFollow.state = .on; checkBreaks.state = .on; checkHide.state = .on
        let tools = [symButton("circle.fill", "Edit Breakpoints", #selector(showBreakpoints(_:)), tint: .systemRed), symButton("list.bullet.rectangle", "Edit Data Areas", #selector(showDataAreas(_:))),
                     symButton("timer", "Show Timers", #selector(showTimers(_:))), symButton("cpu", "Chip Registers", #selector(showChipRegisters(_:))),
                     symButton("photo", "Find Graphics", #selector(showGraphics(_:))), symButton("magnifyingglass", "Search", #selector(findInPane(_:)))]
        root.addSubview(disasm); root.addSubview(memory)

        root.onLayout = { [weak self] in
            guard let self = self else { return }
            let w = self.root.bounds.width, h = self.root.bounds.height
            let rightW: CGFloat = 230, gap: CGFloat = 8, left = w - rightW - gap * 2
            let bottomH: CGFloat = 132
            // left panes
            let paneH = h - bottomH - gap * 2
            self.disasm.frame = NSRect(x: gap, y: gap, width: left, height: (paneH * 0.7).rounded())
            self.memory.frame = NSRect(x: gap, y: self.disasm.frame.maxY + 4, width: left, height: paneH - self.disasm.frame.height - 4)
            // bottom groups
            var x = gap; let by = h - bottomH
            func put(_ b: NSBox, _ width: CGFloat, _ height: CGFloat = 84) { b.frame = NSRect(x: x, y: by, width: width, height: height); x += width + 4 }
            put(memBox, 84, 96)
            let gx = x
            romBox.frame = NSRect(x: gx, y: by, width: 320, height: 46); ramBox.frame = NSRect(x: gx + 324, y: by, width: 232, height: 46)
            secBox.frame = NSRect(x: gx, y: by + 50, width: 556, height: 46)
            x = gx + 560
                for (i, b) in fileButtons.enumerated() { b.frame = NSRect(x: x + 4, y: by + 4 + CGFloat(i) * 32, width: 32, height: 26) }
            var cx = gap; let oy = h - 30
            for b in [self.checkFollow, self.checkHide, self.checkBreaks, self.checkBRK] { let bw = b.fittingSize.width; b.frame = NSRect(x: cx, y: oy, width: bw, height: 22); cx += bw + 14 }
            for b in tools { b.frame = NSRect(x: cx, y: oy - 2, width: 32, height: 26); cx += 36 }
            // right column
            let rx = w - rightW - gap
            flagCaption.frame = NSRect(x: rx + 96, y: gap, width: 110, height: 18)
            flagsLabel.frame = NSRect(x: rx + 40, y: gap + 22, width: 50, height: 20)
            self.flagsField.frame = NSRect(x: rx + 92, y: gap + 20, width: 92, height: 22)
            var ry = gap + 50
            for (ll, lf, rl, rf) in regViews {
                ll.frame = NSRect(x: rx, y: ry + 2, width: 30, height: 20); lf.frame = NSRect(x: rx + 30, y: ry, width: 58, height: 22)
                rl.frame = NSRect(x: rx + 100, y: ry + 2, width: 30, height: 20); rf.frame = NSRect(x: rx + 132, y: ry, width: rf === self.regFields[.i] || rf === self.regFields[.r] ? 40 : 58, height: 22)
                ry += 28
            }
            imLabel.frame = NSRect(x: rx, y: ry + 2, width: 30, height: 20); imField.frame = NSRect(x: rx + 30, y: ry, width: 40, height: 22)
            self.intsCheck.frame = NSRect(x: rx + 132, y: ry + 1, width: 70, height: 22); ry += 28
            tCaption.frame = NSRect(x: rx, y: ry + 2, width: 30, height: 20); self.tLabel.frame = NSRect(x: rx + 30, y: ry, width: 150, height: 22)
            tReset.frame = NSRect(x: rx + 186, y: ry, width: 22, height: 22); ry += 32
            stackTitle.frame = NSRect(x: rx, y: ry, width: 100, height: 18); ry += 20
            self.stackView.frame = NSRect(x: rx, y: ry, width: rightW, height: h - ry - gap)
            self.disasm.reload(); self.memory.reload(); self.stackView.reload()
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
        if window?.firstResponder !== flagsField.currentEditor() {
            let f = Int(z.af.l); flagsField.stringValue = (0..<8).map { (f >> (7 - $0)) & 1 == 1 ? "1" : "0" }.joined()
        }
        tLabel.stringValue = String(t)
        intsCheck.state = z.iff1 ? .on : .off
        stackView.reload()
    }

    private func syncMemoryControls() {
        let v = model.view
        modeButtons.forEach { $0.state = $0.tag == Int(v.mode) ? .on : .off }
        lowerCheck.state = v.lower ? .on : .off; upperCheck.state = v.upper ? .on : .off
        for i in 0..<32 {   // "00 - BASIC1-1": slot number and ROM name, as WinAPE shows it
            let name = emu.settings.string("ROMS", "Upper(\(i))")
            upperPopup.item(at: i)?.title = name.isEmpty ? String(format: "%02d", i) : String(format: "%02d - %@", i, (name as NSString).lastPathComponent)
        }
        upperPopup.selectItem(at: Int(min(v.upper_rom, 31)))
        let i = Int(v.ram_cfg) - 0xC0; ramPopup.selectItem(at: max(0, min(63, i)))
        let plus = emu.cpc.pointee.cfg.plus
        lowerBasePopup.isEnabled = plus; cartBankPopup.isEnabled = plus
        lowerBasePopup.selectItem(at: Int(min(v.lower_base, 2))); cartBankPopup.selectItem(at: Int(min(v.cart_bank, 31)))
    }

    // MARK: actions
    @objc func registerEdited(_ f: NSTextField) {
        guard let id = f.identifier?.rawValue, let r = Reg(rawValue: id) else { return }
        let (v, err) = model.eval(f.stringValue)
        if let v = v { emu.lock.lock(); r.set(&emu.cpc.pointee.z80, Int(v)); emu.lock.unlock() } else { NSSound.beep(); _ = err }
        refreshAll()
    }
    @objc func flagsEdited(_ f: NSTextField) {
        let t = f.stringValue.filter { $0 == "0" || $0 == "1" }
        guard t.count == 8, let v = UInt8(t, radix: 2) else { NSSound.beep(); refreshRegisters(); return }
        emu.lock.lock(); emu.cpc.pointee.z80.af.l = v; emu.lock.unlock(); refreshAll()
    }
    @objc func intsToggled(_ b: NSButton) {
        emu.lock.lock(); emu.cpc.pointee.z80.iff1 = b.state == .on; emu.cpc.pointee.z80.iff2 = b.state == .on; emu.lock.unlock()
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
        model.view.lower_base = UInt8(lowerBasePopup.indexOfSelectedItem); model.view.cart_bank = UInt8(cartBankPopup.indexOfSelectedItem)
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
