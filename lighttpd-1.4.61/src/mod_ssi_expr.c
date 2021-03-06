#include "first.h"

#include "buffer.h"
#include "log.h"
#include "mod_ssi.h"
#include "mod_ssi_expr.h"
#include "mod_ssi_exprparser.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const char *input;
	size_t offset;
	size_t size;

	int line_pos;

	int in_key;
	int in_brace;
	int in_cond;
} ssi_tokenizer_t;

ssi_val_t *ssi_val_init(void) {
	ssi_val_t *s;

	s = calloc(1, sizeof(*s));
	force_assert(s);

	return s;
}

void ssi_val_free(ssi_val_t *s) {
	if (s->str) buffer_free(s->str);

	free(s);
}

__attribute_pure__
int ssi_val_tobool(ssi_val_t *B) {
	if (B->type == SSI_TYPE_STRING) {
		return !buffer_is_blank(B->str);
	} else {
		return B->bo;
	}
}

static int ssi_expr_tokenizer(handler_ctx *p,
			      ssi_tokenizer_t *t, int *token_id, buffer *token) {
	int tid = 0;
	size_t i;

	for (tid = 0; tid == 0 && t->offset < t->size && t->input[t->offset] ; ) {
		char c = t->input[t->offset];
		const data_string *ds;

		switch (c) {
		case '=':
			tid = TK_EQ;

			t->offset++;
			t->line_pos++;

			buffer_copy_string_len(token, CONST_STR_LEN("(=)"));

			break;
		case '>':
			if (t->input[t->offset + 1] == '=') {
				t->offset += 2;
				t->line_pos += 2;

				tid = TK_GE;

				buffer_copy_string_len(token, CONST_STR_LEN("(>=)"));
			} else {
				t->offset += 1;
				t->line_pos += 1;

				tid = TK_GT;

				buffer_copy_string_len(token, CONST_STR_LEN("(>)"));
			}

			break;
		case '<':
			if (t->input[t->offset + 1] == '=') {
				t->offset += 2;
				t->line_pos += 2;

				tid = TK_LE;

				buffer_copy_string_len(token, CONST_STR_LEN("(<=)"));
			} else {
				t->offset += 1;
				t->line_pos += 1;

				tid = TK_LT;

				buffer_copy_string_len(token, CONST_STR_LEN("(<)"));
			}

			break;

		case '!':
			if (t->input[t->offset + 1] == '=') {
				t->offset += 2;
				t->line_pos += 2;

				tid = TK_NE;

				buffer_copy_string_len(token, CONST_STR_LEN("(!=)"));
			} else {
				t->offset += 1;
				t->line_pos += 1;

				tid = TK_NOT;

				buffer_copy_string_len(token, CONST_STR_LEN("(!)"));
			}

			break;
		case '&':
			if (t->input[t->offset + 1] == '&') {
				t->offset += 2;
				t->line_pos += 2;

				tid = TK_AND;

				buffer_copy_string_len(token, CONST_STR_LEN("(&&)"));
			} else {
				log_error(p->errh, __FILE__, __LINE__,
				  "pos: %d missing second &", t->line_pos);
				return -1;
			}

			break;
		case '|':
			if (t->input[t->offset + 1] == '|') {
				t->offset += 2;
				t->line_pos += 2;

				tid = TK_OR;

				buffer_copy_string_len(token, CONST_STR_LEN("(||)"));
			} else {
				log_error(p->errh, __FILE__, __LINE__,
				  "pos: %d missing second |", t->line_pos);
				return -1;
			}

			break;
		case '\t':
		case ' ':
			t->offset++;
			t->line_pos++;
			break;

		case '\'':
			/* search for the terminating " */
			for (i = 1; t->input[t->offset + i] && t->input[t->offset + i] != '\'';  i++);

			if (t->input[t->offset + i]) {
				tid = TK_VALUE;

				buffer_copy_string_len(token, t->input + t->offset + 1, i-1);

				t->offset += i + 1;
				t->line_pos += i + 1;
			} else {
				/* ERROR */
				log_error(p->errh, __FILE__, __LINE__,
				  "pos: %d missing closing quote", t->line_pos);
				return -1;
			}

			break;
		case '(':
			t->offset++;
			t->in_brace++;

			tid = TK_LPARAN;

			buffer_copy_string_len(token, CONST_STR_LEN("("));
			break;
		case ')':
			t->offset++;
			t->in_brace--;

			tid = TK_RPARAN;

			buffer_copy_string_len(token, CONST_STR_LEN(")"));
			break;
		case '$':
			if (t->input[t->offset + 1] == '{') {
				for (i = 2; t->input[t->offset + i] && t->input[t->offset + i] != '}';  i++);

				if (t->input[t->offset + i] != '}') {
					log_error(p->errh, __FILE__, __LINE__,
					  "pos: %d missing closing curly-brace", t->line_pos);
					return -1;
				}

				buffer_copy_string_len(token, t->input + t->offset + 2, i-3);
			} else {
				for (i = 1; isalpha(((unsigned char *)t->input)[t->offset + i]) ||
					    t->input[t->offset + i] == '_' ||
					    ((i > 1) && isdigit(((unsigned char *)t->input)[t->offset + i]));  i++);

				buffer_copy_string_len(token, t->input + t->offset + 1, i-1);
			}

			tid = TK_VALUE;

			if (NULL != (ds = (const data_string *)array_get_element_klen(p->ssi_cgi_env, BUF_PTR_LEN(token)))) {
				buffer_copy_buffer(token, &ds->value);
			} else if (NULL != (ds = (const data_string *)array_get_element_klen(p->ssi_vars, BUF_PTR_LEN(token)))) {
				buffer_copy_buffer(token, &ds->value);
			} else {
				buffer_copy_string_len(token, CONST_STR_LEN(""));
			}

			t->offset += i;
			t->line_pos += i;

			break;
		default:
			for (i = 0; isgraph(((unsigned char *)t->input)[t->offset + i]);  i++) {
				char d = t->input[t->offset + i];
				switch(d) {
				case ' ':
				case '\t':
				case ')':
				case '(':
				case '\'':
				case '=':
				case '!':
				case '<':
				case '>':
				case '&':
				case '|':
					break;
				}
			}

			tid = TK_VALUE;

			buffer_copy_string_len(token, t->input + t->offset, i);

			t->offset += i;
			t->line_pos += i;

			break;
		}
	}

	if (tid) {
		*token_id = tid;

		return 1;
	} else if (t->offset < t->size) {
		log_error(p->errh, __FILE__, __LINE__,
		  "pos: %d foobar", t->line_pos);
	}
	return 0;
}

int ssi_eval_expr(handler_ctx *p, const char *expr) {
	ssi_tokenizer_t t;
	void *pParser;
	int token_id;
	buffer *token;
	ssi_ctx_t context;
	int ret;

	t.input = expr;
	t.offset = 0;
	t.size = strlen(expr);
	t.line_pos = 1;

	t.in_key = 1;
	t.in_brace = 0;
	t.in_cond = 0;

	context.ok = 1;

	/* default context */

	pParser = ssiexprparserAlloc( malloc );
	force_assert(pParser);
	token = buffer_init();
	while((1 == (ret = ssi_expr_tokenizer(p, &t, &token_id, token))) && context.ok) {
		ssiexprparser(pParser, token_id, token, &context);

		token = buffer_init();
	}
	ssiexprparser(pParser, 0, token, &context);
	ssiexprparserFree(pParser, free );

	buffer_free(token);

	if (ret == -1) {
		log_error(p->errh, __FILE__, __LINE__, "expr parser failed");
		return -1;
	}

	if (context.ok == 0) {
		log_error(p->errh, __FILE__, __LINE__,
		  "pos: %d parser failed somehow near here", t.line_pos);
		return -1;
	}
#if 0
	log_error(p->errh, __FILE__, __LINE__,
	  "expr: %s %d", expr, context.val.bo);
#endif
	return context.val.bo;
}
