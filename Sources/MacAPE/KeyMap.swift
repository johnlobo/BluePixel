import CPCCore

/// Default positional mapping from macOS virtual key codes to CPC keyboard matrix keys.
enum KeyMap {
    static let defaultMap: [UInt16: Int] = [
        0: CPCK_A, 1: CPCK_S, 2: CPCK_D, 3: CPCK_F, 4: CPCK_H, 5: CPCK_G, 6: CPCK_Z, 7: CPCK_X, 8: CPCK_C, 9: CPCK_V,
        11: CPCK_B, 12: CPCK_Q, 13: CPCK_W, 14: CPCK_E, 15: CPCK_R, 16: CPCK_Y, 17: CPCK_T,
        18: CPCK_1, 19: CPCK_2, 20: CPCK_3, 21: CPCK_4, 22: CPCK_6, 23: CPCK_5, 24: CPCK_CARET, 25: CPCK_9, 26: CPCK_7,
        27: CPCK_MINUS, 28: CPCK_8, 29: CPCK_0, 30: CPCK_LBRACKET, 31: CPCK_O, 32: CPCK_U, 33: CPCK_AT, 34: CPCK_I, 35: CPCK_P,
        36: CPCK_RETURN, 37: CPCK_L, 38: CPCK_J, 39: CPCK_COLON, 40: CPCK_K, 41: CPCK_SEMICOLON, 42: CPCK_RBRACKET,
        43: CPCK_COMMA, 44: CPCK_SLASH, 45: CPCK_N, 46: CPCK_M, 47: CPCK_DOT, 48: CPCK_TAB, 49: CPCK_SPACE,
        50: CPCK_BACKSLASH, 51: CPCK_DEL, 53: CPCK_ESC, 57: CPCK_CAPS,
        117: CPCK_CLR, 119: CPCK_COPY, 58: CPCK_COPY, 61: CPCK_COPY,
        123: CPCK_LEFT, 124: CPCK_RIGHT, 125: CPCK_DOWN, 126: CPCK_UP,
        76: CPCK_ENTER,
    ]
    /// Keypad keys (NumLock "on" → CPC function keys).
    static let keypadFunction: [UInt16: Int] = [
        82: CPCK_F0, 83: CPCK_F1, 84: CPCK_F2, 85: CPCK_F3, 86: CPCK_F4, 87: CPCK_F5, 88: CPCK_F6, 89: CPCK_F7,
        91: CPCK_F8, 92: CPCK_F9, 65: CPCK_FDOT,
    ]
    /// Keypad keys (NumLock "off" → joystick).
    static let keypadJoystick: [UInt16: Int] = [
        86: CPCK_JOY_LEFT, 88: CPCK_JOY_RIGHT, 91: CPCK_JOY_UP, 84: CPCK_JOY_DOWN, 87: CPCK_JOY_FIRE1, 82: CPCK_JOY_FIRE1,
        92: CPCK_JOY_FIRE2, 89: CPCK_JOY_FIRE3,
    ]
    /// Names of the CPC keys shown in the mapping editor (key value = matrix position).
    static let cpcKeyNames: [String: Int] = [
        "0": CPCK_0, "1": CPCK_1, "2": CPCK_2, "3": CPCK_3, "4": CPCK_4, "5": CPCK_5, "6": CPCK_6, "7": CPCK_7, "8": CPCK_8, "9": CPCK_9,
        "A": CPCK_A, "B": CPCK_B, "C": CPCK_C, "D": CPCK_D, "E": CPCK_E, "F": CPCK_F, "G": CPCK_G, "H": CPCK_H, "I": CPCK_I, "J": CPCK_J, "K": CPCK_K, "L": CPCK_L, "M": CPCK_M,
        "N": CPCK_N, "O": CPCK_O, "P": CPCK_P, "Q": CPCK_Q, "R": CPCK_R, "S": CPCK_S, "T": CPCK_T, "U": CPCK_U, "V": CPCK_V, "W": CPCK_W, "X": CPCK_X, "Y": CPCK_Y, "Z": CPCK_Z,
        "SPACE": CPCK_SPACE, "RETURN": CPCK_RETURN, "ENTER": CPCK_ENTER, "ESC": CPCK_ESC, "TAB": CPCK_TAB, "CAPS LOCK": CPCK_CAPS, "DEL": CPCK_DEL, "CLR": CPCK_CLR, "COPY": CPCK_COPY,
        "UP": CPCK_UP, "DOWN": CPCK_DOWN, "LEFT": CPCK_LEFT, "RIGHT": CPCK_RIGHT, "F0": CPCK_F0, "F1": CPCK_F1, "F2": CPCK_F2, "F3": CPCK_F3, "F4": CPCK_F4, "F5": CPCK_F5,
        "F6": CPCK_F6, "F7": CPCK_F7, "F8": CPCK_F8, "F9": CPCK_F9, "F.": CPCK_FDOT, "-": CPCK_MINUS, "^": CPCK_CARET, "@": CPCK_AT, "[": CPCK_LBRACKET, "]": CPCK_RBRACKET,
        "\\": CPCK_BACKSLASH, ";": CPCK_SEMICOLON, ":": CPCK_COLON, ",": CPCK_COMMA, ".": CPCK_DOT, "/": CPCK_SLASH, "SHIFT": CPCK_SHIFT, "CONTROL": CPCK_CTRL,
        "JOY UP": CPCK_JOY_UP, "JOY DOWN": CPCK_JOY_DOWN, "JOY LEFT": CPCK_JOY_LEFT, "JOY RIGHT": CPCK_JOY_RIGHT, "FIRE 1": CPCK_JOY_FIRE1, "FIRE 2": CPCK_JOY_FIRE2, "FIRE 3": CPCK_JOY_FIRE3,
    ]
    static let macKeyNames: [UInt16: String] = [
        0: "A", 1: "S", 2: "D", 3: "F", 4: "H", 5: "G", 6: "Z", 7: "X", 8: "C", 9: "V", 11: "B", 12: "Q", 13: "W", 14: "E", 15: "R", 16: "Y", 17: "T", 18: "1", 19: "2", 20: "3", 21: "4", 22: "6", 23: "5",
        24: "=", 25: "9", 26: "7", 27: "-", 28: "8", 29: "0", 30: "]", 31: "O", 32: "U", 33: "[", 34: "I", 35: "P", 36: "Return", 37: "L", 38: "J", 39: "'", 40: "K", 41: ";", 42: "\\", 43: ",", 44: "/",
        45: "N", 46: "M", 47: ".", 48: "Tab", 49: "Space", 50: "`", 51: "Delete", 53: "Esc", 57: "Caps Lock", 58: "Option", 61: "Right Option", 117: "Fwd Delete", 119: "End", 115: "Home", 116: "Page Up", 121: "Page Down",
        123: "Left", 124: "Right", 125: "Down", 126: "Up", 76: "Keypad Enter", 82: "Keypad 0", 83: "Keypad 1", 84: "Keypad 2", 85: "Keypad 3", 86: "Keypad 4", 87: "Keypad 5", 88: "Keypad 6", 89: "Keypad 7",
        91: "Keypad 8", 92: "Keypad 9", 65: "Keypad .", 56: "Shift", 60: "Right Shift", 59: "Control", 62: "Right Control",
    ]
    static func keyName(_ codes: [UInt16]) -> String { codes.isEmpty ? "—" : codes.map { macKeyNames[$0] ?? "#\($0)" }.joined(separator: ", ") }
    /// Mac key codes currently mapped to a CPC key (custom overrides replace the defaults for that key).
    static func codes(for cpcKey: Int, settings: Settings) -> [UInt16] {
        if let v = settings["Keyboard", "\(cpcKey)"], let c = UInt16(v) { return [c] }
        return defaultMap.filter { $0.value == cpcKey }.map { $0.key }.sorted() + (cpcKey == Int(CPCK_SHIFT) ? [56, 60] : cpcKey == Int(CPCK_CTRL) ? [59, 62] : [])
    }
    /// Complete code → CPC key table with overrides applied.
    static func effectiveMap(settings: Settings) -> [UInt16: Int] {
        var m = defaultMap
        for k in settings.keys("Keyboard") { if let cpc = Int(k), let code = UInt16(settings["Keyboard", k] ?? "") { for (c, v) in m where v == cpc { m[c] = nil }; m[code] = cpc } }
        return m
    }
    /// Modifier keys, reported through flagsChanged.
    static let shiftCodes: Set<UInt16> = [56, 60]
    static let ctrlCodes: Set<UInt16> = [59, 62]
}
