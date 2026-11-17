import GameController

/// Game controllers drive the CPC joystick, like the web build: left stick or D-pad for the
/// directions, A and B for fire 1 and fire 2. Several controllers are merged (OR of their buttons).
final class GamepadInput {
    private weak var emu: Emulator?
    private var masks: [ObjectIdentifier: UInt8] = [:]
    private var applied: UInt8 = 0
    private var observers: [NSObjectProtocol] = []
    private static let threshold: Float = 0.45

    init(emu: Emulator) {
        self.emu = emu
        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] n in
            if let c = n.object as? GCController { self?.attach(c) }
        })
        observers.append(nc.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] n in
            if let c = n.object as? GCController { self?.masks[ObjectIdentifier(c)] = nil; self?.sync() }
        })
        GCController.controllers().forEach(attach)
    }

    deinit { observers.forEach(NotificationCenter.default.removeObserver) }

    private func attach(_ c: GCController) {
        let id = ObjectIdentifier(c)
        if let g = c.extendedGamepad {
            g.valueChangedHandler = { [weak self] g, _ in
                var m: UInt8 = 0
                if g.leftThumbstick.yAxis.value > Self.threshold || g.dpad.up.isPressed { m |= 1 << 0 }
                if g.leftThumbstick.yAxis.value < -Self.threshold || g.dpad.down.isPressed { m |= 1 << 1 }
                if g.leftThumbstick.xAxis.value < -Self.threshold || g.dpad.left.isPressed { m |= 1 << 2 }
                if g.leftThumbstick.xAxis.value > Self.threshold || g.dpad.right.isPressed { m |= 1 << 3 }
                if g.buttonA.isPressed { m |= 1 << 4 }
                if g.buttonB.isPressed { m |= 1 << 5 }
                self?.masks[id] = m; self?.sync()
            }
        } else if let g = c.microGamepad {
            g.valueChangedHandler = { [weak self] g, _ in
                var m: UInt8 = 0
                if g.dpad.up.isPressed { m |= 1 << 0 }
                if g.dpad.down.isPressed { m |= 1 << 1 }
                if g.dpad.left.isPressed { m |= 1 << 2 }
                if g.dpad.right.isPressed { m |= 1 << 3 }
                if g.buttonA.isPressed { m |= 1 << 4 }
                if g.buttonX.isPressed { m |= 1 << 5 }
                self?.masks[id] = m; self?.sync()
            }
        }
    }

    /// Sends only the joystick bits that changed since the last call (bits: 0 up, 1 down, 2 left, 3 right, 4 fire 1, 5 fire 2).
    private func sync() {
        let now = masks.values.reduce(0 as UInt8, |)
        let diff = now ^ applied
        guard diff != 0, let emu = emu else { return }
        for bit in 0..<6 where diff & (1 << UInt8(bit)) != 0 { emu.joystick(bit, down: now & (1 << UInt8(bit)) != 0) }
        applied = now
    }
}
