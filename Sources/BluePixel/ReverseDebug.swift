import AppKit
import CPCCore

/// Reverse debugging during a replay: step back, run back to a breakpoint or an address, find the last write or
/// read of a byte. The machine is re-simulated from the keyframes (CPCCore/replay.c); a search over a long stretch
/// runs on a background thread, one keyframe interval at a time, and can be cancelled.
extension Emulator {
    struct ReverseResult {
        var found = false, cancelled = false, unavailable = false
        var hit = CPCReverseHit()
        var frame: UInt64 = 0                       // position of the machine when it ends
    }

    /// One instruction back. False at the start of the recording.
    func reverseStep() -> Bool {
        lock.lock(); let ok = cpc_player_step_back(cpc); lock.unlock()
        publish()
        return ok
    }

    func startReverseSearch(mode: CPCReverseMode, lo: UInt16, hi: UInt16, progress: @escaping (Double, UInt64) -> Void, done: @escaping (ReverseResult) -> Void) {
        cancelReverseSearch()
        debugPause()
        searchCancel = false
        let t = Thread { [unowned self] in
            let me = Thread.current
            var result = ReverseResult()
            self.lock.lock()
            let scan = cpc_reverse_begin(self.cpc, mode, lo, hi)
            self.lock.unlock()
            if let scan = scan {
                var finished = false
                while !finished && !self.searchCancel {
                    self.lock.lock()
                    finished = cpc_reverse_slice(self.cpc, scan)
                    let p = cpc_reverse_progress(scan), at = cpc_reverse_cycles(scan)
                    self.lock.unlock()
                    DispatchQueue.main.async { progress(p, at) }
                }
                self.lock.lock()
                result.cancelled = self.searchCancel
                result.found = cpc_reverse_end(self.cpc, scan, !self.searchCancel, &result.hit)
                result.frame = cpc_player_frame(self.cpc)
                self.lock.unlock()
                self.publish()
            } else { result.unavailable = true }
            DispatchQueue.main.async { if self.searchThread === me { self.searchThread = nil }; done(result) }   // not a newer search's
            self.searchDone.signal()
        }
        t.name = "BluePixel reverse search"; t.qualityOfService = .userInitiated
        searchThread = t; t.start()
    }

    /// Cancels a search and waits for it; the machine goes back to where the search started.
    func cancelReverseSearch() {
        guard searchThread != nil else { return }
        searchCancel = true
        searchDone.wait()                      // the thread signals exactly once when it has put the machine back
        searchThread = nil
    }
}

/// Small window with a progress bar and Cancel, shown only when a search takes longer than a moment.
final class ReverseSearchPanel: NSObject {
    private let panel = makePanel("Searching backwards…", width: 340, height: 110)
    private let bar = NSProgressIndicator()
    private let label = makeLabel("")
    private var showTimer: Timer?
    var onCancel: (() -> Void)?

    override init() {
        super.init()
        bar.isIndeterminate = false; bar.minValue = 0; bar.maxValue = 1; bar.style = .bar
        let cancel = makeButton("Cancel", self, #selector(cancelPressed(_:)))
        let all = stack([label, bar, cancel]); all.alignment = .width
        fill(panel, with: all, inset: 14)
    }
    func begin() {
        bar.doubleValue = 0; label.stringValue = "Looking through the recording…"
        showTimer = Timer.scheduledTimer(withTimeInterval: 0.35, repeats: false) { [weak self] _ in
            guard let self = self else { return }
            self.showTimer = nil
            self.panel.center(); self.panel.orderFront(nil)
        }
    }
    func update(_ p: Double, at cycles: UInt64) {
        bar.doubleValue = p
        label.stringValue = "Now at " + Emulator.clock(UInt64(Double(cycles) / 1e6 * Emulator.framesPerSecond)) + " — \(Int(p * 100))% of the way back"
    }
    func end() { showTimer?.invalidate(); showTimer = nil; panel.orderOut(nil) }
    @objc private func cancelPressed(_ s: Any?) { onCancel?() }
}

extension DebuggerController {
    private func describe(_ pc: UInt16) -> String {
        let sym = model.nearestSymbol(Int(pc)).map { " " + $0 } ?? ""
        return String(format: "#%04X", pc) + sym
    }

    /// Reports a finished reverse operation and refreshes the whole window.
    private func reverseDone(_ text: String) {
        show()
        flash(text)
        (NSApp.delegate as? AppDelegate)?.transport?.refresh()
    }

    @objc func stepBack(_ s: Any?) {
        guard emu.replayActive, !emu.isSearching else { NSSound.beep(); return }
        emu.debugPause()
        if emu.reverseStep() { reverseDone("Stepped back one instruction.") }
        else { NSSound.beep(); reverseDone("This is the start of the recording.") }
    }

    /// Back to before the previous statement at this call level: the CALL when the last step was its RET, the first
    /// iteration of an LDIR, otherwise one instruction.
    @objc func stepBackOver(_ s: Any?) {
        search(CPC_REV_STEPOVER, 0, 0, found: { [unowned self] h, f in
            "Stepped back over to " + self.describe(h.pc) + " — " + Emulator.clock(f)
        }, missing: "This is the start of the recording.")
    }

    private func search(_ mode: CPCReverseMode, _ lo: UInt16, _ hi: UInt16, found: @escaping (CPCReverseHit, UInt64) -> String, missing: String) {
        guard emu.replayActive, !emu.isSearching else { NSSound.beep(); return }
        let panel = ReverseSearchPanel()
        panel.onCancel = { [weak self] in self?.emu.searchCancel = true }
        panel.begin()
        emu.startReverseSearch(mode: mode, lo: lo, hi: hi, progress: { panel.update($0, at: $1) }, done: { [weak self] r in
            panel.end()
            guard let self = self else { return }
            if r.unavailable { NSSound.beep(); self.reverseDone("Reverse search is not available."); return }
            if r.cancelled { self.reverseDone("Search cancelled; back where it started."); return }
            if r.found { self.reverseDone(found(r.hit, r.frame)) } else { NSSound.beep(); self.reverseDone(missing) }
        })
    }

    @objc func reverseContinue(_ s: Any?) {
        search(CPC_REV_BREAKPOINTS, 0, 0, found: { [unowned self] h, f in
            let what = h.kind == 1 ? "Breakpoint at " + self.describe(h.pc)
                : String(format: "%@ #%04X by ", h.kind == 3 ? "Write to" : h.kind == 2 ? "Read of" : "I/O at", h.addr) + self.describe(h.pc)
            return what + " — " + Emulator.clock(f)
        }, missing: "No earlier breakpoint hit in this recording (staying here).")
    }

    @objc func reverseToCursor(_ s: Any?) {
        let a = disasm.selected >= 0 ? disasm.selected : model.pc
        search(CPC_REV_CODE, UInt16(a & 0xFFFF), 0, found: { [unowned self] h, f in
            "Last time " + self.describe(h.pc) + " was reached — " + Emulator.clock(f)
        }, missing: String(format: "#%04X was not reached earlier in this recording (staying here).", a))
    }

    /// Last write (or read) of the selected bytes in the memory pane.
    func reverseMemory(write: Bool) {
        let r = memory.selection
        let lo = UInt16(r.lowerBound & 0xFFFF), hi = UInt16(r.upperBound & 0xFFFF)
        let range = lo == hi ? String(format: "#%04X", lo) : String(format: "#%04X-#%04X", lo, hi)
        search(write ? CPC_REV_WRITE : CPC_REV_READ, lo, hi, found: { [unowned self] h, f in
            var s = String(format: "Last %@ of %@", write ? "write" : "read", range)
            s += lo == hi ? " by " : String(format: " at #%04X by ", h.addr)
            s += self.describe(h.pc)
            if write { s += String(format: " — value #%02X (was #%02X)", h.value, h.previous) }
            return s + " — " + Emulator.clock(f)
        }, missing: "Nothing \(write ? "wrote" : "read") \(range) earlier in this recording (staying here).")
    }
    @objc func memoryWhoWrote(_ s: Any?) { reverseMemory(write: true) }
    @objc func memoryWhoRead(_ s: Any?) { reverseMemory(write: false) }
}
