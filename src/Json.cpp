/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "Json.h"

#include <limits.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>


namespace {

class Parser {
public:
								Parser(const std::string& text)
									: fText(text), fPosition(0), fOk(true),
									  fDepth(0) {}

			Json				Parse();

			bool				Ok() const { return fOk; }

private:
			void				_SkipSpace();
			bool				_ParseValue(Json& value);
			bool				_ParseObject(Json& value);
			bool				_ParseArray(Json& value);
			bool				_ParseString(std::string& value);
			bool				_ParseNumber(Json& value);
			bool				_ParseHex4(unsigned int& code);
			bool				_Literal(const char* text, const Json& value,
									Json& result);
	static	void				_AppendUtf8(std::string& out,
									unsigned int code);

			const std::string&	fText;
			size_t				fPosition;
			bool				fOk;
			int					fDepth;	// of arrays and objects: a limit, not
											// the end of the stack
};


void
Parser::_SkipSpace()
{
	while (fPosition < fText.size()) {
		char c = fText[fPosition];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			fPosition++;
		else
			break;
	}
}


Json
Parser::Parse()
{
	Json value;
	_SkipSpace();
	if (!_ParseValue(value)) {
		fOk = false;
		return Json();
	}
	_SkipSpace();
	if (fPosition != fText.size())
		fOk = false;
	return fOk ? value : Json();
}


bool
Parser::_ParseValue(Json& value)
{
	_SkipSpace();
	if (fPosition >= fText.size())
		return false;
	char c = fText[fPosition];
	switch (c) {
		case '{':
		case '[':
		{
			// what Cinema reads is a few levels deep: thousands would
			// overflow the stack of the thread
			if (fDepth >= 64)
				return false;
			fDepth++;
			bool ok = c == '{' ? _ParseObject(value) : _ParseArray(value);
			fDepth--;
			return ok;
		}
		case '"':
		{
			std::string text;
			if (!_ParseString(text))
				return false;
			value = Json(text);
			return true;
		}
		case 't':
			return _Literal("true", Json(true), value);
		case 'f':
			return _Literal("false", Json(false), value);
		case 'n':
			return _Literal("null", Json(), value);
		default:
			if (c == '-' || (c >= '0' && c <= '9'))
				return _ParseNumber(value);
			return false;
	}
}


bool
Parser::_Literal(const char* text, const Json& literal, Json& value)
{
	size_t length = std::strlen(text);
	if (fText.compare(fPosition, length, text) != 0)
		return false;
	fPosition += length;
	value = literal;
	return true;
}


bool
Parser::_ParseObject(Json& value)
{
	value = Json::MakeObject();
	fPosition++;	// {

	_SkipSpace();
	if (fPosition < fText.size() && fText[fPosition] == '}') {
		fPosition++;
		return true;
	}

	while (true) {
		_SkipSpace();
		if (fPosition >= fText.size() || fText[fPosition] != '"')
			return false;
		std::string key;
		if (!_ParseString(key))
			return false;

		_SkipSpace();
		if (fPosition >= fText.size() || fText[fPosition] != ':')
			return false;
		fPosition++;

		Json member;
		if (!_ParseValue(member))
			return false;
		value.Set(key.c_str(), member);

		_SkipSpace();
		if (fPosition >= fText.size())
			return false;
		if (fText[fPosition] == ',') {
			fPosition++;
			continue;
		}
		if (fText[fPosition] == '}') {
			fPosition++;
			return true;
		}
		return false;
	}
}


bool
Parser::_ParseArray(Json& value)
{
	value = Json::MakeArray();
	fPosition++;	// [

	_SkipSpace();
	if (fPosition < fText.size() && fText[fPosition] == ']') {
		fPosition++;
		return true;
	}

	while (true) {
		Json member;
		if (!_ParseValue(member))
			return false;
		value.Add(member);

		_SkipSpace();
		if (fPosition >= fText.size())
			return false;
		if (fText[fPosition] == ',') {
			fPosition++;
			continue;
		}
		if (fText[fPosition] == ']') {
			fPosition++;
			return true;
		}
		return false;
	}
}


bool
Parser::_ParseString(std::string& value)
{
	if (fPosition >= fText.size() || fText[fPosition] != '"')
		return false;
	fPosition++;
	value.clear();

	while (fPosition < fText.size()) {
		unsigned char c = fText[fPosition++];
		if (c == '"')
			return true;
		if (c != '\\') {
			value += (char)c;
			continue;
		}
		if (fPosition >= fText.size())
			return false;
		char escape = fText[fPosition++];
		switch (escape) {
			case '"': value += '"'; break;
			case '\\': value += '\\'; break;
			case '/': value += '/'; break;
			case 'b': value += '\b'; break;
			case 'f': value += '\f'; break;
			case 'n': value += '\n'; break;
			case 'r': value += '\r'; break;
			case 't': value += '\t'; break;
			case 'u':
			{
				unsigned int code;
				if (!_ParseHex4(code))
					return false;
				if (code >= 0xd800 && code <= 0xdbff) {
					// half of a surrogate pair: the other one follows it
					if (fPosition + 1 >= fText.size()
						|| fText[fPosition] != '\\'
						|| fText[fPosition + 1] != 'u')
						return false;
					fPosition += 2;
					unsigned int low;
					if (!_ParseHex4(low) || low < 0xdc00 || low > 0xdfff)
						return false;
					code = 0x10000 + ((code - 0xd800) << 10)
						+ (low - 0xdc00);
				}
				_AppendUtf8(value, code);
				break;
			}
			default:
				return false;
		}
	}
	return false;
}


bool
Parser::_ParseHex4(unsigned int& code)
{
	if (fPosition + 4 > fText.size())
		return false;
	code = 0;
	for (int i = 0; i < 4; i++) {
		char c = fText[fPosition++];
		code <<= 4;
		if (c >= '0' && c <= '9')
			code |= c - '0';
		else if (c >= 'a' && c <= 'f')
			code |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			code |= c - 'A' + 10;
		else
			return false;
	}
	return true;
}


void
Parser::_AppendUtf8(std::string& out, unsigned int code)
{
	if (code < 0x80) {
		out += (char)code;
	} else if (code < 0x800) {
		out += (char)(0xc0 | (code >> 6));
		out += (char)(0x80 | (code & 0x3f));
	} else if (code < 0x10000) {
		out += (char)(0xe0 | (code >> 12));
		out += (char)(0x80 | ((code >> 6) & 0x3f));
		out += (char)(0x80 | (code & 0x3f));
	} else {
		out += (char)(0xf0 | (code >> 18));
		out += (char)(0x80 | ((code >> 12) & 0x3f));
		out += (char)(0x80 | ((code >> 6) & 0x3f));
		out += (char)(0x80 | (code & 0x3f));
	}
}


bool
Parser::_ParseNumber(Json& value)
{
	size_t start = fPosition;
	if (fPosition < fText.size() && fText[fPosition] == '-')
		fPosition++;
	while (fPosition < fText.size()
		&& std::isdigit((unsigned char)fText[fPosition]))
		fPosition++;
	if (fPosition < fText.size() && fText[fPosition] == '.') {
		fPosition++;
		while (fPosition < fText.size()
			&& std::isdigit((unsigned char)fText[fPosition]))
			fPosition++;
	}
	if (fPosition < fText.size()
		&& (fText[fPosition] == 'e' || fText[fPosition] == 'E')) {
		fPosition++;
		if (fPosition < fText.size()
			&& (fText[fPosition] == '+' || fText[fPosition] == '-'))
			fPosition++;
		while (fPosition < fText.size()
			&& std::isdigit((unsigned char)fText[fPosition]))
			fPosition++;
	}
	if (fPosition == start)
		return false;

	std::string number(fText, start, fPosition - start);
	char* end = NULL;
	double result = std::strtod(number.c_str(), &end);
	if (end == number.c_str() || *end != '\0')
		return false;
	value = Json(result);
	return true;
}

}	// namespace


Json::Json()
	:
	fType(kNull),
	fBool(false),
	fNumber(0)
{
}


Json::Json(bool value)
	:
	fType(kBool),
	fBool(value),
	fNumber(0)
{
}


Json::Json(int value)
	:
	fType(kNumber),
	fBool(false),
	fNumber(value)
{
}


Json::Json(double value)
	:
	fType(kNumber),
	fBool(false),
	fNumber(value)
{
}


Json::Json(const char* value)
	:
	fType(kString),
	fBool(false),
	fNumber(0),
	fString(value != NULL ? value : "")
{
}


Json::Json(const std::string& value)
	:
	fType(kString),
	fBool(false),
	fNumber(0),
	fString(value)
{
}


Json
Json::Parse(const std::string& text, bool* ok)
{
	Parser parser(text);
	Json value = parser.Parse();
	if (ok != NULL)
		*ok = parser.Ok();
	return value;
}


Json
Json::MakeObject()
{
	Json value;
	value.fType = kObject;
	return value;
}


Json
Json::MakeArray()
{
	Json value;
	value.fType = kArray;
	return value;
}


bool
Json::Bool(bool fallback) const
{
	return fType == kBool ? fBool : fallback;
}


int
Json::Int(int fallback) const
{
	return fType == kNumber ? (int)fNumber : fallback;
}


double
Json::Number(double fallback) const
{
	return fType == kNumber ? fNumber : fallback;
}


const std::string&
Json::String() const
{
	static const std::string empty;
	return fType == kString ? fString : empty;
}


const Json*
Json::Find(const char* key) const
{
	if (fType != kObject)
		return NULL;
	std::map<std::string, Json>::const_iterator found
		= fObject.find(key);
	return found == fObject.end() ? NULL : &found->second;
}


std::string
Json::GetString(const char* key, const char* fallback) const
{
	const Json* value = Find(key);
	if (value == NULL || value->GetType() != kString)
		return fallback;
	return value->fString;
}


int
Json::GetInt(const char* key, int fallback) const
{
	const Json* value = Find(key);
	if (value == NULL || value->GetType() != kNumber)
		return fallback;
	// out of range, or not a number: the cast would be undefined
	double number = value->fNumber;
	if (!(number >= INT_MIN && number <= INT_MAX))
		return fallback;
	return (int)number;
}


double
Json::GetDouble(const char* key, double fallback) const
{
	const Json* value = Find(key);
	if (value == NULL || value->GetType() != kNumber)
		return fallback;
	return value->fNumber;
}


bool
Json::GetBool(const char* key, bool fallback) const
{
	const Json* value = Find(key);
	if (value == NULL || value->GetType() != kBool)
		return fallback;
	return value->fBool;
}


void
Json::Set(const char* key, const Json& value)
{
	if (fType != kObject) {
		fType = kObject;
		fArray.clear();
		fString.clear();
	}
	fObject[key] = value;
}


void
Json::Add(const Json& value)
{
	if (fType != kArray) {
		fType = kArray;
		fObject.clear();
		fString.clear();
	}
	fArray.push_back(value);
}


std::string
Json::ToString() const
{
	std::string out;
	_Write(out);
	return out;
}


void
Json::_Write(std::string& out) const
{
	switch (fType) {
		case kNull:
			out += "null";
			break;
		case kBool:
			out += fBool ? "true" : "false";
			break;
		case kNumber:
		{
			char buffer[64];
			double whole;
			if (std::modf(fNumber, &whole) == 0 && std::fabs(fNumber) < 1e15)
				std::snprintf(buffer, sizeof(buffer), "%.0f", fNumber);
			else
				std::snprintf(buffer, sizeof(buffer), "%.17g", fNumber);
			out += buffer;
			break;
		}
		case kString:
		{
			out += '"';
			for (size_t i = 0; i < fString.size(); i++) {
				unsigned char c = fString[i];
				switch (c) {
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\b': out += "\\b"; break;
					case '\f': out += "\\f"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					default:
						if (c < 0x20) {
							char buffer[8];
							std::snprintf(buffer, sizeof(buffer), "\\u%04x",
								c);
							out += buffer;
						} else
							out += (char)c;
				}
			}
			out += '"';
			break;
		}
		case kArray:
			out += '[';
			for (size_t i = 0; i < fArray.size(); i++) {
				if (i > 0)
					out += ',';
				fArray[i]._Write(out);
			}
			out += ']';
			break;
		case kObject:
		{
			out += '{';
			bool first = true;
			for (std::map<std::string, Json>::const_iterator it
					= fObject.begin(); it != fObject.end(); ++it) {
				if (!first)
					out += ',';
				first = false;
				Json(it->first)._Write(out);
				out += ':';
				it->second._Write(out);
			}
			out += '}';
			break;
		}
	}
}
