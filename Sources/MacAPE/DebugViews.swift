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

/// Debugger pane shortcuts (WinAPE: Ctrl+F find, Ctrl+G goto, F3 find again, F5 breakpoint/load, Ctrl+Tab hex/ASCII).
enum PaneCommand { case find, findAgain, goto, f5, toggleASCII, follow, back }
func paneCommand(for e: NSEvent) -> PaneCommand? {
    let mods = e.modifierFlags.intersection([.control, .command, .shift, .option])
    let key = e.charactersIgnoringModifiers?.lowercased() ?? ""
    if (mods == .control || mods == .command) && key == "f" { return .find }
    if (mods == .control || mods == .command) && key == "g" { return .goto }
    if mods == .control && e.keyCode == 48 { return .toggleASCII }      // Tab
    if mods.isEmpty || mods == .function {
        if e.keyCode == 99 { return .findAgain }                          // F3
        if e.keyCode == 96 { return .f5 }                                 // F5
    }
    return nil
}

// MARK: - Disassembly pane
final class DisasmView: NSView {
    var model: DebugModel!
    var topAddr = 0 { didSet { topAddr &= 0xFFFF; needsDisplay = true } }
    var selected = -1
    var anchor: Int?                       // start of a dragged/shift-clicked range
    var onSelect: ((Int) -> Void)?
    var onFocus: (() -> Void)?
    var onCommand: ((PaneCommand) -> Void)?
    var followPC = true
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    private var rows: Int { max(1, Int(bounds.height / lineHeight)) }
    /// Selected instructions as an address range (inclusive, up to the last byte of the last instruction).
    var selectionRange: ClosedRange<Int>? {
        guard selected >= 0 else { return nil }
        let a = anchor ?? selected, lo = min(a, selected), hi = max(a, selected)
        return lo...((hi + max(1, model.disasm(hi).1) - 1) & 0xFFFF)
    }
    /// One row per instruction.
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
            let inRange = selectionRange.map { anchor != nil && $0.contains(l.addr) } ?? false
            if isPC { DebugStyle.pcBack.setFill(); row.fill() }
            else if l.addr == selected || inRange { NSColor.unemphasizedSelectedContentBackgroundColor.setFill(); row.fill() }
            if model.hasBreakpoint(at: l.addr) { NSColor.systemRed.setFill(); NSBezierPath(ovalIn: NSRect(x: 2, y: y + 3, width: 10, height: 10)).fill() }
            let fg: NSColor = isPC ? .alternateSelectedControlTextColor : DebugStyle.address
            NSAttributedString(string: String(format: "%04X", l.addr), attributes: [.font: monoFont, .foregroundColor: fg]).draw(at: NSPoint(x: x0, y: y))
            // the operand column (after the 6-character mnemonic) holds 26 characters before the bytes;
            // longer symbolised operands are cut with an ellipsis
            var ins = model.symbolise(l.text)
            let parts = ins.split(separator: " ", maxSplits: 1)
            if parts.count == 2, parts[1].count > 26 { ins = parts[0] + " " + parts[1].prefix(25) + "…" }
            DebugStyle.instruction(ins, highlighted: isPC).draw(at: NSPoint(x: x0 + cw * 5, y: y))
            let raw = (0..<min(l.len, 4)).map { model.read(l.addr + $0) }
            let bytesX = x0 + cw * 38          // fixed columns: no gap that grows with the window
            NSAttributedString(string: raw.map { String(format: "%02X", $0) }.joined(separator: " "), attributes: [.font: monoFont, .foregroundColor: isPC ? fg : DebugStyle.bytes]).draw(at: NSPoint(x: bytesX, y: y))
            DebugStyle.drawGlyphs(raw, at: NSPoint(x: bytesX + cw * 14, y: y), cell: cw, color: isPC ? fg : DebugStyle.bytes, in: self)
            // symbols whose value is this address, after the characters (as in WinAPE)
            if let sym = model.symbol(for: l.addr) {
                NSAttributedString(string: sym, attributes: [.font: monoFont, .foregroundColor: isPC ? fg : DebugStyle.symbol]).draw(at: NSPoint(x: bytesX + cw * 20, y: y))
            }
        }
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self); onFocus?()
        let p = convert(event.locationInWindow, from: nil)
        let i = Int(p.y / lineHeight)
        guard i >= 0, i < lines.count else { return }
        if p.x < gutterWidth && !lines[i].isLabel { model.toggleBreakpoint(at: lines[i].addr) }
        if event.modifierFlags.contains(.shift) && selected >= 0 { if anchor == nil { anchor = selected } } else { anchor = nil }
        selected = lines[i].addr; onSelect?(selected); needsDisplay = true
        if event.clickCount == 2 && !lines[i].isLabel { model.toggleBreakpoint(at: lines[i].addr) }
    }
    override func mouseDragged(with event: NSEvent) {
        let i = Int(convert(event.locationInWindow, from: nil).y / lineHeight)
        guard i >= 0, i < lines.count else { return }
        if anchor == nil { anchor = selected }
        selected = lines[i].addr; needsDisplay = true
    }
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        guard window?.firstResponder === self, let c = paneCommand(for: event) else { return super.performKeyEquivalent(with: event) }
        onCommand?(c); return true
    }
    override func scrollWheel(with event: NSEvent) { followPC = false; scrollLines(event.scrollingDeltaY > 0 ? -3 : 3) }
    override func keyDown(with event: NSEvent) {
        switch event.keyCode {
        case 125: scrollLines(1); followPC = false
        case 126: scrollLines(-1); followPC = false
        case 121: scrollLines(rows - 1); followPC = false
        case 116: scrollLines(-(rows - 1)); followPC = false
        case 36, 76: onCommand?(.follow)        // Return / Enter: follow CALL/JP/JR target
        case 51: onCommand?(.back)              // Delete: back to where the last jump came from
        default: if let c = paneCommand(for: event) { onCommand?(c) } else { super.keyDown(with: event) }
        }
    }
    override func menu(for event: NSEvent) -> NSMenu? {
        if selectionRange.map({ !$0.contains(lineAddress(event) ?? -1) }) ?? true { mouseDown(with: event) }
        return (NSApp.delegate as? AppDelegate)?.debugger?.disasmContextMenu()
    }
    private func lineAddress(_ e: NSEvent) -> Int? { let i = Int(convert(e.locationInWindow, from: nil).y / lineHeight); return i >= 0 && i < lines.count ? lines[i].addr : nil }
}

// MARK: - Stack pane
final class StackListView: NSView {
    var model: DebugModel!
    var offset = 0 { didSet { needsDisplay = true } }   // words above/below SP being shown
    var onPick: ((Int) -> Void)?                          // double-click: show the address in the disassembler
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
            // return addresses (after a CALL/RST) are marked with the calling function
            let note: String
            if v > 0xFF, model.isReturnAddress(v), let caller = model.nearestSymbol(v - 1) { note = " ← " + caller }
            else if v > 0xFF, let s = model.symbol(for: v) { note = " " + s } else { note = "" }
            // "ADDR: VALUE" in a fixed column, then the caller/symbol (orange), cut to the pane width
            let cw = ("0" as NSString).size(withAttributes: [.font: monoFont]).width
            let x0: CGFloat = 8, y = CGFloat(r) * lineHeight
            NSAttributedString(string: String(format: "%04X: %04X", a, v), attributes: [.font: monoFont, .foregroundColor: current ? NSColor.systemBlue : NSColor.labelColor]).draw(at: NSPoint(x: x0, y: y))
            if !note.isEmpty {
                let room = max(0, Int((bounds.width - x0 - cw * 10 - 6) / cw))
                let shown = note.count > room ? String(note.prefix(max(0, room - 1))) + "…" : note
                NSAttributedString(string: shown, attributes: [.font: monoFont, .foregroundColor: DebugStyle.symbol]).draw(at: NSPoint(x: x0 + cw * 10, y: y))
            }
        }
    }
    override func scrollWheel(with event: NSEvent) { offset += event.scrollingDeltaY > 0 ? -2 : 2 }
    override func mouseDown(with event: NSEvent) {
        guard event.clickCount == 2 else { return }
        let r = Int(convert(event.locationInWindow, from: nil).y / lineHeight)
        let a = (Int(model.emu.cpc.pointee.z80.sp.w) + (offset + r) * 2) & 0xFFFF
        onPick?(Int(model.read(a)) | Int(model.read(a + 1)) << 8)
    }
}

// MARK: - Memory dump pane
final class MemoryView: NSView {
    var model: DebugModel!
    var topAddr = 0 { didSet { topAddr = topAddr & 0xFFF0; needsDisplay = true } }
    var cursor = 0                    // selected byte address
    var anchor: Int?                  // selection start for ranges
    var editNibble: Int? = nil
    var asciiMode = false             // typing edits the ASCII column (Ctrl+Tab or click it)
    var onFocus: (() -> Void)?
    var onCommand: ((PaneCommand) -> Void)?
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    var rows: Int { max(1, Int(bounds.height / lineHeight)) }
    private let hexX: CGFloat = 52, charW: CGFloat = 7.25
    private var asciiX: CGFloat { hexX + charW * 52 }     // three character gap after the hex column
    private var asciiW: CGFloat { charW * 1.3 }            // a little air between characters
    var selection: ClosedRange<Int> { let a = anchor ?? cursor; return min(a, cursor)...max(a, cursor) }

    func reload() { needsDisplay = true }
    func show(_ addr: Int) {
        cursor = addr & 0xFFFF; anchor = nil; editNibble = nil
        if cursor < topAddr || cursor >= topAddr + rows * 16 { topAddr = cursor & 0xFFF0 }
        needsDisplay = true
    }

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
                let cell = NSRect(x: x - 2, y: y, width: charW * 2 + 4, height: lineHeight)
                let ax = NSRect(x: asciiX + CGFloat(i) * asciiW, y: y, width: asciiW, height: lineHeight)
                // memory breakpoints: green read, red write, yellow both (as in WinAPE)
                switch model.watchBits(addr) {
                case 1: NSColor.systemGreen.withAlphaComponent(0.35).setFill(); cell.fill()
                case 2: NSColor.systemRed.withAlphaComponent(0.35).setFill(); cell.fill()
                case 3: NSColor.systemYellow.withAlphaComponent(0.45).setFill(); cell.fill()
                default: break
                }
                if selection.contains(addr) { NSColor.selectedContentBackgroundColor.withAlphaComponent(0.5).setFill(); cell.fill(); ax.fill() }
                if addr == cursor {
                    NSColor.systemOrange.withAlphaComponent(asciiMode ? 0.25 : 0.6).setFill(); cell.fill()
                    NSColor.systemOrange.withAlphaComponent(asciiMode ? 0.6 : 0.25).setFill(); ax.fill()
                }
                String(format: "%02X", bytes[i]).draw(at: NSPoint(x: x, y: y), withAttributes: attrs)
                DebugStyle.drawGlyphs([bytes[i]], at: NSPoint(x: asciiX + CGFloat(i) * asciiW + (asciiW - charW) / 2, y: y), cell: asciiW, color: DebugStyle.bytes, in: self)
            }
        }
    }

    /// Byte under the point and whether it is in the ASCII column.
    private func address(at p: NSPoint) -> (Int, Bool)? {
        let r = Int(p.y / lineHeight)
        guard r >= 0, r < rows else { return nil }
        if p.x >= asciiX - 2 {
            let col = Int((p.x - asciiX) / asciiW)
            return col >= 0 && col < 16 ? ((topAddr + r * 16 + col) & 0xFFFF, true) : nil
        }
        var col = Int((p.x - hexX + 2) / (charW * 3))
        if p.x > hexX + charW * 24 + charW * 3 * 8 { col = Int((p.x - hexX - charW + 2) / (charW * 3)) }
        guard col >= 0, col < 16 else { return nil }
        return ((topAddr + r * 16 + col) & 0xFFFF, false)
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self); onFocus?()
        if let (a, ascii) = address(at: convert(event.locationInWindow, from: nil)) {
            if event.modifierFlags.contains(.shift) { if anchor == nil { anchor = cursor } } else { anchor = nil }
            cursor = a; editNibble = nil; asciiMode = ascii; needsDisplay = true
        }
    }
    override func mouseDragged(with event: NSEvent) {
        if let (a, _) = address(at: convert(event.locationInWindow, from: nil)) { if anchor == nil { anchor = cursor }; cursor = a; needsDisplay = true }
    }
    override func menu(for event: NSEvent) -> NSMenu? {
        window?.makeFirstResponder(self); onFocus?()
        if let (a, _) = address(at: convert(event.locationInWindow, from: nil)), !selection.contains(a) { cursor = a; anchor = nil; needsDisplay = true }
        return (NSApp.delegate as? AppDelegate)?.debugger?.memoryContextMenu()
    }
    override func scrollWheel(with event: NSEvent) { topAddr += event.scrollingDeltaY > 0 ? -48 : 48; needsDisplay = true }
    private func move(_ d: Int, extend: Bool = false) {
        if extend { if anchor == nil { anchor = cursor } } else { anchor = nil }
        cursor = (cursor + d) & 0xFFFF; editNibble = nil
        if cursor < topAddr || cursor >= topAddr + rows * 16 { topAddr = cursor & 0xFFF0 - (d > 0 ? (rows - 1) * 16 : 0) }
        needsDisplay = true
    }
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        guard window?.firstResponder === self, let c = paneCommand(for: event) else { return super.performKeyEquivalent(with: event) }
        if c == .toggleASCII { asciiMode.toggle(); editNibble = nil; needsDisplay = true } else { onCommand?(c) }
        return true
    }
    override func keyDown(with event: NSEvent) {
        let ext = event.modifierFlags.contains(.shift)
        switch event.keyCode {
        case 123: move(-1, extend: ext); return
        case 124: move(1, extend: ext); return
        case 125: move(16, extend: ext); return
        case 126: move(-16, extend: ext); return
        case 121: topAddr += (rows - 1) * 16; return
        case 116: topAddr -= (rows - 1) * 16; return
        case 48: if event.modifierFlags.contains(.control) { asciiMode.toggle(); editNibble = nil; needsDisplay = true; return }
        default: break
        }
        if let c = paneCommand(for: event) { onCommand?(c); return }
        if asciiMode, let ch = event.characters?.unicodeScalars.first, ch.value >= 0x20, ch.value < 0x7F, !event.modifierFlags.contains(.command) {
            model.write(cursor, UInt8(ch.value)); move(1); return
        }
        if !asciiMode, let ch = event.charactersIgnoringModifiers?.lowercased().first, let v = ch.hexDigitValue {
            let old = Int(model.read(cursor))
            if editNibble == nil { model.write(cursor, UInt8((old & 0x0F) | (v << 4))); editNibble = 1 }
            else { model.write(cursor, UInt8((old & 0xF0) | v)); editNibble = nil; move(1) }
            needsDisplay = true
        } else { super.keyDown(with: event) }
    }
}
