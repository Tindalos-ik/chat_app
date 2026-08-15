cmake_minimum_required(VERSION 3.19)
project(chat_app LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network)

qt_standard_project_setup()
set(CMAKE_AUTORCC ON) #上面那个只设置了moc和uic，没有设置rcc

qt_add_executable(chat_app
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    resource.qrc
    logindialog.h logindialog.cpp logindialog.ui
    registerdialog.h registerdialog.cpp registerdialog.ui
    global.h
    global.cpp
    singleton.h
    httpmgr.h httpmgr.cpp
    config.ini
    timerbtn.h timerbtn.cpp
    clickedlabel.h clickedlabel.cpp
    tcpmgr.h tcpmgr.cpp
    resetdialog.cpp resetdialog.h resetdialog.ui
    usermgr.h usermgr.cpp
    chatdialog.h chatdialog.cpp chatdialog.ui   #ini、ui、qrc直接加入可执行文件
)

target_include_directories(chat_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(chat_app
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Network
)

install(TARGETS chat_app
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

#复制配置文件到构建目录，然后去main.cpp中读取配置
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/config.ini
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

qt_generate_deploy_app_script(
    TARGET chat_app
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})


