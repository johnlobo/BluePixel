import AppKit
import CPCCore

private let monoFont = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
private let lineHeight: CGFloat = 16

// MARK: - Shared look (WinAPE-style colours, adapted to dark mode)
private func dyn(_ light: UInt32, _ dark: UInt32) -> NSColor {
    func c(_ v: UInt32) -> NSColor { NSColor(srgbRed: CGFloat(v >> 16 & 255) / 255, green: CGFloat(v >> 8 & 255) / 255, blue: CGFloat(v & 255) / 255, alpha: 1) }
    return NSColor(name: nil) { $0.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua ? c(dark) : c(light) }
}
enum DebugStyle {
    static let mnemonic = dyn(0x800000, 0xFF7B72), register = dyn(0x800080, 0xD2A8FF), number = dyn(0x0000C0, 0x79C0FF)
    static let symbol = dyn(0xA05000, 0xFFA657), bytes = dyn(0x006000, 0x7EE787), address = NSColor.labelColor
    static let pcBack = NSColor.selectedContentBackgroundColor, gutter = dyn(0xD8D8D8, 0x3A3A3A)
    /// WinAPE's CPC_Symbols font draws the CPC graphic characters (&01-&1F, &7F-&FF). Its cmap is a
    /// format-0 byte table, which CoreText ignores, and CoreText does not render it as text either, so the
    /// glyph ids are read from the file and each glyph outline is filled as a path.
    private static let cpcGlyphs: [CGPath?]? = {
        guard let f = cpcFont else { return nil }
        return f.glyphs.map { $0 == 0 ? nil : CTFontCreatePathForGlyph(f.font, $0, nil) }
    }()
    private static let cpcAscent: CGFloat = cpcFont.map { CTFontGetAscent($0.font) } ?? 0
    private static let cpcFont: (font: CTFont, glyphs: [CGGlyph])? = {
        let candidates = [AppPaths.support.appendingPathComponent("CPC_Symbols.ttf"), URL(fileURLWithPath: NSHomeDirectory() + "/Documents/WinAPE20B2/CPC_Symbols.ttf")]
        guard let u = candidates.first(where: { FileManager.default.fileExists(atPath: $0.path) }), let data = try? Data(contentsOf: u),
              let descs = CTFontManagerCreateFontDescriptorsFromURL(u as CFURL) as? [CTFontDescriptor], let d = descs.first else { return nil }
        let b = [UInt8](data)
        func u16(_ o: Int) -> Int { o + 1 < b.count ? Int(b[o]) << 8 | Int(b[o + 1]) : 0 }
        func u32(_ o: Int) -> Int { u16(o) << 16 | u16(o + 2) }
        var cmap = -1
        for t in 0..<u16(4) where b.count > 12 + t * 16 + 4 && String(bytes: b[12 + t * 16..<12 + t * 16 + 4], encoding: .ascii) == "cmap" { cmap = u32(12 + t * 16 + 8) }
        guard cmap >= 0 else { return nil }
        for k in 0..<u16(cmap + 2) {
            let sub = cmap + u32(cmap + 4 + k * 8 + 4)
            if u16(sub) == 0 && sub + 6 + 256 <= b.count { return (CTFontCreateWithFontDescriptor(d, 12, nil), (0..<256).map { CGGlyph(b[sub + 6 + $0]) }) }
        }
        return nil
    }()
    /// Draws bytes as characters: ASCII in the monospaced font, CPC graphics from CPC_Symbols, one cell each.
    static func drawGlyphs(_ values: [UInt8], at p: NSPoint, cell: CGFloat, color: NSColor, in view: NSView) {
        for (i, v) in values.enumerated() {
            let x = p.x + CGFloat(i) * cell
            if v >= 0x20 && v < 0x7F {
                NSAttributedString(string: String(UnicodeScalar(v)), attributes: [.font: monoFont, .foregroundColor: color]).draw(at: NSPoint(x: x, y: p.y))
            } else if v != 0, let path = cpcGlyphs?[Int(v)], let ctx = NSGraphicsContext.current?.cgContext {
                ctx.saveGState()
                ctx.setFillColor(color.cgColor)
                ctx.translateBy(x: x, y: view.isFlipped ? p.y + cpcAscent + 1 : p.y)
                if view.isFlipped { ctx.scaleBy(x: 1, y: -1) }
                ctx.addPath(path); ctx.fillPath()
                ctx.restoreGState()
            } else {
                NSAttributedString(string: ".", attributes: [.font: monoFont, .foregroundColor: NSColor.tertiaryLabelColor]).draw(at: NSPoint(x: x, y: p.y))
            }
        }
    }
    private static let registers: Set<String> = ["A", "B", "C", "D", "E", "H", "L", "F", "I", "R", "AF", "BC", "DE", "HL", "SP", "IX", "IY", "IXH", "IXL", "IYH", "IYL", "AF'",
                                                  "NZ", "Z", "NC", "PO", "PE", "P", "M"]
    /// Colours an instruction: mnemonic, registers/conditions, numbers and symbols.
    static func instruction(_ text: String, highlighted: Bool) -> NSAttributedString {
        let out = NSMutableAttributedString()
        func add(_ s: String, _ c: NSColor) { out.append(NSAttributedString(string: s, attributes: [.font: monoFont, .foregroundColor: highlighted ? NSColor.alternateSelectedControlTextColor : c])) }
        let parts = text.split(separator: " ", maxSplits: 1)
        guard let m = parts.first else { return out }
        add(String(m).padding(toLength: 6, withPad: " ", startingAt: 0), mnemonic)
        guard parts.count > 1 else { return out }
        var tok = ""
        func flush() {
            guard !tok.isEmpty else { return }
            let up = tok.uppercased()
            if registers.contains(up) { add(tok, register) }
            else if tok.first == "#" || tok.first == "&" || tok.first == "$" || tok.first!.isNumber { add(tok, number) }
            else { add(tok, symbol) }
            tok = ""
        }
        for ch in parts[1] {
            if ch.isLetter || ch.isNumber || ch == "_" || ch == "#" || ch == "&" || ch == "$" || ch == "'" || ch == "." { tok.append(ch) }
            else { flush(); add(String(ch), NSColor.labelColor) }
        }
        flush()
        return out
    }
}

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
    /// One row per instruction; a symbol at an address gets its own "label:" row above it.
    private var lines: [(addr: Int, len: Int, text: String, isLabel: Bool)] = []
    private var charWidth: CGFloat { ("0" as NSString).size(withAttributes: [.font: monoFont]).width }
    private let gutterWidth: CGFloat = 14

    func reload() {
        let pc = model.pc
        if followPC {
            let visible = lines.filter { !$0.isLabel }.map { $0.addr }
            if !(visible.contains(pc) && (lines.firstIndex { $0.addr == pc && !$0.isLabel } ?? 0) < rows - 2) {
                var a = pc
                for _ in 0..<min(4, rows / 4) { a = model.previousInstruction(a) }
                topAddr = a
            }
        }
        var a = topAddr; lines = []
        while lines.count < rows {
            if let s = model.symbol(for: a) { lines.append((a, 0, s, true)); if lines.count >= rows { break } }
            let (t, n) = model.disasm(a); lines.append((a, n, t, false)); a = (a + max(n, 1)) & 0xFFFF
        }
        needsDisplay = true
    }

    func scrollLines(_ n: Int) {
        if n > 0 { for _ in 0..<n { topAddr = (topAddr + max(1, model.disasm(topAddr).1)) & 0xFFFF } }
        else { for _ in 0..<(-n) { topAddr = model.previousInstruction(topAddr) } }
        reload()
    }

    override func draw(_ dirtyRect: NSRect) {
        NSGraphicsContext.current?.saveGraphicsState(); defer { NSGraphicsContext.current?.restoreGraphicsState() }
        NSBezierPath(rect: bounds).setClip()
        NSColor.textBackgroundColor.setFill(); bounds.fill()
        DebugStyle.gutter.setFill(); NSRect(x: 0, y: 0, width: gutterWidth, height: bounds.height).fill()
        NSColor.separatorColor.setStroke(); NSBezierPath(rect: bounds.insetBy(dx: 0.5, dy: 0.5)).stroke()
        let pc = model.pc, cw = charWidth, x0 = gutterWidth + 4
        for (i, l) in lines.enumerated() {
            let y = CGFloat(i) * lineHeight
            if l.isLabel {
                NSAttributedString(string: l.text + ":", attributes: [.font: monoFont, .foregroundColor: DebugStyle.symbol]).draw(at: NSPoint(x: x0 + cw * 5, y: y))
                continue
            }
            let row = NSRect(x: gutterWidth, y: y, width: bounds.width - gutterWidth, height: lineHeight)
            let isPC = l.addr == pc
            if isPC { DebugStyle.pcBack.setFill(); row.fill() }
            else if l.addr == selected { NSColor.unemphasizedSelectedContentBackgroundColor.setFill(); row.fill() }
            if model.hasBreakpoint(at: l.addr) { NSColor.systemRed.setFill(); NSBezierPath(ovalIn: NSRect(x: 2, y: y + 3, width: 10, height: 10)).fill() }
            let fg: NSColor = isPC ? .alternateSelectedControlTextColor : DebugStyle.address
            NSAttributedString(string: String(format: "%04X", l.addr), attributes: [.font: monoFont, .foregroundColor: fg]).draw(at: NSPoint(x: x0, y: y))
            DebugStyle.instruction(model.symbolise(l.text), highlighted: isPC).draw(at: NSPoint(x: x0 + cw * 5, y: y))
            let raw = (0..<min(l.len, 4)).map { model.read(l.addr + $0) }
            let bytesX = max(x0 + cw * 40, bounds.width * 0.5)
            NSAttributedString(string: raw.map { String(format: "%02X", $0) }.joined(separator: " "), attributes: [.font: monoFont, .foregroundColor: isPC ? fg : DebugStyle.bytes]).draw(at: NSPoint(x: bytesX, y: y))
            DebugStyle.drawGlyphs(raw, at: NSPoint(x: bytesX + cw * 14, y: y), cell: cw, color: isPC ? fg : DebugStyle.bytes, in: self)
        }
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self); onFocus?()
        let p = convert(event.locationInWindow, from: nil)
        let i = Int(p.y / lineHeight)
        guard i >= 0, i < lines.count else { return }
        if p.x < gutterWidth && !lines[i].isLabel { model.toggleBreakpoint(at: lines[i].addr) }
        selected = lines[i].addr; onSelect?(selected); needsDisplay = true
        if event.clickCount == 2 && !lines[i].isLabel { model.toggleBreakpoint(at: lines[i].addr) }
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

// MARK: - Stack pane
final class StackListView: NSView {
    var model: DebugModel!
    var offset = 0 { didSet { needsDisplay = true } }   // words above/below SP being shown
    override var isFlipped: Bool { true }
    func reload() { needsDisplay = true }
    override func draw(_ dirtyRect: NSRect) {
        NSColor.textBackgroundColor.setFill(); bounds.fill()
        NSColor.separatorColor.setStroke(); NSBezierPath(rect: bounds.insetBy(dx: 0.5, dy: 0.5)).stroke()
        let sp = Int(model.emu.cpc.pointee.z80.sp.w)
        for r in 0..<Int(bounds.height / lineHeight) {
            let a = (sp + (offset + r) * 2) & 0xFFFF
            let v = Int(model.read(a)) | Int(model.read(a + 1)) << 8
            let current = offset + r == 0
            let text = String(format: "%04X: %04X", a, v) + (v > 0xFF ? model.symbol(for: v).map { " " + $0 } ?? "" : "")
            NSAttributedString(string: text, attributes: [.font: monoFont, .foregroundColor: current ? NSColor.systemBlue : NSColor.labelColor]).draw(at: NSPoint(x: 4, y: CGFloat(r) * lineHeight))
        }
    }
    override func scrollWheel(with event: NSEvent) { offset += event.scrollingDeltaY > 0 ? -2 : 2 }
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
                DebugStyle.drawGlyphs([bytes[i]], at: NSPoint(x: hexX + charW * 49 + CGFloat(i) * charW, y: y), cell: charW, color: DebugStyle.bytes, in: self)
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
