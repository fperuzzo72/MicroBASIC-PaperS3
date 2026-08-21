#pragma once

#include "config.h"

// Which folder on the SD card the browser and the prose editor are working
// in. The two are genuinely different collections rather than one folder with
// two names: notes are prose (.txt, and the folder MicroWriter has always
// used), programs are BASIC source under /MicroBASIC/programs -- the same
// folder the interpreter's own SAVE/LOAD use, so a program written in the
// editor is LOADable and one typed at the BASIC prompt is editable. The menu
// picks the collection before entering either screen, and everything below
// (listing, titles, new-file naming, save, rename, delete) follows it.
enum class FileCollection { NOTES, PROGRAMS };

void setFileCollection(FileCollection c);  // switches and re-lists
FileCollection getFileCollection();
const char* fileCollectionName();          // "Notes" / "Programs", for headers

void fileManagerSetup();
void refreshFileList();
int getFileCount();
FileInfo* getFileList();

void loadFile(const char* filename);
void saveCurrentFile(bool refreshList = true);
void createNewFile();
// `except` names a file that doesn't count as a collision -- the one being
// renamed, so confirming a rename unchanged doesn't bump it to _2.
void deriveUniqueFilename(const char* title, char* out, int maxLen, const char* except = nullptr);
void updateFileTitle(const char* filename, const char* newTitle);
void deleteFile(const char* filename);
