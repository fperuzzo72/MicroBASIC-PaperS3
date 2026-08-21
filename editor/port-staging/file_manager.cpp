#include "file_manager.h"
#include "text_editor.h"
#include "ascii_fold.h"
#include <Arduino.h>
#include <SDCardManager.h>
#include <Utf8.h>
#include <cstring>

// --- Collections (see file_manager.h) ---
struct CollectionInfo {
  const char* dir;
  const char* ext;      // extension given to files created here
  const char* fallback; // base name when a title sanitises down to nothing
  const char* label;
  bool extFilter;       // list only *ext, or everything the folder holds
  bool rawNames;        // show/edit the filename itself, extension and all
};

// Two things are different about programs, and both come from the same fact:
// a program's name is something the user says to the interpreter. `LOAD "X"`
// has to find the file, so what the browser shows must be what LOAD takes --
// the filename, extension included, not a prettified title. And SAVE stores
// under exactly the name typed with no extension forced on, so the folder is
// listed unfiltered: it can legitimately contain no .bas at all.
//
// Notes keep MicroWriter's behaviour, where the filename is an implementation
// detail and the title is what the user deals with.
static const CollectionInfo COLLECTIONS[] = {
  {"/notes",               ".txt", "note",    "Notes",    true,  false},
  {"/MicroBASIC/programs", ".bas", "program", "Programs", false, true},
};

static FileCollection currentCollection = FileCollection::NOTES;
static const CollectionInfo& coll() { return COLLECTIONS[(int)currentCollection]; }

// --- File list ---
static FileInfo fileList[MAX_FILES];
static int fileCount = 0;

// Shared state
extern UIState currentState;

// Convert filename to what the user sees and edits.
// Notes:    "my_note_2.txt" -> "My Note 2"  (the extension is ours to manage)
// Programs: "pacman.bas"    -> "pacman.bas" (the name is the user's to say)
static void filenameToTitle(const char* filename, char* out, int maxLen) {
  if (coll().rawNames) {
    strncpy(out, filename, maxLen - 1);
    out[maxLen - 1] = '\0';
    return;
  }

  int j = 0;
  bool capitalizeNext = true;
  for (int i = 0; filename[i] != '\0' && filename[i] != '.' && j < maxLen - 1; i++) {
    char c = filename[i];
    if (c == '_') {
      if (j > 0) out[j++] = ' ';
      capitalizeNext = true;
    } else {
      if (capitalizeNext && c >= 'a' && c <= 'z') c -= 32;
      capitalizeNext = false;
      out[j++] = c;
    }
  }
  out[j] = '\0';
  if (j == 0) strncpy(out, "Untitled", maxLen - 1);
}

// Convert what the user typed into a valid FAT filename: lowercase (the SD
// card holds everything lowercase, so LOAD never has to guess at case),
// spaces to underscores, anything else dropped.
//
// The dot is where the two collections part. For notes it is not a legal
// character -- the extension is ours, appended here. For programs it is, and
// a typed one is kept: the user asked to control the name *and* the ending,
// so "novo.bas" stays "novo.bas" rather than becoming "novobas.bas", and the
// default extension is only supplied when none was typed at all.
static void titleToFilename(const char* title, char* out, int maxLen) {
  const CollectionInfo& ci = coll();
  const int extLen = (int)strlen(ci.ext);
  const int maxBase = maxLen - extLen - 1;
  bool hasDot = false;
  int j = 0;
  // Decoded rather than walked byte by byte, because a title is UTF-8: an
  // accented letter is two bytes, and the old byte loop kept neither of them,
  // so "ação" quietly became "aao". Folding to plain ASCII loses the accent
  // but not the letter, and it is what the filename has to be anyway --
  // SdFat rejects high bytes outright (see ascii_fold.h).
  const unsigned char* u = (const unsigned char*)title;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&u)) != 0 && j < maxBase) {
    char c = asciiFold(cp);
    if (c == 0) continue;  // no ASCII stand-in: drop it
    if (c >= 'A' && c <= 'Z') c += 32;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out[j++] = c;
    } else if (ci.rawNames && c == '.') {
      // Not at the start, and not twice: a leading dot hides the file and a
      // second one is a name no BASIC user means to type.
      if (j > 0 && !hasDot) { out[j++] = c; hasDot = true; }
    } else if (c == ' ' || c == '_' || c == '-') {
      if (j > 0 && out[j - 1] != '_') out[j++] = '_';
    }
  }
  while (j > 0 && (out[j - 1] == '_' || out[j - 1] == '.')) { if (out[j-1] == '.') hasDot = false; j--; }
  if (j == 0) { strncpy(out, ci.fallback, maxLen - 1); j = (int)strlen(out); hasDot = false; }
  out[j] = '\0';
  if (!hasDot) strcpy(out + j, ci.ext);
}

// Derive a unique filename in the current collection from a title, handling
// collisions with a _2, _3 suffix.
void deriveUniqueFilename(const char* title, char* out, int maxLen, const char* except) {
  titleToFilename(title, out, maxLen);

  // `except` is the file being renamed. Without it, confirming a rename
  // without actually changing anything collides with the file itself and
  // bumps it to _2 -- which used to be rare, because the field held a
  // prettified title, and is now the common case for programs, where it
  // holds the exact filename and confirming it unchanged is natural.
  if (except && strcmp(out, except) == 0) return;

  char path[320];
  snprintf(path, sizeof(path), "%s/%s", coll().dir, out);
  if (!SdMan.exists(path)) return;

  // Collision — strip .txt, try _2, _3 ...
  // Split at the *last* dot rather than assuming the collection's own
  // extension: with programs the user picks the extension, so "game.b" and
  // "game.bas" both have to collide-suffix into "game_2.b" / "game_2.bas".
  char base[MAX_FILENAME_LEN];
  strncpy(base, out, sizeof(base) - 1);
  base[sizeof(base) - 1] = '\0';
  char* dot = strrchr(base, '.');
  char ext[16];
  snprintf(ext, sizeof(ext), "%s", dot ? dot : coll().ext);
  if (dot) *dot = '\0';

  int suffix = 2;
  while (SdMan.exists(path) && suffix <= 99) {
    snprintf(out, maxLen, "%s_%d%s", base, suffix++, ext);
    snprintf(path, sizeof(path), "%s/%s", coll().dir, out);
  }
}

void setFileCollection(FileCollection c) {
  if (currentCollection == c) return;
  currentCollection = c;
  refreshFileList();
}

FileCollection getFileCollection() { return currentCollection; }
const char* fileCollectionName() { return coll().label; }

void fileManagerSetup() {
  if (!SdMan.begin()) {
    DBG_PRINTLN("SD Card mount failed!");
    return;
  }

  for (const CollectionInfo& c : COLLECTIONS) {
    if (!SdMan.exists(c.dir)) SdMan.mkdir(c.dir);
  }

  DBG_PRINTLN("SD Card initialized");
  refreshFileList();
}

void refreshFileList() {
  fileCount = 0;

  auto root = SdMan.open(coll().dir);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();
  char name[256];

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || fileCount >= MAX_FILES) {
      file.close();
      if (fileCount >= MAX_FILES) break;
      continue;
    }

    // The save path writes .tmp and rotates the previous version to .bak;
    // neither is a document, so neither belongs in a list the user picks from.
    const int nameLen = strlen(name);
    const bool scratch = nameLen > 4 && (strcasecmp(name + nameLen - 4, ".bak") == 0 ||
                                         strcasecmp(name + nameLen - 4, ".tmp") == 0);
    const bool matches = !coll().extFilter ||
                         (nameLen > 4 && strcasecmp(name + nameLen - 4, coll().ext) == 0);
    if (!scratch && matches) {
      strncpy(fileList[fileCount].filename, name, MAX_FILENAME_LEN - 1);
      fileList[fileCount].filename[MAX_FILENAME_LEN - 1] = '\0';

      filenameToTitle(name, fileList[fileCount].title, MAX_TITLE_LEN);
      fileList[fileCount].modTime = 0;
      fileCount++;
    }
    file.close();
  }
  root.close();
  SdMan.sleep();

  DBG_PRINTF("File listing: %d files found\n", fileCount);
}

int getFileCount() { return fileCount; }
FileInfo* getFileList() { return fileList; }

void loadFile(const char* filename) {
  char path[320];
  snprintf(path, sizeof(path), "%s/%s", coll().dir, filename);

  auto file = SdMan.open(path, O_RDONLY);
  if (!file) {
    DBG_PRINTF("Could not open: %s\n", path);
    return;
  }

  // Silently truncates a file larger than TEXT_BUFFER_SIZE — no warning
  // shown, and saveCurrentFile() below will write this truncated buffer
  // back over the original on the next save. See the TEXT_BUFFER_SIZE
  // comment in config.h — TODO, revisit.
  char* buf = editorGetBuffer();
  int readResult = file.read(buf, TEXT_BUFFER_SIZE - 1);
  size_t bytesRead = (readResult > 0) ? (size_t)readResult : 0;
  buf[bytesRead] = '\0';
  file.close();

  editorSetCurrentFile(filename);
  editorLoadBuffer(bytesRead);

  // Title comes from the filename, not the file content
  char title[MAX_TITLE_LEN];
  filenameToTitle(filename, title, MAX_TITLE_LEN);
  editorSetCurrentTitle(title);
  editorSetUnsavedChanges(false);

  currentState = UIState::TEXT_EDITOR;
  SdMan.sleep();
  DBG_PRINTF("Loaded: %s (%d bytes)\n", filename, (int)bytesRead);
}

void saveCurrentFile(bool refreshList) {
  const char* filename = editorGetCurrentFile();
  if (filename[0] == '\0') return;

  char path[320], tmpPath[336], bakPath[336];
  snprintf(path, sizeof(path), "%s/%s", coll().dir, filename);
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  snprintf(bakPath, sizeof(bakPath), "%s.bak", path);

  // Step 1: Write new content to .tmp
  auto file = SdMan.open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    DBG_PRINTF("saveCurrentFile: could not create tmp: %s\n", tmpPath);
    return;
  }

  size_t toWrite = editorGetLength();
  size_t written = file.write((const uint8_t*)editorGetBuffer(), toWrite);
  file.close();

  // Step 2: Verify bytes written match expected length
  if (written != toWrite) {
    DBG_PRINTF("saveCurrentFile: write mismatch (%d/%d) — aborting\n", (int)written, (int)toWrite);
    SdMan.remove(tmpPath);
    return;
  }

  // Step 3: Rotate original → .bak (original is now safe in .tmp, preserve previous .bak)
  if (SdMan.exists(path)) {
    SdMan.remove(bakPath);          // Remove old .bak (if any)
    SdMan.rename(path, bakPath);    // Original becomes new .bak
  }

  // Step 4: Promote .tmp → original
  SdMan.rename(tmpPath, path);

  // Step 5: Discard the backup. It has done its job.
  //
  // The .bak exists to survive a power cut *during* the save: between steps 3
  // and 4 the note's name points at nothing, and without it the old content
  // would be gone. Once step 4 has completed, the note is whole on the card
  // and the copy protects nothing -- it just doubles what the notes occupy and
  // leaves the card full of duplicates when read on a PC.
  //
  // What this gives up, deliberately: recovering the *previous* version by
  // hand, which was never offered in the UI and only ever worked by digging
  // through the card. The crash safety, which is the point, is untouched --
  // it lives in the .tmp / rotate / promote sequence above, not in keeping
  // the file afterwards.
  SdMan.remove(bakPath);

  editorSetUnsavedChanges(false);
  if (refreshList) refreshFileList();
  SdMan.sleep();
  DBG_PRINTF("Saved: %s\n", filename);
}

void createNewFile() {
  editorClear();
  editorSetCurrentFile("");       // filename derived from title when user confirms
  editorSetCurrentTitle("Untitled");
  editorSetUnsavedChanges(true);
}

// Rename a file on disk to match a new title, updating editor state if needed.
void updateFileTitle(const char* filename, const char* newTitle) {
  char newFilename[MAX_FILENAME_LEN];
  deriveUniqueFilename(newTitle, newFilename, MAX_FILENAME_LEN, filename);

  if (strcmp(newFilename, filename) != 0) {
    char oldPath[320], newPath[320];
    snprintf(oldPath, sizeof(oldPath), "%s/%s", coll().dir, filename);
    snprintf(newPath, sizeof(newPath), "%s/%s", coll().dir, newFilename);
    SdMan.rename(oldPath, newPath);

    if (strcmp(editorGetCurrentFile(), filename) == 0) {
      editorSetCurrentFile(newFilename);
    }
  }

  refreshFileList();
  SdMan.sleep();
}

void deleteFile(const char* filename) {
  char path[320], bakPath[336];
  snprintf(path, sizeof(path), "%s/%s", coll().dir, filename);
  snprintf(bakPath, sizeof(bakPath), "%s.bak", path);
  SdMan.remove(path);
  SdMan.remove(bakPath);
  refreshFileList();
  SdMan.sleep();
  DBG_PRINTF("Deleted: %s\n", filename);
}
