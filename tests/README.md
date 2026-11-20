# 测试构建与运行指南

## Windows根构建

按 [环境部署：安装 C++ 依赖](../环境部署.md#2-安装-c-依赖) 设置 `VCPKG_ROOT`。从仓库根目录执行；将 `$qt` 设为已安装的 Qt MSVC x64 kit 路径，以便构建 SQLite 测试。

```powershell
cmake --preset windows-vcpkg "-DCMAKE_PREFIX_PATH=$qt"
cmake --build --preset debug
ctest --test-dir build -C Debug -N
```

VCPKG_ROOT须指向已安装依赖的实际vcpkg根目录。Release使用 `cmake --build --preset release`，运行时将 `-C Debug` 改为 `-C Release`。`-N`列出当前注册项，不执行测试；多配置生成器应显式传入配置名。

根CMake通过include(CTest)默认启用BUILD_TESTING，测试目标随默认构建编译。测试依赖包括GoogleTest、redis++/hiredis、MySQL Connector/C++、Boost头文件、Threads、PNG、libjpeg-turbo，以及Qt 6.5+ Core/Sql/Widgets。测试目标与标签由 [CMakeLists.txt](CMakeLists.txt)定义；覆盖原理见 [测试知识](../note/测试.md)。

## 选择测试

[run_tests.ps1](../run_tests.ps1)只运行build目录已有的CTest配置，不配置和编译CMake：

```powershell
.\run_tests.ps1
.\run_tests.ps1 -Tests tcp,friend,message,sqlite
.\run_tests.ps1 -Configuration Release -Tests tcp,sqlite
```

无Tests参数时逐项询问，输入y或yes运行，回车跳过。Configuration只接受Debug或Release；Tests只接受tcp、friend、message、sqlite、capacity。普通选择合并成CTest正则，capacity另以标签运行。

| 范围 | CTest运行命令 |
| --- | --- |
| TCP解析 | `ctest --test-dir build -C Debug -R TcpStreamParserTest --output-on-failure` |
| 好友事务 | `ctest --test-dir build -C Debug -R FriendTransactionIntegration --output-on-failure` |
| 私聊历史/回执 | `ctest --test-dir build -C Debug -R MessageRetryOfflineSyncTest --output-on-failure` |
| SQLite | `ctest --test-dir build -C Debug -R LocalChatStorageTest --output-on-failure` |
| 群事务 | `ctest --test-dir build -C Debug -R GroupTransactionTest --output-on-failure` |
| 群头像解码 | `ctest --test-dir build -C Debug -R GroupAvatarDecoderTest --output-on-failure` |
| 非容量全部 | `ctest --test-dir build -C Debug -LE capacity --output-on-failure` |

group与decoder没有脚本菜单选项，使用上表命令。unit标签包含TCP、SQLite和头像解码；integration包含真实MySQL数据层测试。标签名称不能代替运行依赖检查。

## 隔离MySQL与工作目录配置

数据库测试会创建用户、关系、消息及临时trigger。只能使用独立测试实例或明确隔离的schema，推荐统一使用chat_app_test_local。不得把开发/生产配置复制为测试目标，也不得指向chat_app_db。MySQL 8启用X Protocol；导入DDL常用3306，C++ X DevAPI连接常用33060，两者要分别配置。

仓库提供 [chat_app_test_local.sql](../sql/chat_app_test_local.sql)：

```powershell
$ddl = Get-Content .\sql\chat_app_test_local.sql -Raw
mysql --default-character-set=utf8mb4 --protocol=tcp -h 127.0.0.1 -P 3306 -u root -p -e $ddl
```

mysql会交互询问管理员密码。脚本创建指定schema和当前完整表结构，不删除旧数据，也不升级已存在的旧表。旧测试库应按 [数据库设计](../note/数据库设计.md)选择迁移。表使用InnoDB才能检验事务回滚。

消息和群用例由CTest启动时，工作目录为build/tests。该目录需由操作者放置完整config.ini，示例如下；密码和连接参数必须替换为隔离环境实际值：

```ini
[Mysql]
host = 127.0.0.1
port = 33060
user = test_user
passwd = <test-db-password>
schema = chat_app_test_local
poolsize = 2
```

测试账号需要隔离schema的数据读写与trigger创建/删除权限。启用binlog的实例创建trigger还可能需要相应管理权限或独立测试实例的log_bin_trust_function_creators配置；不要为运行测试放宽共享生产实例的安全限制。

### 好友事务环境变量

```powershell
$env:CHAT_FRIEND_TEST_CONFIRM_ISOLATED = 'YES'
$env:CHAT_FRIEND_TEST_MYSQL_HOST = '127.0.0.1'
$env:CHAT_FRIEND_TEST_MYSQL_PORT = '33060'
$env:CHAT_FRIEND_TEST_MYSQL_USER = 'test_user'
$env:CHAT_FRIEND_TEST_MYSQL_PASSWORD = '<test-db-password>'
$env:CHAT_FRIEND_TEST_MYSQL_SCHEMA = 'chat_app_test_local'
.\run_tests.ps1 -Tests friend
```

好友fixture根据这些变量创建临时配置，无需build/tests/config.ini。schema必须是最长64字符的字母数字下划线标识、包含test且不能为chat_app_db。缺少配置或YES确认会跳过；显式连接、表结构或权限问题会失败。

### 私聊消息环境变量

先准备build/tests/config.ini，再设置与文件完全相同的host、port、user、schema：

```powershell
$env:CHAT_MESSAGE_TEST_DB_HOST = '127.0.0.1'
$env:CHAT_MESSAGE_TEST_DB_PORT = '33060'
$env:CHAT_MESSAGE_TEST_DB_USER = 'test_user'
$env:CHAT_MESSAGE_TEST_DB_SCHEMA = 'chat_app_test_local'
.\run_tests.ps1 -Tests message
```

密码从config.ini读取，没有CHAT_MESSAGE_TEST_DB_PASSWORD参数。schema必须以chat_app_test_开头。缺少变量、配置不符和当前实现中的连接初始化失败均可能标为Skipped，不能仅凭CTest退出码认为这些用例已执行。

### 群事务环境变量

```powershell
$env:CHAT_GROUP_TEST_SCHEMA = 'chat_app_test_local'
$env:CHAT_GROUP_TEST_CONFIRM_ISOLATED = 'YES'
ctest --test-dir build -C Debug -R GroupTransactionTest --output-on-failure
```

群fixture要求schema以chat_app_test_开头、仅字母数字下划线，并与工作目录config.ini的Mysql.schema一致。缺少隔离条件会跳过；显式配置后连接失败会失败。群操作会使用当前完整群表与版本字段，旧库不能靠CREATE IF NOT EXISTS补齐。

需要从自选隔离目录直接运行exe时，将config.ini放到该目录，并在PowerShell中先保存可执行文件绝对路径，再切换目录运行。程序按当前工作目录读取配置；exe所在目录不自动成为配置目录。

## SQLite与Qt编译器匹配

Qt库、测试程序与GoogleTest必须使用相容编译器。根测试构建先查Qt 6.5+ Core/Sql/Widgets；Qt与根编译器兼容时，local_chat_storage_test为普通目标。

Windows根MSVC检测到MinGW Qt时，CMake从 `chat_app desktop/build` 下面的桌面CMakeCache.txt寻找Ninja、编译器、Qt6_DIR，并从vcpkg buildtrees/gtest/src寻找GoogleTest源码。满足条件后默认构建会创建build/sqlite-client-kit子构建，使用桌面编译器重新编译GoogleTest与SQLite测试。只安装预编译MSVC版GoogleTest不能满足此分支。

根CTest的LocalChatStorageTest会调用 [run_nested_ctest.cmake](sqlite/run_nested_ctest.cmake)，为子构建补Qt bin到PATH和QT_PLUGIN_PATH，再转发CTest。若没有找到Qt，或不匹配分支缺少桌面cache/GTest源码，普通根构建会提示跳过；CHAT_APP_TESTS_ONLY配置下会报错。不存在的测试不能作为已覆盖项计入运行结果。

独立子构建支持显式参数，可用于根Qt自动发现不满足本机布局的环境。以下变量均应填写为本机真实路径：

```powershell
$qtRoot = '<Qt-kit-root>'
$cxx = '<matching-cxx-compiler>'
$ninja = '<ninja-executable>'
$gtestSource = '<googletest-source-root>'
cmake -S tests/sqlite -B build/sqlite-client-kit -G Ninja `
  "-DCMAKE_CXX_COMPILER=$cxx" "-DCMAKE_MAKE_PROGRAM=$ninja" `
  -DCMAKE_BUILD_TYPE=Debug "-DQt6_DIR=$qtRoot/lib/cmake/Qt6" `
  "-DGTEST_SOURCE_DIR=$gtestSource"
cmake --build build/sqlite-client-kit --target local_chat_storage_test
$env:PATH = "$qtRoot/bin;$env:PATH"
$env:QT_PLUGIN_PATH = "$qtRoot/plugins"
ctest --test-dir build/sqlite-client-kit -C Debug --output-on-failure
```

GTEST_SOURCE_DIR必须包含GoogleTest源码CMakeLists.txt。运行要求QSQLITE插件及其依赖可找到；路径错误会导致驱动检查失败。fixture将数据库写入QTemporaryDir并检查路径，不修改正常用户缓存。直接运行子构建只证明SQLite范围，不代表根CTest其余目标也已运行。

## 连接容量

容量测试只在Windows运行，需要目标ChatServer实际监听；测试程序直接建立socket，不要求测试端提供MySQL或Redis凭据。服务自身的启动依赖仍须满足。默认主机127.0.0.1，端口8090、8091，每个端口最低100条，最多尝试20000条，全部探测结束后保持1000ms。

```powershell
$env:CHAT_CONNECTION_HOST = '127.0.0.1'
$env:CHAT_CONNECTION_PORTS = '8090,8091'
$env:CHAT_CONNECTION_MIN = '100'
$env:CHAT_CONNECTION_MAX = '5000'
$env:CHAT_CONNECTION_HOLD_MS = '3000'
.\run_tests.ps1 -Tests capacity
```

直接CTest入口为 `ctest --test-dir build -C Debug -L capacity --output-on-failure`。参数须为正整数且MAX不小于MIN。各端口交替探测，某端口首次失败后停止继续向该端口建连，其他端口继续；最后关闭已有socket并使用RST减少TIME_WAIT残留。

| Windows错误 | 排查方向 |
| --- | --- |
| WSAEADDRINUSE / WSAEADDRNOTAVAIL | 客户端地址或动态端口；可用 `netsh int ipv4 show dynamicport tcp` 查看范围 |
| WSAECONNREFUSED | 目标监听、端口、队列与防火墙 |
| WSAENOBUFS / WSAEMFILE | 句柄、内存和网络缓冲资源 |

非Windows或所有端口一开始都拒绝连接会跳过。达到MAX只达到本次探测上限；空闲socket容量不能推导有登录、心跳和消息负载的业务容量。

## 真实服务安全smoke

[group_service_security_smoke.py](group_service_security_smoke.py)不属于CTest，不负责启动整套后端或初始化MySQL。需要Python的grpcio、Pillow，以及由共享proto生成的message_pb2.py/message_pb2_grpc.py。先在独立运行目录为Gate、Status、双Chat、Resource与独立Redis准备配置并启动，所有服务指向同一隔离测试schema和Redis实例，资源目录也须独立。

脚本当前固定使用回环fixture端口：Gate HTTP38080、Chat TCP38090/38091、Resource TCP39090、Resource RPC35057，MySQL命令行33316/root且未传密码。数据库应按该脚本的本地隔离契约提供访问；生产数据库不能为了适配脚本关闭认证。运行目录的ResourceServer/config.ini提供schema、file_path与非空service_key；各服务相应配置须一致。脚本会占用UID1900710001～1900710006并写入fixture账号、好友与业务数据，只可在专用测试schema运行。

先生成Python RPC代码：

```powershell
$runtime = '<isolated-runtime-directory>'
python -m grpc_tools.protoc -I proto `
  "--python_out=$runtime/generated-python-rpc" `
  "--grpc_python_out=$runtime/generated-python-rpc" proto/message.proto
```

需要事先创建generated-python-rpc目录并安装grpcio-tools。完整模式还会从build/ResourceServer/Debug/ResourceServer.exe另起空service_key资源进程，占用TCP39092/RPC35058；这两个端口需空闲。完整模式以外的分支按源码调用范围使用同一fixture约定。

```powershell
$mysql = '<mysql-client-executable>'
python tests/group_service_security_smoke.py --runtime-dir $runtime --mysql-client $mysql
python tests/group_service_security_smoke.py --runtime-dir $runtime --mysql-client $mysql --negative-avatar-only
python tests/group_service_security_smoke.py --runtime-dir $runtime --mysql-client $mysql --announcement-privacy-only
python tests/group_service_security_smoke.py --runtime-dir $runtime --mysql-client $mysql --management-only
```

模式应分别调用；脚本未定义任意端口、账号或服务启动参数。清理逻辑只清理其部分业务记录与连接，不能认为会删除所有账号、资源、日志或还原隔离实例。使用专用可丢弃环境管理生命周期，日志中不得保存token、服务密钥或配置密码。该脚本只检查服务协议，Qt界面验收范围见 [测试知识](../note/测试.md)。

## 结果解释与CI

查看CTest报告中的失败与Skipped，并检查选定正则确有匹配。数据库隔离保护触发时跳过是安全行为，不能计为业务已验证。需要机器可读结果时可以运行：

```powershell
ctest --test-dir build -C Debug -LE capacity --output-on-failure --output-junit build/ctest-results.xml
```

[tests.yml](../.github/workflows/tests.yml)设计为非容量测试生成JUnit并在存在skipped时失败。当前工作流未提供群隔离变量；CHAT_APP_TESTS_ONLY分支未创建头像测试需要的chat_proto，依赖安装也未显式包含PNG/libjpeg-turbo。使用该构建模式前需修正相应构建与CI配置，不能将其作为现成可用的最小构建命令。本指南的根完整构建创建chat_proto；独立SQLite子构建不依赖它。
