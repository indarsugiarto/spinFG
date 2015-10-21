TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

SOURCES += \
    IONode.c

include(deployment.pri)
qtcAddDeployment()

INCLUDEPATH += \
    /opt/spinnaker_tools_134/include \
    ../Common

OTHER_FILES += \
    Makefile
