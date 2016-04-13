/*--------------------------------------------------------------------*/
/*--- Slicer: Slicing code between functions             sl_main.c ---*/
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

#include "pub_tool_basics.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_libcfile.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_debuginfo.h"
#include "pub_tool_options.h"
#include "pub_tool_machine.h"     // VG_(fnptr_to_fnentry)
#include "pub_tool_threadstate.h"

#include "events.h"
/*-----------------------------------------------------*/
/*--- Globals for counting instructions             ---*/
/*-----------------------------------------------------*/
static HChar *funcs[1024];
static UInt num_funcs = 0;

static VgFile *dumpfile = NULL;

/*----------------------------------------------------*/
/*--- Command line options                         ---*/
/*----------------------------------------------------*/

static const HChar *clo_output="output.log";
static const HChar *clo_func_file="";

static Bool sl_process_cmd_line_option(const HChar *arg)
{
    if VG_STR_CLO(arg, "--output", clo_output) {}
    else if VG_STR_CLO(arg, "--funcs", clo_func_file) {}
    else
        return False;

    tl_assert(clo_output);
    tl_assert(clo_output[0]);
    tl_assert(clo_func_file);
    return True;
}

static void sl_print_usage(void)
{
    VG_(printf)(
"     --output=<name>          output to file named <name> [output.log]\n"
"     --funcs=<name>           read function names from <name> []\n"
    );
}

static void sl_print_debug_usage(void)
{
    VG_(printf)(
"     (none)\n"
    );
}

static void read_func_file(const HChar *filename)
{
    HChar line[256], ch;
    Int fd = VG_(fd_open)(filename, VKI_O_RDONLY, 0);
    if (fd == -1)
    {
        VG_(printf)("No file %s found - preceding with all functions.\n",
                    filename);
        return;
    }

    Int i = 0;
    while (VG_(read)(fd, &ch, 1) > 0)
    {
        if (ch != '\n')
        {
            line[i++] = ch;
        }
        else
        {
            line[i] = '\0';
            funcs[num_funcs] = VG_(malloc)("sl.rff.1", 
                                           VG_(strlen)(line+1));
            VG_(strcpy)(funcs[num_funcs], line);
            num_funcs++;
            i = 0;
        }
    }

    VG_(close)(fd);

//    for (i = 0; i < num_funcs; i++)
//        VG_(printf)("%s, ", funcs[i]);
//    VG_(printf)("\n");
}

/*----------------------------------------------------*/
/*--- Instrumentation functions                    ---*/
/*----------------------------------------------------*/

static Bool isin_funcs(const HChar *fn)
{
    for (Int i = 0; i < num_funcs; i++)
    {
        if (VG_STREQ(fn, funcs[i]) == True)
            return True;
    }
    return False;
}

static IRDirty *create_update_if_first_fn_instr(Addr addr)
{
    ThreadId     tid;
    Bool         retval;
    const HChar *func, *file;
    UInt         line;
    IRDirty     *di;

    di = NULL;

    //Check if instruction is first in function
    retval = VG_(get_fnname_if_entry)(addr, &func);
    if (retval)
    {
        //Check if function is one we care about
        if ((num_funcs == 0) || (num_funcs > 0 && isin_funcs(func)))
        {
            //Gather additional information about function
            retval = VG_(get_filename)(addr, &file);
            if (!retval)    file = "";
            retval = VG_(get_linenum)(addr, &line);
            if (!retval)    line = 0;
            
            //Update thread-specific information
            tid = VG_(get_running_tid)();

            //Update call event
            di = sl_update_call_event(tid, func, file, line);
        }
    }

    return di;
}

static IRDirty *create_instr_count_update(Addr addr)
{
    ThreadId      tid;
    Bool          retval;
    const HChar  *file;
    UInt          filename_len, line;
    IRExpr      **argv;
    IRDirty      *di;

    tid = VG_(get_running_tid)();
    di = NULL;

    //Gather additional information about instruction
    retval = VG_(get_filename)(addr, &file);
    if (!retval)    file = "";
    filename_len = VG_(strlen)(file);
    retval = VG_(get_linenum)(addr, &line);
    if (!retval)    line = 0;

    argv = mkIRExprVec_4(mkIRExpr_HWord( (HWord)tid ),
                         mkIRExpr_HWord( (HWord)file ),
                         mkIRExpr_HWord( (HWord)filename_len ), 
                         mkIRExpr_HWord( (HWord)line ));

    //Create dirty instruction to add instruction info to Thread_Info
    di = unsafeIRDirty_0_N(0, "sl_incr_inst",
                           VG_(fnptr_to_fnentry)( &sl_incr_instr_count ),
                           argv );
    return di;
}

static void sl_post_clo_init(void)
{
  sl_initialize_thread_array();

  if (!VG_STREQ(clo_func_file, ""))
      read_func_file (clo_func_file);

}

static
IRSB* sl_instrument ( VgCallbackClosure* closure,
                      IRSB* bb,
                      const VexGuestLayout* layout, 
                      const VexGuestExtents* vge,
                      const VexArchInfo* archinfo_host,
                      IRType gWordTy, IRType hWordTy )
{
    IRDirty        *di;
    Int             i;
    IRSB           *sbOut; 

    if (gWordTy != hWordTy)
    {
      VG_(tool_panic)("host/guest word size mismatch");
    }

    sbOut = deepCopyIRSBExceptStmts (bb);

    i = 0;
    while (i < bb->stmts_used && bb->stmts[i]->tag != Ist_IMark)
    {
      addStmtToIRSB (sbOut, bb->stmts[i]);
      i++;
    }

    for (; i < bb->stmts_used; i++)
    {
      IRStmt *st = bb->stmts[i];
      if (!st || st->tag == Ist_NoOp)
        continue;

      switch (st->tag)
      {
        case Ist_Dirty:
        case Ist_NoOp:
        case Ist_AbiHint:
        case Ist_Put:
        case Ist_PutI:
        case Ist_MBE:
        case Ist_WrTmp:
        case Ist_Store:
        case Ist_StoreG:
        case Ist_LoadG:
        case Ist_CAS:
        case Ist_LLSC:
        case Ist_Exit:
          addStmtToIRSB( sbOut, st );
          break;
        case Ist_IMark:

          //Update per-function info (if instruction is first in function)
          di = create_update_if_first_fn_instr(st->Ist.IMark.addr);
          if (di != NULL)
              addStmtToIRSB(sbOut, IRStmt_Dirty(di));
          
          //Update per-instruction info (always)
          di = create_instr_count_update(st->Ist.IMark.addr);
          if (di != NULL)
              addStmtToIRSB(sbOut, IRStmt_Dirty(di));
          else
              VG_(tool_panic)("create_instr_count_update failed");

          addStmtToIRSB( sbOut, st );  
          break;
        default:
          ppIRStmt(st);
      }
    }

    return sbOut;
}

static void sl_fini(Int exitcode)
{
  dumpfile = VG_(fopen)(clo_output, VKI_O_WRONLY|VKI_O_TRUNC|VKI_O_CREAT, 
                                        VKI_S_IRUSR|VKI_S_IWUSR|
                                        VKI_S_IRGRP|VKI_S_IWGRP|
                                        VKI_S_IROTH);

  if (dumpfile == NULL)
      VG_(tool_panic)("Dumpfile not opened");
  sl_dump_call_events(dumpfile);
  VG_(fclose)(dumpfile);

  for (Int i = 0; i < num_funcs; i++)
      VG_(free)(funcs[i]);

  sl_clean_up();
}

static void sl_pre_clo_init(void)
{
   VG_(details_name)            ("Slicer");
   VG_(details_version)         ("0.1");
   VG_(details_description)     ("Determine size of 'slices' between functions");
   VG_(details_copyright_author)(
      "Copyright (C) 2016, and GNU GPL'd, by Anthony Carno.");
   VG_(details_bug_reports_to)  (VG_BUGS_TO);

   VG_(details_avg_translation_sizeB) ( 275 );

   VG_(basic_tool_funcs)        (sl_post_clo_init,
                                 sl_instrument,
                                 sl_fini);

   VG_(needs_command_line_options)(sl_process_cmd_line_option,
                                   sl_print_usage,
                                   sl_print_debug_usage);
}

VG_DETERMINE_INTERFACE_VERSION(sl_pre_clo_init)

/*--------------------------------------------------------------------*/
/*--- end                                                          ---*/
/*--------------------------------------------------------------------*/
