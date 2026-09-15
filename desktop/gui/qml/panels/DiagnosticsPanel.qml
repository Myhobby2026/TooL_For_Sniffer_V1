import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UniversalSniffer.Gui 1.0

Rectangle {
    id: root
    color: "#252530"
    border.color: "#3A3A4A"
    border.width: 1

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        RowLayout {
            Layout.fillWidth: true

            Label {
                text: "Diagnostics Log"
                color: "#ECEFF4"
                font.bold: true
                font.pixelSize: 14
            }

            Item { Layout.fillWidth: true }

            Label {
                text: diagnosticsModel && diagnosticsModel.evictedCount > 0 ?
                      diagnosticsModel.evictedCount + " earlier events evicted" : ""
                color: "#EBCB8B"
                font.pixelSize: 11
                visible: diagnosticsModel ? diagnosticsModel.evictedCount > 0 : false
            }

            Button {
                text: "Clear"
                onClicked: if (diagnosticsModel) diagnosticsModel.clear()
            }
        }

        ListView {
            id: listView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: diagnosticsModel

            delegate: Rectangle {
                width: listView.width
                height: 24
                color: index % 2 === 0 ? "transparent" : "#2A2A38"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 4
                    anchors.rightMargin: 4
                    spacing: 8

                    Rectangle {
                        width: 48
                        height: 18
                        radius: 3
                        color: {
                            if (model.level >= 4) return "#BF616A";
                            if (model.level === 3) return "#D08770";
                            if (model.level === 2) return "#88C0D0";
                            return "#4C566A";
                        }
                        Label {
                            anchors.centerIn: parent
                            text: model.levelName || "INFO"
                            color: "#FFFFFF"
                            font.pixelSize: 9
                            font.bold: true
                        }
                    }

                    Label {
                        text: "[" + (model.category || "core") + "]"
                        color: "#81A1C1"
                        font.pixelSize: 11
                        font.family: "monospace"
                    }

                    Label {
                        Layout.fillWidth: true
                        text: model.message || ""
                        color: "#ECEFF4"
                        font.pixelSize: 11
                        font.family: "monospace"
                        elide: Text.ElideRight
                    }
                }
            }

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }
        }
    }
}
