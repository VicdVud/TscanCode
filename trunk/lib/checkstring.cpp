/*
 * TscanCode - A tool for static C/C++ code analysis
 * Copyright (C) 2017 TscanCode team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


//---------------------------------------------------------------------------
#include "checkstring.h"
#include "symboldatabase.h"

//---------------------------------------------------------------------------

// Register this check class (by creating a static instance of it)
namespace {
    CheckString instance;
}


//---------------------------------------------------------------------------
// Writing string literal is UB
//---------------------------------------------------------------------------
void CheckString::stringLiteralWrite()
{
    const SymbolDatabase *symbolDatabase = _tokenizer->getSymbolDatabase();
    const std::size_t functions = symbolDatabase->functionScopes.size();
    for (std::size_t i = 0; i < functions; ++i) {
        const Scope * scope = symbolDatabase->functionScopes[i];
        for (const Token* tok = scope->classStart->next(); tok != scope->classEnd; tok = tok->next()) {
            if (!tok->variable() || !tok->variable()->isPointer())
                continue;
            const Token *str = tok->getValueTokenMinStrSize();
            if (!str)
                continue;
            if (Token::Match(tok, "%var% [") && Token::simpleMatch(tok->linkAt(1), "] ="))
                stringLiteralWriteError(tok, str);
            else if (Token::Match(tok->previous(), "* %var% ="))
                stringLiteralWriteError(tok, str);
        }
    }
}

void CheckString::stringLiteralWriteError(const Token *tok, const Token *strValue)
{
    std::list<const Token *> callstack;
    callstack.push_back(tok);
    if (strValue)
        callstack.push_back(strValue);

    std::string errmsg("Modifying string literal");
    if (strValue) {
        std::string s = strValue->strValue();
        if (s.size() > 15U)
            s = s.substr(0,13) + "..";
        errmsg += " \"" + s + "\"";
    }
    errmsg += " directly or indirectly is undefined behaviour.";

    reportError(callstack, Severity::error, ErrorType::None, "stringLiteralWrite", errmsg);
}

//---------------------------------------------------------------------------
// Check for string comparison involving two static strings.
// if(strcmp("00FF00","00FF00")==0) // <- statement is always true
//---------------------------------------------------------------------------
void CheckString::checkAlwaysTrueOrFalseStringCompare()
{
    if (!_settings->isEnabled("warning"))
        return;

    for (const Token* tok = _tokenizer->tokens(); tok; tok = tok->next()) {
        if (tok->isName() && Token::Match(tok, "memcmp|strncmp|strcmp|stricmp|strverscmp|bcmp|strcmpi|strcasecmp|strncasecmp|strncasecmp_l|strcasecmp_l|wcsncasecmp|wcscasecmp|wmemcmp|wcscmp|wcscasecmp_l|wcsncasecmp_l|wcsncmp|_mbscmp|_memicmp|_memicmp_l|_stricmp|_wcsicmp|_mbsicmp|_stricmp_l|_wcsicmp_l|_mbsicmp_l (")) {
            if (Token::Match(tok->tokAt(2), "%str% , %str% ,|)")) {
                const std::string &str1 = tok->strAt(2);
                const std::string &str2 = tok->strAt(4);
                alwaysTrueFalseStringCompareError(tok, str1, str2);
                tok = tok->tokAt(5);
            } else if (Token::Match(tok->tokAt(2), "%name% , %name% ,|)")) {
                const std::string &str1 = tok->strAt(2);
                const std::string &str2 = tok->strAt(4);
                if (str1 == str2)
                    alwaysTrueStringVariableCompareError(tok, str1, str2);
                tok = tok->tokAt(5);
            } else if (Token::Match(tok->tokAt(2), "%name% . c_str ( ) , %name% . c_str ( ) ,|)")) {
                const std::string &str1 = tok->strAt(2);
                const std::string &str2 = tok->strAt(8);
                if (str1 == str2)
                    alwaysTrueStringVariableCompareError(tok, str1, str2);
                tok = tok->tokAt(13);
            }
        } else if (tok->isName() && Token::Match(tok, "QString :: compare ( %str% , %str% )")) {
            const std::string &str1 = tok->strAt(4);
            const std::string &str2 = tok->strAt(6);
            alwaysTrueFalseStringCompareError(tok, str1, str2);
            tok = tok->tokAt(7);
        } else if (Token::Match(tok, "!!+ %str% ==|!= %str% !!+")) {
            const std::string &str1 = tok->strAt(1);
            const std::string &str2 = tok->strAt(3);
            alwaysTrueFalseStringCompareError(tok, str1, str2);
            tok = tok->tokAt(5);
        }
        if (!tok)
            break;
    }
}

void CheckString::alwaysTrueFalseStringCompareError(const Token *tok, const std::string& str1, const std::string& str2)
{
    const std::size_t stringLen = 10;
    const std::string string1 = (str1.size() < stringLen) ? str1 : (str1.substr(0, stringLen-2) + "..");
    const std::string string2 = (str2.size() < stringLen) ? str2 : (str2.substr(0, stringLen-2) + "..");

    reportError(tok, Severity::warning, ErrorType::None, "staticStringCompare",
                "Unnecessary comparison of static strings.\n"
                "The compared strings, '" + string1 + "' and '" + string2 + "', are always " + (str1==str2?"identical":"unequal") + ". "
				"Therefore the comparison is unnecessary and looks suspicious.", ErrorLogger::GenWebIdentity(tok->str()));
}

void CheckString::alwaysTrueStringVariableCompareError(const Token *tok, const std::string& str1, const std::string& str2)
{
    reportError(tok, Severity::warning, ErrorType::None, "stringCompare",
                "Comparison of identical string variables.\n"
                "The compared strings, '" + str1 + "' and '" + str2 + "', are identical. "
				"This could be a logic bug.", ErrorLogger::GenWebIdentity(tok->str()));
}


//-----------------------------------------------------------------------------
// Detect "str == '\0'" where "*str == '\0'" is correct.
// Comparing char* with each other instead of using strcmp()
//-----------------------------------------------------------------------------
void CheckString::checkSuspiciousStringCompare()
{
    if (!_settings->isEnabled("warning"))
        return;

    const SymbolDatabase* symbolDatabase = _tokenizer->getSymbolDatabase();
    const std::size_t functions = symbolDatabase->functionScopes.size();
    for (std::size_t i = 0; i < functions; ++i) {
        const Scope * scope = symbolDatabase->functionScopes[i];
        for (const Token* tok = scope->classStart->next(); tok != scope->classEnd; tok = tok->next()) {
            if (tok->tokType() != Token::eComparisonOp)
                continue;

            const Token* varTok = tok->astOperand1();
            const Token* litTok = tok->astOperand2();
            if (!varTok || !litTok)  // <- failed to create AST for comparison
                continue;
            if (varTok->tokType() == Token::eString || varTok->tokType() == Token::eNumber)
                std::swap(varTok, litTok);
            else if (litTok->tokType() != Token::eString && litTok->tokType() != Token::eNumber)
                continue;

            // Pointer addition?
            if (varTok->str() == "+" && _tokenizer->isC()) {
                const Token *tokens[2] = { varTok->astOperand1(), varTok->astOperand2() };
                for (int nr = 0; nr < 2; nr++) {
                    const Token *t = tokens[nr];
                    while (t && (t->str() == "." || t->str() == "::"))
                        t = t->astOperand2();
                    if (t && t->variable() && t->variable()->isPointer())
                        varTok = t;
                }
            }

            if (varTok->str() == "*") {
                if (!_tokenizer->isC() || varTok->astOperand2() != nullptr || litTok->tokType() != Token::eString)
                    continue;
                varTok = varTok->astOperand1();
            }

            while (varTok && (varTok->str() == "." || varTok->str() == "::"))
                varTok = varTok->astOperand2();
            if (!varTok || !varTok->isName())
                continue;

            const Variable *var = varTok->variable();

            while (Token::Match(varTok->astParent(), "[.*]"))
                varTok = varTok->astParent();
            const std::string varname = varTok->expressionString();

            if (litTok->tokType() == Token::eString) {
                if (_tokenizer->isC() || (var && var->isArrayOrPointer()))
                    suspiciousStringCompareError(tok, varname);
            } else if (litTok->originalName() == "'\\0'" && var && var->isPointer()) {
                suspiciousStringCompareError_char(tok, varname);
            }
        }
    }
}

void CheckString::suspiciousStringCompareError(const Token* tok, const std::string& var)
{
    reportError(tok, Severity::warning, ErrorType::None, "literalWithCharPtrCompare",
		"String literal compared with variable '" + var + "'. Did you intend to use strcmp() instead?", ErrorLogger::GenWebIdentity(tok->str()));
}

void CheckString::suspiciousStringCompareError_char(const Token* tok, const std::string& var)
{
    reportError(tok, Severity::warning, ErrorType::None, "charLiteralWithCharPtrCompare",
		"Char literal compared with pointer '" + var + "'. Did you intend to dereference it?", ErrorLogger::GenWebIdentity(tok->str()));
}


//---------------------------------------------------------------------------
// Adding C-string and char with operator+
//---------------------------------------------------------------------------

static bool isChar(const Variable* var)
{
	return (var && !var->isPointer() && !var->isArray() && var->typeStartToken()->str() == "char" && var->typeStartToken()->originalName()=="");
}

void CheckString::strPlusChar()
{
    const SymbolDatabase* symbolDatabase = _tokenizer->getSymbolDatabase();
    const std::size_t functions = symbolDatabase->functionScopes.size();
    for (std::size_t i = 0; i < functions; ++i) {
        const Scope * scope = symbolDatabase->functionScopes[i];
        for (const Token* tok = scope->classStart->next(); tok != scope->classEnd; tok = tok->next()) {
			if (tok->str() == "+" && tok->astOperand1()) {
				if (tok->astOperand1()->tokType() == Token::eString) { // string literal...
					
                    if (tok->astOperand2() && (tok->astOperand2()->tokType() == Token::eChar || isChar(tok->astOperand2()->variable()))) // added to char variable or char constant
                        strPlusCharError(tok);
                }
				else if (tok->astOperand1()->tokType() == Token::eVariable){
					const Variable* varTok = tok->astOperand1()->variable();
					if (varTok && varTok->isPointer() && !varTok->isArray() && varTok->typeStartToken()->str() == "char" && isChar(tok->astOperand2()->variable())){
						strPlusCharError(tok);
					}
				}
            }
        }
    }
}

void CheckString::strPlusInteger()
{
	const SymbolDatabase* symbolDatabase = _tokenizer->getSymbolDatabase();
	const std::size_t functions = symbolDatabase->functionScopes.size();
	for (std::size_t i = 0; i < functions; ++i) {
		const Scope * scope = symbolDatabase->functionScopes[i];
		for (const Token* tok = scope->classStart->next(); tok != scope->classEnd; tok = tok->next()) {
			if (tok->str() == "+" && tok->astOperand1()) {
				if (tok->astOperand1()->tokType() == Token::eString) { // string literal...

					if (tok->astOperand2() && (tok->astOperand2()->tokType() == Token::eNumber/* || isChar(tok->astOperand2()->variable())*/)) // added to char variable or char constant
						strPlusIntegerError(tok);
				}
// 				else if (tok->astOperand1()->tokType() == Token::eVariable) {
// 					const Variable* varTok = tok->astOperand1()->variable();
// 					if (varTok && varTok->isPointer() && !varTok->isArray() && varTok->typeStartToken()->str() == "char" && isChar(tok->astOperand2()->variable())) {
// 						strPlusNumberError(tok);
// 					}
// 				}
			}
		}
	}
}

void CheckString::strPlusCharError(const Token *tok)
{
	reportError(tok, Severity::warning, ErrorType::Suspicious, "strPlusChar", "Unusual pointer arithmetic. A value of type 'char' is added to a string literal.", ErrorLogger::GenWebIdentity(tok->str()));
}

void CheckString::strPlusIntegerError(const Token *tok)
{
	reportError(tok, Severity::warning, ErrorType::Suspicious, "strPlusInteger", "Unusual pointer arithmetic. A value of type 'interger' is added to a string literal.", ErrorLogger::GenWebIdentity(tok->str()));
}

//---------------------------------------------------------------------------
// Implicit casts of string literals to bool
// Comparing string literal with strlen() with wrong length
//---------------------------------------------------------------------------
void CheckString::checkIncorrectStringCompare()
{
    if (!_settings->isEnabled("warning"))
        return;

    const SymbolDatabase *symbolDatabase = _tokenizer->getSymbolDatabase();
    const std::size_t functions = symbolDatabase->functionScopes.size();
    for (std::size_t i = 0; i < functions; ++i) {
        const Scope * scope = symbolDatabase->functionScopes[i];
        for (const Token* tok = scope->classStart->next(); tok != scope->classEnd; tok = tok->next()) {
            // skip "assert(str && ..)" and "assert(.. && str)"
            if (tok->str().size() >= 6U && (tok->str().find("assert") == tok->str().size() - 6U || tok->str().find("ASSERT") == tok->str().size() - 6U) &&
                Token::Match(tok, "%name% (") &&
                (Token::Match(tok->tokAt(2), "%str% &&") || Token::Match(tok->next()->link()->tokAt(-2), "&& %str% )")))
                tok = tok->next()->link();

            if (Token::simpleMatch(tok, ". substr (") && Token::Match(tok->tokAt(3)->nextArgument(), "%num% )")) {
                MathLib::bigint clen = MathLib::toLongNumber(tok->linkAt(2)->strAt(-1));
                const Token* begin = tok->previous();
                for (;;) { // Find start of statement
                    while (begin->link() && Token::Match(begin, "]|)|>"))
                        begin = begin->link()->previous();
                    if (Token::Match(begin->previous(), ".|::"))
                        begin = begin->tokAt(-2);
                    else
                        break;
                }
                begin = begin->previous();
                const Token* end = tok->linkAt(2)->next();
                if (Token::Match(begin->previous(), "%str% ==|!=") && begin->strAt(-2) != "+") {
                    std::size_t slen = Token::getStrLength(begin->previous());
                    if (clen != (int)slen) {
                        incorrectStringCompareError(tok->next(), "substr", begin->strAt(-1));
                    }
                } else if (Token::Match(end, "==|!= %str% !!+")) {
                    std::size_t slen = Token::getStrLength(end->next());
                    if (clen != (int)slen) {
                        incorrectStringCompareError(tok->next(), "substr", end->strAt(1));
                    }
                }
            } else if (Token::Match(tok, "&&|%oror%|( %str% &&|%oror%|)") && !Token::Match(tok, "( %str% )")) {
                incorrectStringBooleanError(tok->next(), tok->strAt(1));
            } else if (Token::Match(tok, "if|while ( %str% )")) {
                incorrectStringBooleanError(tok->tokAt(2), tok->strAt(2));
            }
        }
    }
}

void CheckString::incorrectStringCompareError(const Token *tok, const std::string& func, const std::string &string)
{
	reportError(tok, Severity::warning, ErrorType::None, "incorrectStringCompare", "String literal " + string + " doesn't match length argument for " + func + "().", ErrorLogger::GenWebIdentity(tok->str()));
}

void CheckString::incorrectStringBooleanError(const Token *tok, const std::string& string)
{
	reportError(tok, Severity::warning, ErrorType::None, "incorrectStringBooleanError", "Conversion of string literal " + string + " to bool always evaluates to true.", ErrorLogger::GenWebIdentity(tok->str()));
}

//---------------------------------------------------------------------------
// Overlapping source and destination passed to sprintf().
//---------------------------------------------------------------------------
void CheckString::sprintfOverlappingData()
{
    const SymbolDatabase* symbolDatabase = _tokenizer->getSymbolDatabase();
    const std::size_t functions = symbolDatabase->functionScopes.size();
    for (std::size_t i = 0; i < functions; ++i) {
        const Scope * scope = symbolDatabase->functionScopes[i];
        for (const Token* tok = scope->classStart->next(); tok != scope->classEnd; tok = tok->next()) {
            // Get variable id of target buffer..
            unsigned int varid = 0;

            if (Token::Match(tok, "sprintf|snprintf|swprintf ( %var% ,"))
                varid = tok->tokAt(2)->varId();

            else if (Token::Match(tok, "sprintf|snprintf|swprintf ( %name% . %var% ,"))
                varid = tok->tokAt(4)->varId();

            else
                continue;

            // goto next argument
            const Token *tok2 = tok->tokAt(2)->nextArgument();

            if (tok->str() == "snprintf" || tok->str() == "swprintf") { // Jump over second parameter for snprintf and swprintf
                tok2 = tok2->nextArgument();
                if (!tok2)
                    continue;
            }

            // is any source buffer overlapping the target buffer?
            do {
                if (Token::Match(tok2, "%varid% [,)]", varid)) {
                    sprintfOverlappingDataError(tok2, tok2->str());
                    break;
                }
            } while (nullptr != (tok2 = tok2->nextArgument()));
        }
    }
}

void CheckString::sprintfOverlappingDataError(const Token *tok, const std::string &varname)
{
    reportError(tok, Severity::error, ErrorType::None, "sprintfOverlappingData",
                "Undefined behavior: Variable '" + varname + "' is used as parameter and destination in s[n]printf().\n"
                "The variable '" + varname + "' is used both as a parameter and as destination in "
                "s[n]printf(). The origin and destination buffers overlap. Quote from glibc (C-library) "
                "documentation (http://www.gnu.org/software/libc/manual/html_mono/libc.html#Formatted-Output-Functions): "
                "\"If copying takes place between objects that overlap as a result of a call "
				"to sprintf() or snprintf(), the results are undefined.\"", ErrorLogger::GenWebIdentity(tok->str()));
}
