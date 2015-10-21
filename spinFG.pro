TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

SOURCES += \
    FNode/FNodeMaster/FNodeMaster.c \
    FNode/FNodeWorker/FNodeWorker.c \
    IONode/IONode.c \
    Common/utils.c

include(deployment.pri)
qtcAddDeployment()

INCLUDEPATH += \
    /opt/spinnaker_tools_134/include \
    Common
