import Foundation

/// Built-in configuration profiles (same layout as WinAPE .wpf files).
enum Profiles {
    static let builtIn: [String: String] = [
        "CPC464": "[ROMS]\nLower=OS464\nUpper(0)=BASIC1-0\nUpper(7)=\nCartridge Enabled=false\n[Configuration]\nCRTC Type=0\nEnable Plus=false\nExtended RAM=0\nSilicon Disc=false\n",
        "CPC464 with ParaDOS": "[ROMS]\nLower=OS464\nUpper(0)=BASIC1-0\nUpper(7)=ParaDOS 1-2\nCartridge Enabled=false\n[Configuration]\nCRTC Type=0\nEnable Plus=false\nExtended RAM=0\nSilicon Disc=false\n",
        "CPC6128": "[ROMS]\nLower=OS6128\nUpper(0)=BASIC1-1\nUpper(7)=AMSDOS\nCartridge Enabled=false\n[Configuration]\nCRTC Type=0\nEnable Plus=false\nExtended RAM=1\nSilicon Disc=false\n",
        "CPC6128 with ParaDOS": "[ROMS]\nLower=OS6128\nUpper(0)=BASIC1-1\nUpper(7)=ParaDOS 1-2\nCartridge Enabled=false\n[Configuration]\nCRTC Type=0\nEnable Plus=false\nExtended RAM=1\nSilicon Disc=false\n",
        "464 Plus": "[ROMS]\nCartridge=cpc_plus.cpr\nLower=\nUpper(0)=\nUpper(7)=\nCartridge Enabled=true\n[Configuration]\nCRTC Type=3\nEnable Plus=true\nPlus PPI=true\nExtended RAM=0\nSilicon Disc=false\n",
        "464 Plus with ParaDOS": "[ROMS]\nCartridge=cpc_plus.cpr\nLower=\nUpper(0)=\nUpper(7)=ParaDOS 1-2+\nCartridge Enabled=true\n[Configuration]\nCRTC Type=3\nEnable Plus=true\nPlus PPI=true\nExtended RAM=0\nSilicon Disc=false\n",
        "6128 Plus": "[ROMS]\nCartridge=cpc_plus.cpr\nLower=\nUpper(0)=\nUpper(7)=\nCartridge Enabled=true\n[Configuration]\nCRTC Type=3\nEnable Plus=true\nPlus PPI=true\nExtended RAM=1\nSilicon Disc=false\n",
        "6128 Plus with ParaDOS": "[ROMS]\nCartridge=cpc_plus.cpr\nLower=\nUpper(0)=\nUpper(7)=ParaDOS 1-2+\nCartridge Enabled=true\n[Configuration]\nCRTC Type=3\nEnable Plus=true\nPlus PPI=true\nExtended RAM=1\nSilicon Disc=false\n",
    ]

    static func installDefaults() {
        let fm = FileManager.default
        try? fm.createDirectory(at: AppPaths.profiles, withIntermediateDirectories: true)
        for (name, text) in builtIn {
            let u = AppPaths.profiles.appendingPathComponent(name + ".wpf")
            if !fm.fileExists(atPath: u.path) { try? text.write(to: u, atomically: true, encoding: .utf8) }
        }
    }

    static func list() -> [String] {
        let items = (try? FileManager.default.contentsOfDirectory(atPath: AppPaths.profiles.path)) ?? []
        return items.filter { $0.lowercased().hasSuffix(".wpf") }.map { String($0.dropLast(4)) }.sorted()
    }

    static func load(_ name: String) -> Settings? { Settings.load(AppPaths.profiles.appendingPathComponent(name + ".wpf")) }
}
