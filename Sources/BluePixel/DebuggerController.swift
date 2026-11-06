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
    var lastFind: FindQuery?
    var symbolsWindow: SymbolsController?
    var historyWindow: HistoryController?
    var watchWindow: WatchController?
    var profilerWindow: ProfilerController?
    var accessMapWindow: AccessMapController?
    var logWindow: LogController?
    private let screenPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let beamLabel = makeLabel("")
    var flashMessage: String?
    private var flashTimer: Timer?
    private let followPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let followRegs: [Reg?] = [nil, .hl, .de, .bc, .ix, .iy, .sp, .pc]
    private var history: [(top: Int, selected: Int)] = []        // disassembler navigation (Enter / Delete)
    private var lastStopRegs: [Reg: Int] = [:], lastStopFlags = -1
    private var changedRegs: Set<Reg> = [], flagsChanged = false
    private var lastStopInstructions: UInt64 = .max
    var compareWindow: NSPanel?

    init(emulator: Emulator) {
        emu = emulator; model = DebugModel(emu: emulator)
        let win = NSWindow(contentRect: NSRect(x: 120, y: 120, width: 980, height: 740), styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        win.title = "BluePixel Debugger"; win.isReleasedWhenClosed = false
        win.contentMinSize = NSSize(width: 980, height: 600)   // below this the bottom groups would reach the right column
        super.init(window: win)
        win.delegate = self
        disasm.model = model; memory.model = model; stackView.model = model
        disasm.onFocus = { [weak self] in self?.lastPane = self?.disasm }
        memory.onFocus = { [weak self] in self?.lastPane = self?.memory }
        disasm.onCommand = { [weak self] c in self?.paneCommand(c, memoryPane: false) }
        memory.onCommand = { [weak self] c in self?.paneCommand(c, memoryPane: true) }
        stackView.onPick = { [weak self] a in self?.showInDisassembler(a) }
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
        let secBox = box("Secondary ROM Mapping", [hrow([makeLabel("Lower ROM:"), lowerBasePopup, makeLabel("Cart Bank:"), cartBankPopup])])
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
                     symButton("photo", "Find Graphics", #selector(showGraphics(_:))), symButton("magnifyingglass", "Search", #selector(findInPane(_:))),
                    ]
        let fileButtons2 = [symButton("tag", "Symbols", #selector(showSymbols(_:))), symButton("clock.arrow.circlepath", "Execution History", #selector(showHistory(_:))),
                            symButton("eye", "Watch", #selector(showWatch(_:)))]
        let logButton = symButton("text.alignleft", "Log (logpoints)", #selector(showLog(_:)))
        let backButton = symButton("arrow.uturn.backward", "Step Back (⌥F7) — during a replay", #selector(stepBack(_:)), tint: .systemYellow)
        let backOverButton = symButton("arrow.uturn.backward.circle", "Step Back Over (⌥⌘F8): before the CALL that just returned, or the whole LDIR", #selector(stepBackOver(_:)), tint: .systemYellow)
        let revButton = symButton("backward.fill", "Reverse Continue (⌥F9): back to the previous breakpoint", #selector(reverseContinue(_:)), tint: .systemYellow)
        screenPopup.addItems(withTitles: ["Screen: Normal", "Screen: Raster", "Screen: Video RAM"]); screenPopup.controlSize = .small; screenPopup.font = .systemFont(ofSize: 11)
        screenPopup.toolTip = "What the emulator display shows while paused: last frame, frame being drawn with the beam position, or video memory as it is now"
        screenPopup.target = self; screenPopup.action = #selector(screenViewChanged(_:)); root.addSubview(screenPopup)
        beamLabel.font = .monospacedSystemFont(ofSize: 10, weight: .regular); beamLabel.textColor = .secondaryLabelColor; beamLabel.lineBreakMode = .byTruncatingTail
        root.addSubview(beamLabel)
        followPopup.addItems(withTitles: ["Memory: no follow", "Follow HL", "Follow DE", "Follow BC", "Follow IX", "Follow IY", "Follow SP", "Follow PC"])
        followPopup.controlSize = .mini; followPopup.font = .systemFont(ofSize: 10); followPopup.target = self; followPopup.action = #selector(followChanged(_:))
        followPopup.toolTip = "Keep the memory dump at the address held in a register after every step"
        root.addSubview(followPopup)
        root.addSubview(disasm); root.addSubview(memory)

        root.onLayout = { [weak self] in
            guard let self = self else { return }
            let w = self.root.bounds.width, h = self.root.bounds.height
            let rightW: CGFloat = 210, gap: CGFloat = 8, left = w - rightW - gap * 3
            let bottomH: CGFloat = 132
            // left panes
            let paneH = h - bottomH - gap * 2 - 14      // room for the beam indicator
            self.disasm.frame = NSRect(x: gap, y: gap, width: left, height: (paneH * 0.7).rounded())
            self.memory.frame = NSRect(x: gap, y: self.disasm.frame.maxY + 4, width: left, height: paneH - self.disasm.frame.height - 4)
            // bottom groups
            var x = gap; let by = h - bottomH
            func put(_ b: NSBox, _ width: CGFloat, _ height: CGFloat = 84) { b.frame = NSRect(x: x, y: by, width: width, height: height); x += width + 4 }
            put(memBox, 84, 96)
            let gx = x
            romBox.frame = NSRect(x: gx, y: by, width: 320, height: 46); ramBox.frame = NSRect(x: gx + 324, y: by, width: 232, height: 46)
            secBox.frame = NSRect(x: gx, y: by + 50, width: 400, height: 46)
            x = gx + 560
                for (i, b) in fileButtons.enumerated() { b.frame = NSRect(x: x + 4, y: by + 4 + CGFloat(i) * 32, width: 32, height: 26) }
            for (i, b) in fileButtons2.enumerated() { b.frame = NSRect(x: x + 40, y: by + 4 + CGFloat(i) * 32, width: 32, height: 26) }
            var cx = gap; let oy = h - 30
            for b in [self.checkFollow, self.checkHide, self.checkBreaks, self.checkBRK] { let bw = b.fittingSize.width; b.frame = NSRect(x: cx, y: oy, width: bw, height: 22); cx += bw + 10 }
            for b in tools + [logButton, backButton, backOverButton, revButton] { b.frame = NSRect(x: cx, y: oy - 2, width: 30, height: 26); cx += 33 }
            backButton.isHidden = !self.emu.replayActive; backOverButton.isHidden = !self.emu.replayActive; revButton.isHidden = !self.emu.replayActive
            self.screenPopup.frame = NSRect(x: gx + 406, y: by + 66, width: 148, height: 24)
            self.beamLabel.frame = NSRect(x: gap, y: self.memory.frame.maxY + 1, width: left - 132, height: 14)
            self.followPopup.frame = NSRect(x: gap + left - 128, y: self.memory.frame.maxY, width: 128, height: 16)
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
    /// Shown whenever the machine stops (pause, step, breakpoint): the disassembler jumps back to PC
    /// and selects it, even if the user had scrolled elsewhere.
    func show() {
        window?.makeKeyAndOrderFront(nil); window?.makeFirstResponder(disasm)
        noteStop()
        root.needsLayout = true                 // the reverse buttons only exist during a replay
        refreshAll()
        watchWindow?.noteStop()
        focusPC()
    }
    /// Registers that changed since the previous stop are shown in red (WinAPE/RVM style).
    private func noteStop() {
        emu.lock.lock(); let z = emu.cpc.pointee.z80; emu.lock.unlock()
        guard z.instructions != lastStopInstructions else { return }      // re-shown without running: keep the colours
        lastStopInstructions = z.instructions
        var now: [Reg: Int] = [:]; for r in Reg.allCases { now[r] = r.get(z) }
        changedRegs = lastStopRegs.isEmpty ? [] : Set(Reg.allCases.filter { r in r != .r && now[r] != lastStopRegs[r] })
        flagsChanged = lastStopFlags >= 0 && lastStopFlags != Int(z.af.l)
        lastStopRegs = now; lastStopFlags = Int(z.af.l)
    }
    func focusPC() {
        let follow = disasm.followPC
        disasm.followPC = true; disasm.anchor = nil; disasm.selected = model.pc
        disasm.reload()
        disasm.followPC = follow || checkFollow.state == .on
        lastPane = disasm
    }
    func windowDidResize(_ notification: Notification) { refreshAll() }
    /// Closing the debugger resumes emulation, like Run (F9) in WinAPE.
    func windowWillClose(_ notification: Notification) { if emu.paused { emu.debugResume() } }

    func refreshAll() {
        model.sync()
        syncMemoryControls()
        refreshRegisters()
        disasm.followPC = checkFollow.state == .on
        disasm.reload()
        if memory.topAddr == 0 && memory.cursor == 0 { memory.cursor = model.pc; memory.topAddr = model.pc & 0xFFF0 }
        memory.reload()
        breakpointsWindow?.reload(); registersWindow?.reload(); timersWindow?.reload(); dataAreasWindow?.reload(); graphicsWindow?.reload()
        if emu.paused { historyWindow?.reload(); if emu.screenView != .normal { emu.publish() } }
        if emu.paused, let r = followRegs[followPopup.indexOfSelectedItem] {
            emu.lock.lock(); let z = emu.cpc.pointee.z80; emu.lock.unlock()
            memory.show(r.get(z)); memory.reload()
        }
        beamLabel.stringValue = flashMessage ?? (emu.paused ? emu.beamDescription + "   (⌥F8 next scanline, ⌥⇧F8 next frame)" : "")
        beamLabel.textColor = flashMessage == nil ? .secondaryLabelColor : .systemOrange
    }

    func refreshRegisters() {
        emu.lock.lock()
        let z = emu.cpc.pointee.z80
        let t = emu.cpc.pointee.t_counter
        emu.lock.unlock()
        for (r, f) in regFields where window?.firstResponder !== f.currentEditor() {
            f.stringValue = String(format: "%0\(r.digits)X", r.get(z))
            f.textColor = emu.paused && changedRegs.contains(r) ? .systemRed : .labelColor
        }
        flagsField.textColor = emu.paused && flagsChanged ? .systemRed : .labelColor
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
        if let v = v { emu.noteUnreproducible("a register was edited"); emu.lock.lock(); r.set(&emu.cpc.pointee.z80, Int(v)); dbg_changes_rebase(emu.cpc); emu.lock.unlock() } else { NSSound.beep(); _ = err }
        refreshAll()
    }
    @objc func flagsEdited(_ f: NSTextField) {
        let t = f.stringValue.filter { $0 == "0" || $0 == "1" }
        guard t.count == 8, let v = UInt8(t, radix: 2) else { NSSound.beep(); refreshRegisters(); return }
        emu.noteUnreproducible("the flags were edited")
        emu.lock.lock(); emu.cpc.pointee.z80.af.l = v; dbg_changes_rebase(emu.cpc); emu.lock.unlock(); refreshAll()
    }
    @objc func intsToggled(_ b: NSButton) {
        emu.noteUnreproducible("the interrupt flag was changed")
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

    private func paneCommand(_ c: PaneCommand, memoryPane: Bool) {
        lastPane = memoryPane ? memory : disasm
        switch c {
        case .find: findInPane(nil)
        case .findAgain: findAgain(nil)
        case .goto: gotoAddress(nil)
        case .f5: if memoryPane { loadData(nil) } else { toggleBP(nil) }
        case .toggleASCII: break
        case .follow: followTarget()
        case .back: navigateBack()
        }
    }
    /// Moves the disassembler to an address, remembering where it was for Delete (back).
    func showInDisassembler(_ a: Int) {
        if disasm.selected >= 0 || !history.isEmpty { history.append((disasm.topAddr, disasm.selected)); if history.count > 100 { history.removeFirst() } }
        checkFollow.state = .off; disasm.followPC = false; disasm.anchor = nil
        disasm.topAddr = a & 0xFFFF; disasm.selected = a & 0xFFFF; disasm.reload()
        window?.makeFirstResponder(disasm); lastPane = disasm
    }

    /// Enter on a CALL/JP/JR/DJNZ/RST shows its destination without executing anything; on other
    /// instructions a 16-bit operand ((#nnnn) or #nnnn) is shown in the memory pane.
    func followTarget() {
        let a = disasm.selected >= 0 ? disasm.selected : model.pc
        let text = model.disasm(a).0.uppercased()
        let mnemonic = text.split(separator: " ").first.map(String.init) ?? ""
        emu.lock.lock(); let z = emu.cpc.pointee.z80; emu.lock.unlock()
        func lastHex() -> Int? {
            guard let r = text.range(of: "#[0-9A-F]+", options: [.regularExpression, .backwards]) else { return nil }
            return Int(text[r].dropFirst(), radix: 16)
        }
        if ["CALL", "JP", "JR", "DJNZ", "RST"].contains(mnemonic) {
            var target: Int?
            if text.contains("(HL)") { target = Int(z.hl.w) } else if text.contains("(IX)") { target = Int(z.ix.w) } else if text.contains("(IY)") { target = Int(z.iy.w) } else { target = lastHex() }
            if let t = target { showInDisassembler(t); return }
        } else if let v = lastHex(), text.range(of: "#[0-9A-F]{4}", options: .regularExpression) != nil {
            memory.show(v); memory.reload(); lastPane = memory; window?.makeFirstResponder(memory); return
        }
        NSSound.beep()
    }
    func navigateBack() {
        guard let h = history.popLast() else { NSSound.beep(); return }
        checkFollow.state = .off; disasm.followPC = false; disasm.anchor = nil
        disasm.topAddr = h.top; disasm.selected = h.selected; disasm.reload()
        window?.makeFirstResponder(disasm); lastPane = disasm
    }

    // MARK: symbols
    @objc func loadSymbols(_ s: Any?) {
        let p = NSOpenPanel(); p.title = "Load Symbols"; p.message = "NoICE (.noi), SDCC map, Maxam/WinAPE or label = value lists"
        guard p.runModal() == .OK, let u = p.url else { return }
        emu.lock.lock(); _ = dbg_attach(emu.cpc); let n = dbg_symbols_load(emu.cpc, u.path); emu.lock.unlock()
        if n < 0 { let a = NSAlert(); a.messageText = "Cannot load symbols"; a.informativeText = u.path; a.runModal(); return }
        window?.title = "BluePixel Debugger — \(n) symbols from \(u.lastPathComponent)"
        (NSApp.delegate as? AppDelegate)?.symbolsURL = u
        refreshAll(); symbolsWindow?.reload()
    }
    func symbolsReloaded(_ note: String) { window?.title = "BluePixel Debugger" + note; refreshAll() }
    @objc func clearSymbols(_ s: Any?) {
        emu.lock.lock(); dbg_symbol_clear(emu.cpc); emu.lock.unlock()
        window?.title = "BluePixel Debugger"
        refreshAll(); symbolsWindow?.reload()
    }
    @objc func followChanged(_ s: Any?) { refreshAll() }
    /// A message in the line under the memory pane, in orange, for a few seconds (results of reverse operations).
    func flash(_ text: String) {
        flashMessage = text; flashTimer?.invalidate()
        flashTimer = Timer.scheduledTimer(withTimeInterval: 10, repeats: false) { [weak self] _ in self?.flashMessage = nil; self?.refreshAll() }
        refreshAll()
    }
    @objc func showProfiler(_ s: Any?) { if profilerWindow == nil { profilerWindow = ProfilerController(debugger: self) }; profilerWindow?.show() }
    @objc func showAccessMap(_ s: Any?) { if accessMapWindow == nil { accessMapWindow = AccessMapController(debugger: self) }; accessMapWindow?.show() }
    @objc func showWatch(_ s: Any?) { if watchWindow == nil { watchWindow = WatchController(debugger: self) }; watchWindow?.show() }
    @objc func showLog(_ s: Any?) { if logWindow == nil { logWindow = LogController(debugger: self) }; logWindow?.show() }
    @objc func addLogpoint(_ s: Any?) {
        let a = disasm.selected >= 0 ? disasm.selected : model.pc
        guard let msg = promptText("Add Logpoint", String(format: "Message logged each time #%04X runs (the machine keeps running). {expr} hex, {expr:d} decimal:", a), "A={A} HL={HL}"), !msg.isEmpty else { return }
        emu.lock.lock()
        let i = dbg_add_code_bp(emu.cpc, UInt16(a))
        if i >= 0 { dbg_set_log(emu.cpc, i, msg) }
        emu.lock.unlock()
        refreshAll(); showLog(nil)
    }
    @objc func showHistory(_ s: Any?) {
        if historyWindow == nil { historyWindow = HistoryController(debugger: self) }
        historyWindow?.show()
    }
    @objc func screenViewChanged(_ s: Any?) { setScreenView(Emulator.ScreenView(rawValue: screenPopup.indexOfSelectedItem) ?? .normal) }
    func setScreenView(_ v: Emulator.ScreenView) { emu.screenView = v; screenPopup.selectItem(at: v.rawValue) }

    @objc func showSymbols(_ s: Any?) {
        if symbolsWindow == nil { symbolsWindow = SymbolsController(debugger: self) }
        symbolsWindow?.show()
    }

    @objc func gotoAddress(_ s: Any?) {
        let inMemory = lastPane === memory
        guard let t = promptText("Goto", inMemory ? "Position the memory cursor at (address, symbol or expression):" : "Position the disassembler at (address, symbol or expression):", ""), !t.isEmpty else { return }
        guard let v = model.eval(t).0 else { NSSound.beep(); return }
        if inMemory { memory.show(Int(v)) } else { showInDisassembler(Int(v)) }
        disasm.reload(); memory.reload()
    }

    @objc func findInPane(_ s: Any?) {
        guard let q = FindQuery.ask(previous: lastFind) else { return }
        lastFind = q; findAgain(nil)
    }
    @objc func findAgain(_ s: Any?) {
        guard let q = lastFind else { findInPane(nil); return }
        let inMemory = lastPane === memory
        let from = inMemory ? memory.cursor : (disasm.selected >= 0 ? disasm.selected : model.pc)
        guard let r = model.find(q, after: from) else { NSSound.beep(); return }
        if inMemory { memory.show(r); if q.mode != .assembler { memory.anchor = r; memory.cursor = (r + q.length - 1) & 0xFFFF } }
        else { showInDisassembler(r) }
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

    // MARK: context menus (the shortcuts shown are handled by the panes)
    private func fkey(_ n: Int) -> String { String(UnicodeScalar(UInt32(NSF1FunctionKey) + UInt32(n - 1))!) }
    private func menuItem(_ m: NSMenu, _ t: String, _ sel: Selector, _ key: String = "", _ mods: NSEvent.ModifierFlags = []) {
        let i = NSMenuItem(title: t, action: sel, keyEquivalent: key); i.keyEquivalentModifierMask = mods; i.target = self; m.addItem(i)
    }
    func disasmContextMenu() -> NSMenu {
        lastPane = disasm
        let m = NSMenu()
        menuItem(m, "Find…", #selector(findInPane(_:)), "f", .control); menuItem(m, "Find Again", #selector(findAgain(_:)), fkey(3)); m.addItem(.separator())
        menuItem(m, "Goto…", #selector(gotoAddress(_:)), "g", .control); m.addItem(.separator())
        menuItem(m, "Run to Cursor", #selector(runToCursor(_:)), fkey(4))
        if emu.replayActive { menuItem(m, "Run Back to Here", #selector(reverseToCursor(_:)), fkey(4), .option) }
        menuItem(m, "Toggle Breakpoint", #selector(toggleBP(_:)), fkey(5))
        menuItem(m, "Add Logpoint…", #selector(addLogpoint(_:))); m.addItem(.separator())
        menuItem(m, "Disassemble…", #selector(disassembleRange(_:))); m.addItem(.separator())
        menuItem(m, "Mark as Data", #selector(markData(_:))); menuItem(m, "Clear Data Area", #selector(clearData(_:)))
        return m
    }
    func memoryContextMenu() -> NSMenu {
        lastPane = memory
        let m = NSMenu()
        menuItem(m, "Find…", #selector(findInPane(_:)), "f", .control); menuItem(m, "Find Again", #selector(findAgain(_:)), fkey(3)); m.addItem(.separator())
        menuItem(m, "Goto…", #selector(gotoAddress(_:)), "g", .control); menuItem(m, "Select Block…", #selector(selectBlock(_:))); m.addItem(.separator())
        menuItem(m, "Load…", #selector(loadData(_:)), fkey(5)); menuItem(m, "Save…", #selector(saveData(_:))); m.addItem(.separator())
        menuItem(m, "Breakpoint on Read", #selector(memoryBreakRead(_:))); menuItem(m, "Breakpoint on Write", #selector(memoryBreakWrite(_:))); menuItem(m, "Break on Change", #selector(memoryBreakChange(_:))); m.addItem(.separator())
        if emu.replayActive { menuItem(m, "Who Wrote This? (Go Back)", #selector(memoryWhoWrote(_:))); menuItem(m, "Who Read This? (Go Back)", #selector(memoryWhoRead(_:))); m.addItem(.separator()) }
        menuItem(m, "Compare To…", #selector(compareTo(_:))); menuItem(m, "Fill…", #selector(fillMemory(_:))); menuItem(m, "Disassemble…", #selector(disassembleRange(_:))); m.addItem(.separator())
        menuItem(m, "Mark as Data", #selector(markData(_:))); menuItem(m, "Clear Data Area", #selector(clearData(_:)))
        menuItem(m, memory.asciiMode ? "Edit Hex" : "Edit ASCII", #selector(toggleASCII(_:)), "\t", .control)
        return m
    }
    @objc func toggleASCII(_ s: Any?) { memory.asciiMode.toggle(); memory.editNibble = nil; memory.reload() }

    /// Range from "start,end" or "start" + length text; used by Select Block / Compare / Fill.
    private func range(from text: String) -> ClosedRange<Int>? {
        let parts = text.split(separator: ",").map { model.eval(String($0).trimmingCharacters(in: .whitespaces)).0 }
        guard parts.count == 2, let a = parts[0], let b = parts[1] else { return nil }
        return Int(min(a, b)) & 0xFFFF...Int(max(a, b)) & 0xFFFF
    }
    @objc func selectBlock(_ s: Any?) {
        let r = memory.selection
        guard let t = promptText("Select Block", "Start, End:", String(format: "#%04X,#%04X", r.lowerBound, r.upperBound)), let sel = range(from: t) else { return }
        memory.show(sel.lowerBound); memory.anchor = sel.lowerBound; memory.cursor = sel.upperBound; memory.reload()
    }
    @objc func memoryBreakRead(_ s: Any?) { addMemoryBreak(read: true) }
    @objc func memoryBreakWrite(_ s: Any?) { addMemoryBreak(read: false) }
    /// A write that actually changes the value (`value<>previous`), over the selected bytes.
    @objc func memoryBreakChange(_ s: Any?) {
        let r = memory.selection
        emu.lock.lock()
        let i = dbg_add_mem_bp(emu.cpc, UInt16(r.lowerBound), UInt16(r.upperBound), false, true)
        if i >= 0 { _ = dbg_set_condition(emu.cpc, i, "value<>previous", 0) }
        emu.lock.unlock()
        refreshAll()
    }
    private func addMemoryBreak(read: Bool) {
        let r = memory.selection
        emu.lock.lock(); _ = dbg_add_mem_bp(emu.cpc, UInt16(r.lowerBound), UInt16(r.upperBound), read, !read); emu.lock.unlock()
        refreshAll()
    }
    @objc func fillMemory(_ s: Any?) {
        let r = memory.selection
        let a = NSAlert(); a.messageText = "Fill Memory"; a.informativeText = String(format: "Fill #%04X-#%04X (%d bytes) with:", r.lowerBound, r.upperBound, r.count)
        let kind = NSPopUpButton(frame: NSRect(x: 0, y: 30, width: 300, height: 24)); kind.addItems(withTitles: ["Hex bytes (e.g. 01 02 03)", "String"])
        let f = NSTextField(frame: NSRect(x: 0, y: 0, width: 300, height: 24))
        let v = NSView(frame: NSRect(x: 0, y: 0, width: 300, height: 56)); v.addSubview(kind); v.addSubview(f)
        a.accessoryView = v; a.addButton(withTitle: "Fill"); a.addButton(withTitle: "Cancel"); a.window.initialFirstResponder = f
        guard a.runModal() == .alertFirstButtonReturn, !f.stringValue.isEmpty else { return }
        let pat: [UInt8] = kind.indexOfSelectedItem == 0 ? f.stringValue.split(separator: " ").compactMap { UInt8($0, radix: 16) } : Array(f.stringValue.utf8)
        guard !pat.isEmpty else { NSSound.beep(); return }
        emu.lock.lock(); pat.withUnsafeBufferPointer { dbg_fill(emu.cpc, &model.view, UInt16(r.lowerBound), UInt16(r.upperBound), $0.baseAddress, Int32(pat.count)) }; emu.lock.unlock()
        refreshAll()
    }
    @objc func compareTo(_ s: Any?) {
        let r = memory.selection
        let a = NSAlert(); a.messageText = "Compare Memory"
        a.informativeText = String(format: "Compare #%04X-#%04X (%d bytes) with an address or a file.", r.lowerBound, r.upperBound, r.count)
        let kind = NSPopUpButton(frame: NSRect(x: 0, y: 30, width: 300, height: 24)); kind.addItems(withTitles: ["Address", "File…"])
        let f = NSTextField(frame: NSRect(x: 0, y: 0, width: 300, height: 24)); f.placeholderString = "Address (memory) — leave empty for File"
        let v = NSView(frame: NSRect(x: 0, y: 0, width: 300, height: 56)); v.addSubview(kind); v.addSubview(f)
        a.accessoryView = v; a.addButton(withTitle: "Compare"); a.addButton(withTitle: "Cancel")
        guard a.runModal() == .alertFirstButtonReturn else { return }
        let mine = model.readBlock(r.lowerBound, r.count)
        var other: [UInt8], otherName: String
        if kind.indexOfSelectedItem == 1 {
            let o = NSOpenPanel(); guard o.runModal() == .OK, let u = o.url, let d = try? Data(contentsOf: u) else { return }
            other = [UInt8](d.prefix(r.count)); otherName = u.lastPathComponent
        } else {
            guard let b = model.eval(f.stringValue).0 else { NSSound.beep(); return }
            other = model.readBlock(Int(b), r.count); otherName = String(format: "#%04X", Int(b) & 0xFFFF)
        }
        var lines: [String] = [], i = 0
        while i < mine.count {
            guard i >= other.count || mine[i] != other[i] else { i += 1; continue }
            var j = i; while j < mine.count && (j >= other.count || mine[j] != other[j]) { j += 1 }
            let hex = { (b: [UInt8]) in b.prefix(16).map { String(format: "%02X", $0) }.joined(separator: " ") + (b.count > 16 ? " …" : "") }
            lines.append(String(format: "#%04X  %4d byte(s)   ", (r.lowerBound + i) & 0xFFFF, j - i) + hex(Array(mine[i..<j])) + "   |   " + hex(j <= other.count ? Array(other[i..<j]) : Array(other[min(i, other.count)...])))
            i = j
        }
        showCompareResults(String(format: "#%04X-#%04X compared with %@: ", r.lowerBound, r.upperBound, otherName) + (lines.isEmpty ? "identical" : "\(lines.count) difference(s)"), lines)
    }
    private func showCompareResults(_ title: String, _ lines: [String]) {
        let p = compareWindow ?? makePanel("Comparison Results", width: 720, height: 360)
        let tv = NSTextView(); tv.isEditable = false; tv.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        tv.string = title + "\n\n" + lines.joined(separator: "\n")
        let sc = NSScrollView(); sc.documentView = tv; sc.hasVerticalScroller = true; tv.autoresizingMask = [.width]
        p.contentView = sc; compareWindow = p; p.makeKeyAndOrderFront(nil)
    }
    @objc func runToCursor(_ s: Any?) {
        guard disasm.selected >= 0 else { return }
        emu.debugRunTo(UInt16(disasm.selected)); if hideOnRun { window?.orderOut(nil) }
    }
    @objc func toggleBP(_ s: Any?) { if disasm.selected >= 0 { model.toggleBreakpoint(at: disasm.selected); refreshAll() } }
    @objc func markData(_ s: Any?) {
        let r = lastPane === memory ? memory.selection : (disasm.selectionRange ?? model.pc...model.pc)
        emu.lock.lock(); dbg_area_add(emu.cpc, UInt16(r.lowerBound), UInt16(r.upperBound), false); emu.lock.unlock(); refreshAll()
    }
    @objc func clearData(_ s: Any?) {
        let a = lastPane === memory ? memory.cursor : disasm.selected
        guard a >= 0 else { return }
        emu.lock.lock(); dbg_area_remove(emu.cpc, UInt16(a)); emu.lock.unlock(); refreshAll()
    }
    @objc func disassembleRange(_ s: Any?) {
        let sel = lastPane === memory ? memory.selection : (disasm.selectionRange ?? model.pc...((model.pc + 0xFF) & 0xFFFF))
        guard let t = promptText("Disassemble", "Range (start,end):", String(format: "#%04X,#%04X", sel.lowerBound, sel.upperBound)) else { return }
        let parts = t.split(separator: ",").map { model.eval(String($0)).0 }
        guard parts.count == 2, let s0 = parts[0], let e0 = parts[1] else { NSSound.beep(); return }
        var out = "", a = Int(s0)
        while a <= Int(e0) { let (txt, n) = model.disasm(a); out += "    \(txt)\n"; a += max(n, 1) }
        let p = NSSavePanel(); p.nameFieldStringValue = "disassembly.asm"
        if p.runModal() == .OK, let u = p.url { try? out.write(to: u, atomically: true, encoding: .utf8) }
    }
}
