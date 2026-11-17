import AppKit
import CPCCore

/// Copy of what the Plus ASIC window shows, taken under the machine lock so drawing never touches the live machine.
struct AsicSnapshot {
    var plus = false, unlocked = false, pageVisible = false
    var ram = [UInt8](repeating: 0, count: 0x4000)      // the ASIC register page: sprite pixels at 0, attributes at 0x2000
    var spal = [UInt32](repeating: 0, count: 16)        // sprite palette, display colours
    var pal = [UInt32](repeating: 0, count: 17)         // pens 0...15 and border, display colours
    var pal12 = [UInt16](repeating: 0, count: 17)       // the same pens as 12-bit values (0x0GRB)
    var pri: UInt8 = 0, splt: UInt8 = 0, ivr: UInt8 = 0, sscr: UInt8 = 0, dcsr: UInt8 = 0, icsr: UInt8 = 0
    var ssa: UInt16 = 0
    var dma: [(addr: UInt16, prescale: Int, pause: Int, stopped: Bool)] = []

    init() {}
    init(_ c: CPC) {
        plus = c.cfg.plus
        let a = c.asic
        unlocked = a.unlocked; pageVisible = a.page_visible
        ram = withUnsafePointer(to: c) { Array(UnsafeBufferPointer(start: asic_register_page($0), count: 0x4000)) }
        spal = withUnsafeBytes(of: a.spal_rgb) { Array($0.bindMemory(to: UInt32.self)) }
        pal = withUnsafeBytes(of: a.pal_rgb) { Array($0.bindMemory(to: UInt32.self)) }
        pal12 = withUnsafeBytes(of: a.pal12) { Array($0.bindMemory(to: UInt16.self)) }
        pri = a.pri; splt = a.splt; ivr = a.ivr; sscr = a.sscr; dcsr = a.dcsr; icsr = a.icsr; ssa = a.ssa
        dma = [a.dma.0, a.dma.1, a.dma.2].map { (addr: $0.addr, prescale: Int($0.prescale), pause: Int($0.pause), stopped: $0.stopped) }
    }

    /// Sprite n as the core draws it: X and Y are signed 16-bit CRTC-line/CPC-pixel positions, magnification 0 hides it.
    func sprite(_ n: Int) -> (x: Int, y: Int, magX: Int, magY: Int) {
        let o = 0x2000 + n * 8
        func mag(_ v: UInt8) -> Int { v == 3 ? 4 : Int(v) }
        return (Int(Int16(bitPattern: UInt16(ram[o]) | UInt16(ram[o + 1]) << 8)), Int(Int16(bitPattern: UInt16(ram[o + 2]) | UInt16(ram[o + 3]) << 8)),
                mag(ram[o + 4] & 3), mag((ram[o + 4] >> 2) & 3))
    }
}

/// Sprite sheet, palettes and the ASIC control registers, drawn directly.
final class AsicView: NSView {
    var snap = AsicSnapshot() { didSet { needsDisplay = true } }
    override var isFlipped: Bool { true }

    private let font = NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)
    private let small = NSFont.monospacedSystemFont(ofSize: 10, weight: .regular)
    private func color(_ rgb: UInt32) -> NSColor { NSColor(red: CGFloat((rgb >> 16) & 255) / 255, green: CGFloat((rgb >> 8) & 255) / 255, blue: CGFloat(rgb & 255) / 255, alpha: 1) }
    private func text(_ s: String, _ p: NSPoint, font f: NSFont? = nil, color c: NSColor = .labelColor) {
        NSAttributedString(string: s, attributes: [.font: f ?? font, .foregroundColor: c]).draw(at: p)
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor.textBackgroundColor.setFill(); bounds.fill()
        var y: CGFloat = 8
        let state = !snap.plus ? "Not a Plus machine: there is no ASIC" : snap.unlocked ? "ASIC unlocked" : "ASIC locked (sprites and DMA are inactive)"
        text(state + (snap.plus ? "   register page \(snap.pageVisible ? "mapped at #4000" : "not mapped")" : ""), NSPoint(x: 8, y: y), color: snap.unlocked ? .labelColor : .systemOrange)
        y += 22
        guard snap.plus else { return }

        // Sprites: 8 per row, each 16x16 pixels shown at 4x (the real aspect is wider, but the data is what matters here).
        let scale: CGFloat = 4, cellW: CGFloat = 16 * scale + 14, cellH: CGFloat = 16 * scale + 30
        text("Sprites (pen 0 is transparent)", NSPoint(x: 8, y: y), color: .secondaryLabelColor); y += 18
        for n in 0..<16 {
            let ox = 8 + CGFloat(n % 8) * cellW, oy = y + CGFloat(n / 8) * cellH
            let s = snap.sprite(n), visible = s.magX > 0 && s.magY > 0
            for py in 0..<16 { for px in 0..<16 {
                let pen = Int(snap.ram[n * 256 + py * 16 + px] & 15)
                (pen == 0 ? ((px + py) & 1 == 0 ? NSColor(white: 0.30, alpha: 1) : NSColor(white: 0.22, alpha: 1)) : color(snap.spal[pen])).setFill()
                NSRect(x: ox + CGFloat(px) * scale, y: oy + CGFloat(py) * scale, width: scale, height: scale).fill()
            } }
            if !visible { NSColor(white: 0, alpha: 0.55).setFill(); NSRect(x: ox, y: oy, width: 16 * scale, height: 16 * scale).fill() }
            let c: NSColor = visible ? .labelColor : .tertiaryLabelColor
            text(String(format: "S%d  %dx%d", n, s.magX, s.magY), NSPoint(x: ox, y: oy + 16 * scale + 2), font: small, color: c)
            text("x=\(s.x) y=\(s.y)", NSPoint(x: ox, y: oy + 16 * scale + 14), font: small, color: c)
        }
        y += cellH * 2 + 6

        // Palettes
        func palette(_ title: String, _ items: [(UInt32, String)]) {
            text(title, NSPoint(x: 8, y: y), color: .secondaryLabelColor); y += 18
            for (i, it) in items.enumerated() {
                let ox = 8 + CGFloat(i % 9) * 70, oy = y + CGFloat(i / 9) * 34
                color(it.0).setFill(); NSRect(x: ox, y: oy, width: 28, height: 16).fill()
                NSColor.separatorColor.setStroke(); NSBezierPath(rect: NSRect(x: ox + 0.5, y: oy + 0.5, width: 27, height: 15)).stroke()
                text(it.1, NSPoint(x: ox + 31, y: oy + 2), font: small)
            }
            y += CGFloat((items.count + 8) / 9) * 34 + 2
        }
        palette("Pen palette (pens 0-15, border) — #0GRB", (0..<17).map { (snap.pal[$0], String(format: "%@ %03X", $0 == 16 ? "Bd" : String(format: "%X", $0), snap.pal12[$0] & 0xFFF)) })
        palette("Sprite palette (1-15) — #0GRB", (1..<16).map { i in
            let w = UInt16(snap.ram[0x2420 + i * 2]) | UInt16(snap.ram[0x2420 + i * 2 + 1] & 15) << 8
            return (snap.spal[i], String(format: "%X %03X", i, w))
        })

        // Registers
        text("Registers", NSPoint(x: 8, y: y), color: .secondaryLabelColor); y += 18
        let lines = [
            String(format: "PRI  (raster IRQ line) %02X (%d)      SPLT (split line) %02X (%d)      SSA (split address) %04X", snap.pri, snap.pri, snap.splt, snap.splt, snap.ssa),
            String(format: "SSCR (soft scroll) %02X: x %d  y %d      IVR (IM2 vector) %02X      DCSR %02X  ICSR %02X", snap.sscr, snap.sscr & 15, (snap.sscr >> 4) & 7, snap.ivr, snap.dcsr, snap.icsr),
        ] + snap.dma.enumerated().map { i, d in
            String(format: "DMA %d  %@  address %04X  pause %d  prescale %d%@", i, (snap.dcsr >> UInt8(i)) & 1 == 1 ? "on " : "off", d.addr, d.pause, d.prescale, d.stopped ? "  (stopped)" : "")
        }
        for l in lines { text(l, NSPoint(x: 8, y: y)); y += 16 }
    }
}

/// Debug > ASIC Sprites…: refreshed while visible.
final class AsicController: NSObject {
    let emu: Emulator
    let panel = makePanel("CPC Emulator - ASIC Sprites and Registers", width: 640, height: 580)
    private let view = AsicView()
    private var timer: Timer?

    init(emu: Emulator) {
        self.emu = emu
        super.init()
        fill(panel, with: view, inset: 0)
        timer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in if self?.panel.isVisible == true { self?.reload() } }
    }
    func show() { reload(); panel.makeKeyAndOrderFront(nil) }
    func reload() {
        emu.lock.lock(); let s = AsicSnapshot(emu.cpc.pointee); emu.lock.unlock()
        view.snap = s
    }
}
