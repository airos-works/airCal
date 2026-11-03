#include "KeyStoreSecrets.h"

#include <Key.h>
#include <KeyStore.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using json = nlohmann::json;

static const char* kKeyring = "airCal";
static const char* kSecondary = "airCal";


static void
MakeParentDirectories(const std::string& path)
{
	for (size_t slash = path.find('/', 1); slash != std::string::npos;
			slash = path.find('/', slash + 1))
		mkdir(path.substr(0, slash).c_str(), 0700);
}


KeyStoreSecrets::KeyStoreSecrets(const std::string& fallbackPath)
	:
	fFallbackPath(fallbackPath)
{
}


KeyStoreSecrets::~KeyStoreSecrets()
{
}


bool
KeyStoreSecrets::Get(const std::string& key, std::string& value)
{
	std::lock_guard<std::mutex> lock(fLock);
	BKeyStore keyStore;
	BPasswordKey stored;
	if (keyStore.GetKey(kKeyring, B_KEY_TYPE_PASSWORD, key.c_str(), kSecondary,
			false, stored) == B_OK && stored.Password() != NULL) {
		value = stored.Password();
		return true;
	}
	// Not in the key store, or no key store: the fallback file may have it
	// (written while the key store was unavailable).
	return FileGet(key, value);
}


bool
KeyStoreSecrets::Set(const std::string& key, const std::string& value)
{
	std::lock_guard<std::mutex> lock(fLock);
	if (EnsureKeyring()) {
		BKeyStore keyStore;
		BPasswordKey existing;
		if (keyStore.GetKey(kKeyring, B_KEY_TYPE_PASSWORD, key.c_str(),
				kSecondary, false, existing) == B_OK)
			keyStore.RemoveKey(kKeyring, existing);
		BPasswordKey secret(value.c_str(), B_KEY_PURPOSE_GENERIC, key.c_str(),
			kSecondary);
		if (keyStore.AddKey(kKeyring, secret) == B_OK) {
			// A stale copy from a time without key store must not win later.
			FileRemove(key);
			return true;
		}
	}
	return FileSet(key, value);
}


void
KeyStoreSecrets::Remove(const std::string& key)
{
	std::lock_guard<std::mutex> lock(fLock);
	BKeyStore keyStore;
	BPasswordKey existing;
	if (keyStore.GetKey(kKeyring, B_KEY_TYPE_PASSWORD, key.c_str(), kSecondary,
			false, existing) == B_OK)
		keyStore.RemoveKey(kKeyring, existing);
	FileRemove(key);
}


bool
KeyStoreSecrets::EnsureKeyring()
{
	if (fKeyringReady)
		return true;
	BKeyStore keyStore;
	status_t status = keyStore.AddKeyring(kKeyring);
	fKeyringReady = status == B_OK || status == B_NAME_IN_USE;
	return fKeyringReady;
}


// #pragma mark - Fallback file


bool
KeyStoreSecrets::FileGet(const std::string& key, std::string& value)
{
	std::map<std::string, std::string> entries;
	if (!ReadFile(entries))
		return false;
	auto found = entries.find(key);
	if (found == entries.end())
		return false;
	value = found->second;
	return true;
}


bool
KeyStoreSecrets::FileSet(const std::string& key, const std::string& value)
{
	std::map<std::string, std::string> entries;
	ReadFile(entries);
	entries[key] = value;
	return WriteFile(entries);
}


void
KeyStoreSecrets::FileRemove(const std::string& key)
{
	std::map<std::string, std::string> entries;
	if (!ReadFile(entries) || entries.erase(key) == 0)
		return;
	if (entries.empty())
		unlink(fFallbackPath.c_str());
	else
		WriteFile(entries);
}


bool
KeyStoreSecrets::ReadFile(std::map<std::string, std::string>& entries) const
{
	FILE* file = fopen(fFallbackPath.c_str(), "rb");
	if (file == NULL)
		return false;
	std::string text;
	char buffer[8192];
	size_t count;
	while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.append(buffer, count);
	fclose(file);

	json root = json::parse(text, nullptr, false);
	if (!root.is_object())
		return false;
	for (auto it = root.begin(); it != root.end(); ++it) {
		if (it.value().is_string())
			entries[it.key()] = it.value().get<std::string>();
	}
	return true;
}


bool
KeyStoreSecrets::WriteFile(const std::map<std::string, std::string>& entries) const
{
	json root = json::object();
	for (auto& entry : entries)
		root[entry.first] = entry.second;
	std::string text = root.dump(1, '\t');

	MakeParentDirectories(fFallbackPath);
	std::string temporary = fFallbackPath + ".new";
	int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return false;
	fchmod(fd, 0600);
	size_t written = 0;
	while (written < text.size()) {
		ssize_t result = write(fd, text.data() + written, text.size() - written);
		if (result < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			unlink(temporary.c_str());
			return false;
		}
		written += (size_t)result;
	}
	fsync(fd);
	close(fd);
	if (rename(temporary.c_str(), fFallbackPath.c_str()) != 0) {
		unlink(temporary.c_str());
		return false;
	}
	return true;
}
