import AppKit
import CPCCore

private let monoFont = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
private let lineHeight: CGFloat = 16

// MARK: - Disassembly pane
final class DisasmView: NSView {
    var model: DebugModel!
    var topAddr = 0 { didSet { topAddr &= 0xFFFF; needsDisplay = true } }
    var selected = -1
    var onSelect: ((Int) -> Void)?
    var onFocus: (() -> Void)?
    var followPC = true
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    private var rows: Int { max(1, Int(bounds.height / lineHeight)) }
    private var lines: [(addr: Int, len: Int, text: String)] = []
    private var charWidth: CGFloat { ("0" as NSString).size(withAttributes: [.font: monoFont]).width }

    func reload() {
        let pc = model.pc
        if followPC {
            let visible = lines.map { $0.addr }
            if !(visible.contains(pc) && (visible.firstIndex(of: pc) ?? 0) < rows - 2) {
                var a = pc
                for _ in 0..<min(4, rows / 4) { a = model.previousInstruction(a) }
                topAddr = a
            }
        }
        var a = topAddr; lines = []
        for _ in 0..<rows { let (t, n) = model.disasm(a); lines.append((a, n, t)); a = (a + n) & 0xFFFF }
        needsDisplay = true
    }

    func scrollLines(_ n: Int) {
        if n > 0 { for _ in 0..<n { topAddr = (topAddr + model.disasm(topAddr).1) & 0xFFFF } }
        else { for _ in 0..<(-n) { topAddr = model.previousInstruction(topAddr) } }
        reload()
    }

    override func draw(_ dirtyRect: NSRect) {
        NSGraphicsContext.current?.saveGraphicsState(); defer { NSGraphicsContext.current?.restoreGraphicsState() }
        NSBezierPath(rect: bounds).setClip()
        NSColor.textBackgroundColor.setFill(); bounds.fill()
        NSColor.separatorColor.setStroke(); NSBezierPath(rect: bounds.insetBy(dx: 0.5, dy: 0.5)).stroke()
        if ProcessInfo.processInfo.environment["DBGLAYOUT"] != nil { FileHandle.standardError.write("disasm draw bounds=\(bounds) lines=\(lines.count) dirty=\(dirtyRect)\n".data(using: .utf8)!) }
        let pc = model.pc
        for (i, l) in lines.enumerated() {
            let y = CGFloat(i) * lineHeight
            let r = NSRect(x: 0, y: y, width: bounds.width, height: lineHeight)
            if l.addr == pc { NSColor.systemYellow.withAlphaComponent(0.35).setFill(); r.fill() }
            if l.addr == selected { NSColor.selectedContentBackgroundColor.withAlphaComponent(0.5).setFill(); r.fill() }
            if model.hasBreakpoint(at: l.addr) { NSColor.systemRed.setFill(); NSBezierPath(ovalIn: NSRect(x: 3, y: y + 3, width: 10, height: 10)).fill() }
            var bytes = ""
            for k in 0..<min(l.len, 4) { bytes += String(format: "%02X ", model.read(l.addr + k)) }
            // address  bytes  label:  instruction (operands shown as symbols when known)
            let attrs: [NSAttributedString.Key: Any] = [.font: monoFont, .foregroundColor: NSColor.labelColor]
            String(format: "%04X  %@", l.addr, bytes.padding(toLength: 12, withPad: " ", startingAt: 0)).draw(at: NSPoint(x: 18, y: y), withAttributes: attrs)
            if let s = model.symbol(for: l.addr) {
                let label = s.count > 17 ? String(s.prefix(16)) + "…:" : s + ":"
                label.draw(at: NSPoint(x: 18 + charWidth * 18, y: y), withAttributes: [.font: monoFont, .foregroundColor: NSColor.systemOrange])
            }
            model.symbolise(l.text).draw(at: NSPoint(x: 18 + charWidth * 37, y: y), withAttributes: [.font: monoFont, .foregroundColor: NSColor.systemBlue])
        }
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self); onFocus?()
        let p = convert(event.locationInWindow, from: nil)
        let i = Int(p.y / lineHeight)
        guard i >= 0, i < lines.count else { return }
        if p.x < 16 { model.toggleBreakpoint(at: lines[i].addr) }
        selected = lines[i].addr; onSelect?(selected); needsDisplay = true
    }
    override func scrollWheel(with event: NSEvent) { followPC = false; scrollLines(event.scrollingDeltaY > 0 ? -3 : 3) }
    override func keyDown(with event: NSEvent) {
        switch event.keyCode {
        case 125: scrollLines(1); followPC = false
        case 126: scrollLines(-1); followPC = false
        case 121: scrollLines(rows - 1); followPC = false
        case 116: scrollLines(-(rows - 1)); followPC = false
        default: super.keyDown(with: event)
        }
    }
    override func menu(for event: NSEvent) -> NSMenu? {
        mouseDown(with: event)
        return (NSApp.delegate as? AppDelegate)?.debugger?.disasmContextMenu()
    }
}

// MARK: - Memory dump pane
final class MemoryView: NSView {
    var model: DebugModel!
    var topAddr = 0 { didSet { topAddr = topAddr & 0xFFF0; needsDisplay = true } }
    var cursor = 0                    // selected byte address
    var anchor: Int?                  // selection start for ranges
    var editNibble: Int? = nil
    var onFocus: (() -> Void)?
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    var rows: Int { max(1, Int(bounds.height / lineHeight)) }
    private let hexX: CGFloat = 52, charW: CGFloat = 7.25
    var selection: ClosedRange<Int> { let a = anchor ?? cursor; return min(a, cursor)...max(a, cursor) }

    func reload() { needsDisplay = true }

    override func draw(_ dirtyRect: NSRect) {
        NSGraphicsContext.current?.saveGraphicsState(); defer { NSGraphicsContext.current?.restoreGraphicsState() }
        NSBezierPath(rect: bounds).setClip()
        NSColor.textBackgroundColor.setFill(); bounds.fill()
        NSColor.separatorColor.setStroke(); NSBezierPath(rect: bounds.insetBy(dx: 0.5, dy: 0.5)).stroke()
        let attrs: [NSAttributedString.Key: Any] = [.font: monoFont, .foregroundColor: NSColor.labelColor]
        for r in 0..<rows {
            let a = (topAddr + r * 16) & 0xFFFF
            let y = CGFloat(r) * lineHeight
            String(format: "%04X", a).draw(at: NSPoint(x: 4, y: y), withAttributes: [.font: monoFont, .foregroundColor: NSColor.secondaryLabelColor])
            let bytes = model.readBlock(a, 16)
            for i in 0..<16 {
                let addr = (a + i) & 0xFFFF
                let x = hexX + CGFloat(i) * charW * 3 + (i >= 8 ? charW : 0)
                if selection.contains(addr) { NSColor.selectedContentBackgroundColor.withAlphaComponent(0.5).setFill(); NSRect(x: x - 2, y: y, width: charW * 2 + 4, height: lineHeight).fill() }
                if addr == cursor { NSColor.systemOrange.withAlphaComponent(0.6).setFill(); NSRect(x: x - 2, y: y, width: charW * 2 + 4, height: lineHeight).fill() }
                String(format: "%02X", bytes[i]).draw(at: NSPoint(x: x, y: y), withAttributes: attrs)
                let ch = bytes[i] >= 32 && bytes[i] < 127 ? String(UnicodeScalar(bytes[i])) : "."
                ch.draw(at: NSPoint(x: hexX + charW * 49 + CGFloat(i) * charW, y: y), withAttributes: attrs)
            }
        }
    }

    private func address(at p: NSPoint) -> Int? {
        let r = Int(p.y / lineHeight)
        var col = Int((p.x - hexX + 2) / (charW * 3))
        if p.x > hexX + charW * 24 + charW * 3 * 8 { col = Int((p.x - hexX - charW + 2) / (charW * 3)) }
        guard r >= 0, r < rows, col >= 0, col < 16 else { return nil }
        return (topAddr + r * 16 + col) & 0xFFFF
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self); onFocus?()
        if let a = address(at: convert(event.locationInWindow, from: nil)) {
            if event.modifierFlags.contains(.shift) { if anchor == nil { anchor = cursor } } else { anchor = nil }
            cursor = a; editNibble = nil; needsDisplay = true
        }
    }
    override func mouseDragged(with event: NSEvent) {
        if let a = address(at: convert(event.locationInWindow, from: nil)) { if anchor == nil { anchor = cursor }; cursor = a; needsDisplay = true }
    }
    override func scrollWheel(with event: NSEvent) { topAddr += event.scrollingDeltaY > 0 ? -48 : 48; needsDisplay = true }
    private func move(_ d: Int) {
        cursor = (cursor + d) & 0xFFFF; anchor = nil; editNibble = nil
        if cursor < topAddr || cursor >= topAddr + rows * 16 { topAddr = cursor & 0xFFF0 - (d > 0 ? (rows - 1) * 16 : 0) }
        needsDisplay = true
    }
    override func keyDown(with event: NSEvent) {
        switch event.keyCode {
        case 123: move(-1); return
        case 124: move(1); return
        case 125: move(16); return
        case 126: move(-16); return
        case 121: topAddr += (rows - 1) * 16; return
        case 116: topAddr -= (rows - 1) * 16; return
        default: break
        }
        if let ch = event.charactersIgnoringModifiers?.lowercased().first, let v = ch.hexDigitValue {
            let old = Int(model.read(cursor))
            if editNibble == nil { model.write(cursor, UInt8((old & 0x0F) | (v << 4))); editNibble = 1 }
            else { model.write(cursor, UInt8((old & 0xF0) | v)); editNibble = nil; move(1) }
            needsDisplay = true
        } else { super.keyDown(with: event) }
    }
}
