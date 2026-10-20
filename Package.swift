// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "MacAPE",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "MacAPE", targets: ["MacAPE"]),
        .executable(name: "cpcrun", targets: ["cpcrun"]),
    ],
    targets: [
        .target(name: "CPCCore", path: "Sources/CPCCore", cSettings: [.unsafeFlags(["-O2"])], linkerSettings: [.linkedLibrary("z")]),
        .executableTarget(name: "cpcrun", dependencies: ["CPCCore"], path: "Sources/cpcrun"),
        .executableTarget(name: "MacAPE", dependencies: ["CPCCore"], path: "Sources/MacAPE"),
    ]
)
