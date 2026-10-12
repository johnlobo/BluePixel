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
    /// Modifier keys, reported through flagsChanged.
    static let shiftCodes: Set<UInt16> = [56, 60]
    static let ctrlCodes: Set<UInt16> = [59, 62]
}
