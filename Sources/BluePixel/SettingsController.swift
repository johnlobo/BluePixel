import AppKit
import CPCCore
import UniformTypeIdentifiers

/// WinAPE-style settings window: General, Display, Sound, Memory, Input, Other + profiles.
final class SettingsController: NSObject, NSWindowDelegate {
    let emu: Emulator
    let panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 640, height: 560), styleMask: [.titled, .closable], backing: .buffered, defer: false)
    private let tabs = NSTabView()
    private var loaders: [() -> Void] = [], savers: [() -> Void] = []
    private let profilePopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private var romPopups: [NSPopUpButton] = []
    private var romNames: [String] = []
    private var pendingTab: String?
    var onApply: (() -> Void)?
    var currentProfile = "CPC6128"
    private var keyEditor: KeyMapEditor?

    var s: Settings { emu.settings }

    init(emu: Emulator) {
        self.emu = emu
        super.init()
        panel.title = "BluePixel - Setup"; panel.isReleasedWhenClosed = false; panel.delegate = self
        build()
    }

    func show(tab: String? = nil) {
        reloadProfiles(); reloadROMLists(); loaders.forEach { $0() }
        if let t = tab, let i = tabs.tabViewItems.firstIndex(where: { $0.label == t }) { tabs.selectTabViewItem(at: i) }
        panel.center(); panel.makeKeyAndOrderFront(nil)
    }

    // MARK: binding helpers
    private func check(_ title: String, _ sec: String, _ key: String, _ def: Bool) -> NSButton {
        let b = NSButton(checkboxWithTitle: title, target: nil, action: nil)
        loaders.append { [unowned self] in b.state = self.s.bool(sec, key, def) ? .on : .off }
        savers.append { [unowned self] in self.s.set(sec, key, b.state == .on) }
        return b
    }
    private func popup(_ items: [String], _ sec: String, _ key: String, _ def: Int) -> NSPopUpButton {
        let p = NSPopUpButton(frame: .zero, pullsDown: false); p.addItems(withTitles: items)
        loaders.append { [unowned self] in p.selectItem(at: max(0, min(items.count - 1, self.s.int(sec, key, def)))) }
        savers.append { [unowned self] in self.s.set(sec, key, p.indexOfSelectedItem) }
        return p
    }
    private func slider(_ min: Double, _ max: Double, _ sec: String, _ key: String, _ def: Int, label: NSTextField? = nil, format: String = "%d") -> NSSlider {
        let sl = NSSlider(value: Double(def), minValue: min, maxValue: max, target: nil, action: nil)
        let upd: () -> Void = { [unowned sl] in label?.stringValue = String(format: format, Int(sl.doubleValue)) }
        class Hook: NSObject { let f: () -> Void; init(_ f: @escaping () -> Void) { self.f = f }; @objc func go() { f() } }
        let h = Hook(upd); sl.target = h; sl.action = #selector(Hook.go); objc_setAssociatedObject(sl, "hook", h, .OBJC_ASSOCIATION_RETAIN)
        loaders.append { [unowned self] in sl.doubleValue = Double(self.s.int(sec, key, def)); upd() }
        savers.append { [unowned self] in self.s.set(sec, key, Int(sl.doubleValue)) }
        return sl
    }
    private func text(_ sec: String, _ key: String, _ def: String = "", width: CGFloat = 200) -> NSTextField {
        let t = NSTextField(string: def); t.widthAnchor.constraint(equalToConstant: width).isActive = true
        loaders.append { [unowned self] in t.stringValue = self.s.string(sec, key, def) }
        savers.append { [unowned self] in self.s.set(sec, key, t.stringValue) }
        return t
    }
    private func column(_ views: [NSView]) -> NSStackView { let v = NSStackView(views: views); v.orientation = .vertical; v.alignment = .leading; v.spacing = 8; return v }
    private func row(_ views: [NSView]) -> NSStackView { let v = NSStackView(views: views); v.orientation = .horizontal; v.alignment = .centerY; v.spacing = 8; return v }
    private func group(_ title: String, _ views: [NSView]) -> NSView {
        let box = NSBox(); box.title = title
        let c = column(views); c.translatesAutoresizingMaskIntoConstraints = false
        let inner = box.contentView!; inner.addSubview(c)
        NSLayoutConstraint.activate([c.leadingAnchor.constraint(equalTo: inner.leadingAnchor, constant: 8), c.trailingAnchor.constraint(equalTo: inner.trailingAnchor, constant: -8),
                                     c.topAnchor.constraint(equalTo: inner.topAnchor, constant: 6), c.bottomAnchor.constraint(equalTo: inner.bottomAnchor, constant: -6)])
        return box
    }
    private func tab(_ name: String, _ content: NSView) {
        let item = NSTabViewItem(identifier: name); item.label = name
        let wrapper = NSView(); content.translatesAutoresizingMaskIntoConstraints = false; wrapper.addSubview(content)
        NSLayoutConstraint.activate([content.leadingAnchor.constraint(equalTo: wrapper.leadingAnchor, constant: 12), content.topAnchor.constraint(equalTo: wrapper.topAnchor, constant: 12),
                                     content.trailingAnchor.constraint(lessThanOrEqualTo: wrapper.trailingAnchor, constant: -12)])
        item.view = wrapper; tabs.addTabViewItem(item)
    }

    // MARK: build
    private func build() {
        let top = row([makeLabel("Profile:"), profilePopup, makeButton("Load", self, #selector(loadProfile(_:))), makeButton("Save", self, #selector(saveProfile(_:)))])
        profilePopup.target = self; profilePopup.action = #selector(profileChosen(_:))

        // General
        let speedLabel = makeLabel("100%")
        let speed = slider(5, 1000, "Configuration", "Emulation Speed", 100, label: speedLabel, format: "%d%%")
        speed.widthAnchor.constraint(equalToConstant: 260).isActive = true
        let every = check("Display Every", "Configuration", "Frame Skip", false)
        let skipField = text("Configuration", "Refresh Rate", "0", width: 50)
        let timing = group("Timing", [row([makeLabel("Speed"), speed, speedLabel]), row([every, skipField, makeLabel("frame(s)")]), check("Turbo Mode", "Configuration", "Turbo Mode", false)])
        let crtc = popup(["Type 0 (HD6845S/UM6845)", "Type 1 (UM6845R)", "Type 2 (MC6845)", "Type 3 (AMS40489 ASIC)", "Type 4 (AMS40226 pre-ASIC)"], "Configuration", "CRTC Type", 0)
        let options = group("Options", [row([makeLabel("CRTC Type:"), crtc]), check("Enable Plus Features", "Configuration", "Enable Plus", false), check("Enable Four Drives (Non-Standard)", "Configuration", "Enable Four Drives", false),
                                        check("Save Screenshot on Frame Flyback", "Configuration", "Screenshot On Flyback", true), check("Plus PPI Emulation", "Configuration", "Plus PPI", false),
                                        check("Fast Disc Emulation", "Configuration", "Fast Disc", true)])
        tab("General", column([timing, options, makeLabel("BluePixel 0.1 — Mac port of WinAPE features")]))

        // Display
        let monitor = popup(["Colour", "Green", "Greyscale"], "Configuration", "Monitor Type", 0)
        let vhold = slider(-20, 20, "Configuration", "VHOLD Position", 0), bright = slider(-50, 50, "Configuration", "Monitor Brightness", 0)
        vhold.widthAnchor.constraint(equalToConstant: 160).isActive = true; bright.widthAnchor.constraint(equalToConstant: 160).isActive = true
        func winOpts(_ suffix: String, _ title: String) -> NSView {
            group(title, [check("Half Size Display", "Configuration", "Half Size" + suffix, false), check("Render both lines", "Configuration", "Render Both" + suffix, true),
                          check("Hide Mouse Pointer", "Configuration", "Hide Mouse" + suffix, suffix.isEmpty ? true : true), check("Hide Control Panel", "Configuration", "Hide Panel" + suffix, false),
                          check("Hide Menus", "Configuration", "Hide Menus" + suffix, false), check("No Right-Click Menu", "Configuration", "No Right Click" + suffix, false)])
        }
        let displayCol1 = column([group("Monitor", [row([makeLabel("Type"), monitor]), row([makeLabel("V Hold"), vhold]), row([makeLabel("Brightness"), bright])]),
                                  group("Rendering", [check("PAL Emulation", "Configuration", "PAL Emulation", true), check("Linear Palette", "Configuration", "Linear Palette", false),
                                                      check("On-Screen Drive LED", "Configuration", "On Screen Drive LED", false), check("Show Drive Cylinders", "Configuration", "Show Track", false)])])
        let displayRow = row([displayCol1, winOpts("", "Windowed"), winOpts(" FS", "Full Screen")]); displayRow.alignment = .top
        tab("Display", displayRow)

        // Sound
        let output = popup(["None", "Speaker", "CoreAudio"], "Configuration", "Sound Mode", 2)
        let rate = popup(["22 kHz", "44 kHz"], "Configuration", "Sample Rate Index", 1)
        let bits = popup(["8 bit", "16 bit"], "Configuration", "Bits Index", 1)
        let chans = popup(["Mono", "Stereo"], "Configuration", "Sound Stereo Index", 1)
        let volLabel = makeLabel("15"), vol = slider(0, 15, "Configuration", "Sound Volume", 15, label: volLabel)
        vol.widthAnchor.constraint(equalToConstant: 200).isActive = true
        tab("Sound", column([group("Sound Output", [row([makeLabel("Output"), output]), row([makeLabel("Sample Rate"), rate]), row([makeLabel("Bits Per Sample"), bits]), row([makeLabel("Channels"), chans]), row([makeLabel("Volume"), vol, volLabel])]),
                             group("Other Sounds", [check("Disc Drive Sounds", "Configuration", "Disc Sound", true), check("Tape Loading Sounds", "Configuration", "Tape Sound", true), check("AmDrum", "Configuration", "AmDrum Enabled", true)])]))
        // keep the legacy keys in sync
        savers.append { [unowned self] in self.s.set("Configuration", "Sound Stereo", chans.indexOfSelectedItem == 1) }
        loaders.append { [unowned self] in chans.selectItem(at: self.s.bool("Configuration", "Sound Stereo", true) ? 1 : 0); rate.selectItem(at: self.s.int("Configuration", "Sound Mode", 2) == 2 ? 1 : 0) }

        // Memory
        let ram = popup(["64K", "128K", "64K + 256K RAM Expansion", "4M Expansion"], "Configuration", "Extended RAM", 1)
        let romList = NSStackView(); romList.orientation = .vertical; romList.alignment = .leading; romList.spacing = 2
        for i in -1..<32 {
            let p = NSPopUpButton(frame: .zero, pullsDown: false); p.controlSize = .small; p.font = .systemFont(ofSize: 11); p.tag = i
            p.target = self; p.action = #selector(romChosen(_:)); p.widthAnchor.constraint(equalToConstant: 200).isActive = true
            romPopups.append(p)
            let l = makeLabel(i < 0 ? "Lower" : "Upper \(i)"); l.font = .systemFont(ofSize: 11); l.widthAnchor.constraint(equalToConstant: 56).isActive = true
            romList.addArrangedSubview(row([l, p]))
        }
        let scroll = NSScrollView(); scroll.documentView = romList; scroll.hasVerticalScroller = true; scroll.drawsBackground = false
        scroll.translatesAutoresizingMaskIntoConstraints = false; scroll.widthAnchor.constraint(equalToConstant: 300).isActive = true; scroll.heightAnchor.constraint(equalToConstant: 270).isActive = true
        romList.translatesAutoresizingMaskIntoConstraints = false
        let clip = FlippedClipView(); clip.drawsBackground = false; scroll.contentView = clip; scroll.documentView = romList
        NSLayoutConstraint.activate([romList.leadingAnchor.constraint(equalTo: clip.leadingAnchor), romList.topAnchor.constraint(equalTo: clip.topAnchor),
                                     romList.widthAnchor.constraint(equalTo: clip.widthAnchor)])
        let silicon = check("256K Silicon Disc", "Configuration", "Silicon Disc", false)
        let romButtons = column([check("Enable 32 ROMs", "Configuration", "Enable 32 ROMs", true), check("Disable All ROMs", "ROMS", "Disable All", false), makeButton("Disable all but Lower, 0 and 7", self, #selector(disableAllButStandard(_:)))])
        let cartField = text("ROMS", "Cartridge", "", width: 200)
        let mfField = text("ROMS", "Multiface", "", width: 200)
        let memLeft = column([group("RAM", [ram, silicon]), group("Multiface", [row([makeLabel("ROM:"), mfField]), check("Enable Multiface", "ROMS", "Multiface Enabled", true)]),
                              group("Cartridge", [row([makeLabel("File:"), cartField]), check("Enable Cartridge", "ROMS", "Cartridge Enabled", false)])])
        let memRow = row([memLeft, group("ROMs", [scroll, romButtons])]); memRow.alignment = .top
        tab("Memory", memRow)

        // Input
        let keyButton = makeButton("Edit Keyboard Mapping…", self, #selector(editKeys(_:)))
        tab("Input", column([group("Joystick", [check("Enable Joystick", "Configuration", "Joystick Enabled", true), check("Enable AMX Mouse", "Configuration", "AMX Mouse Enabled", false)]),
                             group("Keyboard", [keyButton, makeLabel("Keypad: with NumLock on the keypad maps to F0–F9; press Clear to toggle joystick mode.")])]))

        // Other
        let printer = popup(["Disabled", "File", "Assembler Tab", "Digiblaster"], "Configuration", "Printer Mode", 0)
        savers.append { [unowned self] in self.s.set("Configuration", "Printer Mode", printer.indexOfSelectedItem) }
        tab("Other", column([group("Symbiface II", [check("Enable IDE Devices", "Configuration", "Enable IDE", true), check("Enable Real-Time Clock", "Configuration", "Enable Clock", true),
                                                      check("Enable PS/2 Mouse", "Configuration", "Enable Mouse", true), check("Adjust Mouse for Screen Mode", "Configuration", "Adjust Mouse", true),
                                                      row([makeLabel("Master"), text("Configuration", "Master IDE Path", "")]), row([makeLabel("Slave"), text("Configuration", "Slave IDE Path", "")])]),
                             group("Printer", [printer]), group("Other Devices", [row([makeLabel("Dobbertin SmartWatch ROM Select:"), text("Configuration", "SmartWatch ROM", "-1", width: 50)])])]))

        let ok = makeButton("OK", self, #selector(ok(_:))); ok.keyEquivalent = "\r"
        let apply = makeButton("Apply", self, #selector(apply(_:)))
        let cancel = makeButton("Cancel", self, #selector(cancel(_:)))
        let buttons = row([NSView(), cancel, apply, ok])
        let root = column([top, tabs, buttons]); root.alignment = .width
        fill(panel, with: root, inset: 12)
        tabs.translatesAutoresizingMaskIntoConstraints = false
        tabs.heightAnchor.constraint(greaterThanOrEqualToConstant: 440).isActive = true
    }

    // MARK: ROM lists
    private func reloadROMLists() {
        var names: [String] = []
        if let items = try? FileManager.default.contentsOfDirectory(atPath: emu.romFolder.path) {
            names = items.filter { let l = $0.lowercased(); return l.hasSuffix(".rom") || l.hasSuffix(".bin") }.map { String($0.dropLast(4)) }.sorted { $0.lowercased() < $1.lowercased() }
        }
        romNames = names
        for p in romPopups {
            let key = p.tag < 0 ? "Lower" : "Upper(\(p.tag))"
            let cur = s.string("ROMS", key)
            p.removeAllItems(); p.addItem(withTitle: "(none)"); p.addItems(withTitles: names)
            if !cur.isEmpty && !names.contains(cur) { p.addItem(withTitle: cur) }
            p.menu?.addItem(.separator()); p.addItem(withTitle: "Select a file…")
            p.selectItem(withTitle: cur.isEmpty ? "(none)" : cur)
        }
    }
    @objc private func romChosen(_ p: NSPopUpButton) {
        let key = p.tag < 0 ? "Lower" : "Upper(\(p.tag))"
        if p.titleOfSelectedItem == "Select a file…" {
            let o = NSOpenPanel()
            if o.runModal() == .OK, let u = o.url { s.set("ROMS", key, u.path); reloadROMLists() } else { reloadROMLists() }
        } else { s.set("ROMS", key, p.titleOfSelectedItem == "(none)" ? "" : (p.titleOfSelectedItem ?? "")) }
    }
    @objc private func disableAllButStandard(_ sender: Any?) {
        for i in 0..<32 where i != 0 && i != 7 { s.set("ROMS", "Upper(\(i))", "") }
        reloadROMLists()
    }

    // MARK: profiles
    private func reloadProfiles() {
        profilePopup.removeAllItems(); profilePopup.addItems(withTitles: Profiles.list()); profilePopup.menu?.addItem(.separator()); profilePopup.addItem(withTitle: "Select a file…")
        profilePopup.selectItem(withTitle: currentProfile)
    }
    @objc private func profileChosen(_ s: Any?) {
        if profilePopup.titleOfSelectedItem == "Select a file…" {
            let o = NSOpenPanel(); o.allowedContentTypes = [UTType(filenameExtension: "wpf")].compactMap { $0 }
            if o.runModal() == .OK, let u = o.url, let p = Settings.load(u) { self.s.merge(p); currentProfile = u.deletingPathExtension().lastPathComponent }
            reloadProfiles(); reloadROMLists(); loaders.forEach { $0() }
        }
    }
    @objc private func loadProfile(_ sender: Any?) {
        guard let n = profilePopup.titleOfSelectedItem, let p = Profiles.load(n) else { return }
        s.merge(p); currentProfile = n; reloadROMLists(); loaders.forEach { $0() }
    }
    @objc private func saveProfile(_ sender: Any?) {
        guard let name = promptText("Save Profile", "Profile name:", currentProfile), !name.isEmpty else { return }
        savers.forEach { $0() }
        let out = Settings()
        for sec in ["ROMS", "Configuration"] { for k in s.keys(sec) { out[sec, k] = s[sec, k] } }
        out.save(AppPaths.profiles.appendingPathComponent(name + ".wpf"))
        currentProfile = name; reloadProfiles()
    }

    @objc private func editKeys(_ sender: Any?) { if keyEditor == nil { keyEditor = KeyMapEditor(settings: s) }; keyEditor?.show() }

    // MARK: buttons
    private func commit() {
        savers.forEach { $0() }
        s.set("BluePixel", "Profile", currentProfile)
        emu.settings.set("Configuration", "Frame Skip", s.int("Configuration", "Refresh Rate", 0) > 0)
        onApply?()
    }
    @objc private func ok(_ sender: Any?) { commit(); panel.orderOut(nil) }
    @objc private func apply(_ sender: Any?) { commit() }
    @objc private func cancel(_ sender: Any?) { panel.orderOut(nil) }
}

/// Clip view with a top-left origin so auto-layout document views start at the top.
final class FlippedClipView: NSClipView { override var isFlipped: Bool { true } }

// MARK: - Keyboard mapping editor
final class KeyMapEditor: NSObject, NSTableViewDataSource, NSTableViewDelegate {
    let settings: Settings
    let panel = makePanel("Keyboard Mapping", width: 420, height: 480)
    private let table = NSTableView()
    private var capturing = -1
    private var monitor: Any?
    private var entries: [(name: String, key: Int)] = []

    init(settings: Settings) {
        self.settings = settings
        super.init()
        entries = KeyMap.cpcKeyNames.sorted { $0.value < $1.value }.map { ($0.key, $0.value) }
        for (i, id) in ["cpc", "pc"].enumerated() { let c = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id)); c.title = i == 0 ? "CPC key" : "Mac key (double-click, then press a key)"; c.width = i == 0 ? 120 : 260; table.addTableColumn(c) }
        table.dataSource = self; table.delegate = self; table.target = self; table.doubleAction = #selector(beginCapture(_:))
        let scroll = NSScrollView(); scroll.documentView = table; scroll.hasVerticalScroller = true
        let reset = makeButton("Restore Defaults", self, #selector(restore(_:)))
        let all = stack([scroll, reset]); all.alignment = .width
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 360).isActive = true
        fill(panel, with: all)
    }
    func show() { table.reloadData(); panel.makeKeyAndOrderFront(nil) }
    func numberOfRows(in tableView: NSTableView) -> Int { entries.count }
    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let e = entries[row]
        let t = tableColumn?.identifier.rawValue == "cpc" ? e.name : (row == capturing ? "Press a key…" : KeyMap.keyName(KeyMap.codes(for: e.key, settings: settings)))
        let tf = makeLabel(t); tf.font = .monospacedSystemFont(ofSize: 11, weight: .regular); return tf
    }
    @objc func beginCapture(_ s: Any?) {
        let r = table.clickedRow; guard r >= 0 else { return }
        capturing = r; table.reloadData()
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown]) { [weak self] ev in
            guard let self = self, self.capturing >= 0 else { return ev }
            self.settings["Keyboard", "\(self.entries[self.capturing].key)"] = String(ev.keyCode)
            self.capturing = -1; self.table.reloadData()
            if let m = self.monitor { NSEvent.removeMonitor(m); self.monitor = nil }
            return nil
        }
    }
    @objc func restore(_ s: Any?) { for e in entries { settings["Keyboard", "\(e.key)"] = nil }; table.reloadData() }
}
