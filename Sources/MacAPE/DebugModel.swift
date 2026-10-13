import Foundation
import CPCCore

/// Thread-safe accessors to the emulated machine for the debugger UI.
final class DebugModel {
    let emu: Emulator
    var view = DbgView(mode: 0, lower: true, upper: true, upper_rom: 0, ram_cfg: 0xC0)
    var followMachine = true       // while true, `view` mirrors the live mapping

    init(emu: Emulator) { self.emu = emu }

    var cpc: UnsafeMutablePointer<CPC> { emu.cpc }

    func sync() {
        guard followMachine else { return }
        emu.lock.lock(); var v = view; dbg_view_current(cpc, &v); emu.lock.unlock()
        let mode = view.mode; view = v; view.mode = mode
    }

    func read(_ addr: Int) -> UInt8 {
        emu.lock.lock(); defer { emu.lock.unlock() }
        return dbg_view_read(cpc, &view, UInt16(truncatingIfNeeded: addr))
    }
    func write(_ addr: Int, _ value: UInt8) {
        emu.lock.lock(); dbg_view_write(cpc, &view, UInt16(truncatingIfNeeded: addr), value); emu.lock.unlock()
    }
    func readBlock(_ addr: Int, _ n: Int) -> [UInt8] {
        emu.lock.lock(); defer { emu.lock.unlock() }
        return (0..<n).map { dbg_view_read(cpc, &view, UInt16(truncatingIfNeeded: addr + $0)) }
    }

    /// Disassembles one line; returns (text, length).
    func disasm(_ addr: Int) -> (String, Int) {
        emu.lock.lock(); defer { emu.lock.unlock() }
        var buf = [CChar](repeating: 0, count: 80)
        let n = dbg_disasm(cpc, &view, UInt16(truncatingIfNeeded: addr), &buf, 80)
        return (String(cString: buf), Int(n))
    }

    func symbol(for addr: Int) -> String? {
        emu.lock.lock(); defer { emu.lock.unlock() }
        guard let d = cpc.pointee.dbg, let p = dbg_symbol_for_address(d, UInt16(truncatingIfNeeded: addr)) else { return nil }
        return String(cString: p)
    }

    func describe(_ addr: Int) -> String {
        emu.lock.lock(); defer { emu.lock.unlock() }
        var buf = [CChar](repeating: 0, count: 40)
        dbg_describe_address(cpc, UInt16(truncatingIfNeeded: addr), &buf, 40)
        return String(cString: buf)
    }

    func eval(_ expr: String) -> (Int32?, String) {
        emu.lock.lock(); defer { emu.lock.unlock() }
        var v: Int32 = 0; var err = [CChar](repeating: 0, count: 128)
        let ok = dbg_eval(cpc, expr, &v, &err, 128)
        return (ok ? v : nil, String(cString: err))
    }

    var pc: Int { Int(cpc.pointee.z80.pc) }

    func hasBreakpoint(at addr: Int) -> Bool {
        emu.lock.lock(); defer { emu.lock.unlock() }
        guard let d = cpc.pointee.dbg else { return false }
        let blk = cpc.pointee.rd_map.0
        _ = blk
        return withUnsafePointer(to: &d.pointee.bp) { bp -> Bool in
            bp.withMemoryRebound(to: DbgBreakpoint.self, capacity: Int(DBG_MAX_BP)) { arr in
                for i in 0..<Int(DBG_MAX_BP) where arr[i].used && arr[i].type == BP_CODE && Int(arr[i].start) == addr { return true }
                return false
            }
        }
    }

    func toggleBreakpoint(at addr: Int) { emu.lock.lock(); dbg_toggle_code_bp(cpc, UInt16(truncatingIfNeeded: addr)); emu.lock.unlock() }

    /// Start address of the instruction preceding `addr` (heuristic).
    func previousInstruction(_ addr: Int) -> Int {
        for k in stride(from: 4, through: 1, by: -1) {
            if disasm(addr - k).1 == k { return (addr - k) & 0xFFFF }
        }
        return (addr - 1) & 0xFFFF
    }
}

// MARK: - register access
enum Reg: String, CaseIterable {
    case af = "AF", hl = "HL", de = "DE", bc = "BC", ix = "IX", iy = "IY", pc = "PC"
    case af2 = "AF'", hl2 = "HL'", de2 = "DE'", bc2 = "BC'", sp = "SP", im = "IM", i = "I", r = "R"

    func get(_ z: Z80) -> Int {
        switch self {
        case .af: return Int(z.af.w); case .hl: return Int(z.hl.w); case .de: return Int(z.de.w); case .bc: return Int(z.bc.w)
        case .ix: return Int(z.ix.w); case .iy: return Int(z.iy.w); case .pc: return Int(z.pc)
        case .af2: return Int(z.af2.w); case .hl2: return Int(z.hl2.w); case .de2: return Int(z.de2.w); case .bc2: return Int(z.bc2.w)
        case .sp: return Int(z.sp.w); case .im: return Int(z.im); case .i: return Int(z.i); case .r: return Int(z.r)
        }
    }
    func set(_ z: inout Z80, _ v: Int) {
        let w = UInt16(truncatingIfNeeded: v), b = UInt8(truncatingIfNeeded: v)
        switch self {
        case .af: z.af.w = w; case .hl: z.hl.w = w; case .de: z.de.w = w; case .bc: z.bc.w = w
        case .ix: z.ix.w = w; case .iy: z.iy.w = w; case .pc: z.pc = w
        case .af2: z.af2.w = w; case .hl2: z.hl2.w = w; case .de2: z.de2.w = w; case .bc2: z.bc2.w = w
        case .sp: z.sp.w = w; case .im: z.im = b & 3; case .i: z.i = b; case .r: z.r = b
        }
    }
    var digits: Int { switch self { case .im, .i, .r: return 2; default: return 4 } }
}
