import AppKit
import CPCCore
import UniformTypeIdentifiers

@main
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuItemValidation {
    static func main() {
        let app = NSApplication.shared
        let d = AppDelegate()
        app.delegate = d
        app.setActivationPolicy(.regular)
        app.run()
    }

    var settings = Settings()
    var emu: Emulator!
    var main: MainWindowController!
    var currentProfile = "CPC6128"
    var driveSetup: DriveSetupController!
    var tapeControl: TapeController!
    var lastSnapshot: URL?
    var debugger: DebuggerController?
    var pokes: PokesController?
    var help: HelpController?
    var transport: TransportController!
    var cutTimer: Timer?
    var askingAboutCut = false
    var symbolsURL: URL?            // last symbols file (from /SYM or Load Symbols), reloaded by ⌘R
    var autoRunRequest: String?     // /A value from the command line ("" = pick from the catalogue)
    var settingsWindow: SettingsController!
    func ensureDebugger() -> DebuggerController { if debugger == nil { debugger = DebuggerController(emulator: emu) }; return debugger! }

    func applicationDidFinishLaunching(_ notification: Notification) {
        Profiles.installDefaults()
        if let s = Settings.load(AppPaths.ini) { settings = s }
        else {
            settings = Settings()
            if let p = Profiles.load(currentProfile) { settings.merge(p) }
        }
        if let p = settings["MacAPE", "Profile"] { currentProfile = p }
        emu = Emulator(settings: settings)
        main = MainWindowController(emulator: emu)
        transport = TransportController(emu: emu, main: main)
        transport.onTakeControl = { [weak self] in self?.stopPlayback(nil) }
        main.view.onTransportKey = { [weak self] e in self?.transport.handleKey(e) ?? false }
        main.view.onFileDrop = { [weak self] in self?.openFile($0) }
        driveSetup = DriveSetupController(emu: emu); tapeControl = TapeController(emu: emu)
        settingsWindow = SettingsController(emu: emu); settingsWindow.currentProfile = currentProfile
        settingsWindow.onApply = { [weak self] in guard let self = self else { return }; self.currentProfile = self.settingsWindow.currentProfile; self.emu.applySettings(); self.main.view.keyTable = KeyMap.effectiveMap(settings: self.settings); self.applyDisplayOptions() }
        main.view.keyTable = KeyMap.effectiveMap(settings: settings)
        emu.onBreak = { [weak self] in guard let self = self else { return }; self.ensureDebugger().show() }
        // a recording cut short by an action it cannot reproduce (inserting a disc, editing memory...) is offered for saving
        cutTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self = self, !self.askingAboutCut, let why = self.emu.cutReason else { return }
            self.askingAboutCut = true
            DispatchQueue.main.async { self.finishAndSaveRecording(cutBy: why); self.askingAboutCut = false }
        }
        buildMenus()
        main.showWindow(nil)
        main.window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        if !FileManager.default.fileExists(atPath: emu.romFolder.appendingPathComponent("OS6128.ROM").path) { askForROMFolder() }
        emu.start()
        let opt = LaunchOptions(Array(CommandLine.arguments.dropFirst()))
        if opt.help { print(LaunchOptions.usage); exit(0) }
        for e in opt.errors { FileHandle.standardError.write(("MacAPE: " + e + "\n").data(using: .utf8)!) }
        var discs = 0
        for f in opt.files {
            if ["dsk", "dsc", "hdr"].contains(f.pathExtension.lowercased()) {
                if discs == 0 { openFile(f) } else if !emu.insertDisc(drive: 1, url: f) { alert("Cannot open disc image", f.path) }
                discs += 1
            } else { openFile(f) }
        }
        if let sn = opt.snapshot { openFile(sn) }
        symbolsURL = opt.symbols; autoRunRequest = opt.autoRun
        if let sym = opt.symbols {
            emu.lock.lock(); _ = dbg_attach(emu.cpc); let n = dbg_symbols_load(emu.cpc, sym.path); emu.lock.unlock()
            if n < 0 { alert("Cannot load symbols", sym.path) } else { FileHandle.standardError.write("MacAPE: \(n) symbols loaded from \(sym.lastPathComponent)\n".data(using: .utf8)!) }
        }
        var typing = ""
        if let req = opt.autoRun {
            if let cmd = emu.autoRunCommand(req) { typing += cmd } else { alert("Nothing to run", "No program found on the disc in drive A.") }
        }
        if let tf = opt.typeFile {
            if let t = try? String(contentsOf: tf, encoding: .isoLatin1) { typing += t.replacingOccurrences(of: "\r\n", with: "\n") } else { alert("Cannot read Auto-Type file", tf.path) }
        }
        if let t = opt.typed { typing += t.replacingOccurrences(of: "~", with: "\n") }
        if !typing.isEmpty { DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) { self.emu.type(typing) } }
        let panel = opt.panel, shot = opt.shot, shotWin = opt.shotWin, after = opt.after, panelAt = opt.panelAt
        if let pn = panel { DispatchQueue.main.asyncAfter(deadline: .now() + panelAt) { _ = self.perform(Selector(pn), with: nil) } }
        if let path = shot {
            DispatchQueue.main.asyncAfter(deadline: .now() + after) {
                if let title = shotWin, let w = NSApp.windows.first(where: { $0.title.contains(title) }), let v = (ProcessInfo.processInfo.environment["SHOTCONTENT"] != nil ? w.contentView : (w.contentView?.superview ?? w.contentView)) {
                    w.contentView?.layoutSubtreeIfNeeded(); w.contentView?.displayIfNeeded()
                    let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds)!
                    v.cacheDisplay(in: v.bounds, to: rep)
                    try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
                } else { self.writePNG(URL(fileURLWithPath: path)) }
                NSApp.terminate(nil)
            }
        }
    }

    func applicationWillTerminate(_ notification: Notification) { emu.stop(); settings["MacAPE", "Profile"] = currentProfile; emu.saveSettings() }
    func application(_ sender: NSApplication, openFile filename: String) -> Bool { if emu != nil { openFile(URL(fileURLWithPath: filename)); return true }; return false }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    // MARK: Helpers
    func alert(_ title: String, _ info: String = "") {
        let a = NSAlert(); a.messageText = title; a.informativeText = info; a.runModal()
    }

    func chooseFile(title: String, types: [String], save: Bool = false, name: String = "") -> URL? {
        let p: NSSavePanel = save ? NSSavePanel() : { let o = NSOpenPanel(); o.canChooseFiles = true; o.canChooseDirectories = false; return o }()
        p.title = title
        let ut = types.compactMap { UTType(filenameExtension: $0) }
        if !ut.isEmpty { p.allowedContentTypes = ut }
        if save { p.nameFieldStringValue = name }
        return p.runModal() == .OK ? p.url : nil
    }

    func askForROMFolder() {
        alert("ROM images not found", "MacAPE needs the Amstrad ROM images (OS6128.ROM, BASIC1-1.ROM, AMSDOS.ROM, …). Select the folder that contains them (for example the ROM folder of a WinAPE installation).")
        let p = NSOpenPanel(); p.canChooseDirectories = true; p.canChooseFiles = false; p.title = "Select ROM folder"
        if p.runModal() == .OK, let u = p.url {
            emu.romFolder = u; settings["MacAPE", "ROM Folder"] = u.path
            // Copy to app support so access survives
            try? FileManager.default.createDirectory(at: AppPaths.roms, withIntermediateDirectories: true)
            emu.applySettings()
        }
    }

    func openFile(_ url: URL) {
        switch url.pathExtension.lowercased() {
        case "dsk", "dsc", "hdr": if !emu.insertDisc(drive: 0, url: url) { alert("Cannot open disc image", url.path) } else { emu.reset(cold: false) }
        case "sna": if !emu.loadSnapshot(url: url) { alert("Cannot load snapshot", url.path) } else { lastSnapshot = url }
        case "cdt", "tzx", "voc", "wav": if !emu.insertTape(url: url) { alert("Unsupported or damaged tape image", url.path) }
        case "cpr", "crt": insertCartridge(url)
        case "wpf": if let p = Settings.load(url) { settings.merge(p); emu.applySettings() }
        case "ini": if let p = Settings.load(url) { settings.merge(p); emu.applySettings() }
        default: alert("Unsupported file type", url.lastPathComponent)
        }
    }

    @objc func todo(_ sender: Any?) { alert("Not available yet", "This feature is planned but not implemented in this build.") }

    // MARK: File menu
    @objc func insertDiscA(_ s: Any?) { insertDisc(0) }
    @objc func insertDiscB(_ s: Any?) { insertDisc(1) }
    func insertDisc(_ drive: Int) {
        guard let u = chooseFile(title: "Insert Disc Image in Drive \(drive == 0 ? "A" : "B"):", types: ["dsk", "dsc", "hdr"]) else { return }
        if !emu.insertDisc(drive: drive, url: u) { alert("Cannot open disc image", u.path) }
    }
    @objc func removeDiscA(_ s: Any?) { emu.removeDisc(drive: 0) }
    @objc func removeDiscB(_ s: Any?) { emu.removeDisc(drive: 1) }
    @objc func swapDiscs(_ s: Any?) { emu.swapDiscs() }
    @objc func newBlankDiscA(_ s: Any?) { newBlank(0) }
    @objc func newBlankDiscB(_ s: Any?) { newBlank(1) }
    func newBlank(_ drive: Int) {
        guard let u = chooseFile(title: "New Blank Disc", types: ["dsk"], save: true, name: "blank.dsk") else { return }
        let d = dsk_new()!
        dsk_format(d, DSK_FORMAT_DATA, 40, 1)
        _ = dsk_save_file(d, u.path); dsk_free(d)
        _ = emu.insertDisc(drive: drive, url: u)
    }
    @objc func driveSetup(_ s: Any?) { driveSetup.show() }
    @objc func loadCartridge(_ s: Any?) {
        guard let u = chooseFile(title: "Load Cartridge", types: ["cpr", "crt"]) else { return }
        insertCartridge(u)
    }
    /// Cartridges need the Plus ASIC, so a classic machine is switched to Plus mode first.
    func insertCartridge(_ u: URL) {
        if !settings.bool("Configuration", "Enable Plus", false) {
            settings.set("Configuration", "Enable Plus", true); settings.set("Configuration", "Plus PPI", true); settings.set("Configuration", "CRTC Type", 3)
        }
        settings.set("ROMS", "Cartridge", u.path); settings.set("ROMS", "Cartridge Enabled", true)
        emu.applySettings()
    }
    @objc func showTape(_ s: Any?) { tapeControl.show() }
    @objc func insertTape(_ s: Any?) {
        guard let u = chooseFile(title: "Insert Tape Image", types: ["cdt", "tzx", "voc", "wav"]) else { return }
        if !emu.insertTape(url: u) { alert("Unsupported or damaged tape image", u.path) }
        tapeControl.reload()
    }
    @objc func rewindTape(_ s: Any?) { emu.rewindTape() }
    @objc func removeTape(_ s: Any?) { emu.removeTape(); tapeControl.reload() }
    @objc func pressPlay(_ s: Any?) { emu.playTape(true) }
    @objc func pressRecord(_ s: Any?) { emu.startTapeRecording() }
    @objc func loadSnapshot(_ s: Any?) {
        guard let u = chooseFile(title: "Load Snapshot", types: ["sna"]) else { return }
        if emu.loadSnapshot(url: u) { lastSnapshot = u } else { alert("Cannot load snapshot", u.path) }
    }
    @objc func saveSnapshot(_ s: Any?) {
        guard let opt = SnapshotDialog.ask(), let u = chooseFile(title: "Save Snapshot", types: ["sna"], save: true, name: "snapshot.sna") else { return }
        if emu.saveSnapshot(url: u, version: opt.version, memoryKB: opt.memoryKB, compressed: opt.compressed) { lastSnapshot = u } else { alert("Cannot save snapshot", u.path) }
    }
    @objc func updateSnapshot(_ s: Any?) {
        guard let u = lastSnapshot else { saveSnapshot(s); return }
        _ = emu.saveSnapshot(url: u, version: 3, memoryKB: 0, compressed: true)
    }
    @objc func saveScreenshot(_ s: Any?) {
        guard let u = chooseFile(title: "Save Screenshot", types: ["png"], save: true, name: "screenshot.png") else { return }
        writePNG(u)
    }
    func writePNG(_ u: URL) {
        emu.withFrame { px, w, h in
            let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: w, pixelsHigh: h, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: w * 4, bitsPerPixel: 32)!
            let d = rep.bitmapData!
            for i in 0..<(w * h) { let p = px[i]; d[i * 4] = UInt8((p >> 16) & 255); d[i * 4 + 1] = UInt8((p >> 8) & 255); d[i * 4 + 2] = UInt8(p & 255); d[i * 4 + 3] = 255 }
            try? rep.representation(using: .png, properties: [:])?.write(to: u)
        }
    }
    @objc func autoType(_ s: Any?) {
        let a = NSAlert(); a.messageText = "Auto Type"; a.informativeText = "Text to type into the emulated CPC:"
        let f = NSTextField(frame: NSRect(x: 0, y: 0, width: 360, height: 24)); a.accessoryView = f
        a.addButton(withTitle: "Type"); a.addButton(withTitle: "Cancel")
        if a.runModal() == .alertFirstButtonReturn { emu.type(f.stringValue + "\n") }
    }
    @objc func pasteText(_ s: Any?) { if let t = NSPasteboard.general.string(forType: .string) { emu.type(t) } }

    // MARK: Settings menu
    @objc func showGeneral(_ s: Any?) { settingsWindow.show(tab: "General") }
    @objc func showDisplay(_ s: Any?) { settingsWindow.show(tab: "Display") }
    @objc func showSound(_ s: Any?) { settingsWindow.show(tab: "Sound") }
    @objc func showMemory(_ s: Any?) { settingsWindow.show(tab: "Memory") }
    @objc func showInput(_ s: Any?) { settingsWindow.show(tab: "Input") }
    @objc func showOther(_ s: Any?) { settingsWindow.show(tab: "Other") }
    func applyDisplayOptions() {
        let fs = main.window?.styleMask.contains(.fullScreen) ?? false
        let sfx = fs ? " FS" : ""
        main.panelHidden = settings.bool("Configuration", "Hide Panel" + sfx, false)
        main.view.hideMouse = settings.bool("Configuration", "Hide Mouse" + sfx, true)
        if fs { NSApp.presentationOptions = settings.bool("Configuration", "Hide Menus FS", true) ? [.autoHideMenuBar, .autoHideDock] : [] }
    }
    @objc func normalSpeed(_ s: Any?) { emu.speed = 1.0 }
    @objc func highSpeed(_ s: Any?) { emu.speed = 10.0 }
    @objc func toggleFullScreenAction(_ s: Any?) { main.window?.toggleFullScreen(nil); DispatchQueue.main.asyncAfter(deadline: .now() + 1) { self.applyDisplayOptions() } }
    @objc func resetEmu(_ s: Any?) { emu.reset(cold: false) }
    @objc func coldReset(_ s: Any?) { emu.reset(cold: true) }
    @objc func amxMouse(_ s: Any?) { emu.amxEnabled.toggle() }
    @objc func multifaceStop(_ s: Any?) {
        if !emu.multifaceStop() && (settings.string("ROMS", "Multiface").isEmpty || !settings.bool("ROMS", "Multiface Enabled", true)) {
            alert("Multiface not available", "Enable the Multiface and select its ROM in Settings > Memory.")
        }
    }
    @objc func scale1x(_ s: Any?) { main.setContentScale(1) }
    @objc func scale15x(_ s: Any?) { main.setContentScale(1.5) }
    @objc func scale2x(_ s: Any?) { main.setContentScale(2) }
    @objc func selectProfile(_ s: NSMenuItem) {
        guard let p = Profiles.load(s.title) else { return }
        currentProfile = s.title; settings.merge(p); emu.applySettings(); settingsWindow.currentProfile = s.title
    }

    // MARK: Debug / assembler / help
    @objc func debugRun(_ s: Any?) {
        let d = ensureDebugger()
        if d.hideOnRun { d.window?.orderOut(nil) }
        emu.debugResume()
    }
    /// F7 as in WinAPE: pauses a running machine, single-steps a paused one.
    @objc func debugPause(_ s: Any?) { let d = ensureDebugger(); if emu.paused { emu.debugStepInto() } else { emu.debugPause() }; d.show() }
    @objc func debugStep(_ s: Any?) { let d = ensureDebugger(); emu.debugStepInto(); d.show() }
    @objc func debugStepOver(_ s: Any?) {
        let d = ensureDebugger()
        if emu.debugStepOver() { if d.hideOnRun { d.window?.orderOut(nil) } } else { d.show() }
    }
    @objc func debugRunToCursor(_ s: Any?) { ensureDebugger().runToCursor(s) }
    /// ⌘R: after rebuilding, re-read the disc in drive A and the symbols file, cold reset and run it again.
    @objc func reloadDiscAndSymbols(_ s: Any?) {
        let path = emu.discPaths[0]
        guard !path.isEmpty else { alert("No disc in drive A", "Insert the disc image you are working on first."); return }
        if !emu.insertDisc(drive: 0, url: URL(fileURLWithPath: path)) { alert("Cannot reload disc image", path); return }
        var note = ""
        if let sym = symbolsURL {
            emu.lock.lock(); dbg_symbol_clear(emu.cpc); let n = dbg_symbols_load(emu.cpc, sym.path); emu.lock.unlock()
            note = n < 0 ? " (symbols not found)" : " — \(n) symbols from \(sym.lastPathComponent)"
        }
        if emu.paused { emu.debugResume() }
        emu.reset(cold: true)
        if let cmd = emu.autoRunCommand(autoRunRequest ?? "") { DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) { self.emu.type(cmd) } }
        debugger?.symbolsReloaded(note); debugger?.symbolsWindow?.reload()
        main.window?.title = "MacAPE — " + URL(fileURLWithPath: path).lastPathComponent
    }
    @objc func debugStepBack(_ s: Any?) { let d = ensureDebugger(); d.show(); d.stepBack(s) }
    @objc func debugStepBackOver(_ s: Any?) { let d = ensureDebugger(); d.show(); d.stepBackOver(s) }
    @objc func debugReverseContinue(_ s: Any?) { let d = ensureDebugger(); d.show(); d.reverseContinue(s) }
    @objc func debugRunBackToCursor(_ s: Any?) { let d = ensureDebugger(); d.show(); d.reverseToCursor(s) }
    @objc func debugStepScanline(_ s: Any?) { let d = ensureDebugger(); emu.debugStepRaster(frame: false); d.show() }
    @objc func debugStepFrame(_ s: Any?) { let d = ensureDebugger(); emu.debugStepRaster(frame: true); d.show() }
    @objc func debugStepOut(_ s: Any?) { let d = ensureDebugger(); emu.debugStepOut(); if d.hideOnRun { d.window?.orderOut(nil) } }
    @objc func debugLoadSymbols(_ s: Any?) { let d = ensureDebugger(); d.show(); d.loadSymbols(s) }
    @objc func debugClearSymbols(_ s: Any?) { ensureDebugger().clearSymbols(s) }
    @objc func debugShowSymbols(_ s: Any?) { let d = ensureDebugger(); d.show(); d.showSymbols(s) }
    @objc func debugShowWatch(_ s: Any?) { let d = ensureDebugger(); d.show(); d.showWatch(s) }
    @objc func debugShowLog(_ s: Any?) { let d = ensureDebugger(); d.show(); d.showLog(s) }
    @objc func debugShowHistory(_ s: Any?) { let d = ensureDebugger(); d.show(); d.showHistory(s) }
    @objc func debugScreenView(_ s: NSMenuItem) { ensureDebugger().setScreenView(Emulator.ScreenView(rawValue: s.tag) ?? .normal) }
    @objc func debugFindAgain(_ s: Any?) { ensureDebugger().findAgain(s) }
    @objc func showRegisters(_ s: Any?) { let d = ensureDebugger(); d.showChipRegisters(s) }
    @objc func showBreakpoints(_ s: Any?) { let d = ensureDebugger(); d.show(); d.showBreakpoints(s) }
    @objc func showDataAreas(_ s: Any?) { let d = ensureDebugger(); d.show(); d.showDataAreas(s) }
    @objc func showTimers(_ s: Any?) { let d = ensureDebugger(); d.show(); d.showTimers(s) }
    @objc func showGraphics(_ s: Any?) { let d = ensureDebugger(); d.show(); d.showGraphics(s) }
    @objc func showAssembler(_ s: Any?) { todo(s) }
    @objc func showPokes(_ s: Any?) { if pokes == nil { pokes = PokesController(emu: emu) }; pokes?.show() }
    @objc func showHelp(_ s: Any?) { if help == nil { help = HelpController.make(winapeFolder: emu.romFolder.deletingLastPathComponent()) }; help?.show() }

    func validateMenuItem(_ item: NSMenuItem) -> Bool {
        if item.action == #selector(amxMouse(_:)) { item.state = emu.amxEnabled ? .on : .off }
        if item.action == #selector(recordSession(_:)) { item.title = emu.isRecording ? "Stop Recording…" : "Record Session"; return !emu.isReplaying }
        if item.action == #selector(playSession(_:)) { return !emu.isRecording }
        if item.action == #selector(goToTime(_:)) || item.action == #selector(stopPlayback(_:)) { return emu.isReplaying && !emu.isSearching }
        if item.action == #selector(debugStepBack(_:)) || item.action == #selector(debugStepBackOver(_:)) || item.action == #selector(debugReverseContinue(_:)) || item.action == #selector(debugRunBackToCursor(_:)) { return emu.replayActive && !emu.isSearching }
        if item.action == #selector(debugScreenView(_:)) { item.state = emu.screenView.rawValue == item.tag ? .on : .off }
        return true
    }

    // MARK: Menu construction
    private func fkey(_ n: Int) -> String { String(UnicodeScalar(UInt32(NSF1FunctionKey) + UInt32(n - 1))!) }

    private func item(_ title: String, _ sel: Selector?, _ key: String = "", _ mods: NSEvent.ModifierFlags = []) -> NSMenuItem {
        let i = NSMenuItem(title: title, action: sel, keyEquivalent: key)
        i.keyEquivalentModifierMask = mods
        return i
    }

    private func submenu(_ title: String, _ items: [NSMenuItem]) -> NSMenuItem {
        let m = NSMenu(title: title); items.forEach { m.addItem($0) }
        let i = NSMenuItem(title: title, action: nil, keyEquivalent: ""); i.submenu = m; return i
    }

    func buildMenus() {
        let bar = NSMenu()
        let ctrl: NSEvent.ModifierFlags = .control, shift: NSEvent.ModifierFlags = .shift
        let appMenu = submenu("MacAPE", [
            item("About MacAPE", #selector(NSApplication.orderFrontStandardAboutPanel(_:))),
            .separator(),
            item("Hide MacAPE", #selector(NSApplication.hide(_:)), "h"),
            .separator(),
            item("Quit MacAPE", #selector(NSApplication.terminate(_:)), "q"),
        ])
        func drive(_ n: String, _ f: Int, ins: Selector, new: Selector, rm: Selector) -> NSMenuItem {
            submenu("Drive \(n):", [
                item("Insert Disc Image…", ins, fkey(f), ctrl),
                item("New Blank Disc…", new),
                item("Format Disc Image…", #selector(todo(_:))),
                item("Edit Disc…", #selector(todo(_:)), fkey(f), [ctrl, shift]),
                item("Flip Disc", #selector(todo(_:)), fkey(f), shift),
                item("Remove Disc", rm),
            ])
        }
        let file = submenu("File", [
            drive("A", 1, ins: #selector(insertDiscA(_:)), new: #selector(newBlankDiscA(_:)), rm: #selector(removeDiscA(_:))),
            drive("B", 2, ins: #selector(insertDiscB(_:)), new: #selector(newBlankDiscB(_:)), rm: #selector(removeDiscB(_:))),
            item("Swap Discs A: and B:", #selector(swapDiscs(_:)), fkey(3), [ctrl, shift]),
            item("Drive Setup…", #selector(driveSetup(_:)), fkey(2)),
            item("Load Cartridge…", #selector(loadCartridge(_:)), fkey(3), ctrl),
            submenu("Tape", [
                item("Show Tape Control", #selector(showTape(_:))),
                item("Insert Tape Image…", #selector(insertTape(_:)), fkey(4), ctrl),
                item("Rewind Tape", #selector(rewindTape(_:))), item("Remove Tape", #selector(removeTape(_:))),
                item("Press Play", #selector(pressPlay(_:))), item("Press Record", #selector(pressRecord(_:))),
            ]),
            .separator(),
            item("Load Snapshot…", #selector(loadSnapshot(_:)), fkey(5)),
            item("Save Snapshot…", #selector(saveSnapshot(_:)), fkey(6)),
            item("Update Snapshot", #selector(updateSnapshot(_:)), fkey(6), ctrl),
            .separator(),
            item("Record Session", #selector(recordSession(_:))), item("Playback Session…", #selector(playSession(_:))),
            item("Go to Time…", #selector(goToTime(_:)), "g", [.command, .option]), item("Stop Playback", #selector(stopPlayback(_:))),
            .separator(),
            item("Save Screenshot…", #selector(saveScreenshot(_:)), fkey(7), ctrl),
            item("Record AVI…", #selector(todo(_:))), item("Record WAV…", #selector(todo(_:))), item("Record YM…", #selector(todo(_:))),
            .separator(),
            item("Pokes…", #selector(showPokes(_:)), fkey(8), ctrl),
            .separator(),
            item("Auto Type…", #selector(autoType(_:)), fkey(5), ctrl),
            item("Paste", #selector(pasteText(_:)), fkey(11), ctrl),
        ])
        let profiles = submenu("Profile", Profiles.list().map { item($0, #selector(selectProfile(_:))) })
        let settingsMenu = submenu("Settings", [
            item("General…", #selector(showGeneral(_:)), fkey(12)), item("Display…", #selector(showDisplay(_:))), item("Sound…", #selector(showSound(_:))),
            item("Memory…", #selector(showMemory(_:))), item("Input…", #selector(showInput(_:))), item("Other…", #selector(showOther(_:))),
            profiles,
            .separator(),
            item("Normal Speed (100%)", #selector(normalSpeed(_:)), fkey(3), shift), item("High Speed (1000%)", #selector(highSpeed(_:)), fkey(4), shift),
            .separator(),
            item("Full Screen", #selector(toggleFullScreenAction(_:)), fkey(10)),
            item("Window Size 1×", #selector(scale1x(_:)), "1", .command), item("Window Size 1.5×", #selector(scale15x(_:)), "2", .command), item("Window Size 2×", #selector(scale2x(_:)), "3", .command),
            .separator(),
            item("Reset", #selector(resetEmu(_:)), fkey(9), ctrl), item("Cold Reset", #selector(coldReset(_:)), fkey(9), [ctrl, shift]),
            item("AMX Mouse", #selector(amxMouse(_:)), fkey(12), ctrl), item("Multiface Stop", #selector(multifaceStop(_:)), fkey(11)),
        ])
        let debug = submenu("Debug", [
            item("Run", #selector(debugRun(_:)), fkey(9)), item("Pause", #selector(debugPause(_:)), fkey(7)), item("Step Into", #selector(debugStep(_:)), fkey(7), shift), item("Step Over", #selector(debugStepOver(_:)), fkey(8)), item("Step Out", #selector(debugStepOut(_:)), fkey(8), shift),
            item("Step Scanline", #selector(debugStepScanline(_:)), fkey(8), .option), item("Step Frame", #selector(debugStepFrame(_:)), fkey(8), [.option, shift]), item("Run to Cursor", #selector(debugRunToCursor(_:)), fkey(4)), item("Find Again", #selector(debugFindAgain(_:)), fkey(3)),
            item("Registers", #selector(showRegisters(_:))), item("Breakpoints", #selector(showBreakpoints(_:))),
            item("Data Areas", #selector(showDataAreas(_:))), item("Timers", #selector(showTimers(_:))), item("Find Graphics", #selector(showGraphics(_:))),
            .separator(),
            item("Reload Disc & Symbols", #selector(reloadDiscAndSymbols(_:)), "r", .command),
            .separator(),
            item("Step Back", #selector(debugStepBack(_:)), fkey(7), .option), item("Step Back Over", #selector(debugStepBackOver(_:)), fkey(8), [.option, .command]), item("Reverse Continue", #selector(debugReverseContinue(_:)), fkey(9), .option),
            item("Run Back to Cursor", #selector(debugRunBackToCursor(_:)), fkey(4), .option),
            .separator(),
            item("Execution History…", #selector(debugShowHistory(_:))),
            item("Watch…", #selector(debugShowWatch(_:))), item("Log…", #selector(debugShowLog(_:))),
            { let m = NSMenuItem(title: "Paused Screen", action: nil, keyEquivalent: ""); let sub = NSMenu(title: "Paused Screen")
              for (i, t) in ["Normal (last frame)", "Raster (beam position)", "Video Memory"].enumerated() { let it = NSMenuItem(title: t, action: #selector(debugScreenView(_:)), keyEquivalent: ""); it.tag = i; sub.addItem(it) }
              m.submenu = sub; return m }(),
            item("Symbols…", #selector(debugShowSymbols(_:))), item("Load Symbols…", #selector(debugLoadSymbols(_:))), item("Clear Symbols", #selector(debugClearSymbols(_:))),
        ])
        let asm = submenu("Assembler", [item("Show Assembler", #selector(showAssembler(_:)), fkey(3))])
        let window = submenu("Window", [item("Minimize", #selector(NSWindow.performMiniaturize(_:)), "m", .command)])
        let help = submenu("Help", [item("Contents", #selector(showHelp(_:)), fkey(1))])
        [appMenu, file, settingsMenu, debug, asm, window, help].forEach { bar.addItem($0) }
        NSApp.mainMenu = bar
        NSApp.windowsMenu = window.submenu
    }
}
