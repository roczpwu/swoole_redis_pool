/*
  +----------------------------------------------------------------------+
  | Redis_Coro                                                           |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | rocwu@tencent.com so we can mail you a copy immediately.             |
  +----------------------------------------------------------------------+
  | Author: rocwu  <rocwu@tencent.com>                                   |
  +----------------------------------------------------------------------+
*/

/************************************************************************
错误码整理
 -29998    tcp client connect错误
 -29999    创建tcp client错误
 -30000    ip,port链接失败
 -30001    连接超时
 -30002    auth失败
************************************************************************/
#define PHP_AUTH_ERR -30002
#define PHP_CONN_ERR -29998
#define PHP_TCP_ERR  -29999

extern "C" {

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_redis_coro.h"

#define SW_COROUTINE

#include "swoole/swoole.h"
#include "swoole/Server.h"
#include "swoole/Client.h"
#include "swoole/coroutine.h"
#include "swoole/php_swoole.h"
#include "swoole/swoole_coroutine.h"

#include "hiredis/hiredis.h"
#include "hiredis/async.h"
}

#include <vector>
#include <map>
#include <deque>


static int le_redis_coro;

struct redis_coro_object;

enum connection_status {
    CONN_UESTABLISHED = 0,
    AUTH_UNDERPASS,
    CONN_AVAIABLE,
    CONN_CLOSED,
};

struct redis_connection {
    connection_status status;
    swClient *client;
};

#define close_redis_connection(connection) \
    do { \
        swClient_free(connection->client); \
        free(connection); \
    } while(0)

struct redis_request {
    int type;       // 1:execute   2:pipeline    3:multi
    int size;
    bool is_timeout;
    sds tx_buffer;
    sds rx_buffer;
    swTimer_node *timeout_node;
    php_context *context;
    redis_coro_object *redis_coro_ptr;
};

#define free_redis_request(request) \
    do { \
        if (request->tx_buffer != NULL) \
            sdsfree(request->tx_buffer);\
        if (request->rx_buffer != NULL) \
            sdsfree(request->rx_buffer);\
        free(request); \
    } while(0) \

struct redis_coro_object {
    swString *ip;       // redis ip
    int port;           // redis port
    int timeout;        // 超时时间，默认2s，在__construct中给定
    swString *password; // 密码，可以为空
    int pool_size;      // 等待期间的redis连接数
    int max_active;     // 峰值时候的redis连接数
    std::map<int, redis_connection*> *m_busy_connection;    // 正在提供服务的连接
    std::map<int, redis_connection*> *m_idle_connection;    // 闲置的可用连接
    std::map<int, redis_connection*> *m_invalid_connection; // 暂时不可用连接，未完成连接或未鉴权
    std::deque<redis_request*> *q_waiting_request;          // 等待中的任务
    std::map<int, redis_request*> *m_fd_request;            // fd->任务 的map
    zend_object std;
};

#define GET_REDIS_OBJECT_P(object) ((redis_coro_object *)((char *)(object) - XtOffsetOf(redis_coro_object, std)))

zend_class_entry *redis_class_entry;
static zend_object_handlers redis_object_handlers;

//-------------------------------------------------------------------------------------
void test_coro_onTimeout(swTimer *timer, swTimer_node *tnode) {
    php_context *context = (php_context *)tnode->data;
    zval *zdata, *retval = NULL;
    zval _zdata;
    zdata = &_zdata;
    array_init(zdata);
    add_assoc_long(zdata, "ret", -30001);
    int ret = coro_resume(context, zdata, &retval);
    if (ret == CORO_END && ret) {
        zval_ptr_dtor(retval);
    }
    zval_ptr_dtor(zdata);
    efree(context);
}

PHP_FUNCTION(redis_coro_test)
{
    php_swoole_check_reactor();
    php_context *context = (php_context *)emalloc(sizeof(php_context));
    php_swoole_check_timer(2);
    SwooleG.timer.add(&SwooleG.timer, 2*1000, 0, context, test_coro_onTimeout);
    coro_save(context);
    coro_yield();
}
//-------------------------------------------------------------------------------------

void redis_coro_onTimeout(swTimer *timer, swTimer_node *tnode)
{
    //printf("timeout\n");
    php_context *context = (php_context *)tnode->data;
    redis_request *request = (redis_request *)context->coro_params.value.ptr;
    zval *zdata, *retval = NULL;
    zval _zdata;
    zdata = &_zdata;
    array_init(zdata);
    add_assoc_long(zdata, "ret", -30001);
    int ret = coro_resume(context, zdata, &retval);
    if (ret == CORO_END && ret) {
        zval_ptr_dtor(retval);
    }
    zval_ptr_dtor(zdata);
    efree(context);
    request->is_timeout = true;
}

void resolve_from_reply(redisReply* reply, zval *z_result) {
    int type = reply->type;
    add_assoc_long(z_result, "type", type);
    if(type == 1) {             // REDIS_REPLY_STRING
        add_assoc_stringl(z_result, "data", reply->str, reply->len);
    } else if (type == 2) {     // REDIS_REPLY_ARRAY
        int element_cnt = reply->elements;
        zval z_arr;
        array_init(&z_arr);
        for (int i = 0; i < element_cnt; ++i) {
            //默认都是string，有变化将来再改
            redisReply* element = (redisReply*)(reply->element)[i];
            add_next_index_stringl(&z_arr, element->str, element->len);
        }
        add_assoc_zval(z_result, "data", &z_arr);
    } else if (type == 3) {     // REDIS_REPLY_INTEGER
        add_assoc_long(z_result, "data", reply->integer);
    } else if (type == 4) {     // REDIS_REPLY_NIL
        add_assoc_null(z_result, "data");
    } else if (type == 5) {     // REDIS_REPLY_STATUS
        add_assoc_stringl(z_result, "msg", reply->str, reply->len);
    } else if (type == 6) {     // REDIS_REPLY_ERROR
        add_assoc_stringl(z_result, "msg", reply->str, reply->len);
    }
}

redis_request* get_waiting_request(redis_coro_object *redis_coro_ptr) {
    std::deque<redis_request*> *q_waiting_request = redis_coro_ptr->q_waiting_request;
    redis_request *request = NULL;
    while(q_waiting_request->size() > 0 && request == NULL) {
        request = q_waiting_request->back();
        q_waiting_request->pop_back();
        if (request->is_timeout) {
            free_redis_request(request);
            request = NULL;
        }
    }
    return request;
}

void redis_onReceiveData(swClient *cli, char *data, uint32_t length) {
    //printf("on receive data fd %d\n", cli->socket->fd);
    //printf("on receive data length %d\n", length);
    int check_length = 0;
    redis_coro_object *redis_coro_ptr = (redis_coro_object*)cli->object;
    int fd = cli->socket->fd;
    redis_request *request = (*(redis_coro_ptr->m_fd_request))[fd];
    if (request->rx_buffer == NULL) {
        request->rx_buffer = sdsempty();
    }
    request->rx_buffer = sdscatlen(request->rx_buffer,data,length);
    int size = request->size;
    bool complete = true;
    redisReader *reader = NULL;
    void *reply = NULL;
    reader = redisReaderCreate();
    int ret = redisReaderFeed(reader, request->rx_buffer, sdslen(request->rx_buffer)+1);
    for (int i = 0; i < size && length > check_length; ++i) {
        if (ret != 0) {
            complete = false;
            break;
        }
        ret = redisReaderGetReply(reader, &reply);
        if (reply == NULL) {
            complete = false;
            break;
        }
        freeReplyObject(reply);
        reply = NULL;
    }
    if (complete) { //收到完整的数据包
        // 释放请求资源和连接
        swTimer_del(&SwooleG.timer, request->timeout_node);
        redis_coro_ptr->m_fd_request->erase(fd);
        redis_connection *connection = (*(redis_coro_ptr->m_busy_connection))[fd];

        redis_request *next_request = get_waiting_request(redis_coro_ptr);
        if (next_request != NULL) {
            // 队列有等待的请求，直接用来处理下一个请求
            (*(redis_coro_ptr->m_fd_request))[fd] = next_request;
            int ret = cli->send(cli, next_request->tx_buffer, sdslen(next_request->tx_buffer), 0);
        } else {
            redis_coro_ptr->m_busy_connection->erase(fd);
            (*(redis_coro_ptr->m_idle_connection))[fd] = connection;
            while (redis_coro_ptr->m_idle_connection->size()
                + redis_coro_ptr->m_busy_connection->size()
                + redis_coro_ptr->m_invalid_connection->size()
                > redis_coro_ptr->pool_size 
            && redis_coro_ptr->m_idle_connection->size() > 0) {
                std::map<int,redis_connection*>::iterator it = redis_coro_ptr->m_idle_connection->begin();
                close_redis_connection(it->second);
                redis_coro_ptr->m_idle_connection->erase(it);
            }
        }
        if (length > check_length) {
            redisReaderFree(reader);
            reader = redisReaderCreate();
            redisReaderFeed(reader, request->rx_buffer, sdslen(request->rx_buffer)+1);
        }
        if (!request->is_timeout) {
            // 未超时，把数据返回php
            php_context *context = request->context;
            zval *zdata, *retval = NULL;
            zval _zdata;
            zdata = &_zdata;
            array_init(zdata);
            add_assoc_long(zdata, "ret", 0);
            if (request->type == 1) {
                void *reply = NULL;
                redisReaderGetReply(reader, &reply);
                zval z_result;
                array_init(&z_result);
                resolve_from_reply((redisReply*)reply, &z_result);
                add_assoc_zval(zdata, "result", &z_result);
                freeReplyObject(reply);
            } else if (request->type == 2) {
                zval z_results;
                array_init(&z_results);
                add_assoc_zval(zdata, "results", &z_results);
                for (int i = 0; i < request->size; i++) {
                    void *reply = NULL;
                    redisReaderGetReply(reader, &reply);
                    zval z_result;
                    array_init(&z_result);
                    resolve_from_reply((redisReply*)reply, &z_result);
                    add_next_index_zval(&z_results, &z_result);
                    freeReplyObject(reply);
                }
            } else if (request->type == 3) {
                //TODO: multi
                zval z_results;
                array_init(&z_results);
                //add_assoc_zval(zdata, "results", &z_results);
                void *reply = NULL;
                redisReaderGetReply(reader, &reply);
                freeReplyObject(reply);
                bool has_error = false;
                for (int i = 0; i < request->size-1; i++) {
                    void *reply = NULL;
                    redisReaderGetReply(reader, &reply);
                    redisReply *r = (redisReply*)reply;
                    if (i < request->size-2) {
                        if (r->type == 6) {
                            zval z_result;
                            array_init(&z_result);
                            add_assoc_long(&z_result, "type", 6);
                            add_assoc_long(&z_result, "index", i);
                            add_assoc_stringl(&z_result, "msg", r->str, r->len);
                            add_next_index_zval(&z_results, &z_result);
                            has_error = true;
                        }
                    } else {
                        if (has_error) {
                            goto free_muti_reply;
                        }
                        for (int i = 0; i < r->elements; ++i) {
                            redisReply *sub_reply = r->element[i];
                            zval z_result;
                            array_init(&z_result);
                            resolve_from_reply((redisReply*)sub_reply, &z_result);
                            add_next_index_zval(&z_results, &z_result);
                        }
                    }
                    free_muti_reply:
                    freeReplyObject(reply);
                }
                if (has_error)
                    add_assoc_zval(zdata, "errors", &z_results);
                else
                    add_assoc_zval(zdata, "results", &z_results);
            }
            free_redis_request(request);
            redisReaderFree(reader);
            int ret = coro_resume(context, zdata, &retval);
            if (ret == CORO_END && ret) {
                zval_ptr_dtor(retval);
            }
            zval_ptr_dtor(zdata);
            efree(context);
        } else {
            //已经超时的请求，直接抛掉
            free_redis_request(request);
            redisReaderFree(reader);
        }
    }
}

void redis_onReceiveAuth(swClient *cli, char *data, uint32_t length) {
    //printf("on receive auth fd %d\n", cli->socket->fd);
    int fd = cli->socket->fd;
    redis_coro_object *redis_coro_ptr = (redis_coro_object*)cli->object;
    redis_connection *connection = (*(redis_coro_ptr->m_invalid_connection))[fd];
    //这边直接收鉴权结果，数据不会很长，暂时不处理分包
    sds sds_recv = sdsempty();
    sds_recv = sdscatlen(sds_recv,data,length);
    redisReader *reader = NULL;
    void *reply = NULL;
    reader = redisReaderCreate();
    int ret = redisReaderFeed(reader, sds_recv, sdslen(sds_recv));
    ret = redisReaderGetReply(reader, &reply);
    sdsfree(sds_recv);
    redisReply *r = (redisReply*)reply;
    if (r->type != REDIS_REPLY_STATUS) {
        //鉴权失败，1、关闭连接，2、返回错误信息
        redis_coro_ptr->m_invalid_connection->erase(fd);
        close_redis_connection(connection);
        freeReplyObject(reply);
        redisReaderFree(reader);
        std::deque<redis_request*> *q_waiting_request = redis_coro_ptr->q_waiting_request;
        if (q_waiting_request->size() > 0) {
            redis_request *request = q_waiting_request->back();
            q_waiting_request->pop_back();
            swTimer_del(&SwooleG.timer, request->timeout_node);
            php_context *context = request->context;
            zval *zdata, *retval = NULL;
            zval _zdata;
            zdata = &_zdata;
            array_init(zdata);
            add_assoc_long(zdata, "ret", PHP_AUTH_ERR);
            int ret = coro_resume(context, zdata, &retval);
            if (ret == CORO_END && ret) {
                zval_ptr_dtor(retval);
            }
            zval_ptr_dtor(zdata);
            efree(context);
            free_redis_request(request);
            return;
        }
    } else {
        freeReplyObject(reply);
        redisReaderFree(reader);
        redis_coro_ptr->m_invalid_connection->erase(fd);
        cli->onReceive = redis_onReceiveData;
        redis_request *request = get_waiting_request(redis_coro_ptr);
        if (request != NULL) {
            // 有未超时的待处理节点，放在busy连接池，并处理请求
            (*(redis_coro_ptr->m_busy_connection))[fd] = connection;
            (*(redis_coro_ptr->m_fd_request))[fd] = request;
            int ret = cli->send(cli, request->tx_buffer, strlen(request->tx_buffer), 0);
            //TODO: 处理发送失败的逻辑
        } else {
            // 没有未超时的待处理节点，直接把连接放在idle连接池
            (*(redis_coro_ptr->m_idle_connection))[fd] = connection;
        }
    }
    
}

void redis_onConnect(swClient *cli) {
    //printf("on connect fd %d\n", cli->socket->fd);
    redis_coro_object *redis_coro_ptr = (redis_coro_object*)cli->object;
    if (swString_length(redis_coro_ptr->password) > 0) {
        //require password，先发送一条auth消息
        cli->onReceive = redis_onReceiveAuth;
        char *cmdBuf = NULL;
        char *cmd = (char*)malloc(sizeof(char)*(5+redis_coro_ptr->password->length+1));
        memcpy(cmd, "auth ", 5);
        memcpy(cmd + 5, redis_coro_ptr->password->str, redis_coro_ptr->password->length);
        cmd[5+redis_coro_ptr->password->length] = '\0';
        redisFormatCommand(&cmdBuf, cmd);
        free(cmd);
        int ret = cli->send(cli, cmdBuf, strlen(cmdBuf), 0);
        free(cmdBuf);
        if (ret < 0) {
            std::map<int, redis_connection*>::iterator it = redis_coro_ptr->m_invalid_connection->find(cli->socket->fd);
            close_redis_connection((*(redis_coro_ptr->m_invalid_connection))[it->first]);
            redis_coro_ptr->m_invalid_connection->erase(it);
        }
    } else {
        //not require password，发送等待队列的请求
        cli->onReceive = redis_onReceiveData;
        redis_connection *connection = (*(redis_coro_ptr->m_invalid_connection))[cli->socket->fd];
        redis_coro_ptr->m_invalid_connection->erase(cli->socket->fd);
        std::deque<redis_request*> *q_waiting_request = redis_coro_ptr->q_waiting_request;
        redis_request *request = NULL;
        while(q_waiting_request->size() > 0 && request == NULL) {
            request = q_waiting_request->back();
            q_waiting_request->pop_back();
            if (request->is_timeout) {
                free_redis_request(request);
                request = NULL;
            }
        }
        if (request != NULL) {
            // 有未超时的待处理节点，放在busy连接池，并处理请求
            (*(redis_coro_ptr->m_busy_connection))[cli->socket->fd] = connection;
            (*(redis_coro_ptr->m_fd_request))[cli->socket->fd] = request;
            int ret = cli->send(cli, request->tx_buffer, strlen(request->tx_buffer), 0);
            //TODO: 处理发送失败的逻辑
        } else {
            // 没有未超时的待处理节点，直接把连接放在idle连接池
            (*(redis_coro_ptr->m_idle_connection))[cli->socket->fd] = connection;
        }
    }
}

void redis_onClose(swClient *cli) {
    //printf("on close fd %d\n", cli->socket->fd);
    free(cli);
}

void redis_onError(swClient *cli) {
    //printf("on error fd %d\n", cli->socket->fd);
    free(cli);
}

php_context* send_command(redis_coro_object *redis_coro_ptr, char** commands, void*** commands_argv, int size, int type, int& code) {
    code = 0;
    sds command_sds = sdsempty();
    for (int i = 0; i < size; i++) {
        char* cmdBuf = NULL;
        void** command_argv = NULL;
        if (commands_argv != NULL)
            command_argv = commands_argv[i];
        //printf("%s\n", commands[i]);
        redisFormatCommandArgvv(&cmdBuf, commands[i], command_argv);
        command_sds = sdscatlen(command_sds,cmdBuf,strlen(cmdBuf));
        free(cmdBuf);
    }
    delete(commands);
    if (commands_argv != NULL) {
        for (int i = 0; i < size; ++i) {
            if (commands_argv[i] != NULL)
                delete(commands_argv[i]);
        }
        delete(commands_argv);
    }
    redis_request *request  = (redis_request *)malloc(sizeof(redis_request));
    request->type           = type;
    request->size           = size;
    request->is_timeout     = false;
    request->tx_buffer      = command_sds;
    request->rx_buffer      = NULL;
    request->redis_coro_ptr = redis_coro_ptr;
    php_swoole_check_reactor();
    php_context *context = (php_context *)emalloc(sizeof(php_context));
    context->coro_params.value.ptr = (void *)request;
    request->context = context;
    php_swoole_check_timer(redis_coro_ptr->timeout * 1000);
    request->timeout_node = SwooleG.timer.add(&SwooleG.timer, redis_coro_ptr->timeout * 1000, 0, context, redis_coro_onTimeout);
    std::map<int, redis_connection*>::iterator it = redis_coro_ptr->m_idle_connection->begin();
    if (it != redis_coro_ptr->m_idle_connection->end()) {
        // 有空闲的redis连接，直接使用
        redis_connection *connection = it->second;
        (*(redis_coro_ptr->m_busy_connection))[it->first] = it->second;
        redis_coro_ptr->m_idle_connection->erase(it);
        swClient *cli = connection->client;
        (*(redis_coro_ptr->m_fd_request))[it->first] = request;
        int ret = cli->send(cli, request->tx_buffer, strlen(request->tx_buffer), 0);
        //TODO: 处理发送失败的逻辑
    } else {
        // 没有空闲的redis连接，加入任务列表，并创建一个连接
        redis_coro_ptr->q_waiting_request->push_front(request);
        // 连接数已达上限
        if (redis_coro_ptr->m_idle_connection->size()
            + redis_coro_ptr->m_busy_connection->size()
            + redis_coro_ptr->m_invalid_connection->size()
            >= redis_coro_ptr->max_active) {
            return context;
        }
        swClient *cli = (swClient *)malloc(sizeof(swClient));
        if (swClient_create(cli, SW_SOCK_TCP, 1) < 0) {
            swClient_free(cli);
            free(cli);
            swTimer_del(&SwooleG.timer, request->timeout_node);
            efree(context);
            redis_coro_ptr->q_waiting_request->pop_back();
            free_redis_request(request);
            code = PHP_TCP_ERR;
            return NULL;
        }
        cli->onConnect = redis_onConnect;
        //cli->onReceive = redis_onReceive;
        cli->onClose = redis_onClose;
        cli->onError = redis_onError;
        cli->reactor_fdtype = SW_FD_STREAM_CLIENT;
        cli->object = (void*)redis_coro_ptr;
        //printf("connect %d\n", cli->socket->fd);
        if (cli->connect(cli, redis_coro_ptr->ip->str, redis_coro_ptr->port, 1, 1)) {
            swClient_free(cli);
            free(cli);
            swTimer_del(&SwooleG.timer, request->timeout_node);
            efree(context);
            redis_coro_ptr->q_waiting_request->pop_back();
            free_redis_request(request);
            code = PHP_CONN_ERR;
            return NULL;
        }
        redis_connection *connection = (redis_connection*)malloc(sizeof(redis_connection));
        connection->client = cli;
        connection->status = CONN_UESTABLISHED;
        (*(redis_coro_ptr->m_invalid_connection))[cli->socket->fd] = connection;
    }
    return context;
}

PHP_METHOD(redis_coro, __construct)
{
    char *ip, *password = NULL;
    long ip_len, port, timeout = 2, password_len=0;
    if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,"sl|ls", &ip, &ip_len, &port, &timeout, &password, &password_len) == FAILURE) {
        RETURN_FALSE;
    }
    zval *object = getThis();
    redis_coro_object *redis_coro_ptr = GET_REDIS_OBJECT_P(Z_OBJ_P(object));
    redis_coro_ptr->ip         = swString_dup(ip, ip_len);
    redis_coro_ptr->port       = (int)port;
    redis_coro_ptr->timeout    = (int)timeout;
    redis_coro_ptr->pool_size  = 10; 
    redis_coro_ptr->max_active = 10; 
    redis_coro_ptr->password = password == NULL ? swString_dup("", 0) : swString_dup(password, password_len);
    redis_coro_ptr->m_busy_connection    = new std::map<int, redis_connection*>();
    redis_coro_ptr->m_idle_connection    = new std::map<int, redis_connection*>();
    redis_coro_ptr->m_invalid_connection = new std::map<int, redis_connection*>();
    redis_coro_ptr->q_waiting_request    = new std::deque<redis_request*>();
    redis_coro_ptr->m_fd_request         = new std::map<int, redis_request*>();
}

PHP_METHOD(redis_coro, __destruct)
{
    zval *object = getThis();
    redis_coro_object *redis_coro_ptr = GET_REDIS_OBJECT_P(Z_OBJ_P(object));
    swString_free(redis_coro_ptr->ip);
    swString_free(redis_coro_ptr->password);
    //TODO: 关闭已经打开的链接，并释放资源 
    while(redis_coro_ptr->m_busy_connection->size() > 0) {
        std::map<int, redis_connection*>::iterator it = redis_coro_ptr->m_busy_connection->begin();
        redis_connection *connection = it->second;
        close_redis_connection(connection);
        redis_coro_ptr->m_busy_connection->erase(it);
    }
    while(redis_coro_ptr->m_idle_connection->size() > 0) {
        std::map<int, redis_connection*>::iterator it = redis_coro_ptr->m_idle_connection->begin();
        redis_connection *connection = it->second;
        close_redis_connection(connection);
        redis_coro_ptr->m_idle_connection->erase(it);
    }
    while(redis_coro_ptr->m_invalid_connection->size() > 0) {
        std::map<int, redis_connection*>::iterator it = redis_coro_ptr->m_invalid_connection->begin();
        redis_connection *connection = it->second;
        close_redis_connection(connection);
        redis_coro_ptr->m_invalid_connection->erase(it);
    }
    delete(redis_coro_ptr->m_busy_connection);
    delete(redis_coro_ptr->m_idle_connection);
    delete(redis_coro_ptr->m_invalid_connection);
    // 清除任务列表，释放内部资源
    while(redis_coro_ptr->q_waiting_request->size() > 0) {
        redis_request *request = redis_coro_ptr->q_waiting_request->back();
        redis_coro_ptr->q_waiting_request->pop_back();
        free_redis_request(request);
    }
    delete(redis_coro_ptr->q_waiting_request);
    delete(redis_coro_ptr->m_fd_request);
}

PHP_METHOD(redis_coro, setPool)
{
    long pool_size, max_active = 0;
    if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,"l|l", &pool_size, &max_active) == FAILURE) {
        RETURN_FALSE;
    }
    if (pool_size < 1)
        pool_size = 10;
    if (max_active < pool_size)
        max_active = pool_size;
    zval *object = getThis();
    redis_coro_object *redis_coro_ptr = GET_REDIS_OBJECT_P(Z_OBJ_P(object));
    redis_coro_ptr->pool_size = pool_size;
    redis_coro_ptr->max_active = max_active;
}

PHP_METHOD(redis_coro, execute)
{
    char *command;
    long command_len;
    zval *z_argv = NULL;
    if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,"s|a", &command, &command_len, &z_argv) == FAILURE) {
        RETURN_FALSE;
    }
    zval *object = getThis();
    redis_coro_object *redis_coro_ptr = GET_REDIS_OBJECT_P(Z_OBJ_P(object));
    char** commands = new char*[1];
    commands[0] = command;
    HashTable *ht_argv = z_argv==NULL?NULL:Z_ARRVAL_P(z_argv);
    int argv_c = ht_argv==NULL?0:zend_hash_num_elements(ht_argv);
    char*** argv = new char**[1];
    argv[0] = new char*[argv_c];
    for (int i = 0; i < argv_c; i++) {
        argv[0][i] = Z_STRVAL_P(zend_hash_get_current_data(ht_argv));
        zend_hash_move_forward(ht_argv);
    }
    int code = 0;
    php_context *context = send_command(redis_coro_ptr, commands, (void***)argv,1, 1, code);
    if (code != 0) {
        array_init(return_value);
        add_assoc_long(return_value, "ret", code);
        return;
    }
    coro_save(context);
    coro_yield();
}

PHP_METHOD(redis_coro, execute_pipeline)
{
    zval *z_commands;
    zval *z_commands_argv = NULL;
    if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,"a|a", &z_commands, &z_commands_argv) == FAILURE) {
        RETURN_FALSE;
    }
    zval *object = getThis();
    redis_coro_object *redis_coro_ptr = GET_REDIS_OBJECT_P(Z_OBJ_P(object));
    if (Z_TYPE_P(z_commands) != IS_ARRAY)
        RETURN_FALSE;
    HashTable *ht_commands = Z_ARRVAL_P(z_commands);
    int batch_count = zend_hash_num_elements(ht_commands);
    zend_hash_internal_pointer_reset(ht_commands);
    char** commands = new char*[batch_count];
    HashTable *ht_commands_argv = z_commands_argv==NULL?NULL:Z_ARRVAL_P(z_commands_argv);
    int commands_argc = ht_commands_argv==NULL?0:zend_hash_num_elements(ht_commands_argv);
    char*** commands_argv = commands_argc==0?NULL:new char**[batch_count];
    for (int i = 0; i < batch_count; i++) {
        zval *z_command = zend_hash_get_current_data(ht_commands);
        if (Z_TYPE_P(z_command) == IS_STRING) {
            commands[i] = Z_STRVAL_P(z_command);
        } else {
            RETURN_FALSE;
        }
        zend_hash_move_forward(ht_commands);
        if (commands_argv == NULL)
            continue;
        if (i<commands_argc) {
            zval *z_command_argv = zend_hash_get_current_data(ht_commands_argv);
            if (Z_TYPE_P(z_command_argv) == IS_NULL) {
                commands_argv[i]=NULL;
                continue;
            } else if (Z_TYPE_P(z_command_argv) == IS_ARRAY) {
                HashTable *ht_command_argv = Z_ARRVAL_P(z_command_argv);
                int command_argc = zend_hash_num_elements(ht_command_argv);
                commands_argv[i]=new char*[command_argc];
                for (int j = 0; j < command_argc; j++) {
                    commands_argv[i][j] = Z_STRVAL_P(zend_hash_get_current_data(ht_command_argv));
                    zend_hash_move_forward(ht_command_argv);
                }
            } else {
                RETURN_FALSE;
            }
        } else {
            commands_argv[i]=NULL;
        }
        zend_hash_move_forward(ht_commands_argv);
    }
    int code = 0;
    php_context *context = send_command(redis_coro_ptr, commands, (void***)commands_argv, batch_count, 2, code);
    if (code != 0) {
        array_init(return_value);
        add_assoc_long(return_value, "ret", code);
        return;
    }
    coro_save(context);
    coro_yield();
}

PHP_METHOD(redis_coro, execute_multi)
{
    zval *z_commands;
    zval *z_commands_argv = NULL;
    if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,"a|a", &z_commands, &z_commands_argv) == FAILURE) {
        RETURN_FALSE;
    }
    zval *object = getThis();
    redis_coro_object *redis_coro_ptr = GET_REDIS_OBJECT_P(Z_OBJ_P(object));
    if (Z_TYPE_P(z_commands) != IS_ARRAY)
        RETURN_FALSE;
    HashTable *ht_commands = Z_ARRVAL_P(z_commands);
    int batch_count = zend_hash_num_elements(ht_commands);
    zend_hash_internal_pointer_reset(ht_commands);
    char** commands = new char*[batch_count+2];

    HashTable *ht_commands_argv = z_commands_argv==NULL?NULL:Z_ARRVAL_P(z_commands_argv);
    int commands_argc = ht_commands_argv==NULL?0:zend_hash_num_elements(ht_commands_argv);
    char*** commands_argv = commands_argc==0?NULL:new char**[batch_count+2];
    commands[0]             = (char*)"MULTI";
    commands[batch_count+1] = (char*)"EXEC";
    if (commands_argv != NULL) {
        commands_argv[0]                = NULL;
        commands_argv[batch_count+1]    = NULL;
    }
    for (int i = 0; i < batch_count; i++) {
        zval *z_command = zend_hash_get_current_data(ht_commands);
        if (Z_TYPE_P(z_command) == IS_STRING) {
            commands[i+1] = Z_STRVAL_P(z_command);
        } else {
            RETURN_FALSE;
        }
        zend_hash_move_forward(ht_commands);
        if (commands_argv == NULL)
            continue;
        if (i<commands_argc) {
            zval *z_command_argv = zend_hash_get_current_data(ht_commands_argv);
            if (Z_TYPE_P(z_command_argv) == IS_NULL) {
                commands_argv[i+1]=NULL;
                continue;
            } else if (Z_TYPE_P(z_command_argv) == IS_ARRAY) {
                HashTable *ht_command_argv = Z_ARRVAL_P(z_command_argv);
                int command_argc = zend_hash_num_elements(ht_command_argv);
                commands_argv[i+1]=new char*[command_argc];
                for (int j = 0; j < command_argc; j++) {
                    commands_argv[i+1][j] = Z_STRVAL_P(zend_hash_get_current_data(ht_command_argv));
                    zend_hash_move_forward(ht_command_argv);
                }
            } else {
                RETURN_FALSE;
            }
        } else {
            commands_argv[i+1]=NULL;
        }
        zend_hash_move_forward(ht_commands_argv);
    }
    int code = 0;
    php_context *context = send_command(redis_coro_ptr, commands, (void***)commands_argv, batch_count+2, 3, code);
    if (code != 0) {
        array_init(return_value);
        add_assoc_long(return_value, "ret", code);
        return;
    }
    coro_save(context);
    coro_yield();
}

static zend_function_entry redis_class_methods[] = {
    PHP_ME(redis_coro, __construct, NULL, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
    PHP_ME(redis_coro, __destruct, NULL, ZEND_ACC_PUBLIC | ZEND_ACC_DTOR)
    PHP_ME(redis_coro, setPool, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(redis_coro, execute, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(redis_coro, execute_pipeline, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(redis_coro, execute_multi, NULL, ZEND_ACC_PUBLIC)
    {NULL,NULL,NULL}
};

zend_object *redis_coro_create_handler(zend_class_entry *ce TSRMLS_DC) 
{
    redis_coro_object *obj = (redis_coro_object *)emalloc(sizeof(redis_coro_object) + zend_object_properties_size(ce));
    memset(obj,0,sizeof(redis_coro_object) + zend_object_properties_size(ce));
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &redis_object_handlers;
    return &obj->std;
}

static void redis_free_storage(zend_object *object TSRMLS_DC) {
    redis_coro_object *obj = GET_REDIS_OBJECT_P(object);
    zend_object_std_dtor(object);
}

static void redis_destroy_storage(zend_object *object TSRMLS_DC) {
    redis_coro_object *obj = GET_REDIS_OBJECT_P(object);
    zend_objects_destroy_object(object);
}

PHP_MINIT_FUNCTION(redis_coro)
{
    zend_class_entry ce;
    INIT_CLASS_ENTRY(ce, "redis_coro", redis_class_methods);
    redis_class_entry = zend_register_internal_class(&ce TSRMLS_CC);
    redis_class_entry->create_object = redis_coro_create_handler;
    memcpy(&redis_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    redis_object_handlers.clone_obj = NULL;
    redis_object_handlers.free_obj = redis_free_storage;
    redis_object_handlers.dtor_obj = redis_destroy_storage;
    redis_object_handlers.offset = XtOffsetOf(redis_coro_object, std);
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(redis_coro)
{
    return SUCCESS;
}

PHP_RINIT_FUNCTION(redis_coro)
{
#if defined(COMPILE_DL_REDIS_CORO) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(redis_coro)
{
    return SUCCESS;
}

PHP_MINFO_FUNCTION(redis_coro)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "redis_coro support", "enabled");
    php_info_print_table_end();
}

const zend_function_entry redis_coro_functions[] = {
    PHP_FE(redis_coro_test, NULL)
    PHP_FE_END  /* Must be the last line in redis_coro_functions[] */
};

zend_module_entry redis_coro_module_entry = {
    STANDARD_MODULE_HEADER,
    "redis_coro",
    redis_coro_functions,
    PHP_MINIT(redis_coro),
    PHP_MSHUTDOWN(redis_coro),
    PHP_RINIT(redis_coro),      /* Replace with NULL if there's nothing to do at request start */
    PHP_RSHUTDOWN(redis_coro),  /* Replace with NULL if there's nothing to do at request end */
    PHP_MINFO(redis_coro),
    PHP_REDIS_CORO_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_REDIS_CORO
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(redis_coro)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
