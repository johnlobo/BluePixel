import AppKit
import CPCCore

/// The control strip shown under the display while a session recording plays: play/pause, frame and second
/// steps, reverse play, speed, and a time line that can be dragged to any moment.
final class TransportBar: NSView {
    static let height: CGFloat = 36
    let start = TransportBar.button("backward.end.fill", "Go to the start (Home)")
    let back10 = TransportBar.button("gobackward.10", "Back 10 seconds (⌥←)")
    let back1s = TransportBar.button("gobackward", "Back 1 second (⇧←)")
    let backFrame = TransportBar.button("backward.frame.fill", "Back 1 frame (←)")
    let reverse = TransportBar.button("arrowtriangle.backward.fill", "Play backwards (R)")
    let play = TransportBar.button("play.fill", "Play / Pause (Space)")
    let fwdFrame = TransportBar.button("forward.frame.fill", "Forward 1 frame (→)")
    let fwd1s = TransportBar.button("goforward", "Forward 1 second (⇧→)")
    let fwd10 = TransportBar.button("goforward.10", "Forward 10 seconds (⌥→)")
    let end = TransportBar.button("forward.end.fill", "Go to the end (End)")
    let control = TransportBar.button("hand.raised.fill", "Take control: leave the recording and carry on from here")
    let speed = NSPopUpButton(frame: .zero, pullsDown: false)
    let slider = NSSlider(value: 0, minValue: 0, maxValue: 1, target: nil, action: nil)
    let time = NSTextField(labelWithString: "0:00.00 / 0:00.00")
    let indexLabel = NSTextField(labelWithString: "")
    private let indexLine = NSView()
    static let speeds: [Double] = [0.1, 0.25, 0.5, 1, 2, 5, 10]

    static func button(_ symbol: String, _ tip: String) -> NSButton {
        let b = NSButton(image: NSImage(systemSymbolName: symbol, accessibilityDescription: tip) ?? NSImage(), target: nil, action: nil)
        b.isBordered = false; b.contentTintColor = .white; b.toolTip = tip; b.imageScaling = .scaleProportionallyDown
        return b
    }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = NSColor(white: 0.10, alpha: 1).cgColor
        for b in [start, back10, back1s, backFrame, reverse, play, fwdFrame, fwd1s, fwd10, end, control] { addSubview(b) }
        speed.addItems(withTitles: TransportBar.speeds.map { $0 < 1 ? String(format: "%.2g×", $0) : String(format: "%g×", $0) })
        speed.selectItem(at: 3); speed.controlSize = .small; speed.font = .systemFont(ofSize: 11); speed.toolTip = "Playback speed"
        addSubview(speed)
        slider.isContinuous = true; slider.controlSize = .small; addSubview(slider)
        indexLine.wantsLayer = true; indexLine.layer?.backgroundColor = NSColor.systemGreen.withAlphaComponent(0.7).cgColor; addSubview(indexLine)
        for t in [time, indexLabel] { t.font = .monospacedSystemFont(ofSize: 11, weight: .regular); t.textColor = .white; addSubview(t) }
        indexLabel.textColor = .systemGreen; indexLabel.font = .systemFont(ofSize: 10)
    }
    required init?(coder: NSCoder) { fatalError() }

    /// Fraction (0...1) of the time line that is indexed (seeking there is instant).
    var indexedFraction: Double = 0 { didSet { needsLayout = true } }

    override func layout() {
        super.layout()
        let h = bounds.height, bw: CGFloat = 26
        var x: CGFloat = 8
        for b in [start, back10, back1s, backFrame, reverse, play, fwdFrame, fwd1s, fwd10, end] { b.frame = NSRect(x: x, y: (h - 22) / 2, width: bw, height: 22); x += bw + 2 }
        x += 6
        speed.frame = NSRect(x: x, y: (h - 20) / 2, width: 66, height: 20); x += 72
        control.frame = NSRect(x: bounds.width - 8 - bw, y: (h - 22) / 2, width: bw, height: 22)
        let timeW: CGFloat = 138
        time.frame = NSRect(x: control.frame.minX - 8 - timeW, y: (h - 16) / 2, width: timeW, height: 16); time.alignment = .right
        let sw = max(60, time.frame.minX - 8 - x)
        slider.frame = NSRect(x: x, y: (h - 20) / 2 + 3, width: sw, height: 20)
        // thin line under the track: how much of the recording is indexed
        let inset: CGFloat = 7
        indexLine.frame = NSRect(x: slider.frame.minX + inset, y: slider.frame.minY + 1, width: max(0, (sw - 2 * inset) * CGFloat(min(1, indexedFraction))), height: 2)
        indexLabel.frame = NSRect(x: x, y: 1, width: 120, height: 11)
        indexLabel.isHidden = indexedFraction >= 1
        indexLine.isHidden = indexedFraction >= 1
    }
}

/// Drives the transport bar: forward play and pause use the emulator thread, reverse play shows pictures captured
/// from the keyframes (the machine itself only ever runs forwards), seeks restore exact states.
final class TransportController: NSObject {
    private let emu: Emulator
    private unowned let main: MainWindowController
    private let bar = TransportBar(frame: .zero)
    private var uiTimer: Timer?
    private var reverseTimer: Timer?
    private var reversing = false
    private var shown: UInt64 = 0                    // frame on screen while reversing
    private var cache: (from: UInt64, count: Int, pixels: [UInt32])?
    private var dragging = false, resumeAfterDrag = false
    private var lastSeek = Date.distantPast
    private var savedSpeed = 1.0
    var onTakeControl: (() -> Void)?

    init(emu: Emulator, main: MainWindowController) {
        self.emu = emu; self.main = main
        super.init()
        for (b, a) in [(bar.start, #selector(goStart)), (bar.back10, #selector(back10)), (bar.back1s, #selector(back1s)), (bar.backFrame, #selector(backFrame)),
                       (bar.reverse, #selector(reverse)), (bar.play, #selector(playPause)), (bar.fwdFrame, #selector(fwdFrame)), (bar.fwd1s, #selector(fwd1s)),
                       (bar.fwd10, #selector(fwd10)), (bar.end, #selector(goEnd)), (bar.control, #selector(takeControl))] { b.target = self; b.action = a }
        bar.speed.target = self; bar.speed.action = #selector(speedChanged(_:))
        bar.slider.target = self; bar.slider.action = #selector(sliderMoved(_:))
        emu.onReplayChanged = { [weak self] active in active ? self?.attach() : self?.detach() }
        emu.onReplayEnd = { [weak self] in self?.refresh() }
        emu.beforeResume = { [weak self] in self?.stopReverse() }
    }

    // MARK: show / hide
    private func attach() {
        savedSpeed = emu.speed
        bar.speed.selectItem(at: 3); emu.speed = 1
        main.setTransport(bar)
        refresh()
        uiTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in self?.refresh() }
    }
    private func detach() {
        stopReverse(seek: false)
        uiTimer?.invalidate(); uiTimer = nil
        emu.speed = savedSpeed
        main.setTransport(nil)
    }

    // MARK: state
    private var length: UInt64 { emu.playbackLength }
    private var position: UInt64 { reversing ? shown : emu.playbackFrame }

    private func timeString(_ f: UInt64) -> String {
        let s = Double(f) / Emulator.framesPerSecond
        return String(format: "%d:%05.2f", Int(s) / 60, s - Double(Int(s) / 60 * 60))
    }

    func refresh() {
        guard emu.replayActive else { return }
        let len = length, pos = position
        if !dragging { bar.slider.maxValue = Double(max(len, 1)); bar.slider.doubleValue = Double(pos) }
        bar.time.stringValue = timeString(pos) + " / " + timeString(len)
        let running = !emu.paused && !reversing
        bar.play.image = NSImage(systemSymbolName: running ? "pause.fill" : "play.fill", accessibilityDescription: nil)
        bar.reverse.contentTintColor = reversing ? .systemYellow : .white
        let idx = emu.indexedFrames
        bar.indexedFraction = len == 0 ? 1 : Double(idx) / Double(len)
        bar.indexLabel.stringValue = idx >= len ? "" : String(format: "indexing %d%%", Int(Double(idx) / Double(max(len, 1)) * 100))
    }

    // MARK: actions
    private func goTo(_ frame: UInt64) {
        stopReverse(seek: false)
        emu.debugPause()
        _ = emu.seek(toFrame: min(frame, length))
        refresh()
    }
    private func step(_ delta: Int) {
        let pos = Int64(position)
        goTo(UInt64(max(0, min(Int64(length), pos + Int64(delta)))))
    }
    @objc func goStart() { goTo(0) }
    @objc func goEnd() { goTo(length) }
    @objc func back10() { step(-10 * 50) }
    @objc func back1s() { step(-50) }
    @objc func backFrame() { step(-1) }
    @objc func fwdFrame() { step(1) }
    @objc func fwd1s() { step(50) }
    @objc func fwd10() { step(10 * 50) }

    @objc func playPause() {
        if reversing { stopReverse(); return }
        if emu.paused {
            if emu.playbackFrame >= length { _ = emu.seek(toFrame: 0) }      // at the end: start over
            emu.debugResume()
        } else { emu.debugPause() }
        refresh()
    }

    @objc func takeControl() { stopReverse(); onTakeControl?() }

    @objc func speedChanged(_ s: NSPopUpButton) {
        let v = TransportBar.speeds[max(0, min(TransportBar.speeds.count - 1, s.indexOfSelectedItem))]
        emu.speed = v
        if reversing { startReverseTimer() }
    }

    @objc func sliderMoved(_ s: NSSlider) {
        let type = NSApp.currentEvent?.type
        if type == .leftMouseDown || (!dragging && type == .leftMouseDragged) {
            dragging = true; resumeAfterDrag = !emu.paused && !reversing
            stopReverse(seek: false); emu.debugPause()
        }
        // seek while dragging, a few times a second at most; the last one on release is always exact
        let released = type == .leftMouseUp
        if released || Date().timeIntervalSince(lastSeek) > 0.04 {
            lastSeek = Date()
            _ = emu.seek(toFrame: UInt64(max(0, s.doubleValue)))
            bar.time.stringValue = timeString(emu.playbackFrame) + " / " + timeString(length)
        }
        if released || type == nil {
            dragging = false
            if resumeAfterDrag { resumeAfterDrag = false; emu.debugResume() }
            refresh()
        }
    }

    // MARK: reverse play
    @objc func reverse() {
        if reversing { stopReverse(); return }
        emu.debugPause()
        reversing = true; shown = emu.playbackFrame; emu.displayedFrame = shown; cache = nil
        startReverseTimer()
        refresh()
    }

    private func startReverseTimer() {
        reverseTimer?.invalidate()
        let v = TransportBar.speeds[max(0, min(TransportBar.speeds.count - 1, bar.speed.indexOfSelectedItem))]
        let frames = max(1, Int(v.rounded()))                              // speeds above 1x skip frames
        let interval = 0.02 / (v < 1 ? v : 1)
        reverseTimer = Timer.scheduledTimer(withTimeInterval: interval, repeats: true) { [weak self] _ in self?.reverseTick(step: frames) }
    }

    private func reverseTick(step: Int) {
        guard reversing else { return }
        if shown <= UInt64(step) { shown = 0; stopReverse(); return }
        let target = shown - UInt64(step)
        if let c = pictureCache(for: target) {
            let offset = Int(target - 1 - c.from) * Int(CPC_SCREEN_W) * Int(CPC_SCREEN_H)
            c.pixels.withUnsafeBufferPointer { emu.publish(picture: $0.baseAddress! + offset) }
            shown = target; emu.displayedFrame = target
            let len = length
            if !dragging { bar.slider.doubleValue = Double(shown) }
            bar.time.stringValue = timeString(shown) + " / " + timeString(len)
        } else { shown = 0; stopReverse() }
    }

    /// Pictures for the 50-frame interval holding `frame`, captured from its keyframe when not cached.
    private func pictureCache(for frame: UInt64) -> (from: UInt64, count: Int, pixels: [UInt32])? {
        if let c = cache, frame > c.from, frame <= c.from + UInt64(c.count) { return c }
        let from = (frame - 1) / UInt64(CPC_CHECK_FRAMES) * UInt64(CPC_CHECK_FRAMES)
        let (px, n) = emu.capturePictures(from: from, count: Int(CPC_CHECK_FRAMES))
        guard n > 0, frame <= from + UInt64(n) else { return nil }
        cache = (from, n, px)
        return cache
    }

    /// Stops reverse play. The machine was only borrowed to render pictures, so it is put back on the exact frame.
    func stopReverse(seek: Bool = true) {
        guard reversing else { return }
        reverseTimer?.invalidate(); reverseTimer = nil
        reversing = false; cache = nil; emu.displayedFrame = nil
        if seek { _ = emu.seek(toFrame: shown) }
        refresh()
    }

    // MARK: keyboard (the CPC keys are ignored during a replay, so the keyboard drives the transport)
    func handleKey(_ event: NSEvent) -> Bool {
        guard emu.replayActive else { return false }
        let shift = event.modifierFlags.contains(.shift), option = event.modifierFlags.contains(.option)
        switch event.keyCode {
        case 49: playPause()                                                  // space
        case 123: step(option ? -500 : (shift ? -50 : -1))                   // left
        case 124: step(option ? 500 : (shift ? 50 : 1))                      // right
        case 115: goStart()                                                   // home
        case 119: goEnd()                                                     // end
        case 15: reverse()                                                    // r
        case 33, 30:                                                          // [ ]  slower / faster
            let i = bar.speed.indexOfSelectedItem + (event.keyCode == 30 ? 1 : -1)
            if i >= 0 && i < TransportBar.speeds.count { bar.speed.selectItem(at: i); speedChanged(bar.speed) }
        default: return false
        }
        return true
    }
}
