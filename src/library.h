#ifndef MDMI_LIBRARY_H
#define MDMI_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>

// The libraries of stage 22c (D56): every .mdbank right in the folder MDMI Banks beside the plugin
// file, shared by all projects and handed on with the plugin. Paths and names are UTF-8.
#define LIBRARY_FOLDER "MDMI Banks"
#define LIBRARY_MAX 64
#define LIBRARY_PATH 1040
#define LIBRARY_NAME 128

// The folder for a plugin file at module: beside it, or beside the .vst3 bundle that holds it when
// it is in one (never inside a bundle).
void library_folder_for(const char *module, char *out, size_t size);
// This plugin's folder: MDMI_BANKS when set (tests), otherwise beside this DLL.
void library_folder(char *out, size_t size);

typedef struct {
	// Whether the folder is there at all.
	bool found;
	// Shown, and all the folder has.
	int count;
	int total;
	// File names with their extension, sorted ignoring case.
	char names[LIBRARY_MAX][LIBRARY_NAME];
} library_list;

void library_scan(const char *folder, library_list *list);
// The path of a file of the folder.
void library_path(const char *folder, const char *name, char *out, size_t size);
// Creates the folder if it isn't there. False if it can't be.
bool library_make_folder(const char *folder);

#endif
