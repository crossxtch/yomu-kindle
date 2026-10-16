#ifndef YOMU_FILESERVER_H
#define YOMU_FILESERVER_H

/* HTTP file manager for one folder. It serves the page in
 * src/net/filepage.html and the list/upload/download/mkdir/rename/move/delete
 * calls that page makes.
 * The server runs until fileserver_stop. port 0 asks the OS for a free port.
 */
typedef struct FileServer FileServer;

FileServer *fileserver_start(const char *root, int port);

/* Stops listening and waits for the current request to finish.
 * Returns 1 when a file was created, replaced, renamed, moved, or deleted.
 */
int fileserver_stop(FileServer *fs);

int fileserver_port(const FileServer *fs);

/* Writes up to max URLs, for example http://192.168.1.5:8080/ .
 * Loopback addresses are left out. Returns how many were written.
 */
int fileserver_urls(const FileServer *fs, char urls[][80], int max);

#endif
