import Foundation
import CPCCore
import Darwin

/// Owns the CPC core, its thread, ROM set-up and media.
final class Emulator {
    let cpc: UnsafeMutablePointer<CPC>
    let lock = NSRecursiveLock()
    let settings: Settings
    var audio: AudioOutput?
    private var thread: Thread?
    private(set) var running = false
    var paused = false { didSet { if !paused { resume.signal() } } }
    private let resume = DispatchSemaphore(value: 0)
    var speed: Double = 1.0
    var frameSkip = 0
    var onFrame: (() -> Void)?
    var onBreak: (() -> Void)?

    // Rendered output (768x540 or half), guarded by frameLock
    private let frameLock = NSLock()
    private(set) var rendered = [UInt32](repeating: 0, count: 768 * 540)
    private(set) var renderedSize = (w: 768, h: 540)
    var renderPAL = true, renderBoth = true, halfSize = false, brightness = 0
    /// What the display shows while paused in the debugger.
    enum ScreenView: Int { case normal, raster, videoMemory }
    var screenView = ScreenView.normal { didSet { if paused { publish() } } }
    private var viewBuffer = [UInt32](repeating: 0, count: Int(CPC_SCREEN_W * CPC_SCREEN_H))

    var romFolder: URL
    var discs: [UnsafeMutablePointer<CPCDisc>?] = [nil, nil, nil, nil]
    var discPaths: [String] = ["", "", "", ""]
    var lastCartridge = ""
    // session recording / replay (Session.swift)
    var recordStartFrame: UInt64 = 0
    var replayRecording: OpaquePointer?, replayPlayer: OpaquePointer?
    var replayEndNotified = false
    var onReplayEnd: (() -> Void)?
    var displayedFrame: UInt64?                       // frame on screen while reversing (the machine itself is parked)
    var replayActive = false                         // mirrors cpc.player != nil, readable without the lock
    var onReplayChanged: ((Bool) -> Void)?           // main thread: playback started / ended
    var beforeResume: (() -> Void)?                  // main thread: the machine is about to run again
    var indexThread: Thread?
    var indexCancel = false
    let indexDone = DispatchSemaphore(value: 0)

    init(settings: Settings) {
        self.settings = settings
        cpc = cpc_create(CPC_MODEL_6128)
        romFolder = Emulator.findRomFolder(settings)
        applySettings()
    }

    static func findRomFolder(_ s: Settings) -> URL {
        if let p = s["MacAPE", "ROM Folder"], !p.isEmpty { return URL(fileURLWithPath: p) }
        let candidates = [AppPaths.roms.path, NSHomeDirectory() + "/Documents/WinAPE20B2/ROM"]
        for c in candidates where FileManager.default.fileExists(atPath: c + "/OS6128.ROM") { return URL(fileURLWithPath: c) }
        return AppPaths.roms
    }

    // MARK: ROM handling
    func resolveROM(_ name: String, exts: [String] = ["ROM", "rom"]) -> URL? {
        if name.isEmpty { return nil }
        if name.contains("/") { return URL(fileURLWithPath: name) }
        let fm = FileManager.default
        if let items = try? fm.contentsOfDirectory(atPath: romFolder.path) {
            let low = name.lowercased()
            if let hit = items.first(where: { $0.lowercased() == low }) { return romFolder.appendingPathComponent(hit) }
            for e in exts { if let hit = items.first(where: { $0.lowercased() == low + "." + e.lowercased() }) { return romFolder.appendingPathComponent(hit) } }
        }
        return nil
    }

    func applySettings() {
        lock.lock(); defer { lock.unlock() }
        noteUnreproducible("the settings were changed")
        applyConfig()
        applyROMs()
        cpc_reset(cpc, true)
    }

    /// Applies everything that does not require a reset (display, sound, timing, devices).
    func applyConfig() {
        noteUnreproducible("the settings were changed")
        var cfg = CPCConfig()
        let plus = settings.bool("Configuration", "Enable Plus", false)
        cpc_default_config(&cfg, plus ? CPC_MODEL_6128PLUS : CPC_MODEL_6128)
        cfg.type = UInt8(settings.int("Configuration", "CRTC Type", plus ? 3 : 0))
        cfg.plus = plus
        cfg.plus_ppi = settings.bool("Configuration", "Plus PPI", plus)
        switch settings.int("Configuration", "Extended RAM", 1) {
        case 0: cfg.ram_kb = 64
        case 1: cfg.ram_kb = 128
        case 2: cfg.ram_kb = 320
        default: cfg.ram_kb = 576
        }
        if settings.bool("Configuration", "Silicon Disc", false) { cfg.silicon_disc = true; cfg.ram_kb = 576 }
        cfg.fast_disc = settings.bool("Configuration", "Fast Disc", true)
        cfg.turbo = settings.bool("Configuration", "Turbo Mode", false)
        cfg.four_drives = settings.bool("Configuration", "Enable Four Drives", false)
        cfg.amdrum = settings.bool("Configuration", "AmDrum Enabled", true)
        cfg.digiblaster = settings.string("Configuration", "Printer Mode", "0") == "3"
        cfg.joystick_enabled = settings.bool("Configuration", "Joystick Enabled", true)
        cfg.monitor = CPCMonitor(UInt32(settings.int("Configuration", "Monitor Type", 0)))
        cfg.linear_palette = settings.bool("Configuration", "Linear Palette", false)
        cfg.sample_rate = settings.int("Configuration", "Sound Mode", 2) == 2 ? 44100 : 22050
        cfg.stereo = settings.bool("Configuration", "Sound Stereo", true)
        cfg.volume = settings.int("Configuration", "Sound Mode", 2) == 0 ? 0 : Float(settings.int("Configuration", "Sound Volume", 15)) / 15.0
        cfg.disc_sound = settings.bool("Configuration", "Disc Sound", true)
        cfg.tape_sound = settings.bool("Configuration", "Tape Sound", true)
        cfg.enable_32_roms = settings.bool("Configuration", "Enable 32 ROMs", true)
        cfg.multiface = settings.bool("ROMS", "Multiface Enabled", true)
        cpc_configure(cpc, &cfg)
        cpc.pointee.amx_enabled = settings.bool("Configuration", "AMX Mouse Enabled", false)
        renderPAL = settings.bool("Configuration", "PAL Emulation", true)
        renderBoth = settings.bool("Configuration", "Render Both", true)
        halfSize = settings.bool("Configuration", "Half Size", false)
        brightness = settings.int("Configuration", "Monitor Brightness", 0)
        let skip = settings.int("Configuration", "Refresh Rate", 0)
        frameSkip = skip > 0 ? skip : 0
        speed = max(0.05, min(10.0, Double(settings.int("Configuration", "Emulation Speed", 100)) / 100.0))
        applyDriveOptions()
    }

    /// Relative mouse movement (points, y down) and buttons (bit0 left, bit1 right) for the AMX mouse.
    func amxMouse(dx: CGFloat, dy: CGFloat, buttons: UInt8) {
        lock.lock(); defer { lock.unlock() }
        if cpc.pointee.amx_enabled { cpc_amx_mouse(cpc, Int32(dx.rounded()), Int32(dy.rounded()), buttons) }
    }
    var amxEnabled: Bool {
        get { cpc.pointee.amx_enabled }
        set { noteUnreproducible("the AMX mouse was switched"); lock.lock(); cpc.pointee.amx_enabled = newValue; lock.unlock(); settings.set("Configuration", "AMX Mouse Enabled", newValue) }
    }

    /// Presses the Multiface Stop button; false when it is disabled, has no ROM or is already running.
    func multifaceStop() -> Bool { lock.lock(); defer { lock.unlock() }; return cpc_multiface_stop(cpc) }

    func applyROMs() {
        cpc_clear_roms(cpc)
        if !settings.bool("ROMS", "Disable All", false), let u = resolveROM(settings.string("ROMS", "Lower")), let d = try? Data(contentsOf: u) {
            d.withUnsafeBytes { _ = cpc_load_rom(cpc, -1, $0.bindMemory(to: UInt8.self).baseAddress, d.count) }
        }
        let uppers = settings.bool("ROMS", "Disable All", false) ? 0 : (settings.bool("Configuration", "Enable 32 ROMs", true) ? 32 : 16)
        for i in 0..<uppers {
            if let u = resolveROM(settings.string("ROMS", "Upper(\(i))")), let d = try? Data(contentsOf: u) {
                d.withUnsafeBytes { _ = cpc_load_rom(cpc, Int32(i), $0.bindMemory(to: UInt8.self).baseAddress, d.count) }
            }
        }
        if let u = resolveROM(settings.string("ROMS", "Multiface")), let d = try? Data(contentsOf: u) {
            _ = d.withUnsafeBytes { cpc_load_multiface(cpc, $0.bindMemory(to: UInt8.self).baseAddress, d.count) }
        } else { _ = cpc_load_multiface(cpc, nil, 0) }
        cpc_remove_cartridge(cpc)
        if settings.bool("ROMS", "Cartridge Enabled", false), let u = resolveROM(settings.string("ROMS", "Cartridge"), exts: ["CPR", "cpr", "CRT", "crt"]), let d = try? Data(contentsOf: u) {
            _ = d.withUnsafeBytes { cpc_load_cartridge(cpc, $0.bindMemory(to: UInt8.self).baseAddress, d.count) }
        }
        cpc_update_mapping(cpc)
    }

    // MARK: Thread
    func start() {
        guard !running else { return }
        running = true
        audio = AudioOutput(cpc: cpc, sampleRate: Double(cpc.pointee.cfg.sample_rate))
        audio?.start()
        let t = Thread { [weak self] in self?.loop() }
        t.name = "MacAPE emulation"; t.qualityOfService = .userInteractive
        thread = t; t.start()
    }

    func stop() { running = false; paused = false; audio?.stop(); commitAllDiscs() }

    private func nowUS() -> UInt64 { clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000 }

    private func loop() {
        let framePeriod = 19968.0
        var next = Double(nowUS())
        var shown = 0
        while running {
            if paused { resume.wait(); next = Double(nowUS()); continue }
            lock.lock()
            // Re-check under the lock: a pause/step requested after the check above must not let a whole
            // frame run behind the debugger's back.
            if paused { lock.unlock(); continue }
            let n = speed > 1.0 ? Int(speed.rounded()) : 1
            for _ in 0..<n { cpc_run_frame(cpc) }
            let hit = cpc_dbg_break(cpc)
            let replayDone = cpc_player_active(cpc) && cpc_player_finished(cpc) && !replayEndNotified
            if replayDone { replayEndNotified = true; paused = true }
            lock.unlock()
            if replayDone { DispatchQueue.main.async { self.onReplayEnd?() } }
            audio?.muted = speed != 1.0
            shown += 1
            if frameSkip <= 1 || shown % frameSkip == 0 { publish() }
            if hit { paused = true; DispatchQueue.main.async { self.onBreak?() } }
            if speed == 1.0, let a = audio, a.isRunning {
                // Audio is the master clock: keep about three frames of sound queued.
                let target = Int32(cpc.pointee.cfg.sample_rate) * 3 / 50
                let deadline = nowUS() + 100_000
                while running && !paused && cpc_audio_available(cpc) > target && nowUS() < deadline { usleep(1000) }
                next = Double(nowUS())
                continue
            }
            next += framePeriod / min(speed, 1.0)
            let wait = next - Double(nowUS())
            if wait > 0 { usleep(UInt32(wait)) } else if wait < -100000 { next = Double(nowUS()) }
        }
    }

    /// Renders the current frame, or `picture` (768x270 pixels) when given, into the display buffer.
    func publish(picture: UnsafePointer<UInt32>? = nil) {
        frameLock.lock()
        let half = halfSize
        let view = picture != nil ? .normal : (paused ? screenView : .normal)
        if view != .normal {
            lock.lock()
            viewBuffer.withUnsafeMutableBufferPointer { if view == .raster { cpc_render_raster(cpc, $0.baseAddress) } else { cpc_render_vram(cpc, $0.baseAddress) } }
            lock.unlock()
        }
        rendered.withUnsafeMutableBufferPointer { dst in
            if view == .normal { cpc_render(picture ?? cpc_frame(cpc), dst.baseAddress, renderPAL, renderBoth, half, Int32(brightness)) }
            else { viewBuffer.withUnsafeBufferPointer { cpc_render($0.baseAddress, dst.baseAddress, false, true, half, 0) } }
        }
        renderedSize = half ? (384, 270) : (768, 540)
        frameLock.unlock()
        DispatchQueue.main.async { self.onFrame?() }
    }

    func withFrame<T>(_ body: (UnsafePointer<UInt32>, Int, Int) -> T) -> T {
        frameLock.lock(); defer { frameLock.unlock() }
        return rendered.withUnsafeBufferPointer { body($0.baseAddress!, renderedSize.w, renderedSize.h) }
    }

    // MARK: Media
    func insertDisc(drive: Int, url: URL) -> Bool {
        guard let d = dsk_load_file(url.path) else { return false }
        lock.lock(); defer { lock.unlock() }
        noteUnreproducible("a disc was inserted")
        d.pointee.write_protect = !FileManager.default.isWritableFile(atPath: url.path)
        d.pointee.temp_writes = settings.bool("Drives", "Allow Write \(drive)", false)
        cpc.pointee.single_sided_read.0 = false
        withUnsafeMutablePointer(to: &cpc.pointee.single_sided_read) { $0.withMemoryRebound(to: Bool.self, capacity: 4) { $0[drive] = settings.bool("Drives", "Single Sided \(drive)", false) } }
        commitDisc(drive: drive)
        if let old = discs[drive] { dsk_free(old) }
        discs[drive] = d; discPaths[drive] = url.path
        fdc_insert(cpc.pointee.fdc, Int32(drive), d)
        settings.set("Drives", "Drive(\(drive))", url.path)
        return true
    }

    func removeDisc(drive: Int) {
        commitDisc(drive: drive)
        lock.lock(); defer { lock.unlock() }
        noteUnreproducible("a disc was removed")
        fdc_insert(cpc.pointee.fdc, Int32(drive), nil)
        if let old = discs[drive] { dsk_free(old) }
        discs[drive] = nil; discPaths[drive] = ""
        settings.set("Drives", "Drive(\(drive))", "")
    }

    func swapDiscs() {
        lock.lock(); defer { lock.unlock() }
        noteUnreproducible("the discs were swapped")
        discs.swapAt(0, 1); discPaths.swapAt(0, 1)
        fdc_insert(cpc.pointee.fdc, 0, discs[0]); fdc_insert(cpc.pointee.fdc, 1, discs[1])
    }

    func reset(cold: Bool) { lock.lock(); cpc_reset(cpc, cold); lock.unlock() }

    func type(_ text: String) { lock.lock(); cpc_autotype(cpc, text); lock.unlock() }

    func key(_ k: Int, down: Bool) { lock.lock(); cpc_key(cpc, Int32(k), down); lock.unlock() }
    func joystick(_ bit: Int, down: Bool) { lock.lock(); cpc_joystick(cpc, Int32(bit), down); lock.unlock() }

    func saveSettings() { settings.save(AppPaths.ini) }
}

// MARK: - Tape, snapshots, discs (extended)
extension Emulator {
    var tape: OpaquePointer? { cpc.pointee.tape }

    @discardableResult
    func insertTape(url: URL) -> Bool {
        guard let d = try? Data(contentsOf: url) else { return false }
        let t: OpaquePointer? = d.withUnsafeBytes { raw in
            url.lastPathComponent.withCString { tape_load(raw.bindMemory(to: UInt8.self).baseAddress, d.count, $0) }
        }
        guard let nt = t else { return false }
        lock.lock(); defer { lock.unlock() }
        noteUnreproducible("a tape was inserted")
        tape_free(cpc.pointee.tape)
        cpc.pointee.tape = nt
        tape_set_motor(nt, cpc.pointee.tape_motor)
        return true
    }

    func removeTape() { noteUnreproducible("the tape was removed"); lock.lock(); tape_free(cpc.pointee.tape); cpc.pointee.tape = nil; lock.unlock() }
    func rewindTape() { noteUnreproducible("the tape was rewound"); lock.lock(); tape_rewind(cpc.pointee.tape); lock.unlock() }
    func playTape(_ on: Bool) { noteUnreproducible("the tape was started or stopped"); lock.lock(); tape_play(cpc.pointee.tape, on); lock.unlock() }

    func loadSnapshot(url: URL) -> Bool {
        guard let d = try? Data(contentsOf: url) else { return false }
        lock.lock(); defer { lock.unlock() }
        noteUnreproducible("a snapshot was loaded")
        let ok = d.withUnsafeBytes { cpc_snapshot_load(cpc, $0.bindMemory(to: UInt8.self).baseAddress, d.count) }
        return ok
    }

    func saveSnapshot(url: URL, version: Int, memoryKB: Int, compressed: Bool) -> Bool {
        lock.lock(); defer { lock.unlock() }
        var n = 0
        guard let p = cpc_snapshot_save(cpc, Int32(version), Int32(memoryKB), compressed, &n) else { return false }
        defer { free(p) }
        do { try Data(bytes: p, count: n).write(to: url); return true } catch { return false }
    }

    /// Writes a modified disc image back to its file.
    func commitDisc(drive: Int) {
        guard let d = discs[drive], d.pointee.dirty, !d.pointee.temp_writes, !discPaths[drive].isEmpty else { return }
        _ = dsk_save_file(d, discPaths[drive])
    }
    func commitAllDiscs() { for i in 0..<4 { commitDisc(drive: i) } }
}

extension Emulator {
    func applyDriveOptions() {
        lock.lock(); defer { lock.unlock() }
        noteUnreproducible("the drive options were changed")
        for i in 0..<4 {
            discs[i]?.pointee.temp_writes = settings.bool("Drives", "Allow Write \(i)", false)
            withUnsafeMutablePointer(to: &cpc.pointee.single_sided_read) { $0.withMemoryRebound(to: Bool.self, capacity: 4) { $0[i] = settings.bool("Drives", "Single Sided \(i)", false) } }
        }
    }

    func startTapeRecording() { noteUnreproducible("tape recording was started"); lock.lock(); cpc_tape_record_start(cpc); lock.unlock() }
    func stopTapeRecording(to url: URL) -> Bool {
        lock.lock(); defer { lock.unlock() }
        var n = 0
        guard let p = cpc_tape_record_stop(cpc, &n) else { return false }
        defer { free(p) }
        return (try? Data(bytes: p, count: n).write(to: url)) != nil
    }
    var isRecordingTape: Bool { cpc.pointee.rec_active }
}

// MARK: - Debugger control
extension Emulator {
    var dbg: UnsafeMutablePointer<Debugger> { lock.lock(); defer { lock.unlock() }; return dbg_attach(cpc)! }

    /// Pauses at an instruction boundary (waits for the current frame to finish).
    func debugPause() {
        _ = dbg
        paused = true
        // a pending run-to / step-over / step-out target is cancelled by stopping by hand
        lock.lock(); let d = dbg_attach(cpc)!; d.pointee.temp_bp = -1; d.pointee.step_out = false; lock.unlock()
    }

    func debugResume() {
        beforeResume?()
        lock.lock()
        let d = dbg_attach(cpc)!
        d.pointee.hit = false; d.pointee.skip_once = true; d.pointee.temp_bp = -1; d.pointee.step_out = false
        lock.unlock()
        paused = false
    }

    /// Runs to the start of the next scanline (lines > 0) or to the next frame (VSYNC), stopping early at a breakpoint.
    func debugStepRaster(frame: Bool) {
        debugPause()
        lock.lock()
        let d = dbg_attach(cpc)!
        d.pointee.hit = false; d.pointee.skip_once = true
        let y0 = cpc.pointee.beam_y, f0 = cpc.pointee.frames, start = cpc.pointee.cycles
        while !d.pointee.hit && cpc.pointee.cycles - start < 80_000 {
            cpc_step(cpc)
            if frame ? cpc.pointee.frames != f0 : cpc.pointee.beam_y != y0 { break }
        }
        d.pointee.hit = false
        lock.unlock()
        publish()
    }
    /// Human-readable beam position for the debugger.
    var beamDescription: String {
        lock.lock(); defer { lock.unlock() }
        let k = cpc.pointee.crtc, y = Int(cpc.pointee.beam_y)
        let regs = String(format: "VCC %02X VLC %02X HCC %02X", k.vcc, k.vlc, k.hcc)
        if k.vsync { return "Beam: VSYNC · " + regs }
        if k.vde {   // inside the vertical display area: pixel row of the picture, and where along the line
            let row = Int(k.vcc) * (Int(k.r.9 & 31) + 1) + Int(k.vlc)
            return "Beam: picture row \(row) (screen line \(y)), " + (k.hde ? "char \(k.hcc)" : "side border") + " · " + regs
        }
        return (y < 0 ? "Beam: top border / flyback (\(-y) lines above screen) · " : "Beam: vertical border, line \(y) · ") + regs
    }

    /// Runs until the current function returns (Shift+F8).
    func debugStepOut() {
        debugPause()
        lock.lock(); dbg_step_out(cpc); lock.unlock()
        paused = false
    }

    func debugStepInto() {
        debugPause()
        lock.lock(); dbg_step_into(cpc); lock.unlock()
        publish()
    }

    /// Returns true when the machine continues running (CALL/RST/block instruction).
    @discardableResult
    func debugStepOver() -> Bool {
        debugPause()
        lock.lock(); let run = dbg_step_over(cpc); lock.unlock()
        if run { paused = false } else { publish() }
        return run
    }

    func debugRunTo(_ addr: UInt16) {
        debugPause()
        lock.lock(); dbg_run_to(cpc, addr); lock.unlock()
        paused = false
    }
}
