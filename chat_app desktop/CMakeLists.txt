cmake_minimum_required(VERSION 3.19)
project(chat_app LANGUAGES CXX)

# WebEngineWidgets承载Chromium媒体页，WebChannel连接页面与Qt业务控制器。
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network Sql WebEngineWidgets WebChannel)

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
    clickedoncelabel.h clickedoncelabel.cpp
    tcpmgr.h tcpmgr.cpp
    resetdialog.cpp resetdialog.h resetdialog.ui
    usermgr.h usermgr.cpp
    chatdialog.h chatdialog.cpp chatdialog.ui
    chatuserlist.h chatuserlist.cpp
    loadingdlg.h loadingdlg.cpp loadingdlg.ui
    chatuserwid.h chatuserwid.cpp chatuserwid.ui
    conuserwid.h conuserwid.cpp conuserwid.ui
    listitembase.h listitembase.cpp
    conuserlist.h conuserlist.cpp
    chatview.h chatview.cpp
    chatitembase.h chatitembase.cpp
    bubbleframe.h bubbleframe.cpp
    textbubble.h textbubble.cpp
    picturebubble.h picturebubble.cpp
    messagetextedit.h messagetextedit.cpp
    statewidget.h statewidget.cpp
    avatarutil.h avatarutil.cpp
    searchlist.h searchlist.cpp
    userdata.h userdata.cpp
    adduseritem.h adduseritem.cpp adduseritem.ui
    findsuccessdlg.h findsuccessdlg.cpp findsuccessdlg.ui
    friendinfopage.h friendinfopage.cpp friendinfopage.ui
    applyfriendpage.h applyfriendpage.cpp applyfriendpage.ui
    applyfrienditem.h applyfrienditem.cpp applyfrienditem.ui
    authenfriend.h authenfriend.cpp authenfriend.ui
    applyfriend.h applyfriend.cpp applyfriend.ui
    clickedoncelabel.h clickedoncelabel.cpp
    friendlabel.h friendlabel.cpp friendlabel.ui
    settingdialog.h settingdialog.cpp settingdialog.ui
    resourceclient.cpp
    resourceclient.h
    chatimagetransfer.h
    chatimagetransfer.cpp
    localchatstoragemgr.h
    localchatstoragemgr.cpp
    groupmanagementdialog.h groupmanagementdialog.cpp
    grouprequestsdlg.h grouprequestsdlg.cpp
    groupavatartransfer.h groupavatartransfer.cpp
    # qrc嵌入受信HTML/JS，运行时由chatvideo scheme与唯一配置素材统一映射。
    videotestcontroller.h videotestcontroller.cpp videotest.qrc
    #ini、ui、qrc直接加入可执行文件
)

target_include_directories(chat_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(chat_app PRIVATE /utf-8)
endif()

target_link_libraries(chat_app
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Network
        Qt::Sql
        Qt::WebEngineWidgets
        Qt::WebChannel
)

install(TARGETS chat_app
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

#复制配置文件到构建目录，然后去main.cpp中读取配置
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/config.ini
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

#复制static资源目录到构建目录，运行时按 exe 同目录/static 读取头像等资源
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/static
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

# 固定测试素材按exe相对路径读取；多配置生成器同样复制到实际可执行文件目录。
add_custom_command(TARGET chat_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/media $<TARGET_FILE_DIR:chat_app>/media
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/config.ini $<TARGET_FILE_DIR:chat_app>/config.ini
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/media DESTINATION ${CMAKE_INSTALL_BINDIR})

qt_generate_deploy_app_script(
    TARGET chat_app
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})


