import AppKit
import CPCCore

/// Session recording and replay (see CPCCore/include/replay.h). A recording is the machine state at the start
/// plus every input with its emulated timestamp; replaying it reproduces the session exactly and can jump to
/// any frame. Anything that changes the machine in a way a replay cannot reproduce (inserting media, changing
/// settings, editing memory or registers) ends the recording at that point.
extension Emulator {
    static let framesPerSecond = 50.0

    static func clock(_ frames: UInt64) -> String {
        let s = Int(Double(frames) / framesPerSecond)
        return String(format: "%d:%02d", s / 60, s % 60)
    }

    /// Call before an action that replay cannot reproduce; ends a recording in progress.
    func noteUnreproducible(_ reason: String) {
        lock.lock(); cpc_record_taint(cpc, reason); lock.unlock()
    }

    var isRecording: Bool { lock.lock(); defer { lock.unlock() }; return cpc.pointee.recorder != nil }
    var isReplaying: Bool { lock.lock(); defer { lock.unlock() }; return cpc.pointee.player != nil }
    /// Why a recording was cut short (it waits to be collected with finishRecording), or nil.
    var cutReason: String? {
        lock.lock(); defer { lock.unlock() }
        return cpc_record_cut_reason(cpc).map { String(cString: $0) }
    }

    /// Text for the window title: "● REC 0:12", "▶ 0:12 / 1:30", or "▶ … OUT OF SYNC".
    var sessionStatus: String {
        lock.lock(); defer { lock.unlock() }
        if cpc.pointee.recorder != nil { return "● REC " + Emulator.clock(cpc.pointee.frames - recordStartFrame) }
        if cpc.pointee.player != nil {
            var s = "▶ " + Emulator.clock(displayedFrame ?? cpc_player_frame(cpc)) + " / " + Emulator.clock(cpc_player_length(cpc))
            var f: UInt64 = 0
            if cpc_player_desync(cpc, &f) { s += "  OUT OF SYNC at " + Emulator.clock(f) }
            return s
        }
        return ""
    }

    // MARK: recording
    func startRecording() -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard cpc_record_start(cpc) else { return false }
        recordStartFrame = cpc.pointee.frames
        return true
    }

    /// Ends the recording (or collects one cut short) and attaches a description of the media. Caller frees it.
    func finishRecording() -> OpaquePointer? {
        lock.lock(); defer { lock.unlock() }
        guard let r = cpc_record_stop(cpc) else { return nil }
        recordingMeta().withCString { cpc_recording_set_meta(r, $0) }
        return r
    }

    func recordingLength(_ r: OpaquePointer) -> UInt64 { cpc_recording_frames(r) }
    func recordingTaint(_ r: OpaquePointer) -> String? { cpc_recording_taint(r).map { String(cString: $0) } }

    func saveRecording(_ r: OpaquePointer, to url: URL) -> Bool {
        var n = 0
        guard let p = cpc_recording_serialize(r, &n) else { return false }
        defer { free(p) }
        do { try Data(bytes: p, count: n).write(to: url); return true } catch { return false }
    }

    /// Identifies the media and ROMs so that a replay can warn when they differ.
    private func recordingMeta() -> String {
        var drives: [Any] = []
        for i in 0..<4 {
            let path = discPaths[i]
            if path.isEmpty { drives.append(NSNull()); continue }
            let data = FileManager.default.contents(atPath: path) ?? Data()
            drives.append(["path": path, "size": data.count, "crc": Int(crc(data))])
        }
        var roms: [String: String] = [:]
        for k in ["Lower"] + (0..<32).map({ "Upper(\($0))" }) { let v = settings.string("ROMS", k); if !v.isEmpty { roms[k] = v } }
        let meta: [String: Any] = ["app": "MacAPE", "date": ISO8601DateFormatter().string(from: Date()), "drives": drives, "roms": roms,
                                   "tape": tape.map { String(cString: tape_name($0)) } ?? ""]
        let d = (try? JSONSerialization.data(withJSONObject: meta)) ?? Data()
        return String(decoding: d, as: UTF8.self)
    }

    private func crc(_ d: Data) -> UInt32 { d.withUnsafeBytes { cpc_crc32($0.bindMemory(to: UInt8.self).baseAddress, d.count) } }

    // MARK: playback
    /// Reads a recording file. Returns nil with an explanation when it cannot be used.
    func openRecording(url: URL) -> (rec: OpaquePointer?, error: String?) {
        guard let data = try? Data(contentsOf: url) else { return (nil, "The file cannot be read.") }
        let rec: OpaquePointer? = data.withUnsafeBytes { cpc_recording_deserialize($0.bindMemory(to: UInt8.self).baseAddress, data.count) }
        guard let r = rec else { return (nil, "This is not a MacAPE session recording, or it was made by a different build of MacAPE.") }
        return (r, nil)
    }

    /// Inserts the recorded discs when they are available on disk and returns what does not match.
    func prepareMedia(for rec: OpaquePointer) -> [String] {
        guard let m = cpc_recording_meta(rec), let obj = try? JSONSerialization.jsonObject(with: Data(String(cString: m).utf8)) as? [String: Any] else {
            return ["The recording does not say which disc it used."]
        }
        var warnings: [String] = []
        let drives = obj["drives"] as? [Any] ?? []
        for i in 0..<min(4, drives.count) {
            guard let d = drives[i] as? [String: Any], let path = d["path"] as? String, let want = d["crc"] as? Int else { continue }
            let have = discPaths[i].isEmpty ? nil : FileManager.default.contents(atPath: discPaths[i]).map { Int(crc($0)) }
            if have == want { continue }
            if let data = FileManager.default.contents(atPath: path), Int(crc(data)) == want, insertDisc(drive: i, url: URL(fileURLWithPath: path)) { continue }
            warnings.append("Drive \(["A", "B", "C", "D"][i]): the session used \((path as NSString).lastPathComponent), which is not what is inserted.")
        }
        for (k, v) in (obj["roms"] as? [String: String] ?? [:]) where settings.string("ROMS", k) != v {
            warnings.append("ROM \(k): the session used \(v), now \(settings.string("ROMS", k).isEmpty ? "none" : settings.string("ROMS", k)).")
        }
        return warnings
    }

    /// Restores the starting state and plays. Returns notes about differences that may break the sync, or nil on failure.
    func startPlayback(_ rec: OpaquePointer) -> [String]? {
        stopIndexing()
        lock.lock()
        if cpc.pointee.player != nil { endPlaybackLocked() }
        guard let player = cpc_player_create(rec) else { lock.unlock(); return nil }
        var hz: UInt32 = 0
        guard cpc_player_begin(cpc, player, &hz) else { cpc_player_free(player); lock.unlock(); return nil }
        replayRecording = rec; replayPlayer = player; replayEndNotified = false; replayActive = true
        var notes: [String] = []
        if hz & 1 != 0 { notes.append("A disc was written to since the recording started, so its contents may differ.") }
        if hz & 2 != 0 { notes.append("The inserted tape is not the one used in the recording.") }
        if hz & 4 != 0 { notes.append("The machine configuration (model, memory, CRTC...) differs from the recording.") }
        paused = false
        lock.unlock()
        startIndexing(rec)
        DispatchQueue.main.async { self.onReplayChanged?(true) }
        return notes
    }

    private func endPlaybackLocked() {
        cpc_player_end(cpc)
        if let p = replayPlayer { cpc_player_free(p) }
        if let r = replayRecording { cpc_recording_free(r) }
        replayPlayer = nil; replayRecording = nil; replayActive = false
    }
    /// Leaves the replay: the machine goes on from the current point, taking input again.
    func endPlayback() {
        stopIndexing()
        lock.lock(); let was = replayActive; endPlaybackLocked(); lock.unlock()
        if was { DispatchQueue.main.async { self.onReplayChanged?(false) } }
    }

    func seek(toFrame f: UInt64) -> Bool {
        lock.lock()
        let ok = cpc_player_seek(cpc, f)
        replayEndNotified = false
        lock.unlock()
        publish()
        return ok
    }
    /// Frames up to which seeking is instant (keyframes exist); the whole length once indexing has finished.
    var indexedFrames: UInt64 { lock.lock(); defer { lock.unlock() }; return cpc_player_indexed(cpc) }

    /// Pictures of frames from+1...from+count (as displayed), for reverse playback. Leaves the machine scratched:
    /// call seek(toFrame:) afterwards to put it back on an exact frame.
    func capturePictures(from: UInt64, count: Int) -> (pixels: [UInt32], count: Int) {
        var buf = [UInt32](repeating: 0, count: count * Int(CPC_SCREEN_W) * Int(CPC_SCREEN_H))
        lock.lock()
        let n = buf.withUnsafeMutableBufferPointer { Int(cpc_player_capture(cpc, from, Int32(count), $0.baseAddress)) }
        lock.unlock()
        return (buf, n)
    }

    // MARK: background indexing
    /// A clone of the machine replays the whole recording on another thread; the keyframes it takes are handed to
    /// the visible player so that seeking anywhere is fast.
    func startIndexing(_ rec: OpaquePointer) {
        lock.lock(); let clone = cpc_clone_machine(cpc); lock.unlock()
        guard let c2 = clone, let p2 = cpc_player_create(rec) else { if let c = clone { cpc_clone_free(c) }; return }
        var hz: UInt32 = 0
        guard cpc_player_begin(c2, p2, &hz) else { cpc_player_free(p2); cpc_clone_free(c2); return }
        indexCancel = false
        let t = Thread { [unowned self] in
            while !self.indexCancel && !cpc_player_finished(c2) {
                for _ in 0..<100 where !cpc_player_finished(c2) { cpc_run_frame(c2) }
                self.lock.lock()
                if self.replayRecording == rec { _ = cpc_player_adopt_keyframes(self.cpc, p2) } else { self.indexCancel = true }
                self.lock.unlock()
            }
            cpc_player_free(p2); cpc_clone_free(c2)
            self.indexDone.signal()
        }
        t.name = "MacAPE replay index"; t.qualityOfService = .utility
        indexThread = t; t.start()
    }

    /// Stops the indexing thread and waits for it (never call with the machine lock held).
    func stopIndexing() {
        guard indexThread != nil else { return }
        indexCancel = true
        indexDone.wait()                       // the thread signals exactly once, even if it already finished
        indexThread = nil
    }

    var playbackFrame: UInt64 { lock.lock(); defer { lock.unlock() }; return cpc_player_frame(cpc) }
    var playbackLength: UInt64 { lock.lock(); defer { lock.unlock() }; return cpc_player_length(cpc) }
}

// MARK: - Menu actions
extension AppDelegate {
    @objc func recordSession(_ s: Any?) {
        if emu.isRecording { finishAndSaveRecording(); return }
        if emu.isReplaying { alert("A session is playing", "Stop the playback before recording."); return }
        if !emu.startRecording() { alert("Cannot start recording") }
    }

    /// Ends the recording (also the one cut short by an action it cannot reproduce) and asks where to save it.
    func finishAndSaveRecording(cutBy reason: String? = nil) {
        guard let rec = emu.finishRecording() else { return }
        defer { cpc_recording_free(rec) }
        let length = Emulator.clock(emu.recordingLength(rec))
        if let why = reason ?? emu.recordingTaint(rec) {
            let a = NSAlert(); a.messageText = "The recording was stopped: \(why)"
            a.informativeText = "Everything up to that point (\(length)) is still exact and can be saved."
            a.addButton(withTitle: "Save…"); a.addButton(withTitle: "Discard")
            if a.runModal() != .alertFirstButtonReturn { return }
        }
        let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd HH.mm"
        guard let u = chooseFile(title: "Save Session (\(length))", types: ["cpcrec"], save: true, name: "Session \(f.string(from: Date())).cpcrec") else { return }
        if !emu.saveRecording(rec, to: u) { alert("Cannot save the session", u.path) }
    }

    @objc func playSession(_ s: Any?) {
        if emu.isRecording { alert("A session is being recorded", "Stop the recording before playing another."); return }
        guard let u = chooseFile(title: "Playback Session", types: ["cpcrec"]) else { return }
        let (rec, error) = emu.openRecording(url: u)
        guard let r = rec else { alert("Cannot play this session", error ?? ""); return }
        let warnings = emu.prepareMedia(for: r)
        if !warnings.isEmpty {
            let a = NSAlert(); a.messageText = "The session may not play back exactly"
            a.informativeText = warnings.joined(separator: "\n") + "\n\nThe timer in the title bar turns to OUT OF SYNC if it drifts."
            a.addButton(withTitle: "Play Anyway"); a.addButton(withTitle: "Cancel")
            if a.runModal() != .alertFirstButtonReturn { cpc_recording_free(r); return }
        }
        guard let notes = emu.startPlayback(r) else { cpc_recording_free(r); alert("Cannot start the playback", "The starting state could not be restored."); return }
        if !notes.isEmpty { alert("Note", notes.joined(separator: "\n")) }
    }

    @objc func goToTime(_ s: Any?) {
        guard emu.isReplaying else { return }
        let len = Emulator.clock(emu.playbackLength)
        guard let t = promptText("Go to Time", "mm:ss, seconds, or f<number> for a frame (recording length \(len)):", Emulator.clock(emu.playbackFrame)) else { return }
        transport.stopReverse(seek: false)
        let s = t.trimmingCharacters(in: .whitespaces).lowercased()
        var frame: Double?
        if s.hasPrefix("f") { frame = Double(s.dropFirst()) }
        else if s.contains(":") { let p = s.split(separator: ":").compactMap { Double($0) }; if p.count == 2 { frame = (p[0] * 60 + p[1]) * Emulator.framesPerSecond } }
        else if let v = Double(s) { frame = v * Emulator.framesPerSecond }
        guard let f = frame, f >= 0 else { NSSound.beep(); return }
        _ = emu.seek(toFrame: UInt64(f))
    }

    @objc func stopPlayback(_ s: Any?) {
        guard emu.isReplaying else { return }
        emu.endPlayback()
        if emu.paused { emu.debugResume() }
    }
}
