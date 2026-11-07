// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "BluePixel",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "BluePixel", targets: ["BluePixel"]),
        .executable(name: "cpcrun", targets: ["cpcrun"]),
        .executable(name: "bluepixel-headless", targets: ["bluepixel-headless"]),
        .library(name: "CPCApi", targets: ["CPCApi"]),
        .library(name: "CPCServer", targets: ["CPCServer"]),
    ],
    targets: [
        .target(name: "CPCCore", path: "Sources/CPCCore", cSettings: [.unsafeFlags(["-O2"])], linkerSettings: [.linkedLibrary("z")]),
        .target(name: "CPCApi", dependencies: ["CPCCore"], path: "Sources/CPCApi", publicHeadersPath: "include"),
        .target(name: "CPCServer", dependencies: ["CPCApi"], path: "Sources/CPCServer", publicHeadersPath: "include"),
        .target(name: "CPCHost", dependencies: ["CPCCore", "CPCApi"], path: "Sources/CPCHost", publicHeadersPath: "include"),
        .executableTarget(name: "cpcrun", dependencies: ["CPCCore", "CPCHost"], path: "Sources/cpcrun"),
        .executableTarget(name: "bluepixel-headless", dependencies: ["CPCApi", "CPCServer", "CPCHost"], path: "Sources/bluepixel-headless"),
        .executableTarget(name: "BluePixel", dependencies: ["CPCCore", "CPCHost"], path: "Sources/BluePixel"),
    ]
)
