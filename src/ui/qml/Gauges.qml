import QtQuick

// Resource仪表 rings. Every color here resolves through the generated Tokens object
// (src/ui/theme/generated/Tokens.qml) - no literals, no QSS.
Item {
    id: root
    readonly property real dial: 84
    readonly property real holeRatio: 0.8

    Row {
        anchors.centerIn: parent
        spacing: 30

        Repeater {
            // "gauges" is the context property GaugeGroup sets before setSource(); it must
            // not be re-declared here or the component fails to compile.
            model: gauges

            delegate: Column {
                spacing: 8

                Item {
                    width: root.dial
                    height: root.dial

                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: Tokens.track
                    }

                    Canvas {
                        anchors.fill: parent
                        property real fraction: modelData.fraction
                        onFractionChanged: requestPaint()
                        onPaint: {
                            var ctx = getContext("2d");
                            ctx.reset();
                            var c = width / 2;
                            var ring = c * (1 - root.holeRatio);   // ring thickness
                            var r = c - ring / 2;                  // arc radius (center of the ring)
                            ctx.lineWidth = ring;
                            ctx.strokeStyle = Tokens[modelData.colorKey];
                            ctx.lineCap = "butt";
                            ctx.beginPath();
                            // start at top (-PI/2), sweep clockwise by fraction of a full circle
                            ctx.arc(c, c, r, -Math.PI / 2, -Math.PI / 2 + modelData.fraction * 2 * Math.PI);
                            ctx.stroke();
                        }
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        width: root.dial * root.holeRatio
                        height: root.dial * root.holeRatio
                        radius: width / 2
                        color: Tokens.card
                    }

                    Text {
                        anchors.centerIn: parent
                        text: modelData.value
                        color: Tokens.ink
                        font.family: Tokens.fontHeadingFamily
                        font.pixelSize: 15
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: modelData.label
                    color: Tokens.ink2
                    font.family: Tokens.fontBodyFamily
                    font.pixelSize: 12
                }
            }
        }
    }
}
