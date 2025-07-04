/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/
#pragma once

#include "util/arr.h"
#include "redismodule.h"

#ifndef __cplusplus
#include <stdbool.h>
#endif

///////////////////////////////////////////////////////////////////////////////////////////////

struct QueryError;

#ifdef _DEBUG
#define REDISMODULE_REPLY_DEBUG 1
#endif

struct RedisModule_Reply_StackEntry {
    int count;
    int type; // REDISMODULE_REPLY_ARRAY|MAP|SET
};

typedef struct RedisModule_Reply {
  RedisModuleCtx *ctx;
  bool resp3;
  int count;
  arrayof(struct RedisModule_Reply_StackEntry) stack;
#ifdef REDISMODULE_REPLY_DEBUG
  arrayof(char) json;
#endif
} RedisModule_Reply;

//---------------------------------------------------------------------------------------------

bool RedisModule_HasMap(RedisModule_Reply *reply);
int RedisModule_Reply_LocalCount(RedisModule_Reply *reply);

RedisModule_Reply RedisModule_NewReply(RedisModuleCtx *ctx);
int RedisModule_EndReply(RedisModule_Reply *reply);

int RedisModule_Reply_LongLong(RedisModule_Reply *reply, long long val);
int RedisModule_Reply_Double(RedisModule_Reply *reply, double val);
int RedisModule_Reply_SimpleString(RedisModule_Reply *reply, const char *val);
int RedisModule_Reply_CString(RedisModule_Reply *reply, const char *val);
int RedisModule_Reply_StringBuffer(RedisModule_Reply *reply, const char *val, size_t len);
int RedisModule_Reply_Stringf(RedisModule_Reply *reply, const char *fmt, ...);
int RedisModule_Reply_SimpleStringf(RedisModule_Reply *reply, const char *fmt, ...);
int RedisModule_Reply_String(RedisModule_Reply *reply, const RedisModuleString *val);
int RedisModule_Reply_Null(RedisModule_Reply *reply);
int RedisModule_Reply_Error(RedisModule_Reply *reply, const char *error);
void RedisModule_Reply_QueryError(RedisModule_Reply *reply, struct QueryError *error);
int RedisModule_Reply_Array(RedisModule_Reply *reply);
int RedisModule_Reply_ArrayEnd(RedisModule_Reply *reply);
int RedisModule_Reply_Map(RedisModule_Reply *reply);
int RedisModule_Reply_MapEnd(RedisModule_Reply *reply);
int RedisModule_Reply_Set(RedisModule_Reply *reply);
int RedisModule_Reply_SetEnd(RedisModule_Reply *reply);
int RedisModule_Reply_EmptyArray(RedisModule_Reply *reply);
int RedisModule_Reply_EmptyMap(RedisModule_Reply *reply);

int RedisModule_ReplyKV_LongLong(RedisModule_Reply *reply, const char *key, long long val);
int RedisModule_ReplyKV_Double(RedisModule_Reply *reply, const char *key, double val);
int RedisModule_ReplyKV_SimpleString(RedisModule_Reply *reply, const char *key, const char *val);
int RedisModule_ReplyKV_StringBuffer(RedisModule_Reply *reply, const char *key, const char *val, size_t len);
int RedisModule_ReplyKV_SimpleStringf(RedisModule_Reply *reply, const char *key, const char *fmt, ...);
int RedisModule_ReplyKV_String(RedisModule_Reply *reply, const char *key, const RedisModuleString *val);
int RedisModule_ReplyKV_Null(RedisModule_Reply *reply, const char *key);
int RedisModule_ReplyKV_Array(RedisModule_Reply *reply, const char *key);
int RedisModule_ReplyKV_Map(RedisModule_Reply *reply, const char *key);

/*
 * This function is a workaround helper for replying with a string that may contain
 * newlines or other characters that are not safe for RESP Simple Strings.
 * Should be removed once we can replace all SimpleString replies with BulkString replies.
 */
static inline bool isUnsafeForSimpleString(const char *str) {
  return strpbrk(str, "\r\n") != NULL;
}
/*
 * This function is a workaround helper for replying with a string that may contain
 * newlines or other characters that are not safe for RESP Simple Strings.
 * Should be removed once we can replace all SimpleString replies with BulkString replies.
 */
char *escapeSimpleString(const char *str);

///////////////////////////////////////////////////////////////////////////////////////////////
