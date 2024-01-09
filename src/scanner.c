#include <stdio.h>
#include <string.h>

#include "common.h"
#include "scanner.h"
#include "chunk.h"

typedef struct {
	const char* start;
	const char* current;
	linenr_t line;
} scanner_t;

scanner_t scanner;


static token_t token(token_type_t type) {
	token_t token;
	token.type = type;
	token.start = scanner.start;
	token.length = (size_t)(scanner.current - scanner.start);
	token.line = scanner.line;
	return token;
}

static bool is_at_end() {
	return *scanner.current == '\0';
}

static bool is_digit(char c){
	return c >= '0' && c <= '9';
}

static bool is_alpha(char c) {
	return (c >= 'a' && c <= 'z') ||
	       (c >= 'A' && c <= 'Z') ||
	       c == '_';
}

static char advance() {
	return *scanner.current++;
}

static char peek() {
	return *scanner.current;
}

static char peek_next() {
	if (is_at_end()) return '\0';
	return scanner.current[1];
}

static bool match(char expected) {
	if (is_at_end()) return false;
	if (peek() != expected) return false;
	scanner.current++;
	return true;
}

static void skip_whitespace() {
	for (;;) {
		char c = peek();
		switch (c) {
		case ' ':
		case '\r':
		case '\t':
			advance();
			break;
		case '\n': {
			scanner.line++;
			advance();
			break;
		}
		case '/': {
			if (peek_next() == '/') {
				while (peek() != '\n' && !is_at_end()) advance();
			} else {
				return;
			}
		}
		default:
			return;
		}
	}
}


static token_t error_token(const char* message) {
	token_t token;
	token.type = TOKEN_ERROR;
	token.start = message;
	token.length = (size_t)strlen(message);
	token.line = scanner.line;
	return token;
}

static token_t string() {
	while (peek() != '"' && !is_at_end()) {
		if (peek() == '\n') scanner.line++;
		advance();
	}

	if (is_at_end()) {
		return error_token("Unterminated string.");
	}

	advance();
	return token(TOKEN_STRING);
}

static token_t number() {
	while (is_digit(peek())) advance();

	if (peek() == '.' && is_digit(peek_next())) {
		advance();
		while (is_digit(peek())) advance();
	}
	return token(TOKEN_NUMBER);
}

static token_type_t check_keyword(size_t start, size_t length, const char* rest, token_type_t type) {
	if (scanner.current - scanner.start == start + length &&
	    memcmp(scanner.start + start, rest, length) == 0) {
		return type;
	}
	return TOKEN_IDENTIFIER;
}

static token_type_t ident_type() {
	switch (*scanner.start) {
	// TODO: use tries
	case 'a': return check_keyword(1, 2, "nd", TOKEN_AND);
	case 'c': return check_keyword(1, 4, "lass", TOKEN_CLASS);
	case 'e': return check_keyword(1, 3, "lse", TOKEN_ELSE);
	case 'f': {
		if (scanner.current - scanner.start > 1) {
			switch(scanner.start[1]) {
			case 'a': return check_keyword(2, 3, "lse", TOKEN_FALSE);
			case 'o': return check_keyword(2, 1, "r", TOKEN_FOR);
			case 'u': return check_keyword(2, 1, "n", TOKEN_FUN);
			}
		}
		break;
	}
	case 'i': return check_keyword(1, 1, "f", TOKEN_IF);
	case 'n': return check_keyword(1, 2, "il", TOKEN_NIL);
	case 'o': return check_keyword(1, 1, "r", TOKEN_OR);
	case 'p': return check_keyword(1, 4, "rint", TOKEN_PRINT);
	case 'r': return check_keyword(1, 5, "eturn", TOKEN_RETURN);
	case 's': return check_keyword(1, 4, "uper", TOKEN_SUPER);
	case 't': {
		if (scanner.current - scanner.start > 1) {
			switch (scanner.start[1]) {
			case 'h': return check_keyword(2, 2, "is", TOKEN_THIS);
			case 'r': return check_keyword(2, 2, "ue", TOKEN_TRUE);
			}
		}
		break;
	}
	case 'v': return check_keyword(1, 2, "ar", TOKEN_VAR);
	case 'w': return check_keyword(1, 4, "hile", TOKEN_WHILE);
	}

	return TOKEN_IDENTIFIER;
}

static token_t ident() {
	while (is_alpha(peek()) || is_digit(peek())) advance();
	return token(ident_type());
}

void scanner_init(const char* source) {
	scanner.start = source;
	scanner.current = source;
	scanner.line = 1;
}

token_t scanner_next_token() {
	skip_whitespace();
	scanner.start = scanner.current;

	if (is_at_end()) return token(TOKEN_EOF);

	char c = advance();

	switch (c) {
	// single-character tokens
	case '(': return token(TOKEN_LEFT_PAREN);
	case ')': return token(TOKEN_RIGHT_PAREN);
	case '{': return token(TOKEN_LEFT_BRACE);
	case '}': return token(TOKEN_RIGHT_BRACE);
	case ';': return token(TOKEN_SEMICOLON);
	case ',': return token(TOKEN_COMMA);
	case '.': return token(TOKEN_DOT);
	case '-': return token(TOKEN_MINUS);
	case '+': return token(TOKEN_PLUS);
	case '/': return token(TOKEN_SLASH);
	case '*': return token(TOKEN_STAR);

	// complex tokens
	case '!': {
		return token(match('=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
	}
	case '=': {
		return token(match('=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
	}
	case '<': {
		return token(match('=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
	}
	case '>': {
		return token(match('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
	}

	// string
	case '"': {
		return string();
	}
	}

	if (is_alpha(c)) {
		return ident();
	}
	if (is_digit(c)) {
		return number();
	}

	return error_token("Unexpected character.");
}