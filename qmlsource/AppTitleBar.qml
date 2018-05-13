import QtQuick 2.10

Rectangle {
    id: titleArea
    width: parent.width
//    height: parent.height * 0.1
    height: 42
    anchors.left: parent.left
    anchors.top: parent.top
    color: sleepColor

    property alias iconPath: iconPic.source
    property color sleepColor: "#58F"
    property color activeColor: "#6CF"
    property string fontFamily: "方正兰亭超细黑简体"
    property var target: null

    Component.onCompleted: {
        titleArea.iconPath = "/img/appEasyIcon.png"
    }

    Flow{
        width: parent.width * 0.9
        height: parent.height
        anchors.left: parent.left
        anchors.leftMargin: parent.width * 0.05
        anchors.rightMargin: parent.width * 0.05
        spacing: parent.width * 0.05

        Rectangle {
            id: iconArea
            width: parent.width * 0.20
            height: parent.height
            color: titleArea.color

            Image {
                id: iconPic
                anchors.centerIn: parent
                focus: true
            }
        }

        Rectangle {
            id: nameArea
            height: parent.height
            width: parent.width * 0.28
            color: titleArea.color

            Text {
                text: "刘飞飞飞"
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                font.pixelSize: 14
                font.bold: true
                font.family: fontFamily
                color: "#FFF"
            }
        }

        Rectangle {
            id: controlArea
            height: parent.height
            width: parent.width * 0.52
            color: titleArea.color

            Flow{
                anchors.fill: parent
                layoutDirection: Qt.RightToLeft
            }
        }
    }

    MouseArea {
        property bool isPress: false
        property int mousePosPreX
        property int mousePosPreY

        id: mouseArea
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        propagateComposedEvents: true

        onPressed: {
            isPress = true
            mousePosPreX = mouseX
            mousePosPreY = mouseY
        }

        onReleased: {
            isPress = false
        }

        onPositionChanged: {
            if (isPress == true){
                target.x += mouseX - mousePosPreX
                target.y += mouseY - mousePosPreY
            }
        }
    }
}
