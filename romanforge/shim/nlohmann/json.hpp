// romanforge shim: the editor's property encyclopedia (json) is never loaded here.
#pragma once
#include <string>
#include <map>
namespace nlohmann {
class json {
public:
	struct iterator {
		const std::pair<const std::string, json>* p = nullptr;
		bool operator==(const iterator& o) const { return p == o.p; }
		bool operator!=(const iterator& o) const { return p != o.p; }
		const json& value() const { return p->second; }
		const json* operator->() const { return &p->second; }
		const json& operator*() const { return p->second; }
		const std::string& key() const { return p->first; }
	};
	iterator find(const std::string&) const { return {}; }
	iterator begin() const { return {}; }
	iterator end() const { return {}; }
	bool is_string() const { return false; }
	bool is_object() const { return false; }
	bool is_array() const { return false; }
	bool is_number() const { return false; }
	bool contains(const std::string&) const { return false; }
	const json& at(const std::string&) const { return *this; }
	const json& operator[](const std::string&) const { return *this; }
	template <class T> T get() const { return T(); }
	size_t size() const { return 0; }
};
}
