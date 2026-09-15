pragma Singleton
import QtQuick

QtObject {
    readonly property color background: "#1E1E24"
    readonly property color surface: "#2B2B36"
    readonly property color surfaceHighlight: "#363645"
    readonly property color border: "#404052"
    readonly property color textPrimary: "#ECEFF4"
    readonly property color textSecondary: "#A0A8B8"
    readonly property color accent: "#00CC66"
    readonly property color warning: "#EBCB8B"
    readonly property color error: "#BF616A"
    readonly property color info: "#88C0D0"

    readonly property int fontSizeSmall: 11
    readonly property int fontSizeNormal: 13
    readonly property int fontSizeTitle: 16
    readonly property string fontFamily: "Inter, Segoe UI, sans-serif"
    readonly property string monoFontFamily: "JetBrains Mono, Cascadia Code, Consolas, monospace"
}
