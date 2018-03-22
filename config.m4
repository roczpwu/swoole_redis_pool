PHP_ARG_ENABLE(redis_coro, whether to enable redis_coro support,
  [  --enable-redis_coro           Enable redis_coro support])

if test "$PHP_REDIS_CORO" != "no"; then

  PHP_REQUIRE_CXX()
  PHP_ADD_LIBRARY(stdc++, "", EXTRA_LDFLAGS)

  PHP_ADD_INCLUDE(./include)

  PHP_NEW_EXTENSION(redis_coro, \
    redis_coro.cpp \
    include/hiredis/async.c \
    include/hiredis/dict.c \
    include/hiredis/hiredis.c \
    include/hiredis/net.c \
    include/hiredis/read.c \
    include/hiredis/sds.c \
    , $ext_shared,, -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1)
fi
