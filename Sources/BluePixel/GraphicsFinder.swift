import AppKit
import CPCCore
import UniformTypeIdentifiers

/// Draws the decoded bitmap, scrolls memory with the wheel and keys, and reports the pixel under the pointer.
final class GraphicsView: NSView {
    var image: CGImage? { didSet { needsDisplay = true } }
    var zoom = 4 { didSet { needsDisplay = true } }
    var onScroll: ((Int) -> Void)?                  // wheel: rows (positive = forward in memory)
    var onKey: ((UInt16, Bool) -> Void)?            // key code, shift
    var onHover: ((NSPoint?) -> Void)?              // position in image pixels, nil when the pointer leaves
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    private var wheelRemainder: CGFloat = 0

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: bounds, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect], owner: self))
    }
    override func draw(_ dirtyRect: NSRect) {
        NSColor.black.setFill(); bounds.fill()
        guard let img = image else { return }
        NSGraphicsContext.current?.imageInterpolation = .none
        NSImage(cgImage: img, size: NSSize(width: img.width * zoom, height: img.height * zoom))
            .draw(in: NSRect(x: 0, y: 0, width: img.width * zoom, height: img.height * zoom), from: .zero, operation: .copy, fraction: 1, respectFlipped: true, hints: nil)
    }
    override func mouseDown(with event: NSEvent) { window?.makeFirstResponder(self) }
    override func mouseMoved(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        onHover?(NSPoint(x: p.x / CGFloat(zoom), y: p.y / CGFloat(zoom)))
    }
    override func mouseExited(with event: NSEvent) { onHover?(nil) }
    override func scrollWheel(with event: NSEvent) {
        wheelRemainder += event.hasPreciseScrollingDeltas ? -event.scrollingDeltaY / 6 : -event.scrollingDeltaY
        let rows = Int(wheelRemainder); wheelRemainder -= CGFloat(rows)
        if rows != 0 { onScroll?(rows) }
    }
    override func keyDown(with event: NSEvent) {
        if [123, 124, 125, 126, 116, 121].contains(event.keyCode) { onKey?(event.keyCode, event.modifierFlags.contains(.shift)) } else { super.keyDown(with: event) }
    }
}

/// Debug > Find Graphics: memory shown as CPC graphics (mode 0/1/2, linear or screen layout), as one bitmap or a grid of frames.
final class GraphicsController: NSObject {
    let model: DebugModel
    let panel = makePanel("Find Graphics", width: 760, height: 620)
    private let view = GraphicsView()
    private var address = 0xC000, mode = 1, zoom = 4
    private let addrField = NSTextField(string: "C000"), widthField = NSTextField(string: "16"), heightField = NSTextField(string: "64")
    private let framesField = NSTextField(string: "1"), colsField = NSTextField(string: "4"), strideField = NSTextField(string: "")
    private let modePopup = NSPopUpButton(frame: .zero, pullsDown: false), zoomPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let encPopup = NSPopUpButton(frame: .zero, pullsDown: false), palPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let liveCheck = NSButton(checkboxWithTitle: "Live", target: nil, action: nil)
    private let info = makeLabel("")
    private var timer: Timer?

    // Layout of the last render, for the pointer read-out.
    private struct Grid { var w = 0, height = 0, gap = 2, cols = 1, frames = 1, widthBytes = 1, stride = 0, ppb = 4, interleaved = false, base = 0 }
    private var grid = Grid()
    private var pens: [UInt8] = []                  // pen of every pixel of the grid image (255 in the gaps)
    private var pixels: [UInt32] = []

    init(model: DebugModel) {
        self.model = model
        super.init()
        modePopup.addItems(withTitles: ["Mode 0", "Mode 1", "Mode 2"]); modePopup.selectItem(at: 1)
        zoomPopup.addItems(withTitles: ["1x", "2x", "4x", "8x"]); zoomPopup.selectItem(at: 2)
        encPopup.addItems(withTitles: ["Linear", "CPC screen (8 lines)"])
        palPopup.addItems(withTitles: ["Current palette", "Greyscale"])
        for c in [addrField, widthField, heightField, framesField, colsField, strideField] { c.target = self; c.action = #selector(changed(_:)); c.widthAnchor.constraint(equalToConstant: c === addrField ? 90 : 56).isActive = true }
        addrField.placeholderString = "C000, #C000 or symbol"; strideField.placeholderString = "auto"
        for p in [modePopup, zoomPopup, encPopup, palPopup] { p.target = self; p.action = #selector(changed(_:)) }
        liveCheck.target = self; liveCheck.action = #selector(liveChanged(_:))
        view.translatesAutoresizingMaskIntoConstraints = false
        view.onScroll = { [weak self] rows in self?.move(by: rows * (self?.grid.widthBytes ?? 1)) }
        view.onKey = { [weak self] code, shift in
            guard let self = self else { return }
            let row = self.grid.widthBytes, page = row * self.grid.height * (shift ? max(1, self.grid.frames) : 1)
            switch code { case 126: self.move(by: -row); case 125: self.move(by: row); case 123: self.move(by: -1); case 124: self.move(by: 1)
                          case 116: self.move(by: -page); default: self.move(by: page) }
        }
        view.onHover = { [weak self] p in self?.hover(p) }
        let row1 = NSStackView(views: [makeLabel("Address:"), addrField, makeLabel("Mode:"), modePopup, makeLabel("Width (bytes):"), widthField, makeLabel("Height:"), heightField])
        let row2 = NSStackView(views: [makeLabel("Frames:"), framesField, makeLabel("Columns:"), colsField, makeLabel("Stride (bytes):"), strideField, makeLabel("Encoding:"), encPopup])
        let row3 = NSStackView(views: [makeLabel("Zoom:"), zoomPopup, makeLabel("Colours:"), palPopup, liveCheck, makeButton("Copy", self, #selector(copyImage(_:))),
                                       makeButton("Export PNG…", self, #selector(exportPNG(_:))), makeButton("Mark as data", self, #selector(markData(_:)))])
        for r in [row1, row2, row3] { r.spacing = 6 }
        info.font = .monospacedSystemFont(ofSize: 11, weight: .regular); info.lineBreakMode = .byTruncatingTail
        let all = NSStackView(views: [row1, row2, row3, view, info]); all.orientation = .vertical; all.alignment = .leading; all.spacing = 8
        view.widthAnchor.constraint(equalTo: all.widthAnchor).isActive = true
        view.heightAnchor.constraint(greaterThanOrEqualToConstant: 240).isActive = true
        fill(panel, with: all)
        panel.styleMask.insert(.resizable); panel.contentMinSize = NSSize(width: 640, height: 480)
        timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in if self?.liveCheck.state == .on, self?.panel.isVisible == true { self?.reload() } }
    }
    func show() { reload(); panel.makeKeyAndOrderFront(nil); panel.makeFirstResponder(view) }

    /// Plain hex ("C000", "#C000", "&C000") or any debugger expression: a symbol, a register such as hl, symbol+4…
    private func parseAddress(_ text: String) -> Int? {
        let t = text.trimmingCharacters(in: .whitespaces)
        if t.isEmpty { return nil }
        if t.allSatisfy({ $0.isHexDigit }), let v = Int(t, radix: 16) { return v & 0xFFFF }
        if let v = model.eval(t).0 { return Int(v) & 0xFFFF }
        return nil
    }
    private func number(_ f: NSTextField, _ lo: Int, _ hi: Int, _ fallback: Int) -> Int { max(lo, min(hi, Int(f.stringValue.trimmingCharacters(in: .whitespaces)) ?? fallback)) }

    @objc func changed(_ s: Any?) {
        if let a = parseAddress(addrField.stringValue) { address = a }
        mode = modePopup.indexOfSelectedItem; zoom = [1, 2, 4, 8][zoomPopup.indexOfSelectedItem]
        reload()
    }
    @objc func liveChanged(_ s: Any?) { reload() }
    private func move(by n: Int) {
        address = (address + n) & 0xFFFF; addrField.stringValue = String(format: "%04X", address); reload()
    }

    private func palette() -> [UInt32] {
        if palPopup.indexOfSelectedItem == 1 {
            let levels = [15, 3, 1][mode]
            return (0..<16).map { let g = UInt32(min(255, $0 * 255 / levels)); return 0xFF000000 | g << 16 | g << 8 | g }
        }
        model.emu.lock.lock(); defer { model.emu.lock.unlock() }
        let c = model.cpc.pointee
        return (0..<16).map { c.cfg.plus ? c.asic.pal_rgb_get($0) : c.hw_rgb_get(Int(c.ga_palette_get($0))) }
    }

    private func pen(_ b: Int, _ p: Int) -> Int {
        switch mode {
        case 0: let sh = p == 0 ? [7, 3, 5, 1] : [6, 2, 4, 0]; return sh.enumerated().reduce(0) { $0 | ((b >> $1.element) & 1) << $1.offset }
        case 1: return ((b >> (7 - p)) & 1) | (((b >> (3 - p)) & 1) << 1)
        default: return (b >> (7 - p)) & 1
        }
    }

    func reload() {
        if let a = parseAddress(addrField.stringValue) { address = a }     // re-evaluated each time, so `hl` or a symbol follows the machine
        let pal = palette()
        var g = Grid()
        g.widthBytes = number(widthField, 1, 128, 16); g.height = number(heightField, 1, 512, 64)
        g.frames = number(framesField, 1, 256, 1); g.cols = min(g.frames, number(colsField, 1, 64, 4))
        g.ppb = [2, 4, 8][mode]; g.w = g.widthBytes * g.ppb; g.interleaved = encPopup.indexOfSelectedItem == 1; g.base = address
        g.stride = Int(strideField.stringValue.trimmingCharacters(in: .whitespaces)) ?? 0
        if g.stride <= 0 { g.stride = g.widthBytes * g.height }
        let rows = (g.frames + g.cols - 1) / g.cols
        let gw = g.cols * g.w + (g.cols - 1) * g.gap, gh = rows * g.height + (rows - 1) * g.gap
        guard gw * gh <= 16_000_000 else { info.stringValue = "Too large: reduce frames, width or height"; return }
        var px = [UInt32](repeating: 0xFF303030, count: gw * gh), pn = [UInt8](repeating: 255, count: gw * gh)
        for f in 0..<g.frames {
            let ox = (f % g.cols) * (g.w + g.gap), oy = (f / g.cols) * (g.height + g.gap), fb = address + f * g.stride
            let linear = g.interleaved ? [] : model.readBlock(fb, g.widthBytes * g.height)
            for y in 0..<g.height {
                let bytes = g.interleaved ? model.readBlock(fb + (y & 7) * 0x800 + (y >> 3) * 80, g.widthBytes) : Array(linear[(y * g.widthBytes)..<((y + 1) * g.widthBytes)])
                for (bx, b) in bytes.enumerated() { for p in 0..<g.ppb {
                    let k = pen(Int(b), p), i = (oy + y) * gw + ox + bx * g.ppb + p
                    px[i] = pal[k]; pn[i] = UInt8(k)
                } }
            }
        }
        grid = g; pixels = px; pens = pn
        view.zoom = zoom; view.image = Self.makeImage(px, gw, gh)
        info.stringValue = String(format: "Address: %04X   %d x %d pixels per frame, %d frame%@", address, g.w, g.height, g.frames, g.frames == 1 ? "" : "s")
    }

    private static func makeImage(_ px: [UInt32], _ w: Int, _ h: Int) -> CGImage? {
        let data = Data(bytes: px, count: px.count * 4)
        guard let prov = CGDataProvider(data: data as CFData) else { return nil }
        return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue), provider: prov, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }

    /// Address, byte and pen of the pixel under the pointer.
    private func hover(_ p: NSPoint?) {
        let g = grid
        guard let p = p, p.x >= 0, p.y >= 0 else { return reloadInfo() }
        let ix = Int(p.x), iy = Int(p.y), fx = ix / (g.w + g.gap), fy = iy / (g.height + g.gap), f = fy * g.cols + fx
        let x = ix % (g.w + g.gap), y = iy % (g.height + g.gap)
        guard fx < g.cols, f < g.frames, x < g.w, y < g.height else { return reloadInfo() }
        let off = g.interleaved ? (y & 7) * 0x800 + (y >> 3) * 80 + x / g.ppb : y * g.widthBytes + x / g.ppb
        let addr = (g.base + f * g.stride + off) & 0xFFFF
        let byte = model.read(addr), pn = pens[iy * (g.cols * g.w + (g.cols - 1) * g.gap) + ix]
        info.stringValue = String(format: "Frame %d  x=%d y=%d   %04X = %02X   pen %d", f, x, y, addr, byte, pn) + (model.symbol(for: addr).map { "   " + $0 } ?? "")
    }
    private func reloadInfo() {
        info.stringValue = String(format: "Address: %04X   %d x %d pixels per frame, %d frame%@", address, grid.w, grid.height, grid.frames, grid.frames == 1 ? "" : "s")
    }

    @objc func copyImage(_ s: Any?) {
        guard let cg = view.image else { return }
        NSPasteboard.general.clearContents(); NSPasteboard.general.writeObjects([NSImage(cgImage: cg, size: NSSize(width: cg.width * zoom, height: cg.height * zoom))])
    }
    /// PNG at the current zoom (nearest-neighbour), with the palette in use.
    @objc func exportPNG(_ s: Any?) {
        guard let cg = view.image else { return }
        let sp = NSSavePanel(); sp.allowedContentTypes = [.png]; sp.nameFieldStringValue = String(format: "graphics_%04X.png", address)
        guard sp.runModal() == .OK, let url = sp.url else { return }
        let w = cg.width * zoom, h = cg.height * zoom
        var big = [UInt32](repeating: 0, count: w * h)
        let gw = cg.width
        for y in 0..<h { let sy = y / zoom; for x in 0..<w { big[y * w + x] = pixels[sy * gw + x / zoom] } }
        guard let img = Self.makeImage(big, w, h), let data = NSBitmapImageRep(cgImage: img).representation(using: .png, properties: [:]) else { return }
        do { try data.write(to: url) } catch { NSAlert(error: error).runModal() }
    }
    @objc func markData(_ s: Any?) {
        model.emu.lock.lock()
        dbg_area_add(model.cpc, UInt16(address & 0xFFFF), UInt16((address + (grid.stride * (grid.frames - 1) + grid.widthBytes * grid.height) - 1) & 0xFFFF), false)
        model.emu.lock.unlock()
    }
}
