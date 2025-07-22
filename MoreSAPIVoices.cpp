// MoreSAPIVoices.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <windows.h>
#include <vector>
#include <set>
#include <string>
#include <iostream>
#include <cwctype>


static void PrintWindowsErrorMessage(LSTATUS errorCode) {
	wchar_t* errorMsg = nullptr;

	// Format the error message
	FormatMessageW(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, errorCode, 0, reinterpret_cast<LPWSTR>(&errorMsg), 0, nullptr);

	if (errorMsg) {
		std::wcout << L"Error Code " << errorCode << L": " << errorMsg << std::endl;
		LocalFree(errorMsg); // Free allocated memory
	}
	else {
		std::wcout << L"Unknown error code: " << errorCode << std::endl;
	}
}

static std::wstring ExpandEnvironmentVariables(const std::wstring& input)
{
	// Get the size of the expanded string
	DWORD size = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
	if (size == 0) {
		return std::wstring();
	}

	// Create a buffer to hold the expanded string
	std::vector<wchar_t> buffer(size);

	// Expand the environment string
	DWORD result = ExpandEnvironmentStringsW(input.c_str(), buffer.data(), size);
	if (result == 0 || result > size) {
		return std::wstring();
	}

	// Convert the buffer to a std::wstring
	return std::wstring(buffer.begin(), buffer.end() - 1); // -1 to remove the null terminator
}

static std::wstring GetNamedRegistryStringValue(HKEY hKey, const std::wstring& valueName) {
	DWORD dataSize = 0;
	DWORD type = 0;
	std::wstring result;

	// Query the size of the registry value
	if (RegQueryValueExW(hKey, valueName.c_str(), nullptr, &type, nullptr, &dataSize) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
		// Allocate buffer and retrieve the value
		std::wstring buffer(dataSize / sizeof(wchar_t), L'\0');
		if (RegQueryValueExW(hKey, valueName.c_str(), nullptr, nullptr, reinterpret_cast<LPBYTE>(&buffer[0]), &dataSize) == ERROR_SUCCESS) {
			result = buffer;
		}
		if (type == REG_EXPAND_SZ) {
			// expand the value of %windir% and the like because differently named % variables can have the same value
			result = ExpandEnvironmentVariables(result);
		}
	}

	return result; // "" if the value doesn't exist. Could be changed to "_missing_" if needed.
}


// Case-insensitive comparison function
struct CaseInsensitiveCompare {
	bool operator()(const std::wstring& lhs, const std::wstring& rhs) const {
		return std::lexicographical_compare(
			lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
			[](wchar_t a, wchar_t b) { return std::towlower(a) < std::towlower(b); }
		);
	}
};


static std::vector<std::wstring> CopySpeechRegistryEntriesFromOneCore() {
	std::vector<std::wstring> voices;
	HKEY hKeyOneCore, hKeySAPI;

	std::set<std::wstring, CaseInsensitiveCompare> SAPIvoicepaths;

	// Open OneCore Registry Key
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Speech_OneCore\\Voices\\Tokens", 0, KEY_READ, &hKeyOneCore) != ERROR_SUCCESS) {
		voices.push_back(L"Error: Cannot open Speech_OneCore (WinRT)  registry. This program must be run as Administrator to succeed.");
		return voices;
	}

	// Open or create SAPI Registry Key
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens", 0, KEY_READ | KEY_WRITE, &hKeySAPI) != ERROR_SUCCESS) {
		voices.push_back(L"Error: Cannot open Speech (SAPI) registry. This program must be run as Administrator to succeed.");
		RegCloseKey(hKeyOneCore);
		return voices;
	}

	// Enumerate voices from SAPI registry
	{
		DWORD index = 0;
		WCHAR voiceNameKey[256];    // "TTS_MS_EN-US_DAVID_11.0", etc.
		DWORD size = sizeof(voiceNameKey) / sizeof(voiceNameKey[0]);

		while (RegEnumKeyExW(hKeySAPI, index++, voiceNameKey, &size, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
			// Add the entry's VoicePath value to SAPIvoicepaths
			HKEY hVoiceKey;
			if (RegOpenKeyExW(hKeySAPI, voiceNameKey, 0, KEY_READ, &hVoiceKey) == ERROR_SUCCESS) {
				std::wstring voicePath = GetNamedRegistryStringValue(hVoiceKey, L"VoicePath");  // "%windir%\Speech_OneCore\Engines\TTS\en-US\M1033David", etc.
				if (voicePath.size()) {
					SAPIvoicepaths.insert(voicePath);
				}
				RegCloseKey(hVoiceKey);
			}
			size = sizeof(voiceNameKey) / sizeof(voiceNameKey[0]);  // Reset buffer size
		}
	}

	// Enumerate voices from OneCore registry
	DWORD index = 0;
	WCHAR voiceName[256]; // "MSTTS_V110_enUS_DavidM", etc
	DWORD size = sizeof(voiceName) / sizeof(voiceName[0]);

	while (RegEnumKeyExW(hKeyOneCore, index++, voiceName, &size, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
		// Check if voice exists in SAPI registry; it does if the OneCore's VoicePath is in the SAPIvoicepaths set
		HKEY hOneCoreVoiceKey;
		bool copyVoice{ false };
		if (RegOpenKeyExW(hKeyOneCore, voiceName, 0, KEY_READ, &hOneCoreVoiceKey) == ERROR_SUCCESS) {
			std::wstring voicePath = GetNamedRegistryStringValue(hOneCoreVoiceKey, L"VoicePath");  // "%windir%\Speech_OneCore\Engines\TTS\en-US\M1033David", etc.
			if (voicePath.size()) {
				copyVoice = !SAPIvoicepaths.contains(voicePath);
			}
			if (copyVoice) {
				// Copy the voice registry keys from OneCore to SAPI
				HKEY hNewSAPIVoiceKey;
				if (RegCreateKeyW(hKeySAPI, voiceName, &hNewSAPIVoiceKey) == ERROR_SUCCESS) {
					voices.push_back(voiceName);  // Add OneCore voice key name to list

					// Copy values, subkeys, and their values
					LSTATUS status = RegCopyTreeW(hOneCoreVoiceKey, nullptr, hNewSAPIVoiceKey);

					if (SUCCEEDED(status)) {
						// Add a value to indicate that this voice copy was added by MoreSAPIVoices
						std::wstring valueData = L"Yes";
						(void)RegSetValueExW(hNewSAPIVoiceKey, L"MoreSAPIVoices", 0, REG_SZ,
							reinterpret_cast<const BYTE*>(valueData.c_str()),
							static_cast<DWORD>((valueData.size() + 1) * sizeof(wchar_t)));
					}

					RegCloseKey(hNewSAPIVoiceKey);
				}
			}
			RegCloseKey(hOneCoreVoiceKey);
		}
		size = sizeof(voiceName) / sizeof(voiceName[0]);  // Reset buffer size
	}

	// Cleanup
	RegCloseKey(hKeyOneCore);
	RegCloseKey(hKeySAPI);

	return voices;
}

static std::vector<std::wstring> UnCopySpeechRegistryEntriesFromOneCore() {

	std::vector<std::wstring> voices;

	// Find SAPI voices that have a "MoreSAPIVoices" value, and remove them.

	// First, collect the HKEYs to delete, so that the enumeration isn't disturbed by key deletion.
	std::vector<std::wstring> voiceSubkeysToDeleteByName;

	// Open or create SAPI Registry Key
	HKEY hKeySAPI;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens", 0, KEY_READ | KEY_WRITE, &hKeySAPI) != ERROR_SUCCESS) {
		voices.push_back(L"Error: Cannot open Speech (SAPI) registry. This program must be run as Administrator to succeed.");
		return voices;
	}

	// Enumerate voices from SAPI registry
	{
		DWORD index = 0;
		WCHAR voiceNameKey[256];    // "TTS_MS_EN-US_DAVID_11.0", etc.
		DWORD size = sizeof(voiceNameKey) / sizeof(voiceNameKey[0]);

		while (RegEnumKeyExW(hKeySAPI, index++, voiceNameKey, &size, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
			// Check the entry's MoreSAPIVoices value
			HKEY hVoiceKey;
			if (RegOpenKeyExW(hKeySAPI, voiceNameKey, 0, KEY_READ, &hVoiceKey) == ERROR_SUCCESS) {
				std::wstring Yes = GetNamedRegistryStringValue(hVoiceKey, L"MoreSAPIVoices");  // "" if the MoreSAPIVoices is missing, else "Yes"
				if (Yes.size()) {
					voiceSubkeysToDeleteByName.push_back(voiceNameKey);
				}
				RegCloseKey(hVoiceKey);
			}
			size = sizeof(voiceNameKey) / sizeof(voiceNameKey[0]);  // Reset buffer size
		}
	}

	// delete the voices marked with MoreSAPIVoices
	for (std::wstring voiceNameKey : voiceSubkeysToDeleteByName) {
		LSTATUS status = RegDeleteTreeW(hKeySAPI, voiceNameKey.c_str());
		if (SUCCEEDED(status)) {
			voices.push_back(voiceNameKey);
		}
		else {
			PrintWindowsErrorMessage(status);
		}
	}

	// Cleanup
	RegCloseKey(hKeySAPI);

	return voices;
}


int main(int argc, char* argv[]) {
	bool uninstallMode = false;

	// Check if "uninstall" parameter is passed
	if (argc > 1) {
		std::string arg = argv[1];
		if (arg == "uninstall") {
			uninstallMode = true;
		}
		else {
			std::cout << "Usage: " << argv[0] << " [uninstall]" << std::endl;
			return 1;
		}
	}

	if (uninstallMode) {
		std::cout << "Restoring standard legacy SAPI voices by removing OneCore voices from SAPI..." << std::endl;
		// Perform uninstall actions (e.g., remove registry entries, delete files)
		std::vector<std::wstring> removedVoices = UnCopySpeechRegistryEntriesFromOneCore();
		for (const auto& voice : removedVoices) {
			std::wcout << L"Removed voice copy: " << voice << std::endl;
		}
		std::cout << "(No voices were removed from OneCore)" << std::endl;
	}
	else {
		std::cout << "Adding OneCore voices to legacy SAPI voices, marking with MoreSAPIVoices = \"Yes\"..." << std::endl;
		// Main application logic
		std::vector<std::wstring> voices = CopySpeechRegistryEntriesFromOneCore();
		for (const auto& voice : voices) {
			std::wcout << L"Copied voice: " << voice << std::endl;
		}
	}

	return 0;
}
