/*
* Copyright 2018-2022 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "proc_fulltext_create_index.h"
#include "../value.h"
#include "../errors.h"
#include "../util/arr.h"
#include "../query_ctx.h"
#include "../index/index.h"
#include "../util/rmalloc.h"
#include "../graph/graphcontext.h"
#include "../datatypes/datatypes.h"

//------------------------------------------------------------------------------
// fulltext createNodeIndex
//------------------------------------------------------------------------------

// validate index configuration map
// [required] label <string>
// [optional] stopwords <string[]>
// [optional] language <string>
// configuration can't change if index exists 
static ProcedureResult _validateIndexConfigMap
(
	SIValue config
) {
	SIValue sw;
	SIValue lang;
	SIValue label;

	bool multi_config    = Map_KeyCount(config) > 1;
	bool label_exists    = MAP_GET(config, "label",     label);
	bool lang_exists     = MAP_GET(config, "language",  lang);
	bool stopword_exists = MAP_GET(config, "stopwords", sw);

	if(!label_exists) {
		ErrorCtx_SetError("Label is missing");
		return PROCEDURE_ERR;
	}

	if(multi_config) {
		GraphContext *gc = QueryCtx_GetGraphCtx();
		Schema *s = GraphContext_GetSchema(gc, label.stringval, SCHEMA_NODE);
		if(s && Schema_GetIndex(s, NULL, IDX_FULLTEXT)) {
			ErrorCtx_SetError("Index already exists configuration can't be changed");
			return PROCEDURE_ERR;
		}
	}

	//--------------------------------------------------------------------------
	// validate stopwords
	//--------------------------------------------------------------------------

	if(stopword_exists) {
		if(SI_TYPE(sw) == T_ARRAY) {
			uint stopwords_count = SIArray_Length(sw);
			for (uint i = 0; i < stopwords_count; i++) {
				SIValue stopword = SIArray_Get(sw, i);
				if(SI_TYPE(stopword) != T_STRING) {
					ErrorCtx_SetError("Stopword must be a string");
					return PROCEDURE_ERR;
				}
			}
		} else {
			ErrorCtx_SetError("Stopwords must be array");
			return PROCEDURE_ERR;
		}
	}

	//--------------------------------------------------------------------------
	// validate language
	//--------------------------------------------------------------------------

	if(lang_exists) {
		if(SI_TYPE(lang) != T_STRING) {
			ErrorCtx_SetError("Language must be string");
			return PROCEDURE_ERR;
		}
		if(RediSearch_ValidateLanguage(lang.stringval)) {
			ErrorCtx_SetError("Language is not supported");
			return PROCEDURE_ERR;
		}
	}

	return PROCEDURE_OK;
}

// validate field configuration map
// [required] field <string>
// [optional] weight <number>
// [optional] phonetic <string>
// [optional] nostem <bool>
// configuration can't change if index exists 
static ProcedureResult _validateFieldConfigMap
(
	const char *label,
	SIValue config
) {
	SIValue field;
	SIValue weight;
	SIValue nostem;
	SIValue phonetic;

	bool  multi_config     = Map_KeyCount(config) > 1;
	bool  field_exists     =  MAP_GET(config,  "field",     field);
	bool  weight_exists    =  MAP_GET(config,  "weight",    weight);
	bool  nostem_exists    =  MAP_GET(config,  "nostem",    nostem);
	bool  phonetic_exists  =  MAP_GET(config,  "phonetic",  phonetic);

	// field name is mandatory
	if(!field_exists) {
		ErrorCtx_SetError("Field is missing");
		return PROCEDURE_ERR;
	}

	if((SI_TYPE(field) & T_STRING) == 0) {
		ErrorCtx_SetError("Field must be a string");
		return PROCEDURE_ERR;
	}

	if(weight_exists) {
		if((SI_TYPE(weight) & SI_NUMERIC) == 0) {
			ErrorCtx_SetError("Weight must be numeric");
			return PROCEDURE_ERR;
		}
	}

	if(nostem_exists) {
		if(SI_TYPE(nostem) != T_BOOL) {
			ErrorCtx_SetError("Nostem must be bool");
			return PROCEDURE_ERR;
		}
	}

	if(phonetic_exists) {
		if(SI_TYPE(phonetic) != T_STRING) {
			ErrorCtx_SetError("Phonetic must be a string");
			return PROCEDURE_ERR;
		}
	}

	if(multi_config) {
		// additional configuration is specified
		// make sure field doesn't exists in index, as reconfiguration
		// isn't supported
		GraphContext *gc = QueryCtx_GetGraphCtx();
		Schema *s = GraphContext_GetSchema(gc, label, SCHEMA_NODE);
		Attribute_ID fieldID = GraphContext_GetAttributeID(gc, field.stringval);
		if(fieldID != ATTRIBUTE_ID_NONE && s &&
				Schema_GetIndex(s, &fieldID, IDX_FULLTEXT)) {
			ErrorCtx_SetError("Index already exists configuration can't be changed");
			return PROCEDURE_ERR;
		}
	}

	return PROCEDURE_OK;
}

// CALL db.idx.fulltext.createNodeIndex(label, fields...)
// CALL db.idx.fulltext.createNodeIndex('book', 'title', 'authors')
// CALL db.idx.fulltext.createNodeIndex({label:'L', stopwords:['The']}, 'v')
// CALL db.idx.fulltext.createNodeIndex('L', {field:'v', weight:2.1})
ProcedureResult Proc_FulltextCreateNodeIdxInvoke
(
	ProcedureCtx *ctx,
	const SIValue *args,
	const char **yield
) {
	uint arg_count = array_len((SIValue *)args);
	if(arg_count < 2) {
		ErrorCtx_SetError("Minimum number of arguments is 2");
		return PROCEDURE_ERR;
	}

	// label argument should be of type string or map
	if(!(SI_TYPE(args[0]) & (T_STRING | T_MAP))) {
		ErrorCtx_SetError("Label argument can be string or map");
		return PROCEDURE_ERR;
	}
	if(SI_TYPE(args[0]) == T_MAP &&
			_validateIndexConfigMap(args[0]) == PROCEDURE_ERR) {
		return PROCEDURE_ERR;
	}

	const char *label     = NULL;
	SIValue label_config  = args[0];

	if(SI_TYPE(label_config) == T_STRING) {
		label = label_config.stringval;
	} else if(SI_TYPE(label_config) == T_MAP) {
		SIValue label_value;
		MAP_GET(label_config, "label", label_value);
		label = label_value.stringval;
	}

	// validation, fields arguments should be of type string or map
	for(uint i = 1; i < arg_count; i++) {
		if(!(SI_TYPE(args[i]) & (T_STRING | T_MAP))) {
			ErrorCtx_SetError("Field argument must be string or map");
			return PROCEDURE_ERR;
		}
		if(SI_TYPE(args[i]) == T_MAP &&
			_validateFieldConfigMap(label, args[i]) == PROCEDURE_ERR) {
			return PROCEDURE_ERR;
		}
	}

	// validation passed, create full-text index
	SIValue sw;    // index stopwords
	SIValue lang;  // index language

	int res               = INDEX_FAIL;
	Index *idx            = NULL;
	GraphContext *gc      = QueryCtx_GetGraphCtx();
	uint fields_count     = arg_count - 1; // skip label
	const SIValue *fields = args + 1;      // skip index name

	// introduce fields to index
	for(uint i = 0; i < fields_count; i++) {
		char    *field     =  NULL;
		double  weight     =  INDEX_FIELD_DEFAULT_WEIGHT;
		bool    nostem     =  INDEX_FIELD_DEFAULT_NOSTEM;
		char    *phonetic  =  INDEX_FIELD_DEFAULT_PHONETIC;

		if(SI_TYPE(fields[i]) == T_STRING) {
			field = fields[i].stringval;
		} else {
			SIValue tmp;

			MAP_GET(fields[i], "field", tmp);
			field = tmp.stringval;

			if(MAP_GET(fields[i], "weight", tmp)) weight = SI_GET_NUMERIC(tmp);
			if(MAP_GET(fields[i], "nostem", tmp)) nostem = tmp.longval;
			if(MAP_GET(fields[i], "phonetic", tmp)) phonetic = tmp.stringval;
		}

		res = GraphContext_AddFullTextIndex(&idx, gc, SCHEMA_NODE, label, field,
				weight, nostem, phonetic);
	}

	if(SI_TYPE(label_config) == T_MAP) {
		bool lang_exists     = MAP_GET(label_config, "language",  lang);
		bool stopword_exists = MAP_GET(label_config, "stopwords", sw);

		if(stopword_exists) {
			uint stopwords_count = SIArray_Length(sw);
			char **stopwords = array_new(char*, stopwords_count);
			for (uint i = 0; i < stopwords_count; i++) {
				SIValue stopword = SIArray_Get(sw, i);
				array_append(stopwords, stopword.stringval);
			}
			Index_SetStopwords(idx, stopwords);
			array_free(stopwords);
		}
		if(lang_exists) Index_SetLanguage(idx, lang.stringval);
	}

	// build index
	if(res == INDEX_OK) Index_Construct(idx, gc->g);

	return PROCEDURE_OK;
}

SIValue *Proc_FulltextCreateNodeIdxStep(ProcedureCtx *ctx) {
	return NULL;
}

ProcedureResult Proc_FulltextCreateNodeIdxFree(ProcedureCtx *ctx) {
	return PROCEDURE_OK;
}

ProcedureCtx *Proc_FulltextCreateNodeIdxGen() {
	void *privateData = NULL;
	ProcedureOutput *output = array_new(ProcedureOutput, 0);
	ProcedureCtx *ctx = ProcCtxNew("db.idx.fulltext.createNodeIndex",
								   PROCEDURE_VARIABLE_ARG_COUNT,
								   output,
								   Proc_FulltextCreateNodeIdxStep,
								   Proc_FulltextCreateNodeIdxInvoke,
								   Proc_FulltextCreateNodeIdxFree,
								   privateData,
								   false);

	return ctx;
}

