/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/
#ifndef SRC_REDISEARCH_API_H_
#define SRC_REDISEARCH_API_H_

#include "redismodule.h"
#include <limits.h>
#include "fork_gc.h"
#include "rules.h"
#include "info/indexes_info.h"
#include "obfuscation/hidden.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REDISEARCH_CAPI_VERSION 1

#ifdef REDISEARCH_API_EXTERN
#define MODULE_API_FUNC(T, N) extern T(*N)
#else
#define MODULE_API_FUNC(T, N) T N
#endif

typedef struct RefManager RSIndex;
typedef size_t RSFieldID;
#define RSFIELD_INVALID SIZE_MAX

typedef struct Document RSDoc;
typedef struct RSQueryNode RSQNode;
typedef struct RS_ApiIter RSResultsIterator;
typedef struct RSIdxOptions RSIndexOptions;

#define RSVALTYPE_NOTFOUND 0
#define RSVALTYPE_STRING 1
#define RSVALTYPE_DOUBLE 2

#define RSRANGE_INF (1.0 / 0.0)
#define RSRANGE_NEG_INF (-1.0 / 0.0)

#define RSLECRANGE_INF NULL
#define RSLEXRANGE_NEG_INF NULL

#define RSQNTYPE_INTERSECT 1
#define RSQNTYPE_UNION 2
#define RSQNTYPE_TOKEN 3
#define RSQNTYPE_NUMERIC 4
#define RSQNTYPE_NOT 5
#define RSQNTYPE_OPTIONAL 6
#define RSQNTYPE_GEO 7
#define RSQNTYPE_PREFX 8
#define RSQNTYPE_TAG 11
#define RSQNTYPE_FUZZY 12
#define RSQNTYPE_LEXRANGE 13

#define RSFLDTYPE_DEFAULT 0x00
#define RSFLDTYPE_FULLTEXT 0x01
#define RSFLDTYPE_NUMERIC 0x02
#define RSFLDTYPE_GEO 0x04
#define RSFLDTYPE_TAG 0x08
#define RSFLDTYPE_VECTOR 0x10
// TODO: GEOMETRY #define RSFLDTYPE_GEOMETRY 0x20

#define RSFLDOPT_NONE 0x00
#define RSFLDOPT_SORTABLE 0x01
#define RSFLDOPT_NOINDEX 0x02
#define RSFLDOPT_TXTNOSTEM 0x04
#define RSFLDOPT_TXTPHONETIC 0x08
#define RSFLDOPT_WITHSUFFIXTRIE 0x10

// This enum copies
typedef enum {
  RS_GEO_DISTANCE_INVALID = -1,
  RS_GEO_DISTANCE_KM,
  RS_GEO_DISTANCE_M,
  RS_GEO_DISTANCE_FT,
  RS_GEO_DISTANCE_MI,
} RSGeoDistance;

typedef int (*RSGetValueCallback)(void* ctx, const char* fieldName, const void* id, char** strVal,
                                  double* doubleVal);

MODULE_API_FUNC(int, RediSearch_GetCApiVersion)();

#define RSIDXOPT_DOCTBLSIZE_UNLIMITED 0x01

#define GC_POLICY_NONE -1
#define GC_POLICY_FORK 0

struct RSIdxOptions {
  RSGetValueCallback gvcb;
  void* gvcbData;
  uint32_t flags;
  int gcPolicy;
  char **stopwords;
  int stopwordsLen;
  double score;
  const char *lang;
};

struct RSIdxField {
  HiddenString *path;
  HiddenString *name;

  int types;
  int options;

  double textWeight;
  char tagSeperator;
  int tagCaseSensitive;
};

#define RS_INFO_CURRENT_VERSION 1
#define RS_INFO_INIT_VERSION 1

typedef struct RSIdxInfo {
  uint64_t version;

  // spec params
  int gcPolicy;
  double score;
  const char *lang;

  // fields params
  struct RSIdxField *fields;
  size_t numFields;

  // stats
  size_t numDocuments;
  size_t maxDocId;
  size_t docTableSize;
  size_t sortablesSize;
  size_t docTrieSize;
  size_t numTerms;
  size_t numRecords;
  size_t invertedSize;
  size_t invertedCap;
  size_t skipIndexesSize;
  size_t scoreIndexesSize;
  size_t offsetVecsSize;
  size_t offsetVecRecords;
  size_t termsSize;
  size_t indexingFailures;

  // gc stats
  size_t totalCollected;
  size_t numCycles;
  long long totalMSRun;
  long long lastRunTimeMs;
} RSIdxInfo;

/**
 * Allocate an index options struct. This structure can be used to set global
 * options on the index prior to it being created.
 */
MODULE_API_FUNC(RSIndexOptions*, RediSearch_CreateIndexOptions)(void);

/**
 * Frees the index options previously allocated. The options are _not_ freed
 * in a call to CreateIndex()
 */
MODULE_API_FUNC(void, RediSearch_FreeIndexOptions)(RSIndexOptions*);
MODULE_API_FUNC(void, RediSearch_IndexOptionsSetGetValueCallback)
(RSIndexOptions* opts, RSGetValueCallback cb, void* ctx);
MODULE_API_FUNC(void, RediSearch_IndexOptionsSetStopwords)
(RSIndexOptions* opts, const char **stopwords, int stopwordsLen);
MODULE_API_FUNC(int, RediSearch_IndexOptionsSetScore)(RSIndexOptions*, double);
MODULE_API_FUNC(int, RediSearch_IndexOptionsSetLanguage)(RSIndexOptions*, const char *);
MODULE_API_FUNC(int, RediSearch_ValidateLanguage)(const char*);

/** Set flags modifying index creation. */
MODULE_API_FUNC(void, RediSearch_IndexOptionsSetFlags)(RSIndexOptions* opts, uint32_t flags);

MODULE_API_FUNC(RSIndex*, RediSearch_CreateIndex)
(const char *name, const RSIndexOptions* options);

MODULE_API_FUNC(void, RediSearch_DropIndex)(RSIndex*);

/** Handle Stopwords list */
MODULE_API_FUNC(int, RediSearch_StopwordsList_Contains)(RSIndex* idx, const char *term, size_t len);
MODULE_API_FUNC(void, RediSearch_StopwordsList_Free)(char **list, size_t size);

/** Getter functions */
MODULE_API_FUNC(char **, RediSearch_IndexGetStopwords)(RSIndex*, size_t*);
MODULE_API_FUNC(double, RediSearch_IndexGetScore)(RSIndex*);
MODULE_API_FUNC(const char *, RediSearch_IndexGetLanguage)(RSIndex*);

/**
 * Create a new field in the index
 * @param idx the index
 * @param name the name of the field
 * @param ftype a mask of RSFieldType that should be supported for indexing.
 *  This also indicates the default indexing settings if not otherwise specified
 * @param fopt a mask of RSFieldOptions
 */
MODULE_API_FUNC(RSFieldID, RediSearch_CreateField)
(RSIndex* idx, const char* name, unsigned ftype, unsigned fopt);

#define RediSearch_CreateNumericField(idx, name) \
  RediSearch_CreateField(idx, name, RSFLDTYPE_NUMERIC, RSFLDOPT_NONE)
#define RediSearch_CreateTextField(idx, name) \
  RediSearch_CreateField(idx, name, RSFLDTYPE_FULLTEXT, RSFLDOPT_NONE)
#define RediSearch_CreateTagField(idx, name) \
  RediSearch_CreateField(idx, name, RSFLDTYPE_TAG, RSFLDOPT_NONE)
#define RediSearch_CreateGeoField(idx, name) \
  RediSearch_CreateField(idx, name, RSFLDTYPE_GEO, RSFLDOPT_NONE)
#define RediSearch_CreateVectorField(idx, name) \
  RediSearch_CreateField(idx, name, RSFLDTYPE_VECTOR, RSFLDOPT_NONE)
// TODO: GEOMETRY
// #define RediSearch_CreateGeometryField(idx, name) \
//   RediSearch_CreateField(idx, name, RSFLDTYPE_GEOMETRY, RSFLDOPT_NONE)

MODULE_API_FUNC(void, RediSearch_IndexExisting)(RSIndex* sp, SchemaRuleArgs* args);

MODULE_API_FUNC(void, RediSearch_TextFieldSetWeight)(RSIndex* sp, RSFieldID fs, double w);
MODULE_API_FUNC(void, RediSearch_TagFieldSetSeparator)(RSIndex* sp, RSFieldID fs, char sep);
MODULE_API_FUNC(void, RediSearch_TagFieldSetCaseSensitive)(RSIndex* sp, RSFieldID fs, int enable);

MODULE_API_FUNC(RSDoc*, RediSearch_CreateDocument)
(const void* docKey, size_t len, double score, const char* lang);
#define RediSearch_CreateDocumentSimple(s) RediSearch_CreateDocument(s, strlen(s), 1.0, NULL)
MODULE_API_FUNC(RSDoc*, RediSearch_CreateDocument2)
(const void* docKey, size_t len, RSIndex* sp, double score, const char* lang);
#define RediSearch_CreateDocument2Simple(s, sp) RediSearch_CreateDocument2(s, strlen(s), sp, NAN, NULL)

MODULE_API_FUNC(void, RediSearch_FreeDocument)(RSDoc* doc);

MODULE_API_FUNC(int, RediSearch_DeleteDocument)(RSIndex* sp, const void* docKey, size_t len);
#define RediSearch_DropDocument RediSearch_DeleteDocument
/**
 * Add a field (with value) to the document
 * @param d the document
 * @param fieldName the name of the field
 * @param s the contents of the field to be added (if numeric, the string representation)
 * @param indexAsTypes the types the field should be indexed as. Should be a
 *  bitmask of RSFieldType.
 */
MODULE_API_FUNC(void, RediSearch_DocumentAddField)
(RSDoc* d, const char *fieldname, RedisModuleString* s, unsigned indexAsTypes);

MODULE_API_FUNC(void, RediSearch_DocumentAddFieldString)
(RSDoc* d, const char *fieldname, const char* s, size_t n, unsigned indexAsTypes);
#define RediSearch_DocumentAddFieldCString(doc, fieldname, s, indexAs) \
  RediSearch_DocumentAddFieldString(doc, fieldname, s, strlen(s), indexAs)

MODULE_API_FUNC(void, RediSearch_DocumentAddFieldNumber)
(RSDoc* d, const char *fieldname, double val, unsigned indexAsTypes);

/**
 * Add geo field to a document.
 * Return REDISMODULE_ERR if longitude or latitude is out-of-range
 * otherwise, returns REDISMODULE_OK
 */
MODULE_API_FUNC(int, RediSearch_DocumentAddFieldGeo)
(RSDoc* d, const char* fieldname, double lat, double lon, unsigned indexAsTypes);

/**
 * Replace document if it already exists
 */
#define REDISEARCH_ADD_REPLACE 0x01

MODULE_API_FUNC(int, RediSearch_IndexAddDocument)(RSIndex* sp, RSDoc* d, int flags, char**);
#define RediSearch_SpecAddDocument(sp, d) \
  RediSearch_IndexAddDocument(sp, d, REDISEARCH_ADD_REPLACE, NULL)

MODULE_API_FUNC(RSQNode*, RediSearch_CreateTokenNode)
(RSIndex* sp, const char* fieldName, const char* token);

MODULE_API_FUNC(RSQNode*, RediSearch_CreateNumericNode)
(RSIndex* sp, const char* field, double max, double min, int includeMax, int includeMin);

MODULE_API_FUNC(RSQNode*, RediSearch_CreateGeoNode)
(RSIndex* sp, const char* field, double lat, double lon, double radius, RSGeoDistance unitType);

MODULE_API_FUNC(RSQNode*, RediSearch_CreatePrefixNode)
(RSIndex* sp, const char* fieldName, const char* s);
MODULE_API_FUNC(RSQNode*, RediSearch_CreateContainsNode)
(RSIndex* sp, const char* fieldName, const char* s);
MODULE_API_FUNC(RSQNode*, RediSearch_CreateSuffixNode)
(RSIndex* sp, const char* fieldName, const char* s);

MODULE_API_FUNC(RSQNode*, RediSearch_CreateLexRangeNode)
(RSIndex* sp, const char* fieldName, const char* begin, const char* end, int includeBegin,
 int includeEnd);

MODULE_API_FUNC(RSQNode*, RediSearch_CreateTagNode)(RSIndex* sp, const char* field);
// Used as children of Tag
MODULE_API_FUNC(RSQNode*, RediSearch_CreateTagTokenNode)
(RSIndex* sp, const char* token);
MODULE_API_FUNC(RSQNode*, RediSearch_CreateTagPrefixNode)
(RSIndex* sp, const char* s);
MODULE_API_FUNC(RSQNode*, RediSearch_CreateTagContainsNode)
(RSIndex* sp, const char* s);
MODULE_API_FUNC(RSQNode*, RediSearch_CreateTagSuffixNode)
(RSIndex* sp, const char* s);
MODULE_API_FUNC(RSQNode*, RediSearch_CreateTagLexRangeNode)
(RSIndex* sp, const char* fieldName, const char* begin, const char* end, int includeBegin,
 int includeEnd);

MODULE_API_FUNC(RSQNode*, RediSearch_CreateIntersectNode)(RSIndex* sp, int exact);
MODULE_API_FUNC(RSQNode*, RediSearch_CreateUnionNode)(RSIndex* sp);
MODULE_API_FUNC(RSQNode*, RediSearch_CreateEmptyNode)(RSIndex* sp);
MODULE_API_FUNC(RSQNode*, RediSearch_CreateNotNode)(RSIndex* sp);
MODULE_API_FUNC(void, RediSearch_QueryNodeFree)(RSQNode* qn);

MODULE_API_FUNC(void, RediSearch_QueryNodeAddChild)(RSQNode*, RSQNode*);
MODULE_API_FUNC(void, RediSearch_QueryNodeClearChildren)(RSQNode*);
MODULE_API_FUNC(RSQNode*, RediSearch_QueryNodeGetChild)(const RSQNode*, size_t);
MODULE_API_FUNC(size_t, RediSearch_QueryNodeNumChildren)(const RSQNode*);

MODULE_API_FUNC(int, RediSearch_QueryNodeType)(RSQNode* qn);
MODULE_API_FUNC(int, RediSearch_QueryNodeGetFieldMask)(RSQNode* qn);

MODULE_API_FUNC(RSResultsIterator*, RediSearch_GetResultsIterator)(RSQNode* qn, RSIndex* sp);

MODULE_API_FUNC(void, RediSearch_SetCriteriaTesterThreshold)(size_t num);

MODULE_API_FUNC(const char*, RediSearch_HiddenStringGet)(const HiddenString* hs);

/**
 * Return an iterator over the results of the specified query string
 * @param sp the index
 * @param s the query string
 * @param n the length of the string
 * @param[out] error if not-NULL, will be set to the error message, if there is a
 *  problem parsing the query
 * @return an iterator over the results, or NULL if no iterator can be had
 *  (see err, or no results).
 */
MODULE_API_FUNC(RSResultsIterator*, RediSearch_IterateQuery)
(RSIndex* sp, const char* s, size_t n, char** err);

/**
 * Return an iterator over the results of the specified query string
 * @param sp the index
 * @param s the query string
 * @param n the length of the string
 * @param dialect dialect version to be used for parsing the query
 * @param[out] error if not-NULL, will be set to the error message, if there is a
 *  problem parsing the query
 * @return an iterator over the results, or NULL if no iterator can be had
 *  (see err, or no results).
 */
MODULE_API_FUNC(RSResultsIterator*, RediSearch_IterateQueryWithDialect)
(RSIndex* sp, const char* s, size_t n, unsigned int dialect, char** err);

MODULE_API_FUNC(int, RediSearch_DocumentExists)
(RSIndex* sp, const void* docKey, size_t len);

MODULE_API_FUNC(const void*, RediSearch_ResultsIteratorNext)
(RSResultsIterator* iter, RSIndex* sp, size_t* len);

MODULE_API_FUNC(void, RediSearch_ResultsIteratorFree)(RSResultsIterator* iter);

MODULE_API_FUNC(void, RediSearch_ResultsIteratorReset)(RSResultsIterator* iter);

MODULE_API_FUNC(double, RediSearch_ResultsIteratorGetScore)(const RSResultsIterator* it);

MODULE_API_FUNC(void, RediSearch_IndexOptionsSetGCPolicy)(RSIndexOptions* options, int policy);

MODULE_API_FUNC(size_t, RediSearch_MemUsage)(RSIndex* sp);

MODULE_API_FUNC(size_t, RediSearch_TotalMemUsage)(void);

MODULE_API_FUNC(TotalIndexesInfo, RediSearch_TotalInfo)(void);

MODULE_API_FUNC(InfoGCStats, RediSearch_GC_total)(void);

/**
 * Return an info struct
 * @param sp the index
 * @param info a pointer to RSIdxInfo struct with `.version = RS_INFO_CURRENT`
 */
MODULE_API_FUNC(int, RediSearch_IndexInfo)(RSIndex* sp, RSIdxInfo *info);
MODULE_API_FUNC(void, RediSearch_IndexInfoFree)(RSIdxInfo *info);

#define RS_XAPIFUNC(X)               \
  X(GetCApiVersion)                  \
  X(CreateIndexOptions)              \
  X(IndexOptionsSetGetValueCallback) \
  X(IndexOptionsSetFlags)            \
  X(IndexOptionsSetScore)            \
  X(IndexOptionsSetLanguage)         \
  X(ValidateLanguage)                \
  X(FreeIndexOptions)                \
  X(CreateIndex)                     \
  X(DropIndex)                       \
  X(IndexGetStopwords)               \
  X(StopwordsList_Contains)          \
  X(StopwordsList_Free)              \
  X(IndexGetScore)                   \
  X(IndexGetLanguage)                \
  X(CreateField)                     \
  X(TextFieldSetWeight)              \
  X(TagFieldSetSeparator)            \
  X(CreateDocument)                  \
  X(CreateDocument2)                 \
  X(DeleteDocument)                  \
  X(DocumentAddField)                \
  X(DocumentAddFieldNumber)          \
  X(DocumentAddFieldString)          \
  X(IndexAddDocument)                \
  X(CreateTokenNode)                 \
  X(CreateNumericNode)               \
  X(CreatePrefixNode)                \
  X(CreateContainsNode)              \
  X(CreateSuffixNode)                \
  X(CreateLexRangeNode)              \
  X(CreateTagNode)                   \
  X(CreateTagTokenNode)              \
  X(CreateTagPrefixNode)             \
  X(CreateTagContainsNode)           \
  X(CreateTagSuffixNode)             \
  X(CreateTagLexRangeNode)           \
  X(CreateIntersectNode)             \
  X(CreateUnionNode)                 \
  X(CreateNotNode)                   \
  X(QueryNodeAddChild)               \
  X(QueryNodeClearChildren)          \
  X(QueryNodeGetChild)               \
  X(QueryNodeNumChildren)            \
  X(QueryNodeFree)                   \
  X(QueryNodeType)                   \
  X(QueryNodeGetFieldMask)           \
  X(GetResultsIterator)              \
  X(ResultsIteratorNext)             \
  X(ResultsIteratorFree)             \
  X(ResultsIteratorReset)            \
  X(IterateQuery)                    \
  X(IterateQueryWithDialect)         \
  X(ResultsIteratorGetScore)         \
  X(IndexOptionsSetGCPolicy)         \
  X(MemUsage)                        \
  X(IndexInfo)                       \
  X(IndexInfoFree)                   \
  X(SetCriteriaTesterThreshold)

#define REDISEARCH_MODULE_INIT_FUNCTION(name)                                  \
  if (RedisModule_GetApi("RediSearch_" #name, ((void**)&RediSearch_##name))) { \
    rv__ = REDISMODULE_ERR;                                                    \
    goto rsfunc_init_end__;                                                    \
  }

#ifdef REDISEARCH_API_EXTERN
/**
 * This is implemented as a macro rather than a function so that the inclusion of this
 * header file does not automatically require the symbols to be defined above.
 *
 * We are making use of special GCC statement-expressions `({...})`. This is also
 * supported by clang.
 *
 * This function should not be used if RediSearch is compiled as a
 * static library. In this case, the functions are actually properly
 * linked.
 */
#define RediSearch_Initialize()                                  \
  ({                                                             \
    int rv__ = REDISMODULE_OK;                                   \
    RS_XAPIFUNC(REDISEARCH_MODULE_INIT_FUNCTION);                \
    if (RediSearch_GetCApiVersion() > REDISEARCH_CAPI_VERSION) { \
      rv__ = REDISMODULE_ERR;                                    \
    }                                                            \
  rsfunc_init_end__:;                                            \
    rv__;                                                        \
  })

#define REDISEARCH__API_INIT_NULL(s) __typeof__(RediSearch_##s) RediSearch_##s = NULL;
#define REDISEARCH_API_INIT_SYMBOLS() RS_XAPIFUNC(REDISEARCH__API_INIT_NULL)
#else
#define REDISEARCH_API_INIT_SYMBOLS()
#define RediSearch_Initialize()
#endif

/**
 * Export the C API to be dynamically discoverable by other modules.
 * This is an internal function
 */
int RediSearch_ExportCapi(RedisModuleCtx* ctx);

#define REDISEARCH_INIT_MODULE 0x01
#define REDISEARCH_INIT_LIBRARY 0x02
int RediSearch_Init(RedisModuleCtx* ctx, int mode);
#ifdef __cplusplus
}
#endif
#endif /* SRC_REDISEARCH_API_H_ */
