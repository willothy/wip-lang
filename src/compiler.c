#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "object.h"
#include "scanner.h"
#include "chunk.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

typedef struct Parser {
	Token *tokens;
	size_t count;
	size_t capacity;
	size_t current;
	bool had_error;
	bool panic_mode;
} Parser;

typedef enum {
	PREC_NONE,
	PREC_ASSIGNMENT, // =
	PREC_OR,    // or
	PREC_AND,   // and
	PREC_EQUALITY, // == !=
	PREC_COMPARISON, // < > <= >=
	PREC_TERM,  // + -
	PREC_FACTOR, // * /
	PREC_UNARY, // ! -
	PREC_CALL,  // . ()
	PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool can_assign);

typedef struct  {
	ParseFn prefix;
	ParseFn infix;
	Precedence precedence;
} ParseRule;


Parser parser;
Compiler *current = NULL;

static Chunk *current_chunk() {
	return &current->function->chunk;
}

static void error_at(Token *token, const char* message) {
	if (parser.panic_mode) return;
	parser.panic_mode = true;
	fprintf(stderr, "[line %zu] Error", token->line);

	switch(token->type) {
	case TOKEN_EOF:
		fprintf(stderr, " at end");
		break;
	case TOKEN_ERROR:
		break;
	default:
		fprintf(stderr, " at '%.*s'", (int)token->length, token->start);
		break;
	}
	fprintf(stderr, ": %s\n", message);
	parser.had_error = true;
}

static void error_at_current(const char *message) {
	error_at(&parser.tokens[parser.current], message);
}

static void error(const char* message) {
	error_at(&parser.tokens[parser.current - 1], message);
}

static Token current_token() {
	if (parser.current >= parser.count) {
		if (parser.count > 0 && parser.tokens[parser.count - 1].type == TOKEN_EOF) {
			// Don't attempt to read past the end of the file.
			// Return the EOF token each time if called repeatedly.
			return parser.tokens[parser.count-1];
		}
		// Since we have backtracking and could have more lookahead in the future,
		// this could require multiple reallocations (though this is very unlikely).
		do {
			if (parser.count + 1 >= parser.capacity) {
				size_t capacity = GROW_CAPACITY(parser.capacity);
				parser.tokens = GROW_ARRAY(Token, parser.tokens, parser.capacity, capacity);
				parser.capacity = capacity;
			}
			Token t = scanner_next_token();
			parser.tokens[parser.count++] = t;
		}
		while (parser.count < parser.current && (parser.tokens[parser.count-1].type != TOKEN_EOF));
	}
	return parser.tokens[parser.current];
}

static inline TokenType current_token_type() {
	return current_token().type;
}

static inline Token prev_token() {
	return parser.tokens[parser.current - 1];
}

static inline Token *ref_prev_token() {
	return &parser.tokens[parser.current - 1];
}

static void backtrack(size_t n) {
	parser.current -= n;
}

static void advance() {
	parser.current++;

	if (parser.current >= parser.count) {
		current_token();
	}

	for (;;) {
		if (current_token().type != TOKEN_ERROR) break;
		error_at_current(current_token().start);
	}
}

static size_t skip_newlines() {
	size_t newlines = 0;
	while (current_token_type() == TOKEN_NEWLINE) {
		newlines++;
		advance();
	}
	return newlines;
}

static bool check(TokenType type) {
	size_t skipped = skip_newlines();
	if (skipped != 0 && (type == TOKEN_NEWLINE || type == TOKEN_SEMICOLON)) {
		backtrack(skipped);
		return true;
	}
	bool rv = current_token_type() == type;
	if (!rv) {
		backtrack(skipped);
	}
	return rv;
}

static bool match(TokenType type) {
	if (!check(type)) {
		return false;
	};
	advance();
	return true;
}

static void consume(TokenType type, const char* message) {
	if (match(type)) {
		return;
	}

	error_at_current(message);
}

void emit_byte(uint8_t byte) {
	chunk_write(current_chunk(), byte, prev_token().line);
}

static void emit_bytes(uint32_t byte1, uint32_t byte2) {
	emit_byte(byte1);
	emit_byte(byte2);
}

static void emit_loop(uint32_t loop_start) {
	emit_byte(OP_LOOP);
	uint32_t offset = current_chunk()->count - loop_start + 4;
	if (offset > UINT32_MAX) {
		error("Loop body too large.");
	}
	emit_byte((offset >> 24) & 0xff);
	emit_byte((offset >> 16) & 0xff);
	emit_byte((offset >> 8) & 0xff);
	emit_byte(offset & 0xff);
}

static uint32_t emit_jump(uint8_t instruction) {
	emit_byte(instruction);
	// The source implementation uses a 16-bit offset, but I'm using 32 bits.
	emit_byte(0xff);
	emit_byte(0xff);
	emit_byte(0xff);
	emit_byte(0xff);
	return current_chunk()->count - 4;
}

static void patch_jump(uint32_t offset) {
	Chunk *chunk = current_chunk();
	uint32_t jump = chunk->count - offset - 4;

	if (jump > UINT32_MAX) {
		error("Too much code to jump over.");
	}

	chunk->code[offset + 0] = (jump >> 24) & 0xff;
	chunk->code[offset + 1] = (jump >> 16) & 0xff;
	chunk->code[offset + 2] = (jump >> 8) & 0xff;
	chunk->code[offset + 3] = jump & 0xff;
}

static void emit_return() {
	emit_byte(OP_NIL);
	emit_byte(OP_RETURN);
}

static uint32_t emit_constant(Value value) {
	return chunk_write_constant(current_chunk(), value, prev_token().line);
}

static void parser_init() {
	parser.tokens = NULL;
	parser.count = 0;
	parser.capacity = 0;
	parser.current = 0;
	parser.had_error = false;
	parser.panic_mode = false;
}

void compiler_init(Compiler *compiler, FunctionType type) {
	compiler->enclosing = current;
	compiler->type = type;
	compiler->scope_depth = 0;
	compiler->function = function_new();
	compiler->local_count = 0;
	current = compiler;

	switch(type) {
	case FN_TYPE_NAMED:
		current->function->name = copy_string((char*)prev_token().start, prev_token().length);
		break;
	case FN_TYPE_ANONYMOUS:
		current->function->name = const_string("", 0);
	case FN_TYPE_SCRIPT:
		break;
	}

	Local *local = &current->locals[current->local_count++];
	local->depth = 0;
	local->is_captured = false;
	local->name.start = "";
	local->name.length = 0;
}

Function *end_compilation() {
	emit_return();
	Function *function = current->function;

	#ifdef DEBUG_PRINT_CODE
	if (!parser.had_error) {
		char *name;
		if (function->name == NULL) {
			name = "<script>";
		} else if (function->name->length == 0) {
			name = "<anonymous>";
		} else {
			name = function->name->chars;
		}
		disassemble_chunk(current_chunk(), name);
	}
	#endif

	current = current->enclosing;

	return function;
}

static void expression();
static void binary(bool can_assign);
static void statement();
static void block();
static ParseRule *get_rule(TokenType type);

static void number(bool can_assign) {
	double value = strtod(prev_token().start, NULL);
	emit_constant(NUMBER_VAL(value));
}

static void parse_precedence(Precedence precedence) {
	skip_newlines();
	advance();
	ParseFn prefix_rule = get_rule(prev_token().type)->prefix;
	if (prefix_rule == NULL) {
		error("Expected expression.");
		return;
	}

	bool can_assign = precedence <= PREC_ASSIGNMENT;
	prefix_rule(can_assign);

	while (precedence <= get_rule(current_token().type)->precedence) {
		advance();
		ParseFn infix_rule = get_rule(prev_token().type)->infix;
		infix_rule(can_assign);
	}

	if (can_assign && match(TOKEN_EQUAL)) {
		error("Invalid assignment target.");
	}
}

static void function(FunctionType type);

static void anon_fun(bool can_assign) {
	function(FN_TYPE_ANONYMOUS);
}

static void expression() {
	parse_precedence(PREC_ASSIGNMENT);
}

static void expression_statement() {
	expression();
	consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
	emit_byte(OP_POP);
}

static void if_statement() {
	// TODO: support nil-matching / := / if let expr
	expression();

	uint32_t then_jump = emit_jump(OP_JUMP_IF_FALSE);
	emit_byte(OP_POP);
	// TODO: support expression result from if statements in expr position
	statement();

	uint32_t else_jump = emit_jump(OP_JUMP);

	patch_jump(then_jump);

	emit_byte(OP_POP);

	if (match(TOKEN_ELSE)) {
		statement();
	}

	patch_jump(else_jump);
}

static void synchronize() {
	parser.panic_mode = false;

	while (current_token().type != TOKEN_EOF) {
		if (prev_token().type == TOKEN_SEMICOLON) return;
		switch (current_token().type) {
		case TOKEN_CLASS:
		case TOKEN_FUN:
		case TOKEN_VAR:
		case TOKEN_FOR:
		case TOKEN_IF:
		case TOKEN_WHILE:
		case TOKEN_RETURN:
			return;
		default:
			;
		}

		advance();
	}
}

static uint32_t identifier_constant(Token *name) {
	Chunk *chunk = current_chunk();
	String *ident = ref_string((char*)name->start, name->length);

	// slower at compile time, but saves memory by deduplicating constant idents
	for (uint32_t i = 0; i < chunk->constants.count; i++) {
		Value value = chunk->constants.values[i];
		if (IS_STRING(value) && AS_STRING(value)->hash == ident->hash) {
			// There is a string with the same hash, so we can
			// just return its index.
			return i;
		}
	}
	return chunk_add_constant(current_chunk(), OBJ_VAL(ident));
}

static void add_local(Token name) {
	// TODO: support more than 256 locals
	if (current->local_count == UINT8_COUNT) {
		error("Too many local variables in function.");
		return;
	}
	Local *local = &current->locals[current->local_count++];
	local->name = name;
	local->is_captured = false;
	local->depth = -1;
}

static void mark_initialized() {
	current->locals[current->local_count - 1].depth = current->scope_depth;
}

static void define_variable() {
	mark_initialized();
}

static void declare_variable() {
#ifndef ALLOW_SHADOWING
	for (int i = current->local_count - 1; i >= 0; i--) {
		Local* local = &current->locals[i];
		if (local->depth != -1 && local->depth < current->scope_depth) {
			break;
		}

		if (identifiers_equal(name, &local->name)) {
			error("Already a variable with this name in this scope.");
		}
	}
#endif

	add_local(prev_token());
}

static void begin_scope() {
	current->scope_depth++;
}

static void end_scope() {
	current->scope_depth--;

	// Drop any locals that were declared in the scope that just ended.
	// TODO: this is O(n) in the number of locals in the scope,
	// but could be O(1) if I kept track of the number of locals
	// at the start of the scope and add a SET_TOP or similar instruction.
	while (current->local_count > 0 &&
	       current->locals[current->local_count - 1].depth >
	       current->scope_depth) {
		if (current->locals[current->local_count - 1].is_captured) {
			emit_byte(OP_CLOSE_UPVALUE);
		} else {
			emit_byte(OP_POP);
		}
		current->local_count--;
	}
}

static void parse_variable(const char *message) {
	consume(TOKEN_IDENTIFIER, message);

	declare_variable();
}

static void var_declaration() {
	parse_variable("Expect variable name.");

	if (match(TOKEN_EQUAL)) {
		expression();
	} else {
		emit_byte(OP_NIL);
	}
	consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

	define_variable();
}

static void function(FunctionType type) {
	Compiler compiler;
	Local locals[UINT8_COUNT];
	compiler_init(&compiler, type);
	begin_scope();

	// Compile the parameter list.
	consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
	if (!check(TOKEN_RIGHT_PAREN)) {
		do {
			current->function->arity++;
			if (current->function->arity > UINT8_MAX) {
				error_at_current("Cannot have more than 255 parameters.");
			}
			parse_variable("Expect parameter name.");
			define_variable();
		} while(match(TOKEN_COMMA));
	}
	consume(TOKEN_RIGHT_PAREN, "Expect ')' after function parameters.");

	consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
	block();

	Function *function = end_compilation();
	uint32_t index = chunk_add_constant(current_chunk(), OBJ_VAL(function));
	if (index > UINT8_MAX) {
		emit_bytes(OP_CLOSURE_LONG, index & 0xff);
		emit_bytes( (index >> 8) & 0xff, (index >> 16) & 0xff);
	} else {
		emit_byte(OP_CLOSURE);
		emit_byte(index);
	}

	for (uint32_t i = 0; i < function->upvalue_count; i++) {
		emit_byte(compiler.upvalues[i].is_local ? 1 : 0);
		emit_byte(compiler.upvalues[i].index);
	}
}

static void fun_declaration() {
	parse_variable("Expect function name.");
	mark_initialized();
	function(FN_TYPE_NAMED);
}

static void coroutine_declaration() {
	parse_variable("Expect coroutine name.");
	mark_initialized();
	function(FN_TYPE_NAMED);
	emit_byte(OP_COROUTINE);
}

void declaration() {
	if (match(TOKEN_FUN)) {
		fun_declaration();
	} else if (match(TOKEN_VAR)) {
		var_declaration();
	} else if (match(TOKEN_COROUTINE)) {
		coroutine_declaration();
	} else {
		statement();
	}

	if (parser.panic_mode) synchronize();
}

static void block() {
	while (!check(TOKEN_RIGHT_BRACE) &&!check(TOKEN_EOF)) {
		declaration();
	}

	consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

uint32_t current_continue_jump = 0;
uint32_t current_break_jump = 0;

typedef struct {
	uint32_t depth;
	size_t ip;
} Break;

Break breaks[UINT8_COUNT];
size_t break_count = 0;

static void push_break(uint32_t depth, size_t ip) {
	if (current->local_count == UINT8_COUNT) {
		error("Too many breaks in loop.");
	}
	Break *brk = &breaks[break_count++];
	brk->depth = depth;
	brk->ip = ip;
}

static Break pop_break() {
	return breaks[--break_count];
}

static Break peek_break() {
	if (break_count == 0) {
		return (Break){ .depth = 0, .ip = 0 };
	}
	return breaks[break_count-1];
}

static void patch_breaks(uint32_t depth, uint32_t offset) {
	while (break_count > 0 && peek_break().depth >= depth) {
		Break brk = pop_break();
		uint8_t *op = &current_chunk()->code[brk.ip]-4;
		offset = current_chunk()->count - brk.ip;

		op[3] = offset & 0xff;
		op[2] = (offset >> 8) & 0xff;
		op[1] = (offset >> 16) & 0xff;
		op[0] = (offset >> 24) & 0xff;
	}
}

static void while_statement() {
	size_t loop_start = current_chunk()->count;

	expression();

	uint32_t cont, brk = current_continue_jump, current_break_jump;

	uint32_t exit_jump = emit_jump(OP_JUMP_IF_FALSE);
	current_break_jump = exit_jump;
	current_continue_jump = loop_start;
	emit_byte(OP_POP);
	begin_scope();
	consume(TOKEN_LEFT_BRACE, "Expect '{' after while condition.");
	block();
	end_scope();
	emit_loop(loop_start);

	patch_breaks(current->scope_depth + 1, current_chunk()->count - exit_jump);
	patch_jump(exit_jump);
	emit_byte(OP_POP);

	current_continue_jump = cont;
	current_break_jump = brk;
}

static void break_statement() {
	if (current->scope_depth == 0) {
		error("Cannot break from top-level code.");
	}
	consume(TOKEN_SEMICOLON, "Expect ';' after 'break'.");
	emit_jump(OP_JUMP);
	push_break(current->scope_depth, current_chunk()->count);
}

static void continue_statement() {
	if (current->scope_depth == 0) {
		error("Cannot continue outside of a loop.");
	}
	consume(TOKEN_SEMICOLON, "Expect ';' after 'continue'.");
	emit_loop(current_continue_jump);
}

static void named_variable(Token name, bool can_assign);

// TODO: Better numeric for syntax, for in syntax with ranges.
// TODO: Support break and continue.
//        - Support labeled blocks.
//        - Support break with value.
// TODO: Support match statements / exprs.
// TODO: Support conditionless loops in expression position.
static void for_statement() {
	begin_scope();
	if (match(TOKEN_SEMICOLON)) {
		// No initializer.
	} else if (match(TOKEN_VAR)) {
		var_declaration();
	} else {
		expression_statement();
	}

	uint32_t loop_start = current_chunk()->count;

	uint32_t exit_jump = 0;
	bool has_condition = false;
	if (!match(TOKEN_SEMICOLON)) {
		expression();
		consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

		exit_jump  = emit_jump(OP_JUMP_IF_FALSE);
		emit_byte(OP_POP);
		has_condition = true;
	}

	uint32_t org_loop_start = loop_start;
	if (!check(TOKEN_LEFT_BRACE)) {
		uint32_t body_jump = emit_jump(OP_JUMP);
		uint32_t increment_start = current_chunk()->count;
		expression();
		emit_byte(OP_POP);
		emit_loop(loop_start);
		loop_start = increment_start;
		patch_jump(body_jump);
	}

	consume(TOKEN_LEFT_BRACE, "Expect '{' after for clauses.");

	block();

	emit_loop(loop_start);

	if (has_condition) {
		patch_jump(exit_jump);
		emit_byte(OP_POP);
	}

	end_scope();
}

static void return_statement() {
	if (current->type == FN_TYPE_SCRIPT) {
		error("Cannot return from top-level code.");
	}

	if (match(TOKEN_SEMICOLON))  {
		emit_byte(OP_NIL);
		emit_byte(OP_RETURN);
	} else {
		expression();
		match(TOKEN_SEMICOLON);
		emit_byte(OP_RETURN);
	}
}

static void and_(bool can_assign) {
	uint32_t end_jump = emit_jump(OP_JUMP_IF_FALSE);

	emit_byte(OP_POP);
	parse_precedence(PREC_AND);

	patch_jump(end_jump);
}

static void or_(bool can_assign) {
	uint32_t else_jump = emit_jump(OP_JUMP_IF_FALSE);
	uint32_t end_jump = emit_jump(OP_JUMP);

	patch_jump(else_jump);
	emit_byte(OP_POP);

	parse_precedence(PREC_OR);
	patch_jump(end_jump);
}

static void yield_statement() {
	if (match(TOKEN_SEMICOLON)) {
		emit_byte(OP_NIL);
		emit_byte(OP_YIELD);
	} else {
		expression();
		consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
		emit_byte(OP_YIELD);
	}
}

static void statement() {
	if (match(TOKEN_IF)) {
		// TODO: support if in expression position
		if_statement();
	} else if (match(TOKEN_RETURN)) {
		return_statement();
	} else if (match(TOKEN_FOR)) {
		for_statement();
	} else if (match(TOKEN_WHILE)) {
		while_statement();
	} else if (match(TOKEN_YIELD)) {
		yield_statement();
	} else if (match(TOKEN_BREAK)) {
		break_statement();
	} else if (match(TOKEN_CONTINUE)) {
		continue_statement();
	} else if (match(TOKEN_LEFT_BRACE)) {
		begin_scope();
		block();
		end_scope();
	} else {
		expression_statement();
	}
}

static void grouping(bool can_assign) {
	expression();
	consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void unary(bool can_assign) {
	TokenType operator_type = prev_token().type;

	parse_precedence(PREC_UNARY); // operand

	switch (operator_type) {
	case TOKEN_MINUS: {
		emit_byte(OP_NEGATE);
		break;
	}
	case TOKEN_BANG: {
		emit_byte(OP_NOT);
		break;
	}
	default: return; // unreachable
	}
}

static void literal(bool can_assign) {
	switch (prev_token().type) {
	case TOKEN_FALSE: {
		emit_byte(OP_FALSE);
		break;
	}
	case TOKEN_NIL: {
		emit_byte(OP_NIL);
		break;
	}
	case TOKEN_TRUE: {
		emit_byte(OP_TRUE);
		break;
	}
	default: return; // unreachable
	}
}

static void string(bool can_assign) {
	String *str = ref_string((char*)prev_token().start + 1, prev_token().length-2);
	emit_constant(OBJ_VAL(str));
}

static bool identifiers_equal(Token *a, Token *b) {
	if (a->length != b->length) return false;
	return memcmp(a->start, b->start, a->length) == 0;
}

static uint8_t add_upvalue(Compiler *compiler, uint32_t index, bool is_local) {
	uint32_t upvalue_count = compiler->function->upvalue_count;

	for (uint8_t i = 0; i < upvalue_count; i++) {
		UpvalueMeta *upvalue = &compiler->upvalues[i];
		if (upvalue->index == index && upvalue->is_local == is_local) {
			return i;
		}
	}

	// TODO: is this needed?
	if (upvalue_count == UINT8_COUNT) {
		error("Too many closure variables in function.");
		return 0;
	}

	compiler->upvalues[upvalue_count].is_local = is_local;
	compiler->upvalues[upvalue_count].index = index;
	return compiler->function->upvalue_count++;
}

typedef struct {
	bool success;
	uint32_t index;
} Resolve;

#define RESOLVE_ERROR (Resolve){ .success = false, .index = 0 }
#define RESOLVE_SUCCESS(idx) (Resolve){ .success = true, .index = idx }

static Resolve resolve_local(Compiler *compiler, Token *name) {
	for (int32_t i = compiler->local_count - 1; i >= 0; i--) {
		Local *local = &compiler->locals[i];
		if (identifiers_equal(name, &local->name)) {
			if (local->depth == -1) {
				// Erroring is the default behavior. Instead, my version of Lox will
				// continue searching for similarly named variables in outer scopes.
				// error("Cannot read local variable in its own initializer.");

				continue;
			}
			return RESOLVE_SUCCESS(i);
		}
	}
	return RESOLVE_ERROR;
}

static Resolve resolve_upvalue(Compiler *compiler, Token *name) {
	if (compiler->enclosing == NULL) return RESOLVE_ERROR;

	Resolve local = resolve_local(compiler->enclosing, name);
	if (local.success) {
		compiler->enclosing->locals[local.index].is_captured = true;
		local.index = add_upvalue(compiler, local.index, true);
		return local;
	}

	Resolve upvalue = resolve_upvalue(compiler->enclosing, name);
	if (upvalue.success) {
		upvalue.index = add_upvalue(compiler, upvalue.index, false);
		return upvalue;
	}

	return RESOLVE_ERROR;
}

static void named_variable(Token name, bool can_assign) {
	uint8_t get_op, set_op;

	Resolve res = resolve_local(current, &name);

	if (res.success) {
		if (res.index > UINT8_MAX) {
			get_op = OP_GET_LOCAL_LONG;
			set_op = OP_SET_LOCAL_LONG;
		} else {
			get_op = OP_GET_LOCAL;
			set_op = OP_SET_LOCAL;
		}
	} else {
		res = resolve_upvalue(current, &name);
		get_op = OP_GET_UPVALUE;
		set_op = OP_SET_UPVALUE;
	}
	if (!res.success) {
		uint32_t global = identifier_constant(&name);
		res.index = global;
		if (global > UINT8_MAX) {
			get_op = OP_GET_GLOBAL_LONG;
			set_op = OP_SET_GLOBAL_LONG;
		} else {
			get_op = OP_GET_GLOBAL;
			set_op = OP_SET_GLOBAL;
		}
	}

	bool set = false;
	if (can_assign && match(TOKEN_EQUAL)) {
		expression();
		set = true;
	}
	emit_bytes(set ? set_op : get_op, res.index & 0xff);
	if (res.index > UINT8_MAX) {
		emit_bytes( (res.index >> 8) & 0xff, (res.index >> 16) & 0xff);
	}
}

static void variable(bool can_assign) {
	named_variable(prev_token(), can_assign);
}

static uint8_t argument_list() {
	uint8_t count = 0;
	if (!check(TOKEN_RIGHT_PAREN)) {
		do {
			expression();
			if (count == UINT8_MAX) {
				error("Cannot have more than 255 arguments.");
			}
			count++;
		} while (match(TOKEN_COMMA));
	}
	consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
	return count;
}

static void call(bool can_assign) {
	uint8_t arg_count = argument_list();
	emit_bytes(OP_CALL, arg_count);
}

static uint32_t array_list() {
	uint32_t count = 0;
	if (!check(TOKEN_RIGHT_BRACKET)) {
		do {
			expression();
			count++;
		} while (match(TOKEN_COMMA));
	}
	match(TOKEN_COMMA); // allow trailing comma
	consume(TOKEN_RIGHT_BRACKET, "Expect ']' after list elements.");
	return count;
}

static void list(bool can_assign) {
	uint32_t arg_count = array_list();
	if (arg_count <= UINT8_MAX) {
		return emit_bytes(OP_LIST, arg_count);
	}

	if (arg_count > (UINT32_MAX >> 8)) {
		error("Too many list elements in initializer.");
	}
	emit_bytes(OP_LIST_LONG, arg_count);
	emit_bytes(arg_count >> 8, arg_count >> 16);
}

static uint32_t dict_entry_list() {
	uint32_t count = 0;
	if (!check(TOKEN_RIGHT_BRACE)) {
		do {
			consume(TOKEN_IDENTIFIER, "Expect dict key after '{'.");
			Token *prev = ref_prev_token();
			uint32_t key = identifier_constant(prev);

			String *str = copy_string(prev->start, prev->length);
			emit_constant(OBJ_VAL(str));

			consume(TOKEN_COLON, "Expect ':' after dict key.");
			expression();
			count++;
		} while (match(TOKEN_COMMA));
	}
	match(TOKEN_COMMA); // allow trailing comma
	consume(TOKEN_RIGHT_BRACE, "Expect '}' after dict elements.");
	return count;
}

static void dict(bool can_assign) {
	uint32_t arg_count = dict_entry_list();
	if (arg_count <= UINT8_MAX) {
		return emit_bytes(OP_DICT, arg_count);
	}

	if (arg_count > (UINT32_MAX >> 8)) {
		error("Too many list elements in initializer.");
	}
	emit_bytes(OP_DICT_LONG, arg_count);
	emit_bytes(arg_count >> 8, arg_count >> 16);
}

static void dot(bool can_assign) {
	consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
	Token *prev = ref_prev_token();
	uint32_t name = identifier_constant(prev);

	bool is_long = name > UINT8_MAX;
	emit_bytes(is_long ? OP_CONSTANT_LONG : OP_CONSTANT, name & 0xff);
	if (is_long) {
		emit_byte((name >> 8) & 0xff);
		emit_byte((name >> 16) & 0xff);
	}

	Opcode op;
	if (can_assign && match(TOKEN_EQUAL)) {
		expression();
		emit_byte(OP_SET_FIELD);
	} else {
		emit_byte(OP_GET_FIELD);
	}
}

static void coroutine(bool can_assign) {
	expression();
	emit_byte(OP_COROUTINE);
}

static void await(bool can_assign) {
	expression();
	emit_byte(OP_AWAIT);
}

static void index_(bool can_assign) {
	expression();
	consume(TOKEN_RIGHT_BRACKET, "Expect ']' after index.");

	if (can_assign && match(TOKEN_EQUAL)) {
		expression();
		emit_byte(OP_SET_FIELD);
	} else {
		emit_byte(OP_GET_FIELD);
	}
}

ParseRule rules[] = {
	[TOKEN_LEFT_PAREN]    = {grouping, call,   PREC_CALL},
	[TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
	[TOKEN_LEFT_BRACE]    = {dict,     NULL,   PREC_NONE},
	[TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
	[TOKEN_LEFT_BRACKET]  = {list,     index_, PREC_CALL},       //new
	[TOKEN_RIGHT_BRACKET] = {NULL,     NULL,   PREC_NONE},       //new
	[TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_DOT]           = {NULL,     dot,    PREC_CALL},
	[TOKEN_DOUBLE_DOT]    = {NULL,     NULL,   PREC_NONE},       //new
	[TOKEN_MINUS]         = {unary,    binary, PREC_TERM},
	[TOKEN_MINUS_EQUAL]   = {NULL,     NULL,   PREC_ASSIGNMENT}, //new
	[TOKEN_ARROW]         = {NULL,     NULL,   PREC_NONE}, //new
	[TOKEN_PLUS]          = {NULL,     binary, PREC_TERM},
	[TOKEN_PLUS_EQUAL]    = {NULL,     NULL,   PREC_ASSIGNMENT}, //new
	[TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
	[TOKEN_NEWLINE]       = {NULL,     NULL,   PREC_NONE},
	[TOKEN_SLASH]         = {NULL,     binary, PREC_FACTOR},
	[TOKEN_SLASH_EQUAL]   = {NULL,     NULL,   PREC_ASSIGNMENT}, //new
	[TOKEN_STAR]          = {NULL,     binary, PREC_FACTOR},
	[TOKEN_STAR_EQUAL]    = {NULL,     NULL,   PREC_ASSIGNMENT}, //new
	[TOKEN_BANG]          = {unary,    NULL,   PREC_NONE},
	[TOKEN_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY},
	[TOKEN_EQUAL]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_EQUAL_EQUAL]   = {NULL,     binary, PREC_EQUALITY},
	[TOKEN_GREATER]       = {NULL,     binary, PREC_COMPARISON},
	[TOKEN_GREATER_EQUAL] = {NULL,     binary, PREC_COMPARISON},
	[TOKEN_LESS]          = {NULL,     binary, PREC_COMPARISON},
	[TOKEN_LESS_EQUAL]    = {NULL,     binary, PREC_COMPARISON},
	[TOKEN_IDENTIFIER]    = {variable, NULL,   PREC_NONE},
	[TOKEN_STRING]        = {string,   NULL,   PREC_NONE},
	[TOKEN_NUMBER]        = {number,   NULL,   PREC_NONE},
	[TOKEN_AND]           = {NULL,     and_,   PREC_AND},
	// [TOKEN_CLASS]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE},
	[TOKEN_FALSE]         = {literal,  NULL,   PREC_NONE},
	[TOKEN_FOR]           = {NULL,     NULL,   PREC_NONE},
	[TOKEN_FUN]           = {anon_fun, NULL,   PREC_NONE},
	[TOKEN_IF]            = {NULL,     NULL,   PREC_NONE},
	[TOKEN_NIL]           = {literal,  NULL,   PREC_NONE},
	[TOKEN_OR]            = {NULL,     or_,    PREC_OR},
	[TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE},
	// [TOKEN_SUPER]         = {NULL,     NULL,   PREC_NONE},
	// Superclasses
	// [TOKEN_SUPER]         = {super_,   NULL,   PREC_NONE},
	// [TOKEN_THIS]          = {NULL,     NULL,   PREC_NONE},
	// Methods and Initializers
	// [TOKEN_THIS]          = {this_,    NULL,   PREC_NONE},
	[TOKEN_TRUE]          = {literal,  NULL,   PREC_NONE},
	[TOKEN_VAR]           = {NULL,     NULL,   PREC_NONE},
	[TOKEN_WHILE]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_ERROR]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_IN]            = {NULL,     NULL,   PREC_NONE},
	[TOKEN_CONTINUE]      = {NULL,     NULL,   PREC_NONE},
	[TOKEN_BREAK]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_COROUTINE]     = {coroutine,     NULL,   PREC_NONE},
	[TOKEN_YIELD]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_AWAIT]         = {await,    NULL,   PREC_NONE},
	[TOKEN_EOF]           = {NULL,     NULL,   PREC_NONE},
};

static ParseRule *get_rule(TokenType type) {
	return &rules[type];
}

static void binary(bool can_assign) {
	TokenType operator_type = prev_token().type;
	ParseRule *rule = get_rule(operator_type);
	parse_precedence((Precedence)(rule->precedence + 1)); // right operand

	switch (operator_type) {
	case TOKEN_BANG_EQUAL: emit_bytes(OP_EQUAL, OP_NOT); break;
	case TOKEN_EQUAL_EQUAL: emit_byte(OP_EQUAL); break;
	case TOKEN_GREATER: emit_byte(OP_GREATER); break;
	case TOKEN_GREATER_EQUAL: emit_bytes(OP_LESS, OP_NOT); break;
	case TOKEN_LESS: emit_byte(OP_LESS); break;
	case TOKEN_LESS_EQUAL: emit_bytes(OP_GREATER, OP_NOT); break;
	case TOKEN_PLUS: emit_byte(OP_ADD); break;
	case TOKEN_MINUS: emit_byte(OP_SUBTRACT); break;
	case TOKEN_STAR: emit_byte(OP_MULTIPLY); break;
	case TOKEN_SLASH: emit_byte(OP_DIVIDE); break;
	default: return; // unreachable
	}
}

void compiler_mark_roots() {
	Compiler *compiler = current;
	while (compiler != NULL) {
		mark_object((Object*)compiler->function);
		compiler = compiler->enclosing;
	}
}

Function* compile(char *src) {
	scanner_init(src);

	parser_init();

	Compiler compiler;
	compiler_init(&compiler, FN_TYPE_SCRIPT);

	while (!match(TOKEN_EOF)) {
		declaration();
	}

	Function *function = end_compilation();

	return parser.had_error ? NULL : function;
}

void compile_line(char *src) {
	scanner_init(src);
	parser_init();
	while (!match(TOKEN_EOF)) {
		declaration();
	}
	emit_return();
}
