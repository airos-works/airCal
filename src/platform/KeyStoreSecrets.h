// Account credentials in Haiku's key store (keyring "airCal"), with a
// private JSON file as the fallback when the key store is unavailable (no
// keystore_server, a locked keyring the user refused to open, ...).
#pragma once

#include "sync/Provider.h"

#include <map>
#include <mutex>
#include <string>

class KeyStoreSecrets : public aircal::Secrets {
public:
	explicit KeyStoreSecrets(const std::string& fallbackPath);
	~KeyStoreSecrets() override;

	bool Get(const std::string& key, std::string& value) override;
	bool Set(const std::string& key, const std::string& value) override;
	void Remove(const std::string& key) override;

private:
	bool EnsureKeyring();
	bool FileGet(const std::string& key, std::string& value);
	bool FileSet(const std::string& key, const std::string& value);
	void FileRemove(const std::string& key);
	bool ReadFile(std::map<std::string, std::string>& entries) const;
	bool WriteFile(const std::map<std::string, std::string>& entries) const;

	std::string fFallbackPath;
	std::mutex fLock;
	bool fKeyringReady = false;
};
