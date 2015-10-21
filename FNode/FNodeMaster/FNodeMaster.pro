TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

SOURCES += \
    FNodeMaster.c

include(deployment.pri)
qtcAddDeployment()

INCLUDEPATH += \
    /opt/spinnaker_tools_134/include \
    ../../Common

OTHER_FILES += \
    how-dma_data_t-works.png \
    Makefile
