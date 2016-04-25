/*--------------------------------------------------------------------*/
/*--- Slicer: Slicing code between functions              events.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Slicer.

   Copyright (C) 2016 Anthony Carno
        acarno@vt.edu

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/


#include <limits.h>
#include "pub_tool_basics.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_machine.h" //VG_(fnptr_to_fnentry)
#include "pub_tool_mallocfree.h"
#include "events.h"

/*---------------------------------------------*/
/*--- Constants                             ---*/
/*---------------------------------------------*/
#define EVENT_CACHE_SIZE    10      //Size of event cache (NOT thread-unique)
#define MAX_NAME_LENGTH     256     //Maximum file or function name length
#define MAX_NUM_CALL_EVENTS 10000   //Maximum number of call events (per thread)

/*---------------------------------------------*/
/*--- Internal event structs                ---*/
/*---------------------------------------------*/

/* Location where 'called_func' is actually called
 *      NOTE: This may NOT be within 'calling_func'! */
typedef struct call_loc_info_t {
    HChar  file[MAX_NAME_LENGTH];
    UInt   line;
} Call_Loc_Info;

/* Information about a particular function */
typedef struct func_info_t {
    HChar            func[MAX_NAME_LENGTH];
    Call_Loc_Info    loc;
} Func_Info;

/* Information about a particular function pair.
 *      NOTE: A call event's uniqueness is determined by:
 *              (1) the calling function
 *              (2) the called function
 *              (3) the location of the call */
typedef struct call_event_t {
    ULong           id;             //Unique ID
    Func_Info       calling_func;   //Calling function
    Func_Info       called_func;    //Called function
    Call_Loc_Info   call_loc;       //Location of call
    ULong           max_instrs;     //Max # of instrs (calling --> call)
    ULong           min_instrs;     //Min # of instrs
    ULong           avg_instrs;     //Average # of instrs
    ULong           total_instrs;   //Total # of instrs (for averaging)
    ULong           call_count;     //Total # of calls (for averaging)
} Call_Event;

/* Information about a particular thread. */
typedef struct thread_info_t {
    ThreadId        tid;             //Thread ID
    
    Func_Info       cur_calling;     //Most recent calling function
    Func_Info       cur_called;      //Most recent called function
    Call_Loc_Info   cur_call_loc;    //Most recent call location
    ULong           cur_instr_count; //Instructions since last function call

    ULong           num_events;      //Number of unique call events
    Call_Event     *events[MAX_NUM_CALL_EVENTS];
    ULong           last_event_id;   //Most recently examined event (caching)
} Thread_Info;

/*---------------------------------------------*/
/*--- Global arrays                         ---*/
/*---------------------------------------------*/
Thread_Info *threads;
HChar buf[4096];

/*---------------------------------------------*/
/*--- Static function prototypes            ---*/
/*---------------------------------------------*/
static Bool compare_call_loc_info(Call_Loc_Info, Call_Loc_Info);
static Bool compare_func_info(Func_Info, Func_Info);
static Bool find_call_event(Thread_Info *, ULong *);

static void add_call_event(ThreadId, ULong *);
static void update_existing_event(Thread_Info *, ULong);
static Bool get_call_event_string(ThreadId, ULong, HChar *, UInt);

/*---------------------------------------------*/
/*--- Public functions                      ---*/
/*---------------------------------------------*/
void sl_initialize_thread_array(void) {
    threads = VG_(calloc)("sl.init_thread_array.1", 
                          VG_N_THREADS, sizeof *threads);
    if (threads == NULL)
        VG_(tool_panic)("initialize_thread_array malloc failed");
    for (ThreadId tid = 0; tid < VG_N_THREADS; tid++)
    {
        threads[tid].tid = tid;
    }
}

void sl_clean_up(void)
{
    ThreadId tid;
    Thread_Info *ti;
    ULong i;
    for (tid = 0; tid < VG_N_THREADS; tid++)
    {
        ti = &threads[tid];
        for (i = 0; i < ti->num_events; i++)
        {
            VG_(free)(ti->events[i]);
        }
    }


    VG_(free)(threads);
}

IRDirty *sl_update_call_event(ThreadId tid, const HChar *func, 
                                const HChar *file, UInt line)
{
    Bool          retval;
    Thread_Info  *ti;
    ULong         eventId;
    IRExpr      **argv;
    IRDirty      *di;

    ti = &threads[tid];

    //Update thread-specific information
    VG_(strncpy)(ti->cur_called.func, func, MAX_NAME_LENGTH);
    VG_(strncpy)(ti->cur_called.loc.file, file, MAX_NAME_LENGTH);
    ti->cur_called.loc.line = line;

    //Check if call event exists
    retval = find_call_event(ti, &eventId);
    if (!retval)
    {
        //Create new call event
        add_call_event(tid, &eventId);
    }

    ti->last_event_id = eventId;

    //Update call event with current information
    argv = mkIRExprVec_2(mkIRExpr_HWord( (HWord)ti ),
                         mkIRExpr_HWord( (HWord)eventId ));
    di = unsafeIRDirty_0_N(0, "sl_update_call_event",
                           VG_(fnptr_to_fnentry)( &update_existing_event ),
                           argv);
    return di;
}

void sl_incr_instr_count(ThreadId tid, HChar *file, 
                            UInt filename_len, UInt line)
{
    Thread_Info *ti;

    ti = &threads[tid];
    ti->cur_instr_count++;
    if (!VG_STREQN(filename_len, file, ti->cur_call_loc.file))
    {
        VG_(strncpy)(ti->cur_call_loc.file, file, MAX_NAME_LENGTH);
    }
    ti->cur_call_loc.line = line;
}


void sl_dump_call_events(VgFile *dumpfile) {
    ThreadId tid;
    Thread_Info *ti;
    ULong i;

    //Close out any current call events
    for (tid = 0; tid < VG_N_THREADS; ++tid)
    {
        ti = &threads[tid];
        if (ti->cur_instr_count != 0)
        {
            sl_update_call_event(tid, "", "", 0);
            update_existing_event(ti, ti->num_events-1);
        }
    }

    VG_(fprintf)(dumpfile, "%s,%s,%s,%s,%s,%s\n",
            "tid",
            "calling_func,calling_file,calling_line",
            "called_func,called_filed,called_line",
            "call_file,call_loc",
            "max_instrs,min_instrs,avg_instrs",
            "total_instrs,call_count");

    for (tid = 0; tid < VG_N_THREADS; ++tid)
    {
        ti = &threads[tid];
        for (i = 0; i < ti->num_events; ++i)
        {
            VG_(memset)(buf, 0, 4096);
            get_call_event_string(tid, i, buf, 4096);
            VG_(fprintf)(dumpfile, "%s\n", buf);
        }
    }
}

void sl_DEBUG_thread_info(ThreadId tid)
{
    Thread_Info *ti;
    ti = &threads[tid];

    VG_(printf)("Thread %ld:\n"
                "Calling Func:      %s\n"
                "Calling File:Line: %s:%d\n"
                "Called Func:       %s\n"
                "Called File:Line:  %s:%d\n"
                "Called Loc:        %s:%d\n",
                (unsigned long)ti->tid,
                ti->cur_calling.func, 
                ti->cur_calling.loc.file, 
                (unsigned) ti->cur_calling.loc.line,
                ti->cur_called.func,
                ti->cur_called.loc.file,
                (unsigned) ti->cur_called.loc.line,
                ti->cur_call_loc.file,
                (unsigned) ti->cur_call_loc.line);
}

/*---------------------------------------------*/
/*--- Static function definitions           ---*/
/*---------------------------------------------*/

/* Compares two call locations
 *      Returns (Bool) True if locations are identical, 
 *              (Bool) False otherwise */
static Bool compare_call_loc_info(Call_Loc_Info c1, Call_Loc_Info c2)
{
    if (c1.line == c2.line && VG_STREQ(c1.file, c2.file))
        return True;
    return False;
}

/* Compares to Func_Info structs
 *      Returns (Bool) True if Func_Info's are identical,
 *              (Bool) False otherwise */
static Bool compare_func_info(Func_Info f1, Func_Info f2)
{
    if (compare_call_loc_info(f1.loc, f2.loc) && VG_STREQ(f1.func, f2.func))
        return True;
    return False;
}

/* Searches for pre-existing call event within a given thread
 *      Returns (Bool) True if matching Call_Event found and sets
 *                      eventId to the corresponding Call_Event.id
 *      Returns (Bool) False if no Call_Event found and sets
 *                      eventId to 0 (which is NOT valid) */
static Bool find_call_event(Thread_Info *ti, ULong *eventId)
{
    Call_Event *event;
    
    //If there are no events, immediately return False    
    if (ti->num_events == 0)
    {
        *eventId = 0;
        return False;
    }

    //Quick check with last examined event
    event = ti->events[ti->last_event_id];
    if (compare_func_info(ti->cur_calling, event->calling_func)
        && compare_func_info(ti->cur_called, event->called_func)
        && compare_call_loc_info(ti->cur_call_loc, event->call_loc))
    {
        *eventId = event->id;
        return True;
    }

    //If there are events, search for an existing one
    for (ULong i = 0; i < ti->num_events; i++)
    {
        event = ti->events[i];
        if (!compare_call_loc_info(ti->cur_call_loc, event->call_loc))
            continue;
        if (!compare_func_info(ti->cur_calling, event->calling_func))
            continue;
        if (!compare_func_info(ti->cur_called, event->called_func))
            continue;
        //If event is found, return True
        *eventId = event->id;
        return True;
    }

    //If no existing event was found, return False
    *eventId = 0;
    return False;
}

/* Allocates space for a new Call_Event
 *      Sets eventId to corresponding Call_Event.id */
static void add_call_event(ThreadId tid, ULong *eventId) 
{
    Thread_Info *ti;
    ULong        id;
    Call_Event  *new_event;

    ti = &threads[tid];

    //Check if maximum number of call events is going to be exceeded
    if (ti->num_events < MAX_NUM_CALL_EVENTS)
    {
        //Allocate a new event; if allocation fails, panic tool
        new_event = VG_(calloc)("sl.add_call_event.1", 1, sizeof *new_event);
        if (new_event == NULL)
            VG_(tool_panic)("add_call_event malloc failed!");
        
        //Set the event's index as the ID (note this is a per-thread index,
        //  so two events in different threads may share the same ID)
        id = ti->num_events;
        *eventId = id;

        //Set the event's information with the Thread_Info's cur_* members
        VG_(strncpy)(new_event->calling_func.func, 
                ti->cur_calling.func, MAX_NAME_LENGTH);
        VG_(strncpy)(new_event->calling_func.loc.file,
                ti->cur_calling.loc.file, MAX_NAME_LENGTH);
        new_event->calling_func.loc.line = ti->cur_calling.loc.line;
 
        VG_(strncpy)(new_event->called_func.func, 
                ti->cur_called.func, MAX_NAME_LENGTH);
        VG_(strncpy)(new_event->called_func.loc.file,
                ti->cur_called.loc.file, MAX_NAME_LENGTH);
        new_event->called_func.loc.line = ti->cur_calling.loc.line;
       
        VG_(strncpy)(new_event->call_loc.file,
                ti->cur_call_loc.file, MAX_NAME_LENGTH);
        new_event->call_loc.line = ti->cur_call_loc.line;

        //Set the event's instruction information with default values
        //  (to be updated in falling update_existing_event call)
        new_event->max_instrs = 0;
        new_event->min_instrs = ULONG_MAX;
        new_event->total_instrs = 0;
        new_event->call_count = 0;

        //Place the new event in the thread's list of events
        ti->events[id] = new_event;

        //Increment the number of events in the thread's list of events
        ti->num_events++;
    }
    else
    {
        //If maximum number of call events will be exceeded, panic tool
        VG_(tool_panic)("Maximum number of call events exceeded!");
    }
}

/* Updates event's instruction counters and resets calling function info */
static void update_existing_event(Thread_Info *ti, ULong eventId)
{
    Call_Event  *event;

    event = ti->events[eventId];

    //Update event's instruction counters
    if (ti->cur_instr_count > event->max_instrs)
        event->max_instrs = ti->cur_instr_count;
    if (ti->cur_instr_count < event->min_instrs)
        event->min_instrs = ti->cur_instr_count;
    
    event->total_instrs += ti->cur_instr_count;
    ti->cur_instr_count = 0;
    event->call_count++;
    event->avg_instrs = event->total_instrs/event->call_count;

    //Reset thread's calling func information IF it has changed
    //  Note: assuming no recursion!!
    if (!VG_STREQ(ti->cur_calling.func, ti->cur_called.func))
    {
        VG_(strncpy)(ti->cur_calling.func, 
                ti->cur_called.func, MAX_NAME_LENGTH);
        VG_(strncpy)(ti->cur_calling.loc.file,
                ti->cur_called.loc.file, MAX_NAME_LENGTH);
        ti->cur_calling.loc.line = ti->cur_called.loc.line;
    }

}

/* Creates a string representation of the corresponding Call_Event
 *      Returns (Bool) True if event exists and string is created
 *      Returns (Bool) False if event does not exist */
static Bool get_call_event_string(ThreadId tid, ULong id, 
                                    HChar *strbuf, UInt buf_len)
{
    Thread_Info *ti = &threads[tid];
    Call_Event *event;

    if (id >= ti->num_events)
        return False;

    event = ti->events[id];
    
    VG_(snprintf)(strbuf, buf_len, 
                    "%ld,%s,%s,%d,%s,%s,%d,%s,%d,%ld,%ld,%ld,%ld,%ld",
                        (unsigned long) tid,
                        event->calling_func.func,
                        event->calling_func.loc.file,
                        (unsigned) event->calling_func.loc.line,
                        event->called_func.func,
                        event->called_func.loc.file,
                        (unsigned) event->called_func.loc.line,
                        event->call_loc.file,
                        (unsigned) event->call_loc.line,
                        (unsigned long) event->max_instrs,
                        (unsigned long) event->min_instrs,
                        (unsigned long) event->avg_instrs,
                        (unsigned long) event->total_instrs,
                        (unsigned long) event->call_count
                );
    return True;
}
