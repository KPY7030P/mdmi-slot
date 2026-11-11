#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "library.h"

static bool is_separator(char c)
{
	return c == '\\' || c == '/';
}

// The length of path without its last part and the separator before it.
static size_t parent_length(const char *path, size_t length)
{
	while (length > 0 && !is_separator(path[length - 1])) {
		length--;
	}
	while (length > 0 && is_separator(path[length - 1])) {
		length--;
	}
	return length;
}

// Where the last part of path[0, length) starts.
static size_t last_part(const char *path, size_t length)
{
	size_t from = length;
	while (from > 0 && !is_separator(path[from - 1])) {
		from--;
	}
	return from;
}

// Whether the last part of path[0, length) is name, or with ends only ends with it; ignoring case.
static bool last_part_is(const char *path, size_t length, const char *name, bool ends)
{
	const size_t from = last_part(path, length), n = strlen(name);
	if (ends ? length - from <= n : length - from != n) {
		return false;
	}
	return !_strnicmp(path + length - n, name, n);
}

void library_folder_for(const char *module, char *out, size_t size)
{
	size_t length = parent_length(module, strlen(module));
	// A bundle is <name>.vst3\Contents\<arch>\<file>: the folder goes beside <name>.vst3.
	const size_t contents = parent_length(module, length), bundle = parent_length(module, contents);
	if (bundle > 0 && last_part_is(module, contents, "Contents", false) && last_part_is(module, bundle, ".vst3", true)) {
		length = parent_length(module, bundle);
	}
	snprintf(out, size, "%.*s%s%s", (int)length, module, length ? "\\" : "", LIBRARY_FOLDER);
}

void library_folder(char *out, size_t size)
{
	const char *env = getenv("MDMI_BANKS");
	if (env && *env) {
		snprintf(out, size, "%s", env);
		return;
	}
	HMODULE module = NULL;
	wchar_t wide[MAX_PATH * 2];
	char path[LIBRARY_PATH];
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)&library_folder, &module)
		|| !GetModuleFileNameW(module, wide, MAX_PATH * 2)
		|| !WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, sizeof(path), NULL, NULL)) {
		snprintf(out, size, "%s", LIBRARY_FOLDER);
		return;
	}
	library_folder_for(path, out, size);
}

static int by_name(const void *a, const void *b)
{
	return _stricmp((const char *)a, (const char *)b);
}

void library_scan(const char *folder, library_list *list)
{
	memset(list, 0, sizeof(*list));
	char pattern[LIBRARY_PATH];
	wchar_t wide[LIBRARY_PATH];
	library_path(folder, "*", pattern, sizeof(pattern));
	if (!MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wide, LIBRARY_PATH)) {
		return;
	}
	WIN32_FIND_DATAW found;
	HANDLE find = FindFirstFileW(wide, &found);
	if (find == INVALID_HANDLE_VALUE) {
		const DWORD error = GetLastError();
		list->found = error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES;
		return;
	}
	list->found = true;
	// All names first, so the 64 shown are the first by name whatever order the disk gives.
	size_t room = 64, used = 0;
	char (*names)[LIBRARY_NAME] = malloc(room * LIBRARY_NAME);
	do {
		if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			continue;
		}
		char name[LIBRARY_NAME];
		if (!WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, name, sizeof(name), NULL, NULL)) {
			continue;
		}
		const char *dot = strrchr(name, '.');
		if (!dot || _stricmp(dot, ".mdbank") || dot == name) {
			continue;
		}
		if (used == room) {
			room *= 2;
			void *more = names ? realloc(names, room * LIBRARY_NAME) : NULL;
			if (!more) {
				break;
			}
			names = more;
		}
		if (names) {
			memcpy(names[used++], name, LIBRARY_NAME);
		}
	} while (FindNextFileW(find, &found));
	FindClose(find);
	if (!names) {
		return;
	}
	qsort(names, used, LIBRARY_NAME, by_name);
	list->total = (int)used;
	list->count = used < LIBRARY_MAX ? (int)used : LIBRARY_MAX;
	memcpy(list->names, names, (size_t)list->count * LIBRARY_NAME);
	free(names);
}

void library_path(const char *folder, const char *name, char *out, size_t size)
{
	const size_t length = strlen(folder);
	snprintf(out, size, "%s%s%s", folder, length && !is_separator(folder[length - 1]) ? "\\" : "", name);
}

bool library_make_folder(const char *folder)
{
	wchar_t wide[LIBRARY_PATH];
	if (!MultiByteToWideChar(CP_UTF8, 0, folder, -1, wide, LIBRARY_PATH)) {
		return false;
	}
	return CreateDirectoryW(wide, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}
