#####################################################################
## KvaserCanBusPlugin Plugin:
#####################################################################

qt_internal_add_plugin(KvaserCanBusPlugin
    OUTPUT_NAME qtkvasercanbus
    TYPE canbus
    SOURCES
        main.cpp
        kvasercan_symbols_p.h
        kvasercanbackend.cpp kvasercanbackend.h kvasercanbackend_p.h
    PUBLIC_LIBRARIES
        Qt::Core
        Qt::SerialBus
)
