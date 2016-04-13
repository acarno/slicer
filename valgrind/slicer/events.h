#ifndef EVENTS_H
#define EVENTS_H

#include "pub_tool_libcbase.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_threadstate.h"

void     sl_initialize_thread_array(void);
void     sl_clean_up(void);
IRDirty *sl_update_call_event(ThreadId, const HChar *, const HChar *, UInt);
void     sl_incr_instr_count(ThreadId, HChar *, UInt, UInt);
void     sl_dump_call_events(VgFile *);

void     sl_DEBUG_thread_info(ThreadId);

#endif
