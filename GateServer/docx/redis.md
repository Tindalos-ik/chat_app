# Redis

使用redis实现验证码过期服务



## 安装redis

先去下载<https://github.com/tporadowski/redis/releases>，然后解压



在`redis.windows.conf`中可以修改端口，默认是6379，还可以修改密码requirepass



打开终端，启动redis服务器

```
.\redis-server.exe .\redis.windows.conf
```

启动客户端

```
.\redis-cli.exe -p 6379

#输入密码
127.0.0.1:6379> auth 123456
OK
```





## 安装redis库



使用redis-plus-plus库，使用vcpkg下载

测试

```
#include <hiredis/hiredis.h>

void TestRedis() {
    //连接redis 需要启动才可以进行连接
	//redis默认监听端口为6379 可以再配置文件中修改
    redisContext* c = redisConnect("127.0.0.1", 6379);
    if (c->err)
    {
        printf("Connect to redisServer faile:%s\n", c->errstr);
        redisFree(c);        return;
    }
    printf("Connect to redisServer Success\n");

    std::string redis_password = "123456";
    //执行指令 auth 123456 
    redisReply* r = (redisReply*)redisCommand(c, "AUTH %s", redis_password.c_str());
     if (r->type == REDIS_REPLY_ERROR) {
         printf("Redis certificate failed\n");
    }else {
        printf("Redis certificate success\n");
         }

    //为redis设置key
    const char* command1 = "set stest1 value1";

    //执行redis命令行
    r = (redisReply*)redisCommand(c, command1);

    //如果返回NULL则说明执行失败
    if (NULL == r)
    {
        printf("Execut command1 failure\n");
        redisFree(c);        return;
    }

    //如果执行失败则释放连接
    if (!(r->type == REDIS_REPLY_STATUS && (strcmp(r->str, "OK") == 0 || strcmp(r->str, "ok") == 0)))
    {
        printf("Failed to execute command[%s]\n", command1);
        freeReplyObject(r);
        redisFree(c);        return;
    }

    //执行成功 释放redisCommand执行后返回的redisReply所占用的内存
    freeReplyObject(r);
    printf("Succeed to execute command[%s]\n", command1);

    const char* command2 = "strlen stest1";
    r = (redisReply*)redisCommand(c, command2);

    //如果返回类型不是整形 则释放连接
    if (r->type != REDIS_REPLY_INTEGER)
    {
        printf("Failed to execute command[%s]\n", command2);
        freeReplyObject(r);
        redisFree(c);        return;
    }

    //获取字符串长度
    int length = r->integer;
    freeReplyObject(r);
    printf("The length of 'stest1' is %d.\n", length);
    printf("Succeed to execute command[%s]\n", command2);

    //获取redis键值对信息
    const char* command3 = "get stest1";
    r = (redisReply*)redisCommand(c, command3);
    if (r->type != REDIS_REPLY_STRING)
    {
        printf("Failed to execute command[%s]\n", command3);
        freeReplyObject(r);
        redisFree(c);        return;
    }
    printf("The value of 'stest1' is %s\n", r->str);
    freeReplyObject(r);
    printf("Succeed to execute command[%s]\n", command3);

    const char* command4 = "get stest2";
    r = (redisReply*)redisCommand(c, command4);
    if (r->type != REDIS_REPLY_NIL)
    {
        printf("Failed to execute command[%s]\n", command4);
        freeReplyObject(r);
        redisFree(c);        return;
    }
    freeReplyObject(r);
    printf("Succeed to execute command[%s]\n", command4);

    //释放连接资源
    redisFree(c);

}
```



## 封装Redis操作类和连接池

RedisMgr 一个单例类，并且可接受回调