add_library(akumuli SHARED
    ../include/storage.h
    ../include/config.h
    ../include/page.h
    ../include/akumuli.h
    ../include/util.h
    storage.cpp
    page.cpp
    akumuli.cpp
    util.cpp
)