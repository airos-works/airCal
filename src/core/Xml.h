// A small namespace-aware XML reader and writer for WebDAV/CalDAV bodies.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace aircal {
namespace xml {

struct Element {
	std::string ns;		// namespace URI ("DAV:", "urn:ietf:params:xml:ns:caldav")
	std::string name;	// local name
	std::vector<std::pair<std::string, std::string>> attributes;	// local name, value
	std::string text;	// concatenated character data directly inside
	std::vector<std::unique_ptr<Element>> children;

	const Element* Child(const std::string& ns, const std::string& name) const;
	std::vector<const Element*> Children(const std::string& ns,
		const std::string& name) const;
	// First descendant with the name (depth first).
	const Element* Find(const std::string& ns, const std::string& name) const;
	void FindAll(const std::string& ns, const std::string& name,
		std::vector<const Element*>& out) const;
	// Text of the child, trimmed; "" if absent.
	std::string ChildText(const std::string& ns, const std::string& name) const;
	std::string Attribute(const std::string& name) const;
};

// Parses a document; nullptr on malformed input.
std::unique_ptr<Element> Parse(const std::string& text);

std::string Escape(const std::string& text);	// for text and attribute values

extern const char* const kDav;		// "DAV:"
extern const char* const kCalDav;	// "urn:ietf:params:xml:ns:caldav"
extern const char* const kCalServer;	// "http://calendarserver.org/ns/"
extern const char* const kApple;	// "http://apple.com/ns/ical/"

}	// namespace xml
}	// namespace aircal
