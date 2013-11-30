/* libUIOHook: Cross-platfrom userland keyboard and mouse hooking.
 * Copyright (C) 2006-2014 Alexander Barker.  All Rights Received.
 * https://github.com/kwhat/libuiohook/
 *
 * libUIOHook is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libUIOHook is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/***********************************************************************
 * The following code is based on code provided by Marc-André Moreau
 * to work around a failure to support dead keys in the ToUnicode() API.
 * According to the author some parts were taken directly from
 * Microsoft's  kbd.h header file that is shipped with the Windows
 * Driver Development Kit.
 *
 * The original code was substantially modified to provide the following:
 *   1) More dynamic code structure.
 *   2) Support for compilers that do not implement _ptr64 (GCC / LLVM).
 *   3) Support for Wow64 at runtime via 32-bit binary.
 *   4) Support for contextual language switching.
 *
 * I have contacted Marc-André Moreau who has granted permission for
 * his original source code to be used under the Public Domain.  Although
 * the libUIOHook library as a whole is currently covered under the LGPLv3,
 * please feel free to use and learn from the source code contained in this
 * file under the terms of the Public Domain.
 *
 * For further reading and the original code, please visit:
 *   http://legacy.docdroppers.org/wiki/index.php?title=Writing_Keyloggers
 *   http://www.techmantras.com/content/writing-keyloggers-full-length-tutorial
 *
 ***********************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <uiohook.h>
#include <windows.h>

#include "logger.h"
#include "win_unicode_helper.h"

// Structure and pointers for the keyboard locale cache.
typedef struct _KeyboardLocale {
	HKL id;									// Locale ID
	HINSTANCE library;						// Keyboard DLL instance.
	PVK_TO_BIT pVkToBit;					// Pointers struct arrays.
	PVK_TO_WCHAR_TABLE pVkToWcharTable;
	PDEADKEY pDeadKey;
	struct _KeyboardLocale* next;
} KeyboardLocale;

static KeyboardLocale* locale_first = NULL;
static KeyboardLocale* locale_current = NULL;

// Amount of pointer padding to apply for Wow64 instances.
static short int ptr_padding = 0;

#if defined(_WIN32) && !defined(_WIN64)
// Small function to check and see if we are executing under Wow64.
static BOOL is_wow64() {
	BOOL status = FALSE;

	LPFN_ISWOW64PROCESS pIsWow64Process = (LPFN_ISWOW64PROCESS) 
			GetProcAddress(GetModuleHandle("kernel32"), "IsWow64Process");

	if(pIsWow64Process != NULL) {
		HANDLE current_proc = GetCurrentProcess();
				
		if (!pIsWow64Process(current_proc, &status)) {
			status = FALSE;
			
			logger(LOG_LEVEL_DEBUG,	"%s [%u]: pIsWow64Process(%#p, (%#p) failed!\n", 
				__FUNCTION__, __LINE__, current_proc, &status);
		}
	}

	return status;
}
#endif

// Locate the DLL that contains the current keyboard layout.
static int get_keyboard_layout_file(char *layoutFile, DWORD bufferSize) {
	int status = UIOHOOK_FAILURE;
	HKEY hKey;
	DWORD varType = REG_SZ;

	char kbdName[KL_NAMELENGTH];
	if (GetKeyboardLayoutName(kbdName)) {
		char kbdKeyPath[51 + KL_NAMELENGTH];
		snprintf(kbdKeyPath, 51 + KL_NAMELENGTH, "SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\%s", kbdName);

		if(RegOpenKeyEx(HKEY_LOCAL_MACHINE, (LPCTSTR) kbdKeyPath, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
			if(RegQueryValueEx(hKey, "Layout File", NULL, &varType, (LPBYTE) layoutFile, &bufferSize) == ERROR_SUCCESS) {
				RegCloseKey(hKey);
				status = UIOHOOK_SUCCESS;
			}
		}
	}

	return status;
}

static int refresh_locale_list() {
	int count = 0;

	// Get the number of layouts the user has activated.
	int hkl_size = GetKeyboardLayoutList(0, NULL);
	if (hkl_size > 0) {
		logger(LOG_LEVEL_INFO,	"%s [%u]: GetKeyboardLayoutList(0, NULL) found %i layouts.\n", 
				__FUNCTION__, __LINE__, hkl_size);

		// Get the thread id that currently has focus for our default.
		DWORD focus_pid = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
		HKL hlk_focus = GetKeyboardLayout(focus_pid);
		HKL hlk_default = GetKeyboardLayout(0);
		HKL *hkl_list = malloc(sizeof(HKL) * hkl_size);

		int new_size = GetKeyboardLayoutList(hkl_size, hkl_list);
		if (new_size > 0) {
			if (new_size != hkl_size) {
				logger(LOG_LEVEL_ERROR,	"%s [%u]: Locale size mismatch!  "
						"Expected %i, received %i!\n", 
						__FUNCTION__, __LINE__, hkl_size, new_size);
			}
			else {
				logger(LOG_LEVEL_INFO,	"%s [%u]: Received %i locales.\n", 
						__FUNCTION__, __LINE__, new_size);
			}

			KeyboardLocale* locale_previous = NULL;
			KeyboardLocale* locale_item = locale_first;

			// Go though the linked list and remove KeyboardLocale's that are
			// no longer loaded.
			while (locale_item != NULL) {
				// Check to see if the old HKL is in the new list.
				bool is_loaded = false;
				for (int i = 0; i < new_size && !is_loaded; i++) {
					if (locale_item->id == hkl_list[i]) {
						// Flag and jump out of the loop.
						hkl_list[i] = NULL;
						is_loaded = true;
					}
				}


				if (is_loaded) {
					logger(LOG_LEVEL_DEBUG,	"%s [%u]: Found loacle ID %#p in the cache.\n", 
							__FUNCTION__, __LINE__, locale_item->id);

					// Set the previous local to the current locale.
					locale_previous = locale_item;

					// Check and see if the locale is our current active locale.
					if (locale_item->id == hlk_focus) {
						locale_current = locale_item;
					}

					count++;
				}
				else {
					logger(LOG_LEVEL_DEBUG,	"%s [%u]: Removing loacle ID %#p from the cache.\n", 
							__FUNCTION__, __LINE__, locale_item->id);
					
					// If the old id is not in the new list, remove it.
					locale_previous->next = locale_item->next;

					// Make sure the locale_current points NULL or something valid.
					if (locale_item == locale_current) {
						locale_current = NULL;
					}

					// Free the memory used by locale_item;
					free(locale_item);

					// Set the item to the pervious item to guarantee a next.
					locale_item = locale_previous;
				}

				// Iterate to the next linked list item.
				locale_item = locale_item->next;
			}


			// Insert anything new into the linked list.
			for (int i = 0; i < new_size; i++) {
				// Check to see if the item was already in the list.
				if (hkl_list[i] != NULL) {
					// TODO Unload this to a function, See else clause.
					
					// Set the active keyboard layout for this thread to the HKL.
					ActivateKeyboardLayout(hkl_list[i], 0x00);

					// Try to pull the current keyboard layout DLL from the registry.
					char layoutFile[MAX_PATH];
					if (get_keyboard_layout_file(layoutFile, sizeof(layoutFile)) == UIOHOOK_SUCCESS) {
						//You can't trust the %SYSPATH%, look it up manually.
						char systemDirectory[MAX_PATH];
						if (GetSystemDirectory(systemDirectory, MAX_PATH) != 0) {
							char kbdLayoutFilePath[MAX_PATH];
							snprintf(kbdLayoutFilePath, MAX_PATH, "%s\\%s", systemDirectory, layoutFile);

							logger(LOG_LEVEL_DEBUG,	"%s [%u]: Loading layout for %#p: %s.\n", 
									__FUNCTION__, __LINE__, hkl_list[i], layoutFile);

							// Create the new locale item.
							locale_item = malloc(sizeof(KeyboardLocale));
							locale_item->id = hkl_list[i];
							locale_item->library = LoadLibrary(kbdLayoutFilePath);

							// Get the function pointer from the library to get the keyboard layer descriptor.
							KbdLayerDescriptor pKbdLayerDescriptor = (KbdLayerDescriptor) GetProcAddress(locale_item->library, "KbdLayerDescriptor");
							if(pKbdLayerDescriptor != NULL) {
								PKBDTABLES pKbd = pKbdLayerDescriptor();

								// Store the memory address of the following 3 structures.
								BYTE *base = (BYTE *) pKbd;

								// First element of each structure, no offset adjustment needed.
								locale_item->pVkToBit = pKbd->pCharModifiers->pVkToBit;

								// Second element of pKbd, +4 byte offset on wow64.
								locale_item->pVkToWcharTable = *((PVK_TO_WCHAR_TABLE *) (base + offsetof(KBDTABLES, pVkToWcharTable) + ptr_padding));

								// Third element of pKbd, +8 byte offset on wow64.
								locale_item->pDeadKey = *((PDEADKEY *) (base + offsetof(KBDTABLES, pDeadKey) + (ptr_padding * 2)));


								// This will always be added to the end of the list.
								locale_item->next = NULL;

								// Insert the item into the linked list.
								if (locale_previous == NULL) {
									// If nothing came before, the list is empty.
									locale_first = locale_item;
								}
								else {
									// Append the new locale to the end of the list.
									locale_previous->next = locale_item;
								}

								// Check and see if the locale is our current active locale.
								if (locale_item->id == hlk_focus) {
									locale_current = locale_item;
								}

								// Set the pervious locale item to the new one.
								locale_previous = locale_item;

								count++;
							}
							else {
								logger(LOG_LEVEL_ERROR,	
										"%s [%u]: GetProcAddress() failed for KbdLayerDescriptor!\n", 
										__FUNCTION__, __LINE__);

								FreeLibrary(locale_item->library);
								free(locale_item);
								locale_item = NULL;
							}
						}
						else {
							logger(LOG_LEVEL_ERROR,	
									"%s [%u]: GetSystemDirectory() failed!\n", 
									__FUNCTION__, __LINE__);
						}
					}
					else {
						logger(LOG_LEVEL_ERROR,	
								"%s [%u]: Could not find keyboard map for locale %#p!\n", 
								__FUNCTION__, __LINE__, hkl_list[i]);
					}
				} // End NULL Check.
			} // for (...)
		}
		else {
			logger(LOG_LEVEL_ERROR,	
					"%s [%u]: GetKeyboardLayoutList() failed!\n", 
					__FUNCTION__, __LINE__);

			// TODO Try and recover by using the current layout.
			// Hint: Use locale_id instead of hkl_list[i] in the loop above.
		}

		free(hkl_list);
		ActivateKeyboardLayout(hlk_default, 0x00);
	}

	return count;
}

int load_unicode_helper() {
	int count = 0;

	#if defined(_WIN32) && !defined(_WIN64)
	if (is_wow64()) {
		ptr_padding = sizeof(void *);
	}
	#endif

	count = refresh_locale_list();

	logger(LOG_LEVEL_INFO,	
			"%s [%u]: refresh_locale_lis() found %i locale(s).\n", 
			__FUNCTION__, __LINE__, count);

	return count;
}

// This returns the number of locales that were removed.
int unload_unicode_helper() {
	int count = 0;

	// Cleanup and free memory from the old list.
	KeyboardLocale* locale_item = locale_first;
	while (locale_item != NULL) {
		// Remove the first item from the linked list.
		FreeLibrary(locale_item->library);
		locale_first = locale_item->next;
		free(locale_item);
		locale_item = locale_first;

		count++;
	}

	// Reset the current local.
	locale_current = NULL;

	return count;
}

int convert_vk_to_wchar(int virtualKey, PWCHAR outputChar, PWCHAR deadChar) {
	// Get the thread id that currently has focus and
	DWORD focus_pid = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
	HKL locale_id = GetKeyboardLayout(focus_pid);

	// If the current Locale is not the new locale, search the linked list.
	if (locale_current == NULL || locale_current->id != locale_id) {
		locale_current = NULL;
		KeyboardLocale* locale_item = locale_first;
		while (locale_item != NULL) {
			// Search the linked list.
			if (locale_item->id == locale_id) {
				logger(LOG_LEVEL_INFO,	
					"%s [%u]: Activating keyboard layout %#p.\n", 
					__FUNCTION__, __LINE__, locale_item->id);

				// If they layout changes the dead key state needs to be reset.
				// This is consistent with the way Windows handles locale changes.
				*deadChar = 0;
				locale_current = locale_item;
				locale_item = NULL;
			}
			else {
				locale_item = locale_item->next;
			}
		}

		// If we were unable to find the locale in the list, refresh the list.
		if (locale_current == NULL) {
			logger(LOG_LEVEL_DEBUG,	
					"%s [%u]: Refreshing locale cache.\n", 
					__FUNCTION__, __LINE__);

			refresh_locale_list();
		}
	}


	int charCount = 0;
	*outputChar = 0;

	// Check and make sure the Unicode helper was loaded.
	if (locale_current != NULL) {
		logger(LOG_LEVEL_INFO,	
				"%s [%u]: Using keyboard layout %#p.\n", 
				__FUNCTION__, __LINE__, locale_current->id);

		short state = 0;
		int shift = -1;
		int mod = 0;

		WCHAR baseChar;
		WCHAR diacritic;

		int capsLock = (GetKeyState(VK_CAPITAL) & 0x01);

		PVK_TO_BIT pVkToBit = locale_current->pVkToBit;
		PVK_TO_WCHAR_TABLE pVkToWcharTable = locale_current->pVkToWcharTable;
		PDEADKEY pDeadKey = locale_current->pDeadKey;

		/* Because this is only a structure of two bytes, we don't need to worry
		* about the structure padding of __ptr64 offsets on Wow64.
		*/
		for (int i = 0; pVkToBit[i].Vk != 0; i++) {
			state = GetAsyncKeyState(pVkToBit[i].Vk);

			if(pVkToBit[i].Vk == VK_SHIFT) {
				shift = i + 1; // Get modification number for Shift key
			}

			if(state & ~SHRT_MAX) {
				if (mod == 0) {
					mod = i + 1;
				}
				else {
					mod = 0; // Two modifiers at the same time!
				}
			}
		}

		// Default 32 bit structure size should be 6 bytes (4 for the pointer and 2
		// additional byte fields) that are padded out to 8 bytes by the compiler.
		unsigned short sizeVkToWcharTable = sizeof(VK_TO_WCHAR_TABLE);
		#if defined(_WIN32) && !defined(_WIN64)
		if (is_wow64()) {
			// If we are running under Wow64 the size of the first pointer will be
			// 8 bringing the total size to 10 bytes padded out to 16.
			sizeVkToWcharTable = (sizeVkToWcharTable + ptr_padding + 7) & -8;
		}
		#endif

		BYTE *ptrCurrentVkToWcharTable = (BYTE *) pVkToWcharTable;

		int cbSize, n;
		do {
			// cbSize is used to calculate n, and n is used for the size of pVkToWchars[j].wch[n]
			cbSize = *(ptrCurrentVkToWcharTable + offsetof(VK_TO_WCHAR_TABLE, cbSize) + ptr_padding);
			n = (cbSize - 2) / 2;

			// Same as VK_TO_WCHARS pVkToWchars[] = pVkToWcharTable[i].pVkToWchars
			PVK_TO_WCHARS pVkToWchars = (PVK_TO_WCHARS) ((PVK_TO_WCHAR_TABLE) ptrCurrentVkToWcharTable)->pVkToWchars;

			if(pVkToWchars != NULL && mod < n) {
				// pVkToWchars[j].VirtualKey
				BYTE *pCurrentVkToWchars = (BYTE *) pVkToWchars;

				do {
					if (((PVK_TO_WCHARS) pCurrentVkToWchars)->VirtualKey == virtualKey) {
						if ((((PVK_TO_WCHARS) pCurrentVkToWchars)->Attributes == CAPLOK) && capsLock) {
							if(mod == shift) mod = 0; else mod = shift;
						}
						*outputChar = ((PVK_TO_WCHARS) pCurrentVkToWchars)->wch[mod];
						charCount = 1;

						// Increment the pCurrentVkToWchars by the size of wch[n].
						pCurrentVkToWchars += sizeof(VK_TO_WCHARS) + (sizeof(WCHAR) * n);

						if(*outputChar == WCH_NONE) {
							charCount = 0;
						}
						else if(*outputChar == WCH_DEAD) {
							*deadChar = ((PVK_TO_WCHARS) pCurrentVkToWchars)->wch[mod];
							charCount = 0;
						}
						break;
					}
					else {
						// Add sizeof WCHAR because we are really an array of WCHAR[n] not WCHAR[]
						pCurrentVkToWchars += sizeof(VK_TO_WCHARS) + (sizeof(WCHAR) * n);
					}
				} while ( ((PVK_TO_WCHARS) pCurrentVkToWchars)->VirtualKey != 0 );
			}

			// This is effectively the same as: ptrCurrentVkToWcharTable = pVkToWcharTable[++i];
			ptrCurrentVkToWcharTable += sizeVkToWcharTable;
		} while (cbSize != 0);


		// Code to check for dead characters...
		if( *deadChar != 0) {
			for (int i = 0; pDeadKey[i].dwBoth != 0; i++) {
				baseChar = (WCHAR) pDeadKey[i].dwBoth;
				diacritic = (WCHAR) (pDeadKey[i].dwBoth >> 16);

				if((baseChar == *outputChar) && (diacritic == *deadChar)) {
					*deadChar = 0;
					*outputChar = (WCHAR) pDeadKey[i].wchComposed;
				}
			}
		}
	}

	return charCount;
}
