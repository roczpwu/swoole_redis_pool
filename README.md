# swoole_redis_pool
swoole redis coroutine pool

# 依赖说明
- Swoole 2.0.11（非2.0.11版本需要替换include/swoole的header文件）
- php7.0+

### 接口说明
```
class redis_coro {

    public function __construct($ip, $port, [$timeout], [$password]){...};
    public function __destruct(){...};
    public function setPool($pool_size, [$max_active]){...};
    public function execute($command,[$argv]){...};
    public function execute_pipeline($commands,[$argvs]){...};
    public function execute_multi($commands,[$argvs]){...};
}
```

### 构造函数
构造函数法接受4个参数：
* ```$ip```是redis服务端的地址
* ```$port```是redis服务端的端口
* ```$timeout```是超时时间（单位：s），可选，默认位2
* ```$password```是redis密码，可选，默认为null

### setPool
redis连接池大小设置函数接受两个参数：
* ```$pool_size```是空闲状态下连接维持数量，必须是大于等于1的整数
* ```$max_active```是峰值状态下最大连接数，可选，必须大于等于```$pool_size```。默认等于```$pool_size```。

需要注意的是，当redis请求数量超过```$max_active```时，新的请求会进入FIFO等待队列。

### execute
单条redis命令执行函数接受1个参数：
* ```$command```是单条redis命令，类型是字符串
* ```$argv```redis命令的参数，可用于处理特殊字符。可选，默认null
  例如```execute('set %s %s', ['key', 'test value'])```
* 返回值：
```
{
    "ret":code,        //状态码，参照[返回状态码说明]
    "msg":msg,         //ret非0是返回错误说明
    "result":
    {
        "type":int     //redis返回类型，参照[redis返回类型]
        "data":data    //不同type对应的返回数据
    }
}
```

### execute_pipeline
pipeline redis命令执行函数接受1个参数：
* ```$commands```是redis命令集合，类型是字符串数组
* ```$argvs```redis命令的参数，可用于处理特殊字符。可选，默认null
例如```execute_pipeline(['set %s %s','get %s'], [['key', 'test value'],['key']])```
* 返回值：
```
{
    "ret":code,        //状态码，参照[返回状态码说明]
    "msg":msg,         //ret非0是返回错误说明
    "results":
    {
        {
            "type":int     //redis返回类型，参照[redis返回类型]
            "data":data    //不同type对应的返回数据
        },
        {
            "type":int     //redis返回类型，参照[redis返回类型]
            "data":data    //不同type对应的返回数据
        },
        ... ...
    }
}
```
返回值包含一组redis操作的返回结果。

### execute_multi
multi redis命令执行函数接受1个参数：
* ```$commands```是redis命令集合，类型是字符串数组
* ```$argvs```，同execute_pipeline中的```$argvs```
* 返回值：
```
{
    "ret":code,        //状态码，参照[返回状态码说明]
    "msg":msg,         //ret非0是返回错误说明
    "errors":
    {
        {
            "type":int     //6，参照[redis返回类型]
            "index":data   //存在错误的redis命令序号
            "msg":msg      //错误信息
        },
        {
            "type":int     //6，参照[redis返回类型]
            "index":data   //存在错误的redis命令序号
            "msg":msg      //错误信息
        },
        ... ...
    }
    "results":
    {
        {
            "type":int     //redis返回类型，参照[redis返回类型]
            "data":data    //不同type对应的返回数据
        },
        {
            "type":int     //redis返回类型，参照[redis返回类型]
            "data":data    //不同type对应的返回数据
        },
        ... ...
    }
}
```
multi和pipeline的不同之处在于，multi处理的是一个事务，当其中一条redis命令执行失败，整个redis事务都会回滚。
返回值中errors和results只会出现其中一个。

### 返回状态码说明
```
 0         操作成功
 -29998    tcp client connect错误
 -29999    创建tcp client错误
 -30000    ip,port链接失败
 -30001    连接超时
 -30002    auth失败
```

### redis返回类型
```
 1    REDIS_REPLY_STRING    字符串型
 2    REDIS_REPLY_ARRAY     数组型
 3    REDIS_REPLY_INTEGER   整数型
 4    REDIS_REPLY_NIL       空值
 5    REDIS_REPLY_STATUS    操作返回
 6    REDIS_REPLY_ERROR     命令错误
```
