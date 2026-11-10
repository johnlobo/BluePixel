import Foundation
import CPCCore
import CPCApi
import CPCServer
import Darwin

/// A borrowed API session over the displayed machine. All dispatch runs on Emulator's thread.
final class DesktopControlServer {
    private let emu: Emulator
    private var session: OpaquePointer?
    private var server: OpaquePointer?
    private let sessionLock = NSLock()
    let socketPath: String
    let tokenPath: String
    private(set) var port = -1

    init(emulator: Emulator, options: LaunchOptions) throws {
        emu = emulator
        guard !emu.isRecording && !emu.isReplaying && !emu.isSearching else {
            throw Self.error("Stop session recording or playback before enabling the control server.")
        }
        let directory = AppPaths.ini.deletingLastPathComponent().appendingPathComponent("Control")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        let attributes = try FileManager.default.attributesOfItem(atPath: directory.path)
        guard attributes[.type] as? FileAttributeType == .typeDirectory,
              (attributes[.ownerAccountID] as? NSNumber)?.uint32Value == geteuid() else {
            throw Self.error("The Control directory must be owned by the current user and must not be a symlink.")
        }
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: directory.path)
        socketPath = options.controlUnix ?? directory.appendingPathComponent("control.sock").path
        tokenPath = options.controlTokenFile ?? directory.appendingPathComponent("token").path
        var token = [CChar](repeating: 0, count: 65), error = [CChar](repeating: 0, count: 512)
        guard cpc_server_token_file(tokenPath, &token, &error, error.count) == 0 else { throw Self.error(String(cString: error)) }
        // Stop the audio consumer before remote audio.read/state operations share the same machine.
        emu.onEmulationThread { self.emu.paused = true }
        emu.audio?.stop()
        session = emu.onEmulationThread {
            self.emu.lock.lock(); defer { self.emu.lock.unlock() }
            let session = cpc_api_borrow(self.emu.cpc)
            cpc_api_set_transports(session, CPC_API_UNIX | CPC_API_WEBSOCKET)
            cpc_api_set_yield(session, { context in
                guard let context = context else { return }
                let emulator = Unmanaged<Emulator>.fromOpaque(context).takeUnretainedValue()
                emulator.lock.unlock(); usleep(1); emulator.lock.lock()
            }, Unmanaged.passUnretained(self.emu).toOpaque())
            return session
        }
        guard session != nil else { emu.audio?.start(); throw Self.error("Could not create the desktop API session.") }
        let origins = options.controlOrigins.joined(separator: ",")
        server = socketPath.withCString { socket in
            token.withUnsafeBufferPointer { secret in
                origins.withCString { origin in
                    var config = CPCServerConfig(unix_path: socket, websocket_port: Int32(options.controlPort ?? 0),
                                                 token: secret.baseAddress, allowed_origins: origin,
                                                 dispatch: { context, request in
                        guard let context = context, let request = request else { return nil }
                        let owner = Unmanaged<DesktopControlServer>.fromOpaque(context).takeUnretainedValue()
                        return owner.dispatch(String(cString: request))
                    }, context: Unmanaged.passUnretained(self).toOpaque(), cancel: { context in
                        guard let context = context else { return }
                        let owner = Unmanaged<DesktopControlServer>.fromOpaque(context).takeUnretainedValue()
                        owner.cancelExecution()
                    })
                    return cpc_server_start(&config, &error, error.count)
                }
            }
        }
        guard let server = server else {
            cpc_api_destroy(session); session = nil; emu.audio?.start()
            throw Self.error(String(cString: error))
        }
        port = Int(cpc_server_websocket_port(server))
    }

    private static func error(_ text: String) -> NSError {
        NSError(domain: "BluePixel.ControlServer", code: 1, userInfo: [NSLocalizedDescriptionKey: text])
    }

    private func dispatch(_ request: String) -> UnsafeMutablePointer<CChar>? {
        emu.onEmulationThread {
            self.emu.lock.lock()
            self.emu.paused = true
            let object = request.data(using: .utf8).flatMap { try? JSONSerialization.jsonObject(with: $0) } as? [String: Any]
            let method = object?["method"] as? String ?? ""
            // Flush user-owned media before CPCApi replaces and frees a mounted disc.
            if method == "disc.load", let params = object?["params"] as? [String: Any], let drive = params["drive"] as? Int, (0...1).contains(drive) {
                self.emu.commitDisc(drive: drive)
            }
            self.emu.noteUnreproducible("the local control API was used")
            let reply = cpc_api_dispatch(self.session, request)
            for drive in 0..<4 {
                let mounted = fdc_disc(self.emu.cpc.pointee.fdc, Int32(drive))
                if mounted != self.emu.discs[drive] {
                    self.emu.discs[drive] = mounted
                    self.emu.discPaths[drive] = ""
                    self.emu.settings.set("Drives", "Drive(\(drive))", "")
                    mounted?.pointee.temp_writes = true
                }
            }
            let stopped = cpc_dbg_break(self.emu.cpc)
            self.emu.lock.unlock()
            self.emu.publish()
            if stopped { DispatchQueue.main.async { self.emu.onBreak?() } }
            return reply
        }
    }

    func stop() {
        guard let active = server else { return }
        // Emulator's thread stays alive until all queued callbacks have returned.
        cpc_server_stop(active); server = nil
        emu.onEmulationThread {
            self.sessionLock.lock(); defer { self.sessionLock.unlock() }
            cpc_api_destroy(self.session); self.session = nil
        }
        emu.audio?.start()
    }
    func cancelExecution() {
        sessionLock.lock(); defer { sessionLock.unlock() }
        cpc_api_cancel(session)
    }
    deinit { stop() }
}
