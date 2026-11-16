// swift-tools-version:5.7
import PackageDescription

let package = Package(
    name: "TMUtility",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "TMUtility", targets: ["TMUtility"]),
        .executable(name: "tmutil", targets: ["tmutil"]),
    ],
    targets: [
        // Protocol core (C) + USB transport (Objective-C, IOUSBHost).
        .target(
            name: "TMCore",
            path: "Sources/TMCore",          // SwiftPM builds .m files with ARC
            linkerSettings: [
                .linkedFramework("IOUSBHost"),
                .linkedFramework("IOKit"),
                .linkedFramework("Foundation"),
            ]
        ),
        // Command-line tool.
        .executableTarget(name: "tmutil", dependencies: ["TMCore"], path: "cli",
                         exclude: ["tmqueue.c"]),        // tmqueue: built by the Makefile (libcups)
        // SwiftUI app.
        .executableTarget(name: "TMUtility", dependencies: ["TMCore"], path: "Sources/TMUtility"),
    ]
)
