import AppKit
import CPCCore

/// Window with the emulation view and WinAPE-style control panel.
final class MainWindowController: NSWindowController, NSWindowDelegate {
    let emu: Emulator
    let view = EmulatorView(frame: .zero)
    let panel = NSView()
    private var buttons: [String: NSButton] = [:]
    private let ledA = NSView(), ledB = NSView()
    private let trackA = NSTextField(labelWithString: "99"), trackB = NSTextField(labelWithString: "99")
    private let timing = NSTextField(labelWithString: "Emulation Timing")
    private let playback = NSTextField(labelWithString: "0:00:00")
    private var timer: Timer?
    private var lastFrames: UInt64 = 0, lastTime = Date()
    static let panelHeight: CGFloat = 30
    private var transportBar: NSView?
    var panelHidden = false { didSet { panel.isHidden = panelHidden; layoutContent() } }

    init(emulator: Emulator) {
        emu = emulator
        let win = NSWindow(contentRect: NSRect(x: 100, y: 100, width: 768, height: 540 + MainWindowController.panelHeight),
                           styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        win.title = "BluePixel"
        win.contentMinSize = NSSize(width: 384, height: 270 + MainWindowController.panelHeight)
        win.collectionBehavior = [.fullScreenPrimary]
        super.init(window: win)
        win.delegate = self
        let content = NSView(frame: win.contentLayoutRect)
        content.autoresizingMask = [.width, .height]
        win.contentView = content
        view.emu = emu
        content.addSubview(view); content.addSubview(panel)
        buildPanel()
        emu.onFrame = { [weak self] in self?.view.refresh() }
        timer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in self?.updateStatus() }
        layoutContent()
        win.makeFirstResponder(view)
    }
    required init?(coder: NSCoder) { fatalError() }

    private func buildPanel() {
        panel.wantsLayer = true
        panel.layer?.backgroundColor = NSColor(white: 0.16, alpha: 1).cgColor
        let defs: [(String, String, String, Selector)] = [
            ("run", "play.fill", "Run (F9)", #selector(AppDelegate.debugRun(_:))),
            ("step", "forward.frame.fill", "Single Step (F7)", #selector(AppDelegate.debugStep(_:))),
            ("over", "arrow.uturn.right", "Step Over (F8)", #selector(AppDelegate.debugStepOver(_:))),
            ("pause", "pause.fill", "Pause (F7/F8)", #selector(AppDelegate.debugPause(_:))),
            ("regs", "cpu", "Registers", #selector(AppDelegate.showRegisters(_:))),
            ("asm", "chevron.left.forwardslash.chevron.right", "Assembler (F3)", #selector(AppDelegate.showAssembler(_:))),
            ("disc", "opticaldisc", "Change Disc (F2)", #selector(AppDelegate.driveSetup(_:))),
            ("cart", "memorychip", "Change Cartridge (CTRL+F3)", #selector(AppDelegate.loadCartridge(_:))),
            ("tape", "recordingtape", "Tape Control", #selector(AppDelegate.showTape(_:))),
            ("snapL", "square.and.arrow.down", "Load Snapshot (F5)", #selector(AppDelegate.loadSnapshot(_:))),
            ("snapS", "square.and.arrow.up", "Save Snapshot (F6)", #selector(AppDelegate.saveSnapshot(_:))),
        ]
        for (id, sym, tip, sel) in defs { buttons[id] = makeButton(sym, tip, sel) }
        let right: [(String, String, String, Selector)] = [
            ("setup", "gearshape", "Settings (F12)", #selector(AppDelegate.showGeneral(_:))),
            ("pokes", "wand.and.stars", "Pokes (CTRL+F8)", #selector(AppDelegate.showPokes(_:))),
            ("full", "arrow.up.left.and.arrow.down.right", "Toggle Full Screen (⌃⌘F)", #selector(AppDelegate.toggleFullScreenAction(_:))),
            ("help", "questionmark.circle", "Help (F1)", #selector(AppDelegate.showHelp(_:))),
        ]
        for (id, sym, tip, sel) in right { buttons[id] = makeButton(sym, tip, sel) }
        for v in [ledA, ledB] { v.wantsLayer = true; v.layer?.backgroundColor = NSColor.darkGray.cgColor; v.layer?.cornerRadius = 3; panel.addSubview(v) }
        for t in [trackA, trackB, timing, playback] { t.font = .monospacedSystemFont(ofSize: 11, weight: .regular); t.textColor = .white; panel.addSubview(t) }
        for (id, text) in [("A", "A:"), ("B", "B:")] { let l = NSTextField(labelWithString: text); l.textColor = .lightGray; l.font = .systemFont(ofSize: 11); l.identifier = NSUserInterfaceItemIdentifier("lbl" + id); panel.addSubview(l) }
    }

    private func makeButton(_ symbol: String, _ tip: String, _ action: Selector) -> NSButton {
        let b = NSButton(image: NSImage(systemSymbolName: symbol, accessibilityDescription: tip) ?? NSImage(), target: nil, action: action)
        b.isBordered = false; b.contentTintColor = .white; b.toolTip = tip; b.imageScaling = .scaleProportionallyDown
        panel.addSubview(b); return b
    }

    func layoutContent() {
        guard let content = window?.contentView else { return }
        let ph = panelHidden ? 0 : MainWindowController.panelHeight
        let th = transportBar == nil ? 0 : TransportBar.height
        let b = content.bounds
        panel.frame = NSRect(x: 0, y: 0, width: b.width, height: ph)
        transportBar?.frame = NSRect(x: 0, y: ph, width: b.width, height: th)
        view.frame = NSRect(x: 0, y: ph + th, width: b.width, height: b.height - ph - th)
        var x: CGFloat = 8
        for id in ["run", "step", "over", "pause", "regs", "asm", "disc", "cart", "tape", "snapL", "snapS"] {
            buttons[id]?.frame = NSRect(x: x, y: 4, width: 22, height: 22); x += 26
        }
        x += 8
        func place(_ v: NSView, w: CGFloat) { v.frame = NSRect(x: x, y: 7, width: w, height: 16); x += w + 4 }
        if let l = panel.subviews.first(where: { $0.identifier?.rawValue == "lblA" }) { place(l, w: 18) }
        place(ledA, w: 14); place(trackA, w: 22)
        if let l = panel.subviews.first(where: { $0.identifier?.rawValue == "lblB" }) { place(l, w: 18) }
        place(ledB, w: 14); place(trackB, w: 22)
        place(playback, w: 56)
        var rx = b.width - 8
        for id in ["help", "full", "pokes", "setup"] { rx -= 26; buttons[id]?.frame = NSRect(x: rx, y: 4, width: 22, height: 22) }
        timing.frame = NSRect(x: x + 8, y: 7, width: max(40, rx - x - 12), height: 16)
        timing.alignment = .right
    }

    /// Shows the transport bar under the display (growing the window to make room) or removes it.
    func setTransport(_ bar: NSView?) {
        guard transportBar !== bar, let content = window?.contentView else { return }
        let fullScreen = window?.styleMask.contains(.fullScreen) ?? false
        if let old = transportBar { old.removeFromSuperview(); transportBar = nil; if !fullScreen, let w = window { w.setContentSize(NSSize(width: content.bounds.width, height: content.bounds.height - TransportBar.height)) } }
        if let new = bar {
            content.addSubview(new); transportBar = new
            if !fullScreen, let w = window { w.setContentSize(NSSize(width: content.bounds.width, height: content.bounds.height + TransportBar.height)) }
        }
        layoutContent()
    }

    func windowDidResize(_ notification: Notification) { layoutContent() }
    func windowWillClose(_ notification: Notification) { NSApp.terminate(nil) }

    /// Resize window to an integral multiple of the CPC screen.
    func setContentScale(_ scale: CGFloat) {
        guard let w = window else { return }
        let ph = panelHidden ? 0 : MainWindowController.panelHeight
        w.setContentSize(NSSize(width: 768 * scale, height: 540 * scale + ph))
    }

    func updateStatus() {
        let c = emu.cpc
        let motor = fdc_motor(c.pointee.fdc)
        ledA.layer?.backgroundColor = (motor ? NSColor.systemRed : NSColor.darkGray).cgColor
        ledB.layer?.backgroundColor = NSColor.darkGray.cgColor
        trackA.stringValue = String(format: "%02d", fdc_track(c.pointee.fdc, 0))
        trackB.stringValue = String(format: "%02d", fdc_track(c.pointee.fdc, 1))
        let now = Date(); let dt = now.timeIntervalSince(lastTime)
        if dt >= 1 {
            let fr = c.pointee.frames
            let pct = Double(fr >= lastFrames ? fr - lastFrames : 0) / dt / 50.08 * 100      // frames go backwards when a state is restored
            timing.stringValue = String(format: "Emulation Timing %3.0f%%", pct)
            lastFrames = fr; lastTime = now
        }
        let base = emu.discPaths[0].split(separator: "/").last.map { "BluePixel — \($0)" } ?? "BluePixel"
        let status = emu.sessionStatus
        window?.title = status.isEmpty ? base : base + "     " + status
    }
}
