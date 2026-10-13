import AppKit
import CPCCore

/// Displays the emulator frame and translates keyboard input.
final class EmulatorView: NSView {
    weak var emu: Emulator?
    private var pressed: [UInt16: Int] = [:]
    var numLock = true
    var keyTable: [UInt16: Int] = KeyMap.defaultMap
    var onFileDrop: ((URL) -> Void)?
    var aspect: CGFloat { 768.0 / 540.0 }

    var hideMouse = false
    override var acceptsFirstResponder: Bool { true }
    override func mouseMoved(with event: NSEvent) { if hideMouse { NSCursor.setHiddenUntilMouseMoves(true) }; amx(event) }
    // AMX mouse: movement and buttons go to the joystick port when enabled.
    private var amxButtons: UInt8 = 0
    private func amx(_ e: NSEvent, set: UInt8 = 0, clear: UInt8 = 0) {
        amxButtons = (amxButtons | set) & ~clear
        emu?.amxMouse(dx: e.deltaX, dy: e.deltaY, buttons: amxButtons)
    }
    override func mouseDragged(with event: NSEvent) { amx(event) }
    override func rightMouseDragged(with event: NSEvent) { amx(event) }
    override func mouseDown(with event: NSEvent) { amx(event, set: 1) }
    override func mouseUp(with event: NSEvent) { amx(event, clear: 1) }
    override func rightMouseDown(with event: NSEvent) { if emu?.amxEnabled == true { amx(event, set: 2) } else { super.rightMouseDown(with: event) } }
    override func rightMouseUp(with event: NSEvent) { if emu?.amxEnabled == true { amx(event, clear: 2) } else { super.rightMouseUp(with: event) } }
    override func updateTrackingAreas() { super.updateTrackingAreas(); trackingAreas.forEach(removeTrackingArea); addTrackingArea(NSTrackingArea(rect: bounds, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect], owner: self, userInfo: nil)) }
    override var isOpaque: Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        layer?.magnificationFilter = .nearest
        layer?.minificationFilter = .linear
        layer?.contentsGravity = .resizeAspect
        registerForDraggedTypes([.fileURL])
    }
    required init?(coder: NSCoder) { fatalError() }

    func refresh() {
        guard let emu = emu else { return }
        emu.withFrame { px, w, h in
            let data = Data(bytes: px, count: w * h * 4)
            guard let provider = CGDataProvider(data: data as CFData) else { return }
            let cs = CGColorSpace(name: CGColorSpace.sRGB)!
            let img = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4, space: cs,
                              bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue),
                              provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
            CATransaction.begin(); CATransaction.setDisableActions(true)
            layer?.contents = img
            CATransaction.commit()
        }
    }

    // MARK: Keyboard
    private func cpcKey(for code: UInt16) -> (key: Int, joystick: Bool)? {
        if let k = KeyMap.keypadFunction[code], numLock { return (k, false) }
        if !numLock, let j = KeyMap.keypadJoystick[code] { return (j, true) }
        if let k = keyTable[code] { return (k, false) }
        return nil
    }

    private func setKey(_ code: UInt16, down: Bool) {
        guard let emu = emu else { return }
        if code == 71 { if down { numLock.toggle() }; return }
        if down {
            guard pressed[code] == nil, let m = cpcKey(for: code) else { return }
            pressed[code] = m.key
            if m.joystick { emu.joystick(m.key & 7, down: true) } else { emu.key(m.key, down: true) }
        } else if let k = pressed.removeValue(forKey: code) {
            if k >> 3 == 9 && k != Int(CPCK_DEL) { emu.joystick(k & 7, down: false) } else { emu.key(k, down: false) }
        }
    }

    override func keyDown(with event: NSEvent) {
        if event.modifierFlags.contains(.command) { super.keyDown(with: event); return }
        if !event.isARepeat { setKey(event.keyCode, down: true) }
    }
    override func keyUp(with event: NSEvent) { setKey(event.keyCode, down: false) }

    override func flagsChanged(with event: NSEvent) {
        let f = event.modifierFlags
        let code = event.keyCode
        if KeyMap.shiftCodes.contains(code) { shiftState[code] = f.contains(.shift); syncModifier(CPCK_SHIFT, KeyMap.shiftCodes) }
        else if KeyMap.ctrlCodes.contains(code) { ctrlState[code] = f.contains(.control); syncModifier(CPCK_CTRL, KeyMap.ctrlCodes) }
        else if code == 57 { emu?.key(CPCK_CAPS, down: f.contains(.capsLock)) }
        else if code == 58 || code == 61 { setKey(code, down: f.contains(.option)) }
    }
    private var shiftState: [UInt16: Bool] = [:], ctrlState: [UInt16: Bool] = [:]
    private func syncModifier(_ key: Int, _ codes: Set<UInt16>) {
        let st = key == Int(CPCK_SHIFT) ? shiftState : ctrlState
        emu?.key(key, down: codes.contains { st[$0] == true })
    }

    override func resignFirstResponder() -> Bool { releaseAll(); return true }
    func releaseAll() {
        for (_, k) in pressed { if k >> 3 == 9 && k != Int(CPCK_DEL) { emu?.joystick(k & 7, down: false) } else { emu?.key(k, down: false) } }
        pressed.removeAll(); shiftState.removeAll(); ctrlState.removeAll()
        emu?.key(CPCK_SHIFT, down: false); emu?.key(CPCK_CTRL, down: false)
    }

    // MARK: Drag & drop
    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation { .copy }
    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        guard let urls = sender.draggingPasteboard.readObjects(forClasses: [NSURL.self], options: nil) as? [URL], let u = urls.first else { return false }
        onFileDrop?(u); return true
    }
}
