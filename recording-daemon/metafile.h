#ifndef _METAFILE_H_
#define _METAFILE_H_

#include "types.h"

void metafile_setup(void);
void metafile_cleanup(void);

void metafile_change(char *name);
void metafile_delete(char *name);

int metafile_stop_recording(char *call_id);
int metafile_start_recording(char *call_id);
#endif
