import AppKit
import CPCCore
import UniformTypeIdentifiers

// MARK: - helpers
func makeLabel(_ s: String, bold: Bool = false) -> NSTextField {
    let t = NSTextField(labelWithString: s)
    if bold { t.font = .boldSystemFont(ofSize: NSFont.systemFontSize) }
    return t
}
func makeCheck(_ title: String) -> NSButton { NSButton(checkboxWithTitle: title, target: nil, action: nil) }
func makeButton(_ title: String, _ target: AnyObject?, _ action: Selector) -> NSButton {
    let b = NSButton(title: title, target: target, action: action); b.bezelStyle = .rounded; return b
}
func makePanel(_ title: String, width: CGFloat, height: CGFloat) -> NSPanel {
    let p = NSPanel(contentRect: NSRect(x: 0, y: 0, width: width, height: height), styleMask: [.titled, .closable, .utilityWindow], backing: .buffered, defer: false)
    p.title = title; p.isReleasedWhenClosed = false; p.hidesOnDeactivate = false; p.center()
    return p
}
func stack(_ views: [NSView], axis: NSUserInterfaceLayoutOrientation = .vertical, spacing: CGFloat = 8) -> NSStackView {
    let s = NSStackView(views: views); s.orientation = axis; s.spacing = spacing; s.alignment = axis == .vertical ? .leading : .centerY
    s.translatesAutoresizingMaskIntoConstraints = false; return s
}
func fill(_ panel: NSPanel, with view: NSView, inset: CGFloat = 16) {
    let c = panel.contentView!
    view.translatesAutoresizingMaskIntoConstraints = false
    c.addSubview(view)
    NSLayoutConstraint.activate([
        view.leadingAnchor.constraint(equalTo: c.leadingAnchor, constant: inset), view.trailingAnchor.constraint(equalTo: c.trailingAnchor, constant: -inset),
        view.topAnchor.constraint(equalTo: c.topAnchor, constant: inset), view.bottomAnchor.constraint(equalTo: c.bottomAnchor, constant: -inset),
    ])
}

// MARK: - Drive setup
final class DriveSetupController: NSObject {
    let emu: Emulator
    let panel = makePanel("Drive Setup", width: 560, height: 330)
    private var pathLabels: [NSTextField] = [], single: [NSButton] = [], temp: [NSButton] = [], prompt: [NSButton] = []
    var onChange: (() -> Void)?

    init(emu: Emulator) {
        self.emu = emu
        super.init()
        var boxes: [NSView] = []
        for d in 0..<2 {
            let title = d == 0 ? "Floppy A:" : "Floppy B:"
            let path = NSTextField(labelWithString: ""); path.lineBreakMode = .byTruncatingMiddle
            path.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
            let open = makeButton("Open…", self, #selector(openDisc(_:))); open.tag = d
            let eject = makeButton("Eject", self, #selector(ejectDisc(_:))); eject.tag = d
            let edit = makeButton("Edit", self, #selector(editDisc(_:))); edit.tag = d
            let s = makeCheck("Allow Single-sided read"), t = makeCheck("Allow Temporary Writes"), p = makeCheck("Prompt to Save Changes")
            for c in [s, t, p] { c.tag = d; c.target = self; c.action = #selector(optionChanged(_:)) }
            s.identifier = NSUserInterfaceItemIdentifier("single"); t.identifier = NSUserInterfaceItemIdentifier("temp"); p.identifier = NSUserInterfaceItemIdentifier("prompt")
            pathLabels.append(path); single.append(s); temp.append(t); prompt.append(p)
            let row = stack([path, open, eject, edit], axis: .horizontal)
            path.setContentHuggingPriority(.defaultLow, for: .horizontal)
            let inner = stack([makeLabel(title, bold: true), row, s, t, p]); inner.alignment = .leading
            row.distribution = .fill
            boxes.append(inner)
        }
        let swap = makeButton("Swap", self, #selector(swapDiscs(_:)))
        let flip = makeButton("Flip", self, #selector(flipDisc(_:)))
        let ok = makeButton("OK", self, #selector(close(_:))); ok.keyEquivalent = "\r"
        let buttons = stack([swap, flip, NSView(), ok], axis: .horizontal)
        let all = stack(boxes + [buttons], spacing: 14); all.alignment = .width
        fill(panel, with: all)
        for b in boxes { b.widthAnchor.constraint(equalTo: all.widthAnchor).isActive = true }
        refresh()
    }

    func show() { refresh(); panel.makeKeyAndOrderFront(nil) }

    func refresh() {
        for d in 0..<2 {
            let p = emu.discPaths[d]
            pathLabels[d].stringValue = p.isEmpty ? "No disk selected for drive" : p
            single[d].state = emu.settings.bool("Drives", "Single Sided \(d)", false) ? .on : .off
            temp[d].state = emu.settings.bool("Drives", "Allow Write \(d)", false) ? .on : .off
            prompt[d].state = emu.settings.bool("Drives", "Prompt Write \(d)", false) ? .on : .off
        }
    }

    @objc func openDisc(_ s: NSButton) {
        let p = NSOpenPanel(); p.allowedContentTypes = ["dsk", "dsc", "hdr"].compactMap { UTType(filenameExtension: $0) }
        guard p.runModal() == .OK, let u = p.url else { return }
        if !emu.insertDisc(drive: s.tag, url: u) { NSAlert(error: NSError(domain: "BluePixel", code: 1, userInfo: [NSLocalizedDescriptionKey: "Cannot open disc image"])).runModal() }
        refresh(); onChange?()
    }
    @objc func ejectDisc(_ s: NSButton) { emu.removeDisc(drive: s.tag); refresh(); onChange?() }
    @objc func editDisc(_ s: NSButton) { NSApp.sendAction(Selector(s.tag == 0 ? "editDiscA:" : "editDiscB:"), to: nil, from: s) }
    @objc func optionChanged(_ s: NSButton) {
        let d = s.tag, on = s.state == .on
        switch s.identifier?.rawValue {
        case "single": emu.settings.set("Drives", "Single Sided \(d)", on)
        case "temp": emu.settings.set("Drives", "Allow Write \(d)", on)
        default: emu.settings.set("Drives", "Prompt Write \(d)", on)
        }
        emu.applyDriveOptions()
    }
    @objc func swapDiscs(_ s: Any?) { emu.swapDiscs(); refresh() }
    @objc func flipDisc(_ s: Any?) { NSApp.sendAction(Selector("flipDiscA:"), to: nil, from: s) }
    @objc func close(_ s: Any?) { panel.orderOut(nil) }
}

// MARK: - Tape control
final class TapeController: NSObject, NSTableViewDataSource, NSTableViewDelegate {
    let emu: Emulator
    let panel = makePanel("Tape Control", width: 460, height: 400)
    private let table = NSTableView()
    private let nameLabel = makeLabel("Tape: (none)")
    private let posLabel = makeLabel("00:00:00")
    private var timer: Timer?
    private var lastCount = -1

    init(emu: Emulator) {
        self.emu = emu
        super.init()
        func sym(_ name: String, _ tip: String, _ sel: Selector) -> NSButton {
            let b = NSButton(image: NSImage(systemSymbolName: name, accessibilityDescription: tip)!, target: self, action: sel)
            b.toolTip = tip; b.bezelStyle = .texturedRounded; return b
        }
        let controls = stack([sym("backward.end.fill", "Rewind", #selector(rewind(_:))), sym("play.fill", "Play", #selector(play(_:))),
                              sym("stop.fill", "Stop", #selector(stop(_:))), sym("eject.fill", "Eject", #selector(eject(_:))),
                              sym("record.circle", "Record", #selector(record(_:))), NSView(), makeButton("Open…", self, #selector(open(_:)))], axis: .horizontal)
        let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("block")); col.title = "Block"; col.width = 400
        table.addTableColumn(col); table.dataSource = self; table.delegate = self; table.headerView = nil
        table.target = self; table.doubleAction = #selector(seekSelected(_:))
        let scroll = NSScrollView(); scroll.documentView = table; scroll.hasVerticalScroller = true
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 220).isActive = true
        let all = stack([nameLabel, controls, scroll, posLabel]); all.alignment = .width
        fill(panel, with: all)
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in self?.tick() }
    }

    func show() { reload(); panel.makeKeyAndOrderFront(nil) }

    func reload() {
        let t = emu.tape
        nameLabel.stringValue = t == nil ? "Tape: (none)" : "Tape: " + String(cString: tape_name(t))
        lastCount = tape_block_count(t).asInt
        table.reloadData()
    }

    private func tick() {
        guard panel.isVisible else { return }
        let t = emu.tape
        if tape_block_count(t).asInt != lastCount { reload() }
        let secs = Int(tape_position_seconds(t))
        posLabel.stringValue = String(format: "%@%02d:%02d:%02d%@", emu.isRecordingTape ? "● REC  " : (tape_playing(t) ? "▶ " : ""), secs / 3600, (secs / 60) % 60, secs % 60, "")
        let cur = tape_block(t).asInt
        if cur < table.numberOfRows, !tape_is_audio(t) { table.selectRowIndexes(IndexSet(integer: cur), byExtendingSelection: false); table.scrollRowToVisible(cur) }
    }

    func numberOfRows(in tableView: NSTableView) -> Int { max(tape_block_count(emu.tape).asInt, tape_is_audio(emu.tape) ? 1 : 0) }
    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let tf = NSTextField(labelWithString: tape_is_audio(emu.tape) ? "Audio recording (\(Int(tape_length_seconds(emu.tape))) s)" : "\(row + 1)  " + String(cString: tape_block_info(emu.tape, Int32(row))))
        tf.font = .monospacedSystemFont(ofSize: 11, weight: .regular); return tf
    }

    @objc func seekSelected(_ s: Any?) { let r = table.clickedRow; if r >= 0 { emu.lock.lock(); tape_seek_block(emu.tape, Int32(r)); emu.lock.unlock() } }
    @objc func rewind(_ s: Any?) { emu.rewindTape() }
    @objc func play(_ s: Any?) { emu.playTape(true) }
    @objc func stop(_ s: Any?) {
        if emu.isRecordingTape { finishRecording() } else { emu.playTape(false) }
    }
    @objc func eject(_ s: Any?) { emu.removeTape(); reload() }
    @objc func open(_ s: Any?) {
        let p = NSOpenPanel(); p.allowedContentTypes = ["cdt", "tzx", "voc", "wav", "csw"].compactMap { UTType(filenameExtension: $0) }
        guard p.runModal() == .OK, let u = p.url else { return }
        if !emu.insertTape(url: u) { NSAlert(error: NSError(domain: "BluePixel", code: 2, userInfo: [NSLocalizedDescriptionKey: "Unsupported or damaged tape image"])).runModal() }
        reload()
    }
    @objc func record(_ s: Any?) { emu.startTapeRecording() }
    func finishRecording() {
        let p = NSSavePanel(); p.allowedContentTypes = [UTType.wav]; p.nameFieldStringValue = "recording.wav"
        if p.runModal() == .OK, let u = p.url { _ = emu.stopTapeRecording(to: u) } else { _ = emu.stopTapeRecording(to: URL(fileURLWithPath: "/dev/null")) }
    }
}
private extension Int32 { var asInt: Int { Int(self) } }

// MARK: - Snapshot format
enum SnapshotDialog {
    static func ask() -> (version: Int, memoryKB: Int, compressed: Bool)? {
        let a = NSAlert(); a.messageText = "Select Snapshot Format"
        let ver = NSPopUpButton(frame: .zero, pullsDown: false); ver.addItems(withTitles: ["Version 3", "Version 2", "Version 1"])
        let mem = NSPopUpButton(frame: .zero, pullsDown: false); mem.addItems(withTitles: ["First 64K", "First 128K", "All memory in use"]); mem.selectItem(at: 2)
        let comp = makeCheck("Compressed"); comp.state = .on
        let grid = NSGridView(views: [[makeLabel("Version"), ver], [makeLabel("Memory To Save"), mem], [NSView(), comp]])
        grid.frame = NSRect(x: 0, y: 0, width: 300, height: 90)
        a.accessoryView = grid; a.addButton(withTitle: "Save"); a.addButton(withTitle: "Cancel")
        guard a.runModal() == .alertFirstButtonReturn else { return nil }
        let v = 3 - ver.indexOfSelectedItem
        let m = [64, 128, 0][mem.indexOfSelectedItem]
        return (v, m, comp.state == .on)
    }
}
