import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UniversalSniffer.Gui 1.0

ApplicationWindow {
    id: window
    width: 1200
    height: 750
    visible: true
    title: "Universal Sniffer — Digital Signal & Protocol Analyzer"
    color: "#1E1E24"

    menuBar: MenuBar {
        Menu {
            title: "&File"
            Action { text: "&Open Capture..."; shortcut: "Ctrl+O" }
            Action { text: "&Save Capture As..."; shortcut: "Ctrl+S" }
            MenuSeparator { }
            Action { text: "E&xit"; onTriggered: Qt.quit() }
        }
        Menu {
            title: "&Capture"
            Action {
                text: captureViewModel && captureViewModel.isCapturing ? "&Stop" : "&Start"
                shortcut: "F5"
                onTriggered: {
                    if (!captureViewModel) return;
                    if (captureViewModel.isCapturing) captureViewModel.stopCapture();
                    else captureViewModel.startCapture();
                }
            }
        }
        Menu {
            title: "&View"
            Action { text: "Reset &Zoom"; shortcut: "Ctrl+0"; onTriggered: if (waveformViewModel) waveformViewModel.resetView() }
        }
        Menu {
            title: "&Help"
            Action { text: "&About Universal Sniffer" }
        }
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        // Left control pane
        CapturePanel {
            SplitView.preferredWidth: 320
            SplitView.minimumWidth: 260
            SplitView.maximumWidth: 450
        }

        // Center / Right content
        SplitView {
            orientation: Qt.Vertical
            SplitView.fillWidth: true

            // Waveform workspace area (placeholder for WaveformView in Phase 2/5)
            Rectangle {
                SplitView.fillWidth: true
                SplitView.fillHeight: true
                color: "#18181E"

                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: 8

                    Label {
                        text: "Waveform Viewport"
                        color: "#606075"
                        font.pixelSize: 18
                        font.bold: true
                        Layout.alignment: Qt.AlignHCenter
                    }

                    Label {
                        text: "Hardware capture stream and decoded protocol packets"
                        color: "#4A4A5A"
                        font.pixelSize: 12
                        Layout.alignment: Qt.AlignHCenter
                    }
                }
            }

            // Bottom Diagnostics panel
            DiagnosticsPanel {
                SplitView.fillWidth: true
                SplitView.preferredHeight: 220
                SplitView.minimumHeight: 120
            }
        }
    }

    footer: ToolBar {
        height: 26
        background: Rectangle {
            color: "#18181E"
            border.color: "#2B2B36"
            border.width: 1
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8

            Label {
                text: "Device: FakeCaptureDevice (Ready)"
                color: "#A0A8B8"
                font.pixelSize: 11
            }

            Item { Layout.fillWidth: true }

            Label {
                text: "Wire v" + 1 + " · Qt 6"
                color: "#606075"
                font.pixelSize: 11
            }
        }
    }
}
