import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    color: "#252530"
    border.color: "#3A3A4A"
    border.width: 1

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        Label {
            text: "Capture Controls"
            color: "#ECEFF4"
            font.bold: true
            font.pixelSize: 14
        }

        // Status indicator
        Rectangle {
            Layout.fillWidth: true
            height: 36
            radius: 4
            color: captureViewModel && captureViewModel.isCapturing ? "#2E4A3E" : "#353545"

            RowLayout {
                anchors.fill: parent
                anchors.margins: 8

                Rectangle {
                    width: 10
                    height: 10
                    radius: 5
                    color: captureViewModel && captureViewModel.isCapturing ? "#A3BE8C" : "#88C0D0"
                }

                Label {
                    text: "State: " + (captureViewModel ? captureViewModel.stateString : "Idle")
                    color: "#ECEFF4"
                    font.bold: true
                }

                Item { Layout.fillWidth: true }

                Label {
                    text: captureViewModel ? captureViewModel.samplesCaptured + " samples" : "0 samples"
                    color: "#D8DEE9"
                    font.family: "monospace"
                }
            }
        }

        // Sample rate
        RowLayout {
            Layout.fillWidth: true
            Label {
                text: "Sample Rate:"
                color: "#D8DEE9"
                Layout.preferredWidth: 100
            }
            ComboBox {
                Layout.fillWidth: true
                model: ["100 kHz", "500 kHz", "1 MHz", "2 MHz", "5 MHz", "10 MHz", "20 MHz"]
                currentIndex: 2
                onActivated: function(index) {
                    if (!captureViewModel) return;
                    var rates = [100000, 500000, 1000000, 2000000, 5000000, 10000000, 20000000];
                    captureViewModel.sampleRateHz = rates[index];
                }
            }
        }

        // Channel count
        RowLayout {
            Layout.fillWidth: true
            Label {
                text: "Channels:"
                color: "#D8DEE9"
                Layout.preferredWidth: 100
            }
            ComboBox {
                Layout.fillWidth: true
                model: ["8 channels (1 byte/sample)", "16 channels (2 bytes/sample)", "32 channels (4 bytes/sample)"]
                currentIndex: 0
                onActivated: function(index) {
                    if (!captureViewModel) return;
                    var counts = [8, 16, 32];
                    captureViewModel.channelCount = counts[index];
                }
            }
        }

        // Buttons
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                Layout.fillWidth: true
                text: captureViewModel && captureViewModel.isCapturing ? "Stop Capture" : "Start Capture"
                highlighted: !(captureViewModel && captureViewModel.isCapturing)
                onClicked: {
                    if (!captureViewModel) return;
                    if (captureViewModel.isCapturing) {
                        captureViewModel.stopCapture();
                    } else {
                        captureViewModel.startCapture();
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
