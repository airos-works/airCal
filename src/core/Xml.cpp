#include "Xml.h"

#include <cstdlib>
#include <cstring>
#include <map>

namespace aircal {
namespace xml {

const char* const kDav = "DAV:";
const char* const kCalDav = "urn:ietf:params:xml:ns:caldav";
const char* const kCalServer = "http://calendarserver.org/ns/";
const char* const kApple = "http://apple.com/ns/ical/";


static std::string
Trim(const std::string& text)
{
	size_t begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos)
		return "";
	size_t end = text.find_last_not_of(" \t\r\n");
	return text.substr(begin, end - begin + 1);
}


const Element*
Element::Child(const std::string& n, const std::string& local) const
{
	for (auto& child : children) {
		if (child->name == local && child->ns == n)
			return child.get();
	}
	return nullptr;
}


std::vector<const Element*>
Element::Children(const std::string& n, const std::string& local) const
{
	std::vector<const Element*> result;
	for (auto& child : children) {
		if (child->name == local && child->ns == n)
			result.push_back(child.get());
	}
	return result;
}


const Element*
Element::Find(const std::string& n, const std::string& local) const
{
	for (auto& child : children) {
		if (child->name == local && child->ns == n)
			return child.get();
		const Element* found = child->Find(n, local);
		if (found != nullptr)
			return found;
	}
	return nullptr;
}


void
Element::FindAll(const std::string& n, const std::string& local,
	std::vector<const Element*>& out) const
{
	for (auto& child : children) {
		if (child->name == local && child->ns == n)
			out.push_back(child.get());
		child->FindAll(n, local, out);
	}
}


std::string
Element::ChildText(const std::string& n, const std::string& local) const
{
	const Element* child = Child(n, local);
	return child != nullptr ? Trim(child->text) : std::string();
}


std::string
Element::Attribute(const std::string& local) const
{
	for (auto& attribute : attributes) {
		if (attribute.first == local)
			return attribute.second;
	}
	return "";
}


namespace {

void
AppendUtf8(std::string& out, unsigned long code)
{
	if (code < 0x80)
		out += (char)code;
	else if (code < 0x800) {
		out += (char)(0xc0 | (code >> 6));
		out += (char)(0x80 | (code & 0x3f));
	} else if (code < 0x10000) {
		out += (char)(0xe0 | (code >> 12));
		out += (char)(0x80 | ((code >> 6) & 0x3f));
		out += (char)(0x80 | (code & 0x3f));
	} else if (code < 0x110000) {
		out += (char)(0xf0 | (code >> 18));
		out += (char)(0x80 | ((code >> 12) & 0x3f));
		out += (char)(0x80 | ((code >> 6) & 0x3f));
		out += (char)(0x80 | (code & 0x3f));
	}
}


std::string
Decode(const std::string& text)
{
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] != '&') {
			out += text[i];
			continue;
		}
		size_t end = text.find(';', i);
		if (end == std::string::npos || end - i > 12) {
			out += text[i];
			continue;
		}
		std::string entity = text.substr(i + 1, end - i - 1);
		if (entity == "lt")
			out += '<';
		else if (entity == "gt")
			out += '>';
		else if (entity == "amp")
			out += '&';
		else if (entity == "quot")
			out += '"';
		else if (entity == "apos")
			out += '\'';
		else if (!entity.empty() && entity[0] == '#') {
			unsigned long code = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')
				? strtoul(entity.c_str() + 2, nullptr, 16)
				: strtoul(entity.c_str() + 1, nullptr, 10);
			AppendUtf8(out, code);
		} else {
			out += text.substr(i, end - i + 1);
		}
		i = end;
	}
	return out;
}


struct Parser {
	const std::string& text;
	size_t pos = 0;
	int depth = 0;

	explicit Parser(const std::string& t) : text(t) {}

	bool StartsWith(const char* s) const
	{
		return text.compare(pos, strlen(s), s) == 0;
	}

	void SkipSpace()
	{
		while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t'
				|| text[pos] == '\r' || text[pos] == '\n'))
			pos++;
	}

	// Skips <?...?>, <!--...-->, <!DOCTYPE ...>.
	bool SkipMisc()
	{
		if (StartsWith("<?")) {
			size_t end = text.find("?>", pos);
			if (end == std::string::npos)
				return false;
			pos = end + 2;
			return true;
		}
		if (StartsWith("<!--")) {
			size_t end = text.find("-->", pos);
			if (end == std::string::npos)
				return false;
			pos = end + 3;
			return true;
		}
		if (StartsWith("<!DOCTYPE") || StartsWith("<!doctype")) {
			size_t end = text.find('>', pos);
			if (end == std::string::npos)
				return false;
			pos = end + 1;
			return true;
		}
		return false;
	}

	std::string Name()
	{
		size_t start = pos;
		while (pos < text.size()) {
			char c = text[pos];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/'
				|| c == '>' || c == '=')
				break;
			pos++;
		}
		return text.substr(start, pos - start);
	}

	typedef std::map<std::string, std::string> Scope;

	static std::string Resolve(const std::string& prefix, const Scope& scope)
	{
		auto found = scope.find(prefix);
		return found != scope.end() ? found->second : std::string();
	}

	std::unique_ptr<Element> ParseElement(const Scope& parentScope)
	{
		if (++depth > 200)
			return nullptr;
		if (pos >= text.size() || text[pos] != '<')
			return nullptr;
		pos++;
		std::string qname = Name();
		if (qname.empty())
			return nullptr;

		Scope scope = parentScope;
		std::vector<std::pair<std::string, std::string>> rawAttributes;
		bool selfClosing = false;
		while (true) {
			SkipSpace();
			if (pos >= text.size())
				return nullptr;
			if (text[pos] == '/') {
				if (pos + 1 >= text.size() || text[pos + 1] != '>')
					return nullptr;
				pos += 2;
				selfClosing = true;
				break;
			}
			if (text[pos] == '>') {
				pos++;
				break;
			}
			std::string attributeName = Name();
			SkipSpace();
			if (attributeName.empty() || pos >= text.size() || text[pos] != '=')
				return nullptr;
			pos++;
			SkipSpace();
			if (pos >= text.size() || (text[pos] != '"' && text[pos] != '\''))
				return nullptr;
			char quote = text[pos++];
			size_t end = text.find(quote, pos);
			if (end == std::string::npos)
				return nullptr;
			std::string value = Decode(text.substr(pos, end - pos));
			pos = end + 1;
			if (attributeName == "xmlns")
				scope[""] = value;
			else if (attributeName.compare(0, 6, "xmlns:") == 0)
				scope[attributeName.substr(6)] = value;
			else
				rawAttributes.push_back({attributeName, value});
		}

		auto element = std::make_unique<Element>();
		size_t colon = qname.find(':');
		if (colon != std::string::npos) {
			element->ns = Resolve(qname.substr(0, colon), scope);
			element->name = qname.substr(colon + 1);
		} else {
			element->ns = Resolve("", scope);
			element->name = qname;
		}
		for (auto& attribute : rawAttributes) {
			size_t c = attribute.first.find(':');
			element->attributes.push_back({c == std::string::npos
				? attribute.first : attribute.first.substr(c + 1), attribute.second});
		}
		if (selfClosing) {
			depth--;
			return element;
		}

		while (pos < text.size()) {
			if (text[pos] != '<') {
				size_t end = text.find('<', pos);
				if (end == std::string::npos)
					return nullptr;
				element->text += Decode(text.substr(pos, end - pos));
				pos = end;
				continue;
			}
			if (StartsWith("<![CDATA[")) {
				size_t end = text.find("]]>", pos);
				if (end == std::string::npos)
					return nullptr;
				element->text += text.substr(pos + 9, end - pos - 9);
				pos = end + 3;
				continue;
			}
			if (StartsWith("<!--") || StartsWith("<?")) {
				if (!SkipMisc())
					return nullptr;
				continue;
			}
			if (StartsWith("</")) {
				size_t end = text.find('>', pos);
				if (end == std::string::npos)
					return nullptr;
				std::string closing = Trim(text.substr(pos + 2, end - pos - 2));
				if (closing != qname)
					return nullptr;
				pos = end + 1;
				depth--;
				return element;
			}
			std::unique_ptr<Element> child = ParseElement(scope);
			if (!child)
				return nullptr;
			element->children.push_back(std::move(child));
		}
		return nullptr;
	}
};

}	// namespace


std::unique_ptr<Element>
Parse(const std::string& text)
{
	Parser parser(text);
	// A UTF-8 byte order mark.
	if (text.compare(0, 3, "\xef\xbb\xbf") == 0)
		parser.pos = 3;
	while (true) {
		parser.SkipSpace();
		if (parser.pos >= text.size())
			return nullptr;
		if (!parser.SkipMisc())
			break;
	}
	Parser::Scope scope;
	scope["xml"] = "http://www.w3.org/XML/1998/namespace";
	return parser.ParseElement(scope);
}


std::string
Escape(const std::string& text)
{
	std::string out;
	for (char c : text) {
		switch (c) {
			case '<': out += "&lt;"; break;
			case '>': out += "&gt;"; break;
			case '&': out += "&amp;"; break;
			case '"': out += "&quot;"; break;
			case '\'': out += "&apos;"; break;
			default: out += c;
		}
	}
	return out;
}

}	// namespace xml
}	// namespace aircal
