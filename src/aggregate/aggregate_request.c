/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/
#include "aggregate.h"
#include "reducer.h"

#include <query.h>
#include <extension.h>
#include <result_processor.h>
#include <util/arr.h>
#include <rmutil/util.h>
#include "ext/default.h"
#include "extension.h"
#include "profile.h"
#include "config.h"
#include "util/timeout.h"
#include "query_optimizer.h"
#include "resp3.h"
#include "obfuscation/hidden.h"

extern RSConfig RSGlobalConfig;

/**
 * Ensures that the user has not requested one of the 'extended' features. Extended
 * in this case refers to reducers which re-create the search results.
 * @param areq the request
 * @param name the name of the option that requires simple mode. Used for error
 *   formatting
 * @param status the error object
 */
static bool ensureSimpleMode(AREQ *areq) {
  if(areq->reqflags & QEXEC_F_IS_AGGREGATE) {
    return false;
  }
  areq->reqflags |= QEXEC_F_IS_SEARCH;
  return true;
}

/**
 * Like @ref ensureSimpleMode(), but does the opposite -- ensures that one of the
 * 'simple' options - i.e. ones which rely on the field to be the exact same as
 * found in the document - was not requested.
 * name argument must not contain any user data, as it is used for error formatting
 */
static int ensureExtendedMode(AREQ *areq, const char *name, QueryError *status) {
  if (areq->reqflags & QEXEC_F_IS_SEARCH) {
    QueryError_SetWithoutUserDataFmt(status, QUERY_EINVAL,
                           "option `%s` is mutually exclusive with simple (i.e. search) options",
                           name);
    return 0;
  }
  areq->reqflags |= QEXEC_F_IS_AGGREGATE;
  return 1;
}

static int parseSortby(PLN_ArrangeStep *arng, ArgsCursor *ac, QueryError *status, int allowLegacy);

static void ReturnedField_Free(ReturnedField *field) {
  rm_free(field->highlightSettings.openTag);
  rm_free(field->highlightSettings.closeTag);
  rm_free(field->summarizeSettings.separator);
}

void FieldList_Free(FieldList *fields) {
  for (size_t ii = 0; ii < fields->numFields; ++ii) {
    ReturnedField_Free(fields->fields + ii);
  }
  ReturnedField_Free(&fields->defaultField);
  rm_free(fields->fields);
}

ReturnedField *FieldList_GetCreateField(FieldList *fields, const char *name, const char *path) {
  size_t foundIndex = -1;
  for (size_t ii = 0; ii < fields->numFields; ++ii) {
    if (!strcmp(fields->fields[ii].name, name)) {
      return fields->fields + ii;
    }
  }

  fields->fields = rm_realloc(fields->fields, sizeof(*fields->fields) * ++fields->numFields);
  ReturnedField *ret = fields->fields + (fields->numFields - 1);
  memset(ret, 0, sizeof *ret);
  ret->name = name;
  ret->path = path ? path : name;
  return ret;
}

static void FieldList_RestrictReturn(FieldList *fields) {
  size_t oix = 0;
  for (size_t ii = 0; ii < fields->numFields; ++ii) {
    if (fields->fields[ii].explicitReturn == 0) {
      ReturnedField_Free(fields->fields + ii);
    } else if (ii != oix) {
      fields->fields[oix++] = fields->fields[ii];
    } else {
      ++oix;
    }
  }
  fields->numFields = oix;
}

static int parseCursorSettings(AREQ *req, ArgsCursor *ac, QueryError *status) {
  ACArgSpec specs[] = {{.name = "MAXIDLE",
                        .type = AC_ARGTYPE_UINT,
                        .target = &req->cursorMaxIdle,
                        .intflags = AC_F_GE1},
                       {.name = "COUNT",
                        .type = AC_ARGTYPE_UINT,
                        .target = &req->cursorChunkSize,
                        .intflags = AC_F_GE1},
                       {NULL}};

  int rv;
  ACArgSpec *errArg = NULL;
  if ((rv = AC_ParseArgSpec(ac, specs, &errArg)) != AC_OK && rv != AC_ERR_ENOENT) {
    QERR_MKBADARGS_AC(status, errArg->name, rv);
    return REDISMODULE_ERR;
  }

  if (req->cursorMaxIdle == 0 || req->cursorMaxIdle > RSGlobalConfig.cursorMaxIdle) {
    req->cursorMaxIdle = RSGlobalConfig.cursorMaxIdle;
  }
  req->reqflags |= QEXEC_F_IS_CURSOR;
  return REDISMODULE_OK;
}

static int parseRequiredFields(AREQ *req, ArgsCursor *ac, QueryError *status){

  ArgsCursor args = {0};
  int rv = AC_GetVarArgs(ac, &args);
  if (rv != AC_OK) {
    QueryError_SetWithUserDataFmt(status, QUERY_EPARSEARGS, "Bad arguments", " for _REQUIRED_FIELDS: %s", AC_Strerror(rv));
    return REDISMODULE_ERR;
  }

  int requiredFieldNum = AC_NumArgs(&args);
  // This array contains shallow copy of the required fields names. Those copies are to use only for lookup.
  // If we need to use them in reply we should make a copy of those strings.
  const char** requiredFields = array_new(const char*, requiredFieldNum);
  for(size_t i=0; i < requiredFieldNum; i++) {
    const char *s = AC_GetStringNC(&args, NULL); {
      if(!s) {
        array_free(requiredFields);
        return REDISMODULE_ERR;
      }
    }
    array_append(requiredFields, s);
  }

  req->requiredFields = requiredFields;

  return REDISMODULE_OK;
}

int parseDialect(unsigned int *dialect, ArgsCursor *ac, QueryError *status) {
  if (AC_NumRemaining(ac) < 1) {
      QueryError_SetError(status, QUERY_EPARSEARGS, "Need an argument for DIALECT");
      return REDISMODULE_ERR;
    }
    if ((AC_GetUnsigned(ac, dialect, AC_F_GE1) != AC_OK) || (*dialect > MAX_DIALECT_VERSION)) {
      QueryError_SetWithoutUserDataFmt(
        status, QUERY_EPARSEARGS,
        "DIALECT requires a non negative integer >=%u and <= %u",
        MIN_DIALECT_VERSION, MAX_DIALECT_VERSION
      );
      return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}

// Parse the available formats for search result values: FORMAT STRING|EXPAND
int parseValueFormat(uint32_t *flags, ArgsCursor *ac, QueryError *status) {
  const char *format;
  int rv = AC_GetString(ac, &format, NULL, 0);
  if (rv != AC_OK) {
    QueryError_SetError(status, QUERY_EBADVAL, "Need an argument for FORMAT");
    return REDISMODULE_ERR;
  }
  if (!strcasecmp(format, "EXPAND")) {
    *flags |= QEXEC_FORMAT_EXPAND;
  } else if (!strcasecmp(format, "STRING")) {
    *flags &= ~QEXEC_FORMAT_EXPAND;
  } else {
    QueryError_SetWithUserDataFmt(status, QUERY_EPARSEARGS, "FORMAT", " %s is not supported", format);
    return REDISMODULE_ERR;
  }
  *flags &= ~QEXEC_FORMAT_DEFAULT;
  return REDISMODULE_OK;
}

// Parse the timeout value
int parseTimeout(long long *timeout, ArgsCursor *ac, QueryError *status) {
  if (AC_NumRemaining(ac) < 1) {
    QueryError_SetError(status, QUERY_EPARSEARGS, "Need an argument for TIMEOUT");
    return REDISMODULE_ERR;
  }

  if (AC_GetLongLong(ac, timeout, AC_F_GE0) != AC_OK) {
    QueryError_SetError(status, QUERY_EPARSEARGS,
      "TIMEOUT requires a non negative integer.");
    return REDISMODULE_ERR;
  }

  return REDISMODULE_OK;
}

int SetValueFormat(bool is_resp3, bool is_json, uint32_t *flags, QueryError *status) {
  if (*flags & QEXEC_FORMAT_DEFAULT) {
    *flags &= ~QEXEC_FORMAT_EXPAND;
    *flags &= ~QEXEC_FORMAT_DEFAULT;
  }

  if (*flags & QEXEC_FORMAT_EXPAND) {
    if (!is_resp3) {
      QueryError_SetError(status, QUERY_EBADVAL, "EXPAND format is only supported with RESP3");
      return REDISMODULE_ERR;
    }
    if (!is_json) {
      QueryError_SetError(status, QUERY_EBADVAL, "EXPAND format is only supported with JSON");
      return REDISMODULE_ERR;
    }
    if (japi_ver < 4) {
      QueryError_SetError(status, QUERY_EBADVAL, "EXPAND format requires a newer RedisJSON (with API version RedisJSON_V4)");
      return REDISMODULE_ERR;
    }
  }
  return REDISMODULE_OK;
}

void SetSearchCtx(RedisSearchCtx *sctx, const AREQ *req) {
  if (req->reqflags & QEXEC_FORMAT_EXPAND) {
    sctx->expanded = 1;
    sctx->apiVersion = MAX(APIVERSION_RETURN_MULTI_CMP_FIRST, req->reqConfig.dialectVersion);
  } else {
    sctx->apiVersion = req->reqConfig.dialectVersion;
  }
}

#define ARG_HANDLED 1
#define ARG_ERROR -1
#define ARG_UNKNOWN 0

static int handleCommonArgs(AREQ *req, ArgsCursor *ac, QueryError *status, int allowLegacy) {
  int rv;
  bool dialect_specified = false;
  // This handles the common arguments that are not stateful
  if (AC_AdvanceIfMatch(ac, "LIMIT")) {
    PLN_ArrangeStep *arng = AGPLN_GetOrCreateArrangeStep(&req->ap);
    arng->isLimited = 1;
    // Parse offset, length
    if (AC_NumRemaining(ac) < 2) {
      QueryError_SetError(status, QUERY_EPARSEARGS, "LIMIT requires two arguments");
      return ARG_ERROR;
    }
    if ((rv = AC_GetU64(ac, &arng->offset, 0)) != AC_OK ||
        (rv = AC_GetU64(ac, &arng->limit, 0)) != AC_OK) {
      QueryError_SetError(status, QUERY_EPARSEARGS, "LIMIT needs two numeric arguments");
      return ARG_ERROR;
    }

    if (arng->limit == 0 && arng->offset != 0) {
      QueryError_SetError(status, QUERY_ELIMIT, "The `offset` of the LIMIT must be 0 when `num` is 0");
      return ARG_ERROR;
    }

    if (arng->isLimited && arng->limit == 0) {
      // LIMIT 0 0 - only count
      req->reqflags |= QEXEC_F_NOROWS;
      req->reqflags |= QEXEC_F_SEND_NOFIELDS;
      // TODO: unify if when req holds only maxResults according to the query type.
      //(SEARCH / AGGREGATE)
    } else if ((arng->limit > req->maxSearchResults) &&
               (req->reqflags & QEXEC_F_IS_SEARCH)) {
      QueryError_SetWithoutUserDataFmt(status, QUERY_ELIMIT, "LIMIT exceeds maximum of %llu",
                             req->maxSearchResults);
      return ARG_ERROR;
    } else if ((arng->limit > req->maxAggregateResults) &&
               !(req->reqflags & QEXEC_F_IS_SEARCH)) {
      QueryError_SetWithoutUserDataFmt(status, QUERY_ELIMIT, "LIMIT exceeds maximum of %llu",
                             req->maxAggregateResults);
      return ARG_ERROR;
    } else if (arng->offset > req->maxSearchResults) {
      QueryError_SetWithoutUserDataFmt(status, QUERY_ELIMIT, "OFFSET exceeds maximum of %llu",
                             req->maxSearchResults);
      return ARG_ERROR;
    }
  } else if (AC_AdvanceIfMatch(ac, "SORTBY")) {
    PLN_ArrangeStep *arng = AGPLN_GetOrCreateArrangeStep(&req->ap);
    if ((parseSortby(arng, ac, status, req->reqflags & QEXEC_F_IS_SEARCH)) != REDISMODULE_OK) {
      return ARG_ERROR;
    }
  } else if (AC_AdvanceIfMatch(ac, "TIMEOUT")) {
    if (AC_NumRemaining(ac) < 1) {
      QueryError_SetError(status, QUERY_EPARSEARGS, "Need argument for TIMEOUT");
      return ARG_ERROR;
    }
    if (AC_GetLongLong(ac, &req->reqConfig.queryTimeoutMS, AC_F_GE0) != AC_OK) {
      QueryError_SetError(status, QUERY_EPARSEARGS, "TIMEOUT requires a non negative integer");
      return ARG_ERROR;
    }
  } else if (AC_AdvanceIfMatch(ac, "WITHCURSOR")) {
    if (parseCursorSettings(req, ac, status) != REDISMODULE_OK) {
      return ARG_ERROR;
    }
  } else if (AC_AdvanceIfMatch(ac, "_NUM_SSTRING")) {
    req->reqflags |= QEXEC_F_TYPED;
  } else if (AC_AdvanceIfMatch(ac, "WITHRAWIDS")) {
    req->reqflags |= QEXEC_F_SENDRAWIDS;
  } else if (AC_AdvanceIfMatch(ac, "PARAMS")) {
    if (parseParams(&(req->searchopts.params), ac, status) != REDISMODULE_OK) {
      return ARG_ERROR;
    }
  } else if(AC_AdvanceIfMatch(ac, "_REQUIRED_FIELDS")) {
    if (parseRequiredFields(req, ac, status) != REDISMODULE_OK) {
      return ARG_ERROR;
    }
    req->reqflags |= QEXEC_F_REQUIRED_FIELDS;
  } else if(AC_AdvanceIfMatch(ac, "DIALECT")) {
    dialect_specified = true;
    if (parseDialect(&req->reqConfig.dialectVersion, ac, status) != REDISMODULE_OK) {
      return ARG_ERROR;
    }
  } else if(AC_AdvanceIfMatch(ac, "FORMAT")) {
    if (parseValueFormat(&req->reqflags, ac, status) != REDISMODULE_OK) {
      return ARG_ERROR;
    }
  } else if (AC_AdvanceIfMatch(ac, "_INDEX_PREFIXES")) {
    // Set the offset of the prefixes in the query, for further processing later
    req->prefixesOffset = ac->offset - 1;

    ArgsCursor tmp = {0};
    if (AC_GetVarArgs(ac, &tmp) != AC_OK) {
      RS_LOG_ASSERT(false, "Bad arguments for _INDEX_PREFIXES (coordinator)");
    }
  } else if (AC_AdvanceIfMatch(ac, "BM25STD_TANH_FACTOR")) {
    if (AC_NumRemaining(ac) < 1) {
      QueryError_SetError(status, QUERY_EPARSEARGS, "Need an argument for BM25STD_TANH_FACTOR");
      return ARG_ERROR;
    }
    if (AC_GetUnsignedLongLong(ac, (unsigned long long *)&req->reqConfig.BM25STD_TanhFactor, AC_F_GE1) != AC_OK) {
      QueryError_SetWithoutUserDataFmt(status, QUERY_EPARSEARGS, "BM25STD_TANH_FACTOR must be between %d and %d inclusive",
      BM25STD_TANH_FACTOR_MIN, BM25STD_TANH_FACTOR_MAX);
      return ARG_ERROR;
    }
  } else {
    return ARG_UNKNOWN;
  }

  if (dialect_specified && req->reqConfig.dialectVersion < APIVERSION_RETURN_MULTI_CMP_FIRST && req->reqflags & QEXEC_FORMAT_EXPAND) {
    QueryError_SetWithoutUserDataFmt(status, QUERY_ELIMIT, "EXPAND format requires dialect %u or greater", APIVERSION_RETURN_MULTI_CMP_FIRST);
    return ARG_ERROR;
  }

  return ARG_HANDLED;
}

static int parseSortby(PLN_ArrangeStep *arng, ArgsCursor *ac, QueryError *status, int isLegacy) {
  // Prevent multiple SORTBY steps
  if (arng->sortKeys != NULL) {
    if (isLegacy) {
      QueryError_SetError(status, QUERY_EPARSEARGS, "Multiple SORTBY steps are not allowed");
    } else {
      QueryError_SetError(status, QUERY_EPARSEARGS, "Multiple SORTBY steps are not allowed. Sort multiple fields in a single step");
    }
    return REDISMODULE_ERR;
  }

  // Assume argument is at 'SORTBY'
  ArgsCursor subArgs = {0};
  int rv;
  int legacyDesc = 0;

  // We build a bitmap of maximum 64 sorting parameters. 1 means asc, 0 desc
  // By default all bits are 1. Whenever we encounter DESC we flip the corresponding bit
  uint64_t ascMap = SORTASCMAP_INIT;
  const char **keys = NULL;

  if (isLegacy) {
    if (AC_NumRemaining(ac) > 0) {
      // Mimic subArgs to contain the single field we already have
      AC_GetSlice(ac, &subArgs, 1);
      if (AC_AdvanceIfMatch(ac, "DESC")) {
        legacyDesc = 1;
      } else if (AC_AdvanceIfMatch(ac, "ASC")) {
        legacyDesc = 0;
      }
    } else {
      goto err;
    }
  } else {
    rv = AC_GetVarArgs(ac, &subArgs);
    if (rv != AC_OK) {
      QueryError_SetWithUserDataFmt(status, QUERY_EPARSEARGS, "Bad arguments", " for SORTBY: %s", AC_Strerror(rv));
      goto err;
    }
  }

  keys = array_new(const char *, 8);

  if (isLegacy) {
    // Legacy demands one field and an optional ASC/DESC parameter. Both
    // of these are handled above, so no need for argument parsing
    const char *s = AC_GetStringNC(&subArgs, NULL);
    array_append(keys, s);

    if (legacyDesc) {
      SORTASCMAP_SETDESC(ascMap, 0);
    }
  } else {
    while (!AC_IsAtEnd(&subArgs)) {

      const char *s = AC_GetStringNC(&subArgs, NULL);
      if (*s == '@') {
        if (array_len(keys) >= SORTASCMAP_MAXFIELDS) {
          QueryError_SetWithoutUserDataFmt(status, QUERY_ELIMIT, "Cannot sort by more than %lu fields", SORTASCMAP_MAXFIELDS);
          goto err;
        }
        s++;
        array_append(keys, s);
        continue;
      }

      if (!strcasecmp(s, "ASC")) {
        SORTASCMAP_SETASC(ascMap, array_len(keys) - 1);
      } else if (!strcasecmp(s, "DESC")) {
        SORTASCMAP_SETDESC(ascMap, array_len(keys) - 1);
      } else {
        // Unknown token - neither a property nor ASC/DESC
        QueryError_SetWithUserDataFmt(status, QUERY_EPARSEARGS, "MISSING ASC or DESC after sort field", " (%s)", s);
        goto err;
      }
    }
  }

  // Parse optional MAX
  // MAX is not included in the normal SORTBY arglist.. so we need to switch
  // back to `ac`
  if (AC_AdvanceIfMatch(ac, "MAX")) {
    if (isLegacy) {
      QueryError_SetError(status, QUERY_EPARSEARGS, "SORTBY MAX is not supported by FT.SEARCH");
      goto err;
    }
    unsigned mx = 0;
    if ((rv = AC_GetUnsigned(ac, &mx, 0) != AC_OK)) {
      QueryError_SetWithUserDataFmt(status, QUERY_EPARSEARGS, "Bad arguments", " for MAX: %s", AC_Strerror(rv));
      goto err;
    }
    arng->limit = mx;
  }

  arng->sortAscMap = ascMap;
  arng->sortKeys = keys;
  return REDISMODULE_OK;
err:
  QueryError_SetError(status, QUERY_EPARSEARGS, "Bad SORTBY arguments");
  if (keys) {
    array_free(keys);
  }
  return REDISMODULE_ERR;
}

static int parseQueryLegacyArgs(ArgsCursor *ac, RSSearchOptions *options, bool *hasEmptyFilterValue, QueryError *status) {
  if (AC_AdvanceIfMatch(ac, "FILTER")) {
    // Numeric filter
    LegacyNumericFilter **curpp = array_ensure_tail(&options->legacy.filters, LegacyNumericFilter *);
    *curpp = NumericFilter_LegacyParse(ac, hasEmptyFilterValue, status);
    if (!*curpp) {
      return ARG_ERROR;
    }
  } else if (AC_AdvanceIfMatch(ac, "GEOFILTER")) {
    LegacyGeoFilter *cur_gf = rm_new(*cur_gf);
    array_ensure_append_1(options->legacy.geo_filters, cur_gf);
    if (GeoFilter_LegacyParse(cur_gf, ac, hasEmptyFilterValue, status) != REDISMODULE_OK) {
      return ARG_ERROR;
    }
  } else {
    return ARG_UNKNOWN;
  }
  return ARG_HANDLED;
}

static int parseQueryArgs(ArgsCursor *ac, AREQ *req, RSSearchOptions *searchOpts,
                          AggregatePlan *plan, QueryError *status) {
  // Parse query-specific arguments..
  const char *languageStr = NULL;
  ArgsCursor returnFields = {0};
  ArgsCursor inKeys = {0};
  ArgsCursor inFields = {0};
  ACArgSpec querySpecs[] = {
      {.name = "INFIELDS", .type = AC_ARGTYPE_SUBARGS, .target = &inFields},  // Comment
      {.name = "SLOP",
       .type = AC_ARGTYPE_INT,
       .target = &searchOpts->slop,
       .intflags = AC_F_COALESCE},
      {.name = "LANGUAGE", .type = AC_ARGTYPE_STRING, .target = &languageStr},
      {.name = "EXPANDER", .type = AC_ARGTYPE_STRING, .target = &searchOpts->expanderName},
      {.name = "INKEYS", .type = AC_ARGTYPE_SUBARGS, .target = &inKeys},
      {.name = "SCORER", .type = AC_ARGTYPE_STRING, .target = &searchOpts->scorerName},
      {.name = "RETURN", .type = AC_ARGTYPE_SUBARGS, .target = &returnFields},
      {AC_MKBITFLAG("INORDER", &searchOpts->flags, Search_InOrder)},
      {AC_MKBITFLAG("VERBATIM", &searchOpts->flags, Search_Verbatim)},
      {AC_MKBITFLAG("WITHSCORES", &req->reqflags, QEXEC_F_SEND_SCORES)},
      {AC_MKBITFLAG("ADDSCORES", &req->reqflags, QEXEC_F_SEND_SCORES_AS_FIELD)},
      {AC_MKBITFLAG("WITHSORTKEYS", &req->reqflags, QEXEC_F_SEND_SORTKEYS)},
      {AC_MKBITFLAG("WITHPAYLOADS", &req->reqflags, QEXEC_F_SEND_PAYLOADS)},
      {AC_MKBITFLAG("NOCONTENT", &req->reqflags, QEXEC_F_SEND_NOFIELDS)},
      {AC_MKBITFLAG("NOSTOPWORDS", &searchOpts->flags, Search_NoStopWords)},
      {AC_MKBITFLAG("EXPLAINSCORE", &req->reqflags, QEXEC_F_SEND_SCOREEXPLAIN)},
      {.name = "PAYLOAD",
       .type = AC_ARGTYPE_STRING,
       .target = &req->ast.udata,
       .len = &req->ast.udatalen},
      {NULL}};

  req->reqflags |= QEXEC_FORMAT_DEFAULT;
  bool optimization_specified = false;
  bool hasEmptyFilterValue = false;
  while (!AC_IsAtEnd(ac)) {
    ACArgSpec *errSpec = NULL;
    int rv = AC_ParseArgSpec(ac, querySpecs, &errSpec);
    if (rv == AC_OK) {
      continue;
    }

    if (rv != AC_ERR_ENOENT) {
      QERR_MKBADARGS_AC(status, errSpec->name, rv);
      return REDISMODULE_ERR;
    }

    // See if this is one of our arguments which requires special handling
    if (AC_AdvanceIfMatch(ac, "SUMMARIZE")) {
      if(!ensureSimpleMode(req)) {
        QueryError_SetError(status, QUERY_EPARSEARGS, "SUMMARIZE is not supported on FT.AGGREGATE");
        return REDISMODULE_ERR;
      }
      if (ParseSummarize(ac, &req->outFields) == REDISMODULE_ERR) {
        QueryError_SetError(status, QUERY_EPARSEARGS, "Bad arguments for SUMMARIZE");
        return REDISMODULE_ERR;
      }
      req->reqflags |= QEXEC_F_SEND_HIGHLIGHT;

    } else if (AC_AdvanceIfMatch(ac, "HIGHLIGHT")) {
      if(!ensureSimpleMode(req)) {
        QueryError_SetError(status, QUERY_EPARSEARGS, "HIGHLIGHT is not supported on FT.AGGREGATE");
        return REDISMODULE_ERR;
      }

      if (ParseHighlight(ac, &req->outFields) == REDISMODULE_ERR) {
        QueryError_SetError(status, QUERY_EPARSEARGS, "Bad arguments for HIGHLIGHT");
        return REDISMODULE_ERR;
      }
      req->reqflags |= QEXEC_F_SEND_HIGHLIGHT;

    } else if ((req->reqflags & QEXEC_F_IS_SEARCH) &&
               ((rv = parseQueryLegacyArgs(ac, searchOpts, &hasEmptyFilterValue, status)) != ARG_UNKNOWN)) {
      if (rv == ARG_ERROR) {
        return REDISMODULE_ERR;
      }
    } else if (AC_AdvanceIfMatch(ac, "WITHCOUNT")) {
      req->reqflags &= ~QEXEC_OPTIMIZE;
      optimization_specified = true;
    } else if (AC_AdvanceIfMatch(ac, "WITHOUTCOUNT")) {
      req->reqflags |= QEXEC_OPTIMIZE;
      optimization_specified = true;
    } else {
      int rv = handleCommonArgs(req, ac, status, 1);
      if (rv == ARG_HANDLED) {
        // nothing
      } else if (rv == ARG_ERROR) {
        return REDISMODULE_ERR;
      } else {
        break;
      }
    }
  }

  // In dialect 2, we require a non empty numeric filter
  if (req->reqConfig.dialectVersion >= 2 && hasEmptyFilterValue){
      QueryError_SetError(status, QUERY_EPARSEARGS, "Numeric/Geo filter value/s cannot be empty");
      return REDISMODULE_ERR;
  }

  if (!optimization_specified && req->reqConfig.dialectVersion >= 4) {
    // If optimize was not enabled/disabled explicitly, enable it by default starting with dialect 4
    req->reqflags |= QEXEC_OPTIMIZE;
  }

  if ((req->reqflags & QEXEC_F_SEND_SCOREEXPLAIN) && !(req->reqflags & QEXEC_F_SEND_SCORES)) {
    QueryError_SetError(status, QUERY_EPARSEARGS, "EXPLAINSCORE must be accompanied with WITHSCORES");
    return REDISMODULE_ERR;
  }

  if (IsSearch(req) && HasScoreInPipeline(req)) {
    QueryError_SetError(status, QUERY_EPARSEARGS, "ADDSCORES is not supported on FT.SEARCH");
    return REDISMODULE_ERR;
  }

  searchOpts->inkeys = (const sds*)inKeys.objs;
  searchOpts->ninkeys = inKeys.argc;
  searchOpts->legacy.infields = (const char **)inFields.objs;
  searchOpts->legacy.ninfields = inFields.argc;
  // if language is NULL, set it to RS_LANG_UNSET and it will be updated
  // later, taking the index language
  if(languageStr == NULL) {
    searchOpts->language = RS_LANG_UNSET;
  } else {
    searchOpts->language = RSLanguage_Find(languageStr, 0);
  }

  if (AC_IsInitialized(&returnFields)) {
    if(!ensureSimpleMode(req)) {
        QueryError_SetError(status, QUERY_EPARSEARGS, "RETURN is not supported on FT.AGGREGATE");
        return REDISMODULE_ERR;
    }

    req->outFields.explicitReturn = 1;
    if (returnFields.argc == 0) {
      req->reqflags |= QEXEC_F_SEND_NOFIELDS;
    }

    while (!AC_IsAtEnd(&returnFields)) {
      const char *path = AC_GetStringNC(&returnFields, NULL);
      const char *name = path;
      if (AC_AdvanceIfMatch(&returnFields, SPEC_AS_STR)) {
        int rv = AC_GetString(&returnFields, &name, NULL, 0);
        if (rv != AC_OK) {
          QueryError_SetError(status, QUERY_EPARSEARGS, "RETURN path AS name - must be accompanied with NAME");
          return REDISMODULE_ERR;
        } else if (!strcasecmp(name, SPEC_AS_STR)) {
          QueryError_SetError(status, QUERY_EPARSEARGS, "Alias for RETURN cannot be `AS`");
          return REDISMODULE_ERR;
        }
      }
      ReturnedField *f = FieldList_GetCreateField(&req->outFields, name, path);
      f->explicitReturn = 1;
    }
    FieldList_RestrictReturn(&req->outFields);
  }
  return REDISMODULE_OK;
}

static char *getReducerAlias(PLN_GroupStep *g, const char *func, const ArgsCursor *args) {

  sds out = sdsnew("__generated_alias");
  out = sdscat(out, func);
  // only put parentheses if we actually have args
  char buf[255];
  ArgsCursor tmp = *args;
  while (!AC_IsAtEnd(&tmp)) {
    size_t l;
    const char *s = AC_GetStringNC(&tmp, &l);
    while (*s == '@') {
      // Don't allow the leading '@' to be included as an alias!
      ++s;
      --l;
    }
    out = sdscatlen(out, s, l);
    if (!AC_IsAtEnd(&tmp)) {
      out = sdscat(out, ",");
    }
  }

  // only put parentheses if we actually have args
  sdstolower(out);

  // duplicate everything. yeah this is lame but this function is not in a tight loop
  char *dup = rm_strndup(out, sdslen(out));
  sdsfree(out);
  return dup;
}

static void groupStepFree(PLN_BaseStep *base) {
  PLN_GroupStep *g = (PLN_GroupStep *)base;
  if (g->reducers) {
    size_t nreducers = array_len(g->reducers);
    for (size_t ii = 0; ii < nreducers; ++ii) {
      PLN_Reducer *gr = g->reducers + ii;
      rm_free(gr->alias);
    }
    array_free(g->reducers);
  }

  RLookup_Cleanup(&g->lookup);
  rm_free(base);
}

static RLookup *groupStepGetLookup(PLN_BaseStep *bstp) {
  return &((PLN_GroupStep *)bstp)->lookup;
}


PLN_Reducer *PLNGroupStep_FindReducer(PLN_GroupStep *gstp, const char *name, ArgsCursor *ac) {
  long long nvars;
  if (AC_GetLongLong(ac, &nvars, 0) != AC_OK) {
    return NULL;
  }
  size_t nreducers = array_len(gstp->reducers);
  for (size_t ii = 0; ii < nreducers; ++ii) {
    PLN_Reducer *gr = gstp->reducers + ii;
    if (nvars == AC_NumArgs(&gr->args) && !strcasecmp(gr->name, name) && AC_Equals(ac, &gr->args)) {
      return gr;
    }
  }
  return NULL;
}

int PLNGroupStep_AddReducer(PLN_GroupStep *gstp, const char *name, ArgsCursor *ac,
                            QueryError *status) {
  // Just a list of functions..
  PLN_Reducer *gr = array_ensure_tail(&gstp->reducers, PLN_Reducer);

  gr->name = name;
  int rv = AC_GetVarArgs(ac, &gr->args);
  if (rv != AC_OK) {
    QERR_MKBADARGS_AC(status, name, rv);
    goto error;
  }

  const char *alias = NULL;
  // See if there is an alias
  if (AC_AdvanceIfMatch(ac, "AS")) {
    rv = AC_GetString(ac, &alias, NULL, 0);
    if (rv != AC_OK) {
      QueryError_SetWithUserDataFmt(status, QUERY_EPARSEARGS, "Bad arguments", " for AS: %s", AC_Strerror(rv));
      goto error;
    }
  }
  if (alias == NULL) {
    gr->alias = getReducerAlias(gstp, name, &gr->args);
  } else {
    gr->alias = rm_strdup(alias);
  }
  gr->isHidden = 0; // By default, reducers are not hidden
  return REDISMODULE_OK;

error:
  array_pop(gstp->reducers);
  return REDISMODULE_ERR;
}

static void genericStepFree(PLN_BaseStep *p) {
  rm_free(p);
}

PLN_GroupStep *PLNGroupStep_New(const char **properties, size_t nproperties) {
  PLN_GroupStep *gstp = rm_calloc(1, sizeof(*gstp));
  gstp->properties = properties;
  gstp->nproperties = nproperties;
  gstp->base.dtor = groupStepFree;
  gstp->base.getLookup = groupStepGetLookup;
  gstp->base.type = PLN_T_GROUP;
  return gstp;
}

static int parseGroupby(AREQ *req, ArgsCursor *ac, QueryError *status) {
  ArgsCursor groupArgs = {0};
  const char *s;
  AC_GetString(ac, &s, NULL, AC_F_NOADVANCE);
  int rv = AC_GetVarArgs(ac, &groupArgs);
  if (rv != AC_OK) {
    QueryError_SetWithUserDataFmt(status, QUERY_EPARSEARGS, "Bad arguments", " for GROUPBY: %s", AC_Strerror(rv));
    return REDISMODULE_ERR;
  }

  for (size_t ii = 0; ii < groupArgs.argc; ++ii) {
    if (*(char*)groupArgs.objs[ii] != '@') {
      QueryError_SetWithUserDataFmt(status, QUERY_EPARSEARGS, "Bad arguments for GROUPBY", ": Unknown property `%s`. Did you mean `@%s`?",
                         groupArgs.objs[ii], groupArgs.objs[ii]);
      return REDISMODULE_ERR;
    }
  }

  // Number of fields.. now let's see the reducers
  PLN_GroupStep *gstp = PLNGroupStep_New((const char **)groupArgs.objs, groupArgs.argc);
  AGPLN_AddStep(&req->ap, &gstp->base);

  while (AC_AdvanceIfMatch(ac, "REDUCE")) {
    const char *name;
    if (AC_GetString(ac, &name, NULL, 0) != AC_OK) {
      QueryError_SetWithUserDataFmt(status, QUERY_EPARSEARGS, "Bad arguments", " for REDUCE: %s", AC_Strerror(rv));
      return REDISMODULE_ERR;
    }
    if (PLNGroupStep_AddReducer(gstp, name, ac, status) != REDISMODULE_OK) {
      goto error;
    }
  }
  return REDISMODULE_OK;

error:
  return REDISMODULE_ERR;
}

static void freeFilterStep(PLN_BaseStep *bstp) {
  PLN_MapFilterStep *fstp = (PLN_MapFilterStep *)bstp;
  if (fstp->parsedExpr) {
    ExprAST_Free(fstp->parsedExpr);
  }
  HiddenString_Free(fstp->expr, true);
  rm_free((void *)fstp->base.alias);
  rm_free(bstp);
}

PLN_MapFilterStep *PLNMapFilterStep_New(const HiddenString* expr, int mode) {
  PLN_MapFilterStep *stp = rm_calloc(1, sizeof(*stp));
  stp->base.dtor = freeFilterStep;
  stp->base.type = mode;
  stp->expr = HiddenString_Duplicate(expr);
  return stp;
}

static int handleApplyOrFilter(AREQ *req, ArgsCursor *ac, QueryError *status, int isApply) {
  // Parse filters!
  const char *expr = NULL;
  size_t exprLen;
  int rv = AC_GetString(ac, &expr, &exprLen, 0);
  if (rv != AC_OK) {
    QueryError_SetWithUserDataFmt(status, QUERY_EPARSEARGS, "Bad arguments", " for APPLY/FILTER: %s", AC_Strerror(rv));
    return REDISMODULE_ERR;
  }

  HiddenString* expression = NewHiddenString(expr, exprLen, false);
  PLN_MapFilterStep *stp = PLNMapFilterStep_New(expression, isApply ? PLN_T_APPLY : PLN_T_FILTER);
  HiddenString_Free(expression, false);
  AGPLN_AddStep(&req->ap, &stp->base);

  if (isApply) {
    if (AC_AdvanceIfMatch(ac, "AS")) {
      const char *alias;
      size_t aliasLen;
      if (AC_GetString(ac, &alias, &aliasLen, 0) != AC_OK) {
        QueryError_SetError(status, QUERY_EPARSEARGS, "AS needs argument");
        goto error;
      }
      stp->base.alias = rm_strndup(alias, aliasLen);
    } else {
      stp->base.alias = rm_strndup(expr, exprLen);
    }
  }
  return REDISMODULE_OK;

error:
  if (stp) {
    AGPLN_PopStep(&stp->base);
    stp->base.dtor(&stp->base);
  }
  return REDISMODULE_ERR;
}

static void loadDtor(PLN_BaseStep *bstp) {
  PLN_LoadStep *lstp = (PLN_LoadStep *)bstp;
  rm_free(lstp->keys);
  rm_free(lstp);
}

static int handleLoad(AREQ *req, ArgsCursor *ac, QueryError *status) {
  ArgsCursor loadfields = {0};
  int rc = AC_GetVarArgs(ac, &loadfields);
  if (rc == AC_ERR_PARSE) {
    // Didn't get a number, but we might have gotten a '*'
    const char *s = NULL;
    rc = AC_GetString(ac, &s, NULL, 0);
    if (rc != AC_OK) {
      QueryError_SetWithUserDataFmt(status, QUERY_EPARSEARGS, "Bad arguments", " for LOAD: %s", AC_Strerror(rc));
      return REDISMODULE_ERR;
    } else if (strcmp(s, "*")) {
      QueryError_SetError(status, QUERY_EPARSEARGS, "Bad arguments for LOAD: Expected number of fields or `*`");
      return REDISMODULE_ERR;
    }
    // Successfully got a '*', load all fields
    req->reqflags |= QEXEC_AGG_LOAD_ALL;
  } else if (rc != AC_OK) {
    QueryError_SetWithUserDataFmt(status, QUERY_EPARSEARGS, "Bad arguments", " for LOAD: %s", AC_Strerror(rc));
    return REDISMODULE_ERR;
  }

  PLN_LoadStep *lstp = rm_calloc(1, sizeof(*lstp));
  lstp->base.type = PLN_T_LOAD;
  lstp->base.dtor = loadDtor;
  if (loadfields.argc > 0) {
    lstp->args = loadfields;
    lstp->keys = rm_calloc(loadfields.argc, sizeof(*lstp->keys));
  }

  if (req->reqflags & QEXEC_AGG_LOAD_ALL) {
    lstp->base.flags |= PLN_F_LOAD_ALL;
  }

  AGPLN_AddStep(&req->ap, &lstp->base);
  return REDISMODULE_OK;
}

AREQ *AREQ_New(void) {
  AREQ* req = rm_calloc(1, sizeof(AREQ));
  /*
  unsigned int dialectVersion;
  long long queryTimeoutMS;
  RSTimeoutPolicy timeoutPolicy;
  int printProfileClock;
  uint64_t BM25STD_TanhFactor;
  */
  req->reqConfig = RSGlobalConfig.requestConfigParams;

  // TODO: save only one of the configuration parameters according to the query type
  // once query offset is bounded by both.
  req->maxSearchResults = RSGlobalConfig.maxSearchResults;
  req->maxAggregateResults = RSGlobalConfig.maxAggregateResults;
  req->optimizer = QOptimizer_New();
  req->profile = Profile_PrintDefault;
  return req;
}

static bool hasQuerySortby(const AGGPlan *pln);

int AREQ_Compile(AREQ *req, RedisModuleString **argv, int argc, QueryError *status) {
  req->args = rm_malloc(sizeof(*req->args) * argc);
  req->nargs = argc;
  // Copy the arguments into an owned array of sds strings
  for (size_t ii = 0; ii < argc; ++ii) {
    size_t n;
    const char *s = RedisModule_StringPtrLen(argv[ii], &n);
    req->args[ii] = sdsnewlen(s, n);
  }

  // Parse the query and basic keywords first..
  ArgsCursor ac = {0};
  ArgsCursor_InitSDS(&ac, req->args, req->nargs);

  if (AC_IsAtEnd(&ac)) {
    QueryError_SetError(status, QUERY_EPARSEARGS, "No query string provided");
    return REDISMODULE_ERR;
  }

  req->query = AC_GetStringNC(&ac, NULL);
  AGPLN_Init(&req->ap);

  RSSearchOptions *searchOpts = &req->searchopts;
  RSSearchOptions_Init(searchOpts);
  if (parseQueryArgs(&ac, req, searchOpts, &req->ap, status) != REDISMODULE_OK) {
    goto error;
  }

  int hasLoad = 0;

  // Now we have a 'compiled' plan. Let's get some more options..

  while (!AC_IsAtEnd(&ac)) {
    int rv = handleCommonArgs(req, &ac, status, req->reqflags & QEXEC_F_IS_SEARCH);
    if (rv == ARG_HANDLED) {
      continue;
    } else if (rv == ARG_ERROR) {
      goto error;
    }

    if (AC_AdvanceIfMatch(&ac, "GROUPBY")) {
      if (!ensureExtendedMode(req, "GROUPBY", status)) {
        goto error;
      }
      if (parseGroupby(req, &ac, status) != REDISMODULE_OK) {
        goto error;
      }
    } else if (AC_AdvanceIfMatch(&ac, "APPLY")) {
      if (handleApplyOrFilter(req, &ac, status, 1) != REDISMODULE_OK) {
        goto error;
      }
    } else if (AC_AdvanceIfMatch(&ac, "LOAD")) {
      if (handleLoad(req, &ac, status) != REDISMODULE_OK) {
        goto error;
      }
    } else if (AC_AdvanceIfMatch(&ac, "FILTER")) {
      if (handleApplyOrFilter(req, &ac, status, 0) != REDISMODULE_OK) {
        goto error;
      }
    } else {
      QueryError_FmtUnknownArg(status, &ac, "<main>");
      goto error;
    }
  }

  if (!(req->reqflags & QEXEC_F_SEND_HIGHLIGHT) && !IsScorerNeeded(req) && (!IsSearch(req) || hasQuerySortby(&req->ap))) {
    // We can skip collecting full results structure and metadata from the iterators if:
    // 1. We don't have a highlight/summarize step,
    // 2. We are not required to return scores explicitly,
    // 3. This is not a search query with implicit sorting by query score.
    searchOpts->flags |= Search_CanSkipRichResults;
  }

  return REDISMODULE_OK;

error:
  return REDISMODULE_ERR;
}

static int applyGlobalFilters(RSSearchOptions *opts, QueryAST *ast, const RedisSearchCtx *sctx, unsigned dialect, QueryError *status) {
  /** The following blocks will set filter options on the entire query */
  if (opts->legacy.filters) {
    for (size_t ii = 0; ii < array_len(opts->legacy.filters); ++ii) {
      LegacyNumericFilter *filter = opts->legacy.filters[ii];

      const FieldSpec *fs = IndexSpec_GetField(sctx->spec, filter->field);
      filter->base.fieldSpec = fs;
      if (!fs || !FIELD_IS(fs, INDEXFLD_T_NUMERIC)) {
        if (dialect != 1) {
          const HiddenString *fieldName = filter->field;
          if (fs) {
            QueryError_SetWithUserDataFmt(status, QUERY_EINVAL, "Field is not a numeric field", ", field: %s", HiddenString_GetUnsafe(fieldName, NULL));
          } else {
            QueryError_SetWithUserDataFmt(status, QUERY_EINVAL, "Unknown Field", " '%s'", HiddenString_GetUnsafe(fieldName, NULL));
          }
          return REDISMODULE_ERR;
        } else {
          // On DIALECT 1, we keep the legacy behavior of having an empty iterator when the field is invalid
          QAST_GlobalFilterOptions legacyFilterOpts = {.empty = true};
          QAST_SetGlobalFilters(ast, &legacyFilterOpts);
          continue; // Keep the filter entry in the legacy filters array for AREQ_Free()
        }
      }
      RS_ASSERT(filter->field);
      // Need to free the hidden string since we pass the base pointer to the query AST
      // And we are about to zero out the filter in the legacy filters
      HiddenString_Free(filter->field, false);
      filter->field = NULL;
      QAST_GlobalFilterOptions legacyFilterOpts = {.numeric = &filter->base};
      QAST_SetGlobalFilters(ast, &legacyFilterOpts);
      opts->legacy.filters[ii] = NULL;  // so AREQ_Free() doesn't free the filters themselves, which
                                        // are now owned by the query object
    }
  }
  if (opts->legacy.geo_filters) {
    for (size_t ii = 0; ii < array_len(opts->legacy.geo_filters); ++ii) {
      LegacyGeoFilter *gf = opts->legacy.geo_filters[ii];

      const FieldSpec *fs = IndexSpec_GetField(sctx->spec, gf->field);
      gf->base.fieldSpec = fs;
      if (!fs || !FIELD_IS(fs, INDEXFLD_T_GEO)) {
        if (dialect != 1) {
          const char *generalError = fs ? "Field is not a geo field" : "Unknown Field";
          QueryError_SetWithUserDataFmt(status, QUERY_EINVAL, generalError, ", field: %s", HiddenString_GetUnsafe(gf->field, NULL));
          return REDISMODULE_ERR;
        } else {
          // On DIALECT 1, we keep the legacy behavior of having an empty iterator when the field is invalid
          QAST_GlobalFilterOptions legacyOpts = {.empty = true};
          QAST_SetGlobalFilters(ast, &legacyOpts);
        }
      } else {
        RS_ASSERT(gf->field);
        // Need to free the hidden string since we pass the base pointer to the query AST
        // And we are about to zero out the filter in the legacy filters
        HiddenString_Free(gf->field, false);
        gf->field = NULL;
        QAST_GlobalFilterOptions legacyOpts = {.geo = &gf->base};
        QAST_SetGlobalFilters(ast, &legacyOpts);
        opts->legacy.geo_filters[ii] = NULL;  // so AREQ_Free() doesn't free the filter itself, which is now owned
                                              // by the query object
      }
    }
  }

  if (opts->inkeys) {
    QAST_GlobalFilterOptions filterOpts = {.keys = opts->inkeys, .nkeys = opts->ninkeys};
    QAST_SetGlobalFilters(ast, &filterOpts);
  }
  return REDISMODULE_OK;
}

static bool IsIndexCoherent(AREQ *req) {
  if (req->prefixesOffset == 0) {
    // No prefixes in the query --> No validation needed.
    return true;
  }

  IndexSpec *spec = req->sctx->spec;
  sds *args = req->args;
  long long n_prefixes = strtol(args[req->prefixesOffset + 1], NULL, 10);

  arrayof(HiddenUnicodeString*) spec_prefixes = req->sctx->spec->rule->prefixes;
  if (n_prefixes != array_len(spec_prefixes)) {
    return false;
  }

  // Validate that the prefixes in the arguments are the same as the ones in the
  // index (also in the same order)
  // The first argument is at req->prefixesOffset + 2
  uint base_idx = req->prefixesOffset + 2;
  for (uint i = 0; i < n_prefixes; i++) {
    sds arg = args[base_idx + i];
    if (HiddenUnicodeString_CompareC(spec_prefixes[i], arg) != 0) {
      // Unmatching prefixes
      return false;
    }
  }

  return true;
}

int AREQ_ApplyContext(AREQ *req, RedisSearchCtx *sctx, QueryError *status) {
  // Sort through the applicable options:
  IndexSpec *index = sctx->spec;
  RSSearchOptions *opts = &req->searchopts;
  req->sctx = sctx;

  if (!IsIndexCoherent(req)) {
    QueryError_SetError(status, QUERY_EMISSMATCH, NULL);
    return REDISMODULE_ERR;
  }

  if (isSpecJson(index) && (req->reqflags & QEXEC_F_SEND_HIGHLIGHT)) {
    QueryError_SetError(
        status, QUERY_EINVAL,
        "HIGHLIGHT/SUMMARIZE is not supported with JSON indexes");
    return REDISMODULE_ERR;
  }

  if ((index->flags & Index_StoreByteOffsets) == 0 && (req->reqflags & QEXEC_F_SEND_HIGHLIGHT)) {
    QueryError_SetError(
        status, QUERY_EINVAL,
        "Cannot use HIGHLIGHT/SUMMARIZE because NOOFSETS was specified at index level");
    return REDISMODULE_ERR;
  }

  // Go through the query options and see what else needs to be filled in!
  // 1) INFIELDS
  if (opts->legacy.ninfields) {
    opts->fieldmask = 0;
    for (size_t ii = 0; ii < opts->legacy.ninfields; ++ii) {
      const char *s = opts->legacy.infields[ii];
      t_fieldMask bit = IndexSpec_GetFieldBit(index, s, strlen(s));
      opts->fieldmask |= bit;
    }
  }

  if (opts->language == RS_LANG_UNSET) {
    opts->language = index->rule->lang_default;
  } else if (opts->language == RS_LANG_UNSUPPORTED) {
    QueryError_SetError(status, QUERY_EINVAL, "No such language");
    return REDISMODULE_ERR;
  }

  if (opts->scorerName) {
    if (Extensions_GetScoringFunction(NULL, opts->scorerName) == NULL) {
      QueryError_SetWithoutUserDataFmt(status, QUERY_EINVAL, "No such scorer %s", opts->scorerName);
      return REDISMODULE_ERR;
    }
  }

  bool resp3 = req->protocol == 3;
  if (SetValueFormat(resp3, isSpecJson(index), &req->reqflags, status) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }

  if (!(opts->flags & Search_NoStopWords)) {
    opts->stopwords = sctx->spec->stopwords;
    StopWordList_Ref(sctx->spec->stopwords);
  }

  SetSearchCtx(sctx, req);
  QueryAST *ast = &req->ast;

  unsigned long dialectVersion = req->reqConfig.dialectVersion;

  int rv = QAST_Parse(ast, sctx, opts, req->query, strlen(req->query), dialectVersion, status);
  if (rv != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }

  if (QAST_EvalParams(ast, opts, dialectVersion, status) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }
  if (applyGlobalFilters(opts, ast, sctx, dialectVersion, status) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }

  if (QAST_CheckIsValid(ast, req->sctx->spec, opts, status) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }

  if (!(opts->flags & Search_Verbatim)) {
    if (QAST_Expand(ast, opts->expanderName, opts, sctx, status) != REDISMODULE_OK) {
      return REDISMODULE_ERR;
    }
  }

  // set queryAST configuration parameters
  iteratorsConfig_init(&ast->config);

  if (IsOptimized(req)) {
    // parse inputs for optimizations
    QOptimizer_Parse(req);
    // check possible optimization after creation of QueryNode tree
    QOptimizer_QueryNodes(req->ast.root, req->optimizer);
  }

  if (QueryError_HasError(status)) {
    return REDISMODULE_ERR;
  }

  return REDISMODULE_OK;
}

static ResultProcessor *buildGroupRP(PLN_GroupStep *gstp, RLookup *srclookup,
                                     const RLookupKey ***loadKeys, QueryError *err) {
  const RLookupKey *srckeys[gstp->nproperties], *dstkeys[gstp->nproperties];
  for (size_t ii = 0; ii < gstp->nproperties; ++ii) {
    const char *fldname = gstp->properties[ii] + 1;  // account for the @-
    size_t fldname_len = strlen(fldname);
    srckeys[ii] = RLookup_GetKey_ReadEx(srclookup, fldname, fldname_len, RLOOKUP_F_NOFLAGS);
    if (!srckeys[ii]) {
      if (loadKeys) {
        // We failed to get the key for reading, so we know getting it for loading will succeed.
        srckeys[ii] = RLookup_GetKey_LoadEx(srclookup, fldname, fldname_len, fldname, RLOOKUP_F_NOFLAGS);
        *loadKeys = array_ensure_append_1(*loadKeys, srckeys[ii]);
      }
      // We currently allow implicit loading only for known fields from the schema.
      // If we can't load keys, or the key we loaded is not in the schema, we fail.
      if (!loadKeys || !(srckeys[ii]->flags & RLOOKUP_F_SCHEMASRC)) {
        QueryError_SetWithUserDataFmt(err, QUERY_ENOPROPKEY, "No such property", " `%s`", fldname);
        return NULL;
      }
    }
    dstkeys[ii] = RLookup_GetKey_WriteEx(&gstp->lookup, fldname, fldname_len, RLOOKUP_F_NOFLAGS);
    if (!dstkeys[ii]) {
      QueryError_SetWithUserDataFmt(err, QUERY_EDUPFIELD, "Property", " `%s` specified more than once", fldname);
      return NULL;
    }
  }

  Grouper *grp = Grouper_New(srckeys, dstkeys, gstp->nproperties);

  size_t nreducers = array_len(gstp->reducers);
  for (size_t ii = 0; ii < nreducers; ++ii) {
    // Build the actual reducer
    PLN_Reducer *pr = gstp->reducers + ii;
    ReducerOptions options = REDUCEROPTS_INIT(pr->name, &pr->args, srclookup, loadKeys, err);
    ReducerFactory ff = RDCR_GetFactory(pr->name);
    if (!ff) {
      // No such reducer!
      Grouper_Free(grp);
      QueryError_SetWithUserDataFmt(err, QUERY_ENOREDUCER, "No such reducer", ": %s", pr->name);
      return NULL;
    }
    Reducer *rr = ff(&options);
    if (!rr) {
      Grouper_Free(grp);
      return NULL;
    }

    // Set the destination key for the grouper!
    uint32_t flags = pr->isHidden ? RLOOKUP_F_HIDDEN : RLOOKUP_F_NOFLAGS;
    RLookupKey *dstkey = RLookup_GetKey_Write(&gstp->lookup, pr->alias, flags);
    // Adding the reducer before validating the key, so we free the reducer if the key is invalid
    Grouper_AddReducer(grp, rr, dstkey);
    if (!dstkey) {
      Grouper_Free(grp);
      QueryError_SetWithUserDataFmt(err, QUERY_EDUPFIELD, "Property", " `%s` specified more than once", pr->alias);
      return NULL;
    }
  }

  return Grouper_GetRP(grp);
}

/** Pushes a processor up the stack. Returns the newly pushed processor
 * @param req the request
 * @param rp the processor to push
 * @param rpUpstream previous processor (used as source for rp)
 * @return the processor passed in `rp`.
 */
static ResultProcessor *pushRP(AREQ *req, ResultProcessor *rp, ResultProcessor *rpUpstream) {
  rp->upstream = rpUpstream;
  rp->parent = &req->qiter;
  req->qiter.endProc = rp;
  return rp;
}

static ResultProcessor *getGroupRP(AREQ *req, PLN_GroupStep *gstp, ResultProcessor *rpUpstream,
                                   QueryError *status, bool forceLoad) {
  AGGPlan *pln = &req->ap;
  RLookup *lookup = AGPLN_GetLookup(pln, &gstp->base, AGPLN_GETLOOKUP_PREV);
  RLookup *firstLk = AGPLN_GetLookup(pln, &gstp->base, AGPLN_GETLOOKUP_FIRST); // first lookup can load fields from redis
  const RLookupKey **loadKeys = NULL;
  ResultProcessor *groupRP = buildGroupRP(gstp, lookup, (firstLk == lookup && firstLk->spcache) ? &loadKeys : NULL, status);

  if (!groupRP) {
    array_free(loadKeys);
    return NULL;
  }

  // See if we need a LOADER group here...?
  if (loadKeys) {
    ResultProcessor *rpLoader = RPLoader_New(req, firstLk, loadKeys, array_len(loadKeys), forceLoad);
    array_free(loadKeys);
    RS_LOG_ASSERT(rpLoader, "RPLoader_New failed");
    rpUpstream = pushRP(req, rpLoader, rpUpstream);
  }

  return pushRP(req, groupRP, rpUpstream);
}

static ResultProcessor *getAdditionalMetricsRP(AREQ *req, RLookup *rl, QueryError *status) {
  MetricRequest *requests = req->ast.metricRequests;
  for (size_t i = 0; i < array_len(requests); i++) {
    const char *name = requests[i].metric_name;
    size_t name_len = strlen(name);
    if (IndexSpec_GetFieldWithLength(req->sctx->spec, name, name_len)) {
      QueryError_SetWithUserDataFmt(status, QUERY_EINDEXEXISTS, "Property", " `%s` already exists in schema", name);
      return NULL;
    }
    RLookupKey *key = RLookup_GetKey_WriteEx(rl, name, name_len, RLOOKUP_F_NOFLAGS);
    if (!key) {
      QueryError_SetWithUserDataFmt(status, QUERY_EDUPFIELD, "Property", " `%s` specified more than once", name);
      return NULL;
    }

    // In some cases the iterator that requested the additional field can be NULL (if some other iterator knows early
    // that it has no results), but we still want the rest of the pipeline to know about the additional field name,
    // because there is no syntax error and the sorter should be able to "sort" by this field.
    // If there is a pointer to the node's RLookupKey, write the address.
    if (requests[i].key_ptr)
      *requests[i].key_ptr = key;
  }
  return RPMetricsLoader_New();
}

static ResultProcessor *getArrangeRP(AREQ *req, AGGPlan *pln, const PLN_BaseStep *stp,
                                     QueryError *status, ResultProcessor *up, bool forceLoad) {
  ResultProcessor *rp = NULL;
  PLN_ArrangeStep astp_s = {.base = {.type = PLN_T_ARRANGE}};
  PLN_ArrangeStep *astp = (PLN_ArrangeStep *)stp;
  IndexSpec *spec = req->sctx ? req->sctx->spec : NULL; // check for sctx?
  // Store and count keys that require loading from Redis.
  const RLookupKey **loadKeys = NULL;

  if (!astp) {
    astp = &astp_s;
  }

  size_t maxResults = astp->offset + astp->limit;
  if (!maxResults) {
    maxResults = DEFAULT_LIMIT;
  }

  // TODO: unify if when req holds only maxResults according to the query type.
  //(SEARCH / AGGREGATE)
  maxResults = MIN(maxResults, IsSearch(req) ? req->maxSearchResults : req->maxAggregateResults);

  if (IsCount(req) || !maxResults) {
    rp = RPCounter_New();
    up = pushRP(req, rp, up);
    return up;
  }

  if (req->optimizer->type != Q_OPT_NO_SORTER) {
    if (astp->sortKeys) {
      size_t nkeys = array_len(astp->sortKeys);
      astp->sortkeysLK = rm_malloc(sizeof(*astp->sortKeys) * nkeys);

      const RLookupKey **sortkeys = astp->sortkeysLK;

      RLookup *lk = AGPLN_GetLookup(pln, stp, AGPLN_GETLOOKUP_PREV);

      for (size_t ii = 0; ii < nkeys; ++ii) {
        const char *keystr = astp->sortKeys[ii];
        RLookupKey *sortkey = RLookup_GetKey_Read(lk, keystr, RLOOKUP_F_NOFLAGS);
        if (!sortkey) {
          // if the key is not sortable, and also not loaded by another result processor,
          // add it to the loadkeys list.
          // We failed to get the key for reading, so we can't fail to get it for loading.
          sortkey = RLookup_GetKey_Load(lk, keystr, keystr, RLOOKUP_F_NOFLAGS);
          // We currently allow implicit loading only for known fields from the schema.
          // If the key we loaded is not in the schema, we fail.
          if (!(sortkey->flags & RLOOKUP_F_SCHEMASRC)) {
            QueryError_SetWithUserDataFmt(status, QUERY_ENOPROPKEY, "Property", " `%s` not loaded nor in schema", keystr);
            goto end;
          }
          *array_ensure_tail(&loadKeys, const RLookupKey *) = sortkey;
        }
        sortkeys[ii] = sortkey;
      }
      if (loadKeys) {
        // If we have keys to load, add a loader step.
        ResultProcessor *rpLoader = RPLoader_New(req, lk, loadKeys, array_len(loadKeys), forceLoad);
        up = pushRP(req, rpLoader, up);
      }
      rp = RPSorter_NewByFields(maxResults, sortkeys, nkeys, astp->sortAscMap);
      up = pushRP(req, rp, up);
    } else if (IsSearch(req) && (!IsOptimized(req) || HasScorer(req))) {
      // No sort? then it must be sort by score, which is the default.
      // In optimize mode, add sorter for queries with a scorer.
      rp = RPSorter_NewByScore(maxResults);
      up = pushRP(req, rp, up);
    }
  }

  if (astp->offset || (astp->limit && !rp)) {
    rp = RPPager_New(astp->offset, astp->limit);
    up = pushRP(req, rp, up);
  } else if (IsSearch(req) && IsOptimized(req) && !rp) {
    rp = RPPager_New(0, maxResults);
    up = pushRP(req, rp, up);
  }

end:
  array_free(loadKeys);
  return rp;
}

// Assumes that the spec is locked
static ResultProcessor *getScorerRP(AREQ *req, RLookup *rl) {
  const char *scorer = req->searchopts.scorerName;
  if (!scorer) {
    scorer = DEFAULT_SCORER_NAME;
  }
  ScoringFunctionArgs scargs = {0};
  if (req->reqflags & QEXEC_F_SEND_SCOREEXPLAIN) {
    scargs.scrExp = rm_calloc(1, sizeof(RSScoreExplain));
  }
  if (!strcmp(scorer, BM25_STD_NORMALIZED_TANH_SCORER_NAME)) {
    // Add the tanh factor to the scoring function args
    scargs.tanhFactor = req->reqConfig.BM25STD_TanhFactor;
  }
  ExtScoringFunctionCtx *fns = Extensions_GetScoringFunction(&scargs, scorer);
  RS_LOG_ASSERT(fns, "Extensions_GetScoringFunction failed");
  IndexSpec_GetStats(req->sctx->spec, &scargs.indexStats);
  scargs.qdata = req->ast.udata;
  scargs.qdatalen = req->ast.udatalen;
  const RLookupKey *scoreKey = NULL;
  if (HasScoreInPipeline(req)) {
    scoreKey = RLookup_GetKey_Write(rl, UNDERSCORE_SCORE, RLOOKUP_F_NOFLAGS);
  }
  ResultProcessor *rp = RPScorer_New(fns, &scargs, scoreKey);
  return rp;
}

static bool hasQuerySortby(const AGGPlan *pln) {
  const PLN_BaseStep *bstp = AGPLN_FindStep(pln, NULL, NULL, PLN_T_GROUP);
  const PLN_ArrangeStep *arng = (PLN_ArrangeStep *)AGPLN_FindStep(pln, NULL, bstp, PLN_T_ARRANGE);
  return arng && arng->sortKeys;
}

#define PUSH_RP()                           \
  rpUpstream = pushRP(req, rp, rpUpstream); \
  rp = NULL;

/**
 * Builds the implicit pipeline for querying and scoring, and ensures that our
 * subsequent execution stages actually have data to operate on.
 */
static void buildImplicitPipeline(AREQ *req, QueryError *Status) {
  RedisSearchCtx *sctx = req->sctx;
  req->qiter.conc = &req->conc;
  req->qiter.sctx = sctx;
  req->qiter.err = Status;

  IndexSpecCache *cache = IndexSpec_GetSpecCache(req->sctx->spec);
  RS_LOG_ASSERT(cache, "IndexSpec_GetSpecCache failed")
  RLookup *first = AGPLN_GetLookup(&req->ap, NULL, AGPLN_GETLOOKUP_FIRST);

  RLookup_Init(first, cache);

  ResultProcessor *rp = RPIndexIterator_New(req->rootiter);
  ResultProcessor *rpUpstream = NULL;
  req->qiter.rootProc = req->qiter.endProc = rp;
  PUSH_RP();

  // Load results metrics according to their RLookup key.
  // We need this RP only if metricRequests is not empty.
  if (req->ast.metricRequests) {
    rp = getAdditionalMetricsRP(req, first, Status);
    if (!rp) {
      return;
    }
    PUSH_RP();
  }

  /** Create a scorer if:
   *  * WITHSCORES is defined
   *  * there is no subsequent sorter within this grouping */
  if (IsScorerNeeded(req) ||
      (IsSearch(req) && !IsCount(req) &&
       (IsOptimized(req) ? HasScorer(req) : !hasQuerySortby(&req->ap)))) {
    rp = getScorerRP(req, first);
    PUSH_RP();
    const char *scorerName = req->searchopts.scorerName;
    if (scorerName && !strcmp(scorerName, BM25_STD_NORMALIZED_MAX_SCORER_NAME )) {
      const RLookupKey *scoreKey = NULL;
      if (HasScoreInPipeline(req)) {
        scoreKey = RLookup_GetKey_Write(first, UNDERSCORE_SCORE, RLOOKUP_F_OVERRIDE);
      }
      rp = RPMaxScoreNormalizer_New(scoreKey);
      PUSH_RP();
      }
    }
  }

/**
 * This handles the RETURN and SUMMARIZE keywords, which operate on the result
 * which is about to be returned. It is only used in FT.SEARCH mode
 */
int buildOutputPipeline(AREQ *req, uint32_t loadFlags, QueryError *status, bool forceLoad) {
  AGGPlan *pln = &req->ap;
  ResultProcessor *rp = NULL, *rpUpstream = req->qiter.endProc;

  RLookup *lookup = AGPLN_GetLookup(pln, NULL, AGPLN_GETLOOKUP_LAST);
  // Add a LOAD step...
  const RLookupKey **loadkeys = NULL;
  if (req->outFields.explicitReturn) {
    // Go through all the fields and ensure that each one exists in the lookup stage
    loadFlags |= RLOOKUP_F_EXPLICITRETURN;
    for (size_t ii = 0; ii < req->outFields.numFields; ++ii) {
      const ReturnedField *rf = req->outFields.fields + ii;
      RLookupKey *lk = RLookup_GetKey_Load(lookup, rf->name, rf->path, loadFlags);
      if (lk) {
        *array_ensure_tail(&loadkeys, const RLookupKey *) = lk;
      }
    }
  }

  // If we have explicit return and some of the keys' values are missing,
  // or if we don't have explicit return, meaning we use LOAD ALL
  if (loadkeys || !req->outFields.explicitReturn) {
    rp = RPLoader_New(req, lookup, loadkeys, array_len(loadkeys), forceLoad);
    if (isSpecJson(req->sctx->spec)) {
      // On JSON, load all gets the serialized value of the doc, and doesn't make the fields available.
      lookup->options &= ~RLOOKUP_OPT_ALL_LOADED;
    }
    array_free(loadkeys);
    PUSH_RP();
  }

  if (req->reqflags & QEXEC_F_SEND_HIGHLIGHT) {
    RLookup *lookup = AGPLN_GetLookup(pln, NULL, AGPLN_GETLOOKUP_LAST);
    for (size_t ii = 0; ii < req->outFields.numFields; ++ii) {
      ReturnedField *ff = req->outFields.fields + ii;
      if (req->outFields.defaultField.mode == SummarizeMode_None && ff->mode == SummarizeMode_None) {
        // Ignore - this is a field for `RETURN`, not `SUMMARIZE`
        // (Default mode is not any of the summarize modes, and also there is no mode explicitly specified for this field)
        continue;
      }
      RLookupKey *kk = RLookup_GetKey_Read(lookup, ff->name, RLOOKUP_F_NOFLAGS);
      if (!kk) {
        QueryError_SetWithUserDataFmt(status, QUERY_ENOPROPKEY, "No such property", " `%s`", ff->name);
        goto error;
      } else if (!(kk->flags & RLOOKUP_F_SCHEMASRC)) {
        QueryError_SetWithUserDataFmt(status, QUERY_EINVAL, "Property", " `%s` is not in schema", ff->name);
        goto error;
      }
      ff->lookupKey = kk;
    }
    rp = RPHighlighter_New(&req->searchopts, &req->outFields, lookup);
    PUSH_RP();
  }

  return REDISMODULE_OK;
error:
  return REDISMODULE_ERR;
}

int AREQ_BuildPipeline(AREQ *req, QueryError *status) {
  if (!(req->reqflags & QEXEC_F_BUILDPIPELINE_NO_ROOT)) {
    buildImplicitPipeline(req, status);
    if (status->code != QUERY_OK) {
      goto error;
    }
  }

  AGGPlan *pln = &req->ap;
  ResultProcessor *rp = NULL, *rpUpstream = req->qiter.endProc;

  // If we have a JSON spec, and an "old" API version (DIALECT < 3), we don't store all the data of a multi-value field
  // in the SV as we want to return it, so we need to load and override all requested return fields that are SV source.
  bool forceLoad = req->sctx && isSpecJson(req->sctx->spec) && (req->sctx->apiVersion < APIVERSION_RETURN_MULTI_CMP_FIRST);
  uint32_t loadFlags = forceLoad ? RLOOKUP_F_FORCE_LOAD : RLOOKUP_F_NOFLAGS;

  // Whether we've applied a SORTBY yet..
  int hasArrange = 0;

  for (const DLLIST_node *nn = pln->steps.next; nn != &pln->steps; nn = nn->next) {
    const PLN_BaseStep *stp = DLLIST_ITEM(nn, PLN_BaseStep, llnodePln);

    switch (stp->type) {
      case PLN_T_GROUP: {
        // Adds group result processor and loader if needed.
        rpUpstream = getGroupRP(req, (PLN_GroupStep *)stp, rpUpstream, status, forceLoad);
        if (!rpUpstream) {
          goto error;
        }
        break;
      }

      case PLN_T_ARRANGE: {
        rp = getArrangeRP(req, pln, stp, status, rpUpstream, forceLoad);
        if (!rp) {
          goto error;
        }
        hasArrange = 1;
        rpUpstream = rp;

        break;
      }

      case PLN_T_APPLY:
      case PLN_T_FILTER: {
        PLN_MapFilterStep *mstp = (PLN_MapFilterStep *)stp;
        mstp->parsedExpr = ExprAST_Parse(mstp->expr, status);
        if (!mstp->parsedExpr) {
          goto error;
        }

        // Ensure the lookups can actually find what they need
        RLookup *curLookup = AGPLN_GetLookup(pln, stp, AGPLN_GETLOOKUP_PREV);
        if (!ExprAST_GetLookupKeys(mstp->parsedExpr, curLookup, status)) {
          goto error;
        }

        if (stp->type == PLN_T_APPLY) {
          uint32_t flags = mstp->noOverride ? RLOOKUP_F_NOFLAGS : RLOOKUP_F_OVERRIDE;
          RLookupKey *dstkey = RLookup_GetKey_Write(curLookup, stp->alias, flags);
          if (!dstkey) {
            // Can only happen if we're in noOverride mode
            QueryError_SetWithUserDataFmt(status, QUERY_EDUPFIELD, "Property", " `%s` specified more than once", stp->alias);
            goto error;
          }
          rp = RPEvaluator_NewProjector(mstp->parsedExpr, curLookup, dstkey);
        } else {
          rp = RPEvaluator_NewFilter(mstp->parsedExpr, curLookup);
        }
        PUSH_RP();
        break;
      }

      case PLN_T_LOAD: {
        PLN_LoadStep *lstp = (PLN_LoadStep *)stp;
        RLookup *curLookup = AGPLN_GetLookup(pln, stp, AGPLN_GETLOOKUP_PREV);
        RLookup *rootLookup = AGPLN_GetLookup(pln, NULL, AGPLN_GETLOOKUP_FIRST);
        if (curLookup != rootLookup) {
          QueryError_SetError(status, QUERY_EINVAL,
                              "LOAD cannot be applied after projectors or reducers");
          goto error;
        }
        // Get all the keys for this lookup...
        while (!AC_IsAtEnd(&lstp->args)) {
          size_t name_len;
          const char *name, *path = AC_GetStringNC(&lstp->args, &name_len);
          if (*path == '@') {
            path++;
            name_len--;
          }
          if (AC_AdvanceIfMatch(&lstp->args, SPEC_AS_STR)) {
            int rv = AC_GetString(&lstp->args, &name, &name_len, 0);
            if (rv != AC_OK) {
              QueryError_SetError(status, QUERY_EPARSEARGS, "LOAD path AS name - must be accompanied with NAME");
              return REDISMODULE_ERR;
            } else if (!strcasecmp(name, SPEC_AS_STR)) {
              QueryError_SetError(status, QUERY_EPARSEARGS, "Alias for LOAD cannot be `AS`");
              return REDISMODULE_ERR;
            }
          } else {
            // Set the name to the path. name_len is already the length of the path.
            name = path;
          }

          RLookupKey *kk = RLookup_GetKey_LoadEx(curLookup, name, name_len, path, loadFlags);
          // We only get a NULL return if the key already exists, which means
          // that we don't need to retrieve it again.
          if (kk) {
            lstp->keys[lstp->nkeys++] = kk;
          }
        }
        if (lstp->nkeys || lstp->base.flags & PLN_F_LOAD_ALL) {
          rp = RPLoader_New(req, curLookup, lstp->keys, lstp->nkeys, forceLoad);
          if (isSpecJson(req->sctx->spec)) {
            // On JSON, load all gets the serialized value of the doc, and doesn't make the fields available.
            curLookup->options &= ~RLOOKUP_OPT_ALL_LOADED;
          }
          PUSH_RP();
        }
        break;
      }
      case PLN_T_ROOT:
        // Placeholder step for initial lookup
        break;
      case PLN_T_DISTRIBUTE:
        // This is the root already
        break;
      case PLN_T_INVALID:
      case PLN_T__MAX:
        // not handled yet
        RS_ABORT("Oops");
        break;
    }
  }

  // If no LIMIT or SORT has been applied, do it somewhere here so we don't
  // return the entire matching result set!
  if (!hasArrange && IsSearch(req)) {
    rp = getArrangeRP(req, pln, NULL, status, rpUpstream, forceLoad);
    if (!rp) {
      goto error;
    }
    rpUpstream = rp;
  }

  // If this is an FT.SEARCH command which requires returning of some of the
  // document fields, handle those options in this function
  if (IsSearch(req) && !(req->reqflags & QEXEC_F_SEND_NOFIELDS)) {
    if (buildOutputPipeline(req, loadFlags, status, forceLoad) != REDISMODULE_OK) {
      goto error;
    }
  }

  // In profile mode, we need to add RP_Profile before each RP
  if (IsProfile(req) && req->qiter.endProc) {
    Profile_AddRPs(&req->qiter);
  }

  // Copy timeout policy to the parent struct of the result processors
  req->qiter.timeoutPolicy = req->reqConfig.timeoutPolicy;

  return REDISMODULE_OK;
error:
  return REDISMODULE_ERR;
}

void AREQ_Free(AREQ *req) {
  // First, free the result processors
  ResultProcessor *rp = req->qiter.endProc;
  while (rp) {
    ResultProcessor *next = rp->upstream;
    rp->Free(rp);
    rp = next;
  }
  if (req->rootiter) {
    req->rootiter->Free(req->rootiter);
    req->rootiter = NULL;
  }
  if (req->optimizer) {
    QOptimizer_Free(req->optimizer);
  }

  // Go through each of the steps and free it..
  AGPLN_FreeSteps(&req->ap);

  QAST_Destroy(&req->ast);

  if (req->searchopts.stopwords) {
    StopWordList_Unref((StopWordList *)req->searchopts.stopwords);
  }

  ConcurrentSearchCtx_Free(&req->conc);

  // Finally, free the context. If we are a cursor or have multi workers threads,
  // we need also to detach the ("Thread Safe") context.
  RedisModuleCtx *thctx = NULL;
  if (req->sctx) {
    if (req->reqflags & QEXEC_F_IS_CURSOR) {
      thctx = req->sctx->redisCtx;
      req->sctx->redisCtx = NULL;
    }
    // Here we unlock the spec
    SearchCtx_Free(req->sctx);
  }
  for (size_t ii = 0; ii < req->nargs; ++ii) {
    sdsfree(req->args[ii]);
  }
  if (req->searchopts.legacy.filters) {
    for (size_t ii = 0; ii < array_len(req->searchopts.legacy.filters); ++ii) {
      LegacyNumericFilter *nf = req->searchopts.legacy.filters[ii];
      if (nf) {
        LegacyNumericFilter_Free(req->searchopts.legacy.filters[ii]);
      }
    }
    array_free(req->searchopts.legacy.filters);
  }
  if (req->searchopts.legacy.geo_filters) {
    array_foreach(req->searchopts.legacy.geo_filters, gf, if (gf) LegacyGeoFilter_Free(gf));
    array_free(req->searchopts.legacy.geo_filters);
  }
  if (req->searchopts.params) {
    Param_DictFree(req->searchopts.params);
  }
  FieldList_Free(&req->outFields);
  if (thctx) {
    RedisModule_FreeThreadSafeContext(thctx);
  }
  if(req->requiredFields) {
    array_free(req->requiredFields);
  }
  rm_free(req->args);
  rm_free(req);
}
