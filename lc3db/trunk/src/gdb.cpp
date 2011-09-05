/*\
 *  LC-3 Simulator
 *  Copyright (C) 2004  Anthony Liguori <aliguori@cs.utexas.edu>
 *  Copyright (C) 2004  Ehren Kret <kret@cs.utexas.edu>
 *  Modifications 2010  Edgar Lakis <edgar.lakis@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\*/

#include <stdio.h>
#include <signal.h>
#if defined(USE_READLINE)
#include <readline/readline.h>
#include <readline/history.h>
#else
static char* readline (const char* prompt);
#endif
#include <set>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "cpu.hpp"
#include "memory.hpp"
#include "hardware.hpp"
#include "lexical_cast.hpp"
#include "source_info.hpp"
#include "breakpoints.hpp"

extern char* path_ptr;

//namespace std {
//  struct ichar_traits : std::char_traits<char> {
//    static int compare(const char *lhs, const char *rhs, size_t len) {
//      return strncasecmp(lhs, rhs, len);
//    }
//  };
//  typedef basic_string<char, ichar_traits, allocator<char> > istring;
//} 
struct str_case_cmp {
  bool operator()(const char *a, const char *b) const
  {
    return strcasecmp(a, b) < 0;
  }
};


struct DisplayInfo{
  int id;
  bool isActive;
  VariableInfo *variable;
  DisplayInfo(int _id, VariableInfo* _variable) :
    id(_id), variable(_variable),
    isActive(true) {}
};

struct FrameInfo {
  int id;
  uint16_t scope;
  uint16_t framePointer;
  FunctionInfo *function;
  char* sourceLocation;
  FrameInfo(int _id, uint16_t _scope, uint16_t _framePointer, FunctionInfo* _function, const char* _sourceLocation):
  	id(_id), scope(_scope), framePointer(_framePointer), function(_function), sourceLocation(strdup(_sourceLocation)) {}
  ~FrameInfo() { free(sourceLocation); }
};

struct BacktraceInfo {
  uint16_t fromPC;
  std::vector<FrameInfo*> frames;
};

static BacktraceInfo backtrace;
static std::vector<FrameInfo*>::iterator selected_frame=backtrace.frames.end();
static int selected_frame_id = -1;

//static std::map<std::istring, int16_t *> cpu_special_variables;
static std::map<const char*, int16_t *, str_case_cmp> cpu_special_variables;

static int lastDisplay = 0;
static std::vector<DisplayInfo> displays;
typedef std::vector<DisplayInfo>::reverse_iterator DisplayInfoIterator;
const int DISPLAY_NOT_FOUND = -1;


const char * HELP =
"Commands: - shortcuts shown in ()\n"
" compile <filename.asm>\n"
"   Assembles filename.asm.\n\n"

" tty <terminal_device>\n"
"   Use pseudoterminal instead of stdin/stdout.\n\n"

" backtrace (bt) (where)\n"
"   Show the backtrace (function call stack).\n\n"

" break (b) <addr|sym>\n"
"   Set breakpoint at address <addr> or at the label indicated by sym.\n\n"

" continue (c)\n"
"   Executions instructions until halt.\n\n"

" display [expression]\n"
"   Adds expressions to automatic display list. The expressions will be printed on each stop.\n\n"

" disassemble (dasm) <start> <end>\n"
"   Disassemble insructions from address <start> to <end>.\n\n"

" dump (d) <start> <end>\n"
"   Dump the memory from address start to end.\n\n"

" finish\n"
"   Continue untill return\n\n"

" force (f) <symbol> <value>\n"
"   Sets the data represented by symbol to value\n\n"

" frame [<number>]\n"
"   Show the currently selected frame (of function call stack). Or frame with given number.\n\n"

" down [<number>]\n"
"   Select the frame below.\n\n"

" up [<number>]\n"
"   Select the frame above.\n\n"


" info args\n"
"   Show the arguments in the current scope (see backtrace/frame/up/down commands)\n\n"

" info breakpoint\n"
"   Show all the breakpoints\n\n"

" info locals\n"
"   Show the local variables in the current scope (see backtrace/frame/up/down commands)\n\n"

" info variables\n"
"   Show the global scoped variables (also file static)\n\n"

" load (l) <filename.obj>\n"
"   Loads a file named filename.obj. It will also attempt to read a debug\n"
"   named filename.dbg.  The PC will be set to where the file was loaded.\n\n"

" next (n)\n"
"   Steps over the next source line (useful to skip the function calls)\n\n"

" nexti (ni)\n"
"   Steps over the next instruction (useful for TRAP and JSR commands)\n\n"

" print (p) <data>\n"
"   Prints the value of the symbol, register, or address specified by\n"
"   data.  Use \"lc3CPU\" as <data> to see contents of registers.\n\n"

" run\n"
"   Initializes the machine and runs the operating system.\n\n"

" set variable <varname> = <new_value>\n"
"   Command is only supported for setting simple variables.\n\n"

" step (s) [num]\n"
"   Executes the next num steps (line changes of the source).\n\n"

" stepi (si) [num]\n"
"   Executes the next num instruction.\n\n"

" quit (q) exit\n"
"   Quits the simulation session.\n\n"

" x regs\n"
"   Examine contents of registers.\n\n"

" help (h)\n"
"   Displays this help screen\n";

#define MARKER "\x1a\x1a"

extern "C" {
  int lc3_asm(int, const char **);
}

uint16_t load_prog(const char *file, SourceInfo &src_info, Memory &mem, uint16_t *pEntry)
{
  char buf[4096];
  const char *line_error = NULL;
  int debugLine;
  int linenum;
  int fileId = -1;
  int typeId = -1;
  int addr, lastAddr;
  int c;
  FILE *f;
  uint16_t ret;
  const char *entryLabel = NULL;
  std::string object = file;
  std::string base = object.substr(0, object.rfind(".obj"));
  std::string debug = base + ".dbg";

  ret  = mem.load(object);
  if (ret == 0xFFFF) return ret;

  f = fopen(debug.c_str(), "rt");
  if (!f) return ret;

  /* Instead of introducing one more special directive to the assembler,
   * assume that we have C program if fileID > 0 was encountered.
   * See processing of '@' lines below.
   *
  if (1 == fscanf(f, "ENTRY:%s\n", buf)) {
    entryLabel = strdup(buf);
  } else {
    fprintf(stderr, "ENTRY label missing (will break on first word)\n");
  }
  */

  /* Discard all previous Higher Level Language source information.
   * The assembly level information is word based, and will be overwritten
   * individually, to allow loading of multiple source files.
   */
  src_info.reset_HLL_info();
  fprintf(stderr, "\n--------------------- starting parsing debug file: %s -------------------------------\n", debug.c_str());


  debugLine = 0;
  while (!feof(f)) {
    debugLine++;
    c = fgetc(f);

    /* default error if not changed by parsing */
    line_error = "Wrong line format";

    switch(c) {
    case '#':
      if (1 == fscanf(f, "%d:", &fileId)) {
	int i;
        fgets(buf, sizeof(buf), f);
	i = strlen(buf);
	while (i>=1 && isspace(buf[i-1])) {
	  i--;
	  buf[i] = 0;
	}
        src_info.add_source_file(fileId, std::string(buf));
	fprintf(stderr, "add_source_file: %d %s\n", fileId, buf);
	line_error = NULL;
      }
      break;

    case '!':
      if (2 == fscanf(f, "%x:%s\n", &addr, buf)) {
        src_info.symbol[buf] = addr;
	line_error = NULL;
      }
      break;

    case '@':
      if (4 == fscanf(f, "%d:%d:%x:%x\n", &fileId, &linenum, &addr, &lastAddr)) {
	src_info.add_source_line(addr & 0xFFFF, lastAddr & 0xFFFF, fileId, linenum);
	if (fileId > 0) { /* Assume C source if non 0 file ID found */
	  entryLabel = "main";
	}
	line_error = NULL;
      }
      break;

    case 'T':
      if (2 == fscanf(f, " %d=%s\n", &typeId, buf)) {
	/* TODO: maybe it's better to have all the parsing/format in one place, so decode the type here */
	src_info.add_type(typeId, buf);
	line_error = NULL;
      }
      break;

    case 'B':
      {
	char kind;
	char functionName[256];
	int level;
        //fgets(buf, sizeof(buf), f);
	//printf("buf: %s; Ret: %d; kind:%c, n:%s, l:%d, %04x\n", buf, sscanf(buf, " %c:%[^:]:%d:%x\n", &kind, functionName, &level, &addr), kind, functionName, level, addr);
	//if (4 == sscanf(buf, " %c:%[^:]:%d:%x\n", &kind, functionName, &level, &addr)) {
	if (4 == fscanf(f, " %c:%[^:]:%d:%x\n", &kind, functionName, &level, &addr)) {
	  line_error = NULL;
	  if (kind == 'S') {
	    src_info.start_declaration_block(functionName, level, addr);
	  } else if (kind == 'E') {
	    src_info.finish_declaration_block(functionName, level, addr);
	  } else {
	    line_error = "Wrong declaration block kind ('S':start or 'E':end expected)";
	  }
	}
      }
      break;

    case 'S':
      {
	char kind;
	char functionName[256];
	char info1[256];
	char info2[256];
	int level;
	if (4 == fscanf(f, " %c%d:%[^:]:%s\n", &kind, &typeId, info1, info2)) {
	  line_error = NULL;
	  /* maybe it's better to have all the parsing/format in one place, so we decode the symbol here */
	  switch (kind) {
	    case 'G':
	    case 'S':
	    case 's':
	      {
		VariableKind varKind = (kind=='G') ? FileGlobal : (kind=='S') ? FileStatic : FunctionStatic;
		// Scope, Type, C (source) name, LC3 (assembler) label
		src_info.add_absolute_variable(varKind, typeId, info1, info2);
	      }
	      break;
	    case 'l':
	    case 'p':
	      {
		int offset = atoi(info2);
		VariableKind varKind = (kind=='l') ? FunctionLocal : FunctionParameter;
		// Scope, Type, C (source) name, Frame offset
		src_info.add_stack_variable(varKind, typeId, info1, offset);
	      }
	      break;
	    case 'F':
	    case 'f':
	      {
		int isStatic = ('f'==kind); // Is this static or global function
		// isStatic, ReturnType, C (source) name, LC3 (assembler) label
		src_info.add_function(isStatic, typeId, info1, info2);
	      }
	      break;

	    default:
	      line_error = "Unrecognised symbol kind";
	  }
	}
      }
      break;

    default:
      	line_error = "Unrecognised line type";
    }

    if (line_error) {
        fprintf(stderr, "%s in debug information file: %s (line: %d)\n", line_error, debug.c_str(), debugLine);
        /* read reminder of the line */
        fgets(buf, sizeof(buf), f);
    }

  }
  fprintf(stderr, "-----------------------------------------------------------------\n");

  if (pEntry) {
    if (entryLabel && src_info.symbol.count(entryLabel)) {
      *pEntry = src_info.symbol[entryLabel];
    } else {
      *pEntry = ret;
    }
  }

  return ret;
}

static int signal_received = 0;
void sigproc(int sig)
{
  signal(SIGINT, sigproc); /* reset for portability */
  fprintf(stderr, "<Interrupted>\n");
  signal_received = 1;
  // TODO: make proper synchronization to avoid race conditions
}

void print_registers(LC3::CPU &cpu, Memory &mem, SourceInfo &src_info, const char *value_sep, const char *reg_sep);
void print_displays(LC3::CPU &cpu, Memory &mem, SourceInfo &src_info, bool show_values);

void show_execution_position(LC3::CPU &cpu, SourceInfo &src_info, Memory &mem, bool gui_mode, bool quiet_mode, uint16_t scope=0)
{
  if (scope==0) {
    scope = cpu.PC;
  }
  if (gui_mode) {
    SourceLocation sl = src_info.find_source_location_absolute(scope);
    if (sl.lineNo > 0) {
      printf(MARKER "%s:%d:0:%s:0x%.4x\n",
          //mem.debug[scope].c_str(), scope & 0xFFFF);
          sl.fileName, sl.lineNo,
          (sl.isHLLSource && scope > sl.firstAddr) ? "middle" : "beg",
              scope & 0xFFFF);
    } else {
      printf("%.4x in <unknown>\n", scope & 0xFFFF);
    }
  } else if (!quiet_mode) {
    printf("0x%.4x: %.4x: ", scope & 0xFFFF, mem[scope] & 0xFFFF);
    cpu.decode(mem[scope]);
  }
  print_displays(cpu, mem, src_info, true);
}


VariableInfo* find_variable(LC3::CPU &cpu, Memory &mem, SourceInfo &src_info, const char* name, uint16_t scope){
  VariableInfo* v = src_info.find_variable(scope, name);
  if (!v) {
    uint16_t addr;
    if (strcmp(name,"lc3CPU")==0 ||
	strcasecmp(name,"lc3CPU.MCR")==0 ||
	strcasecmp(name,"MCR")==0) {
#warning "refactor the variable info into class wich returns the address (depending on the kind of the variable) and is able to print/set the value (depending on it's type)"
      v = new VariableInfo(name, CpuSpecial);
    } else if (cpu_special_variables.count(name)) {
      v = new VariableInfo(name, CpuSpecial);
    } else if (src_info.symbol.count(name)) {
      addr = src_info.symbol[name];
      v = new VariableInfo(name, AssemblerLabel, 0, addr);
    } else {
      try {
	addr = lexical_cast<uint16_t>(name);
	v = new VariableInfo(name, AssemblerLabel, 0, addr);
      } catch(bad_lexical_cast &e) {
	return NULL;
      }
    }
  }
  return v;
  // TODO: FixMe: handle freeing/aliasing
}

void set_variable(VariableInfo *v, LC3::CPU &cpu, Memory &mem, SourceInfo &src_info, std::string valueString){
  try {
    int16_t val = lexical_cast<int16_t>(valueString);
    if (v->isCpuSpecial) {
      if (strcmp(v->name,"lc3CPU")==0) {
	fprintf(stderr, "Registers can only be set individually\n");
      } else if (strcasecmp(v->name,"lc3CPU.MCR")==0 ||
	  strcasecmp(v->name,"MCR")==0) {
	mem[0xFFFE] = val;
      } else if(cpu_special_variables.count(v->name)) {
	*cpu_special_variables[v->name] = val;
      } else {
	fprintf(stderr, "Can't set \"%s\" wrong special variable\n", v->name);
      }
    } else {
      uint16_t addr = v->address + (v->isAddressAbsolute ? 0 : cpu.R[5]);
      mem[addr] = val;
    }
  } catch(bad_lexical_cast &e) {
    fprintf(stderr, "Can't set \"%s\" to \"%s\", wrong value (only single value integers from [-0x8000..0xFFFF] range supported)\n", v->name, valueString.c_str());
  }

}


void print_variable(VariableInfo *v, LC3::CPU &cpu, Memory &mem, SourceInfo &src_info, bool compressed=false){
  //const char *fmt = compressed ? "%s=0x%.4x %d" : "%s = 0x%.4x %d\n";
  const char *fmt = compressed ? "%s=%d" : "%s = %d\n";
  int16_t val;

  if (v->isCpuSpecial) {
    if (strcmp(v->name,"lc3CPU")==0) {
      printf("lc3CPU = {");
      print_registers(cpu, mem, src_info, " = ",", ");
      printf("}\n");
      return;
    } else if (strcasecmp(v->name,"lc3CPU.MCR")==0 ||
	strcasecmp(v->name,"MCR")==0) {
      val = mem[0xFFFE] & 0xFFFF;
    } else if(cpu_special_variables.count(v->name)) {
      val = *cpu_special_variables[v->name] ;
    } else {
      fprintf(stderr, "Can't print \"%s\" wrong special variable\n", v->name);
      return;
    }
  } else {
    uint16_t addr = v->address + (v->isAddressAbsolute ? 0 : cpu.R[5]);
    val = mem[addr] & 0xFFFF;
  }

  //printf(fmt, v->name, val, val);
  printf(fmt, v->name, val);
}

void print_registers(LC3::CPU &cpu, Memory &mem, SourceInfo &src_info, const char *value_sep, const char *reg_sep)
{
  for (int i = 0; i < 8; i++) {
#if 1
    printf("R%d%s0x%.4x %d%s", i, value_sep, cpu.R[i] & 0xFFFF, cpu.R[i], reg_sep);
  }
  printf("PC%s0x%.4x %5d%s",value_sep, cpu.PC & 0xFFFF, cpu.PC & 0xFFFF, reg_sep);
  printf("MCR%s0x%.4x %s%s",value_sep, mem[0xFFFE] & 0xFFFF, (mem[0xFFFE]&0x8000)?"":"Halted", reg_sep);
  printf("PSR%s0x%.4x %s Pri:%.1x %c%c%c",value_sep, cpu.PSR & 0xFFFF,
      (cpu.PSR & 0x8000) ? "User" : "Kern",
      (cpu.PSR >> 8) & 0x7,
      (cpu.PSR&0x4)?'N':'-', (cpu.PSR&0x2)?'Z':'-', (cpu.PSR&0x1)?'P':'-');
#else
    printf("R%d%s0x%.4x%s", i, value_sep, cpu.R[i] & 0xFFFF, reg_sep);
  }
  
  printf("PC%s0x%.4x%s",value_sep, cpu.PC & 0xFFFF, reg_sep);
  printf("MCR%s0x%.4x %s%s",value_sep, mem[0xFFFE] & 0xFFFF, (mem[0xFFFE]&0x8000)?"":"Halted", reg_sep);
  printf("PSR%s0x%.4x %s Pri:%.1x %c%c%c",value_sep, cpu.PSR & 0xFFFF,
      (cpu.PSR & 0x8000) ? "User" : "Kern",
      (cpu.PSR >> 8) & 0x7,
      (cpu.PSR&0x4)?'N':'-', (cpu.PSR&0x2)?'Z':'-', (cpu.PSR&0x1)?'P':'-');
#endif
}

void print_displays(LC3::CPU &cpu, Memory &mem, SourceInfo &src_info, bool show_values){
  DisplayInfoIterator it;

  if (!show_values) {
    if (displays.size()>0) {
      printf("Num Enb Expression\n");
    } else {
      printf("There are no auto-display expressions now.\n");
    }
  }
  //for (it=displays.begin(); it != displays.end(); it++) {
  for (it=displays.rbegin(); it != displays.rend(); ++it) {
    printf("%d: ", it->id);
    if (show_values) {
      print_variable(it->variable, cpu, mem, src_info);
    } else {
      printf("  %c  %s\n", it->isActive ? 'y' : 'n', it->variable->name);
    }
  }
}


int find_display(int id) {
  int i;
  for (i=0; i < displays.size(); i++) {
    if (id==displays[i].id) {
      return i;
    }
  }
  return DISPLAY_NOT_FOUND;
}

void print_locals(LC3::CPU &cpu, Memory &mem, SourceInfo &src_info, uint16_t scope=0){
  int cnt = 0;
  if (scope==0) {
    scope = cpu.PC;
  }
  SourceBlock *sb = src_info.find_source_block(scope);
  while(sb) {
    std::list<VariableInfo*>::const_iterator it;
    for (it=sb->variables.begin(); it != sb->variables.end(); it++) {
      print_variable(*it, cpu, mem, src_info);
      cnt++;
    }
    sb = sb->parent;
  }
  if (cnt==0) {
    printf("No locals.\n");
  }
}
void print_args(LC3::CPU &cpu, Memory &mem, SourceInfo &src_info, uint16_t scope=0){
  int cnt = 0;
  if (scope==0) {
    scope = cpu.PC;
  }
  SourceBlock *sb = src_info.find_source_block(scope);
  if(sb && sb->function) {
    std::list<VariableInfo*>::const_iterator it;
    FunctionInfo *f = sb->function;
    for (it=f->args.begin(); it != f->args.end(); it++) {
      print_variable(*it, cpu, mem, src_info);
      cnt++;
    }
  }
  if (cnt==0) {
    printf("No args.\n");
  }
}
void print_globals(LC3::CPU &cpu, Memory &mem, SourceInfo &src_info, uint16_t scope=0){
  int cnt = 0;
  std::map<std::string, VariableInfo*>::const_iterator it;
  for (it=src_info.globalVariables.begin(); it != src_info.globalVariables.end(); it++) {
    print_variable(it->second, cpu, mem, src_info);
    cnt++;
  }
  if (cnt==0) {
    printf("No global variables.\n");
  }
}

void update_backtrace(LC3::CPU &cpu, Memory &mem, SourceInfo &src_info){
  static char buff[512];

  if (backtrace.fromPC != cpu.PC) {
    // remove old
    for (int i=0; i < backtrace.frames.size(); i++) {
      delete backtrace.frames[i];
    }
    backtrace.frames.clear();

    // create new
    backtrace.fromPC = cpu.PC;
    uint16_t scope = cpu.PC;
    uint16_t framePointer = (uint16_t)cpu.R[5];
    int cnt = 0;
    SourceBlock *sb;

    do {
      sb = src_info.find_source_block(scope);
      if(!sb || !sb->function) {
	break;
      }

      SourceLocation sl = src_info.find_source_location_short(scope);
      if (sl.lineNo > 0) {
	snprintf(buff, sizeof(buff), "at %s:%d", sl.fileName, sl.lineNo);
      } else {
	snprintf(buff, sizeof(buff), "<unknown source location>");
      }

      backtrace.frames.push_back(new FrameInfo(cnt, scope, framePointer, sb->function, buff));

      // calculate new frame
      cnt++;
      scope = mem[framePointer+2] & 0xFFFF;
      framePointer = mem[framePointer+1] & 0xFFFF;
    } while (strcmp(sb->function->name, "main")!=0);

    selected_frame = backtrace.frames.begin();
    selected_frame_id = 0;
  }
}

void print_frame(FrameInfo *frame, LC3::CPU &cpu, Memory &mem, SourceInfo &src_info) {
  FunctionInfo* function = frame->function;

  // "#1  0x08048495 in functionABC (arg_a=1, arg_b=97 'a', arg_c=0x8048650 "Hello") at debug_info.c:23\n"
  printf("#%d  ", frame->id);
  if (frame->id) {
    printf("0x%04x ", frame->scope);
  }
  printf("%s (", function->name);

  std::list<VariableInfo*>::const_iterator it;
  for (it=function->args.begin(); it != function->args.end(); it++) {
    if (it!=function->args.begin()) {
      printf(", ");
    }
    print_variable(*it, cpu, mem, src_info, true);
  }
  printf(") %s\n", frame->sourceLocation);
}

int gdb_mode(LC3::CPU &cpu, SourceInfo &src_info, Memory &mem, Hardware &hw,
	     bool gui_mode, bool quiet_mode, const char *exec_file)
{
  std::string param1;
  std::string param2;
  uint16_t off = 0;
  uint16_t x_addr, x_off;
  char x_string[256];
  char sys_string[2048];

  int instruction_count = 0;

  if (!quiet_mode) {
    printf("Type `help' for a list of commands.\n");
  }

  cpu_special_variables["R0"] = &cpu.R[0];
  cpu_special_variables["R1"] = &cpu.R[1];
  cpu_special_variables["R2"] = &cpu.R[2];
  cpu_special_variables["R3"] = &cpu.R[3];
  cpu_special_variables["R4"] = &cpu.R[4];
  cpu_special_variables["R5"] = &cpu.R[5];
  cpu_special_variables["R6"] = &cpu.R[6];
  cpu_special_variables["R7"] = &cpu.R[7];
  cpu_special_variables["PC"] = (int16_t *)&cpu.PC;
  cpu_special_variables["PSR"] = (int16_t *)&cpu.PSR;
  cpu_special_variables["lc3CPU.R0"] = &cpu.R[0];
  cpu_special_variables["lc3CPU.R1"] = &cpu.R[1];
  cpu_special_variables["lc3CPU.R2"] = &cpu.R[2];
  cpu_special_variables["lc3CPU.R3"] = &cpu.R[3];
  cpu_special_variables["lc3CPU.R4"] = &cpu.R[4];
  cpu_special_variables["lc3CPU.R5"] = &cpu.R[5];
  cpu_special_variables["lc3CPU.R6"] = &cpu.R[6];
  cpu_special_variables["lc3CPU.R7"] = &cpu.R[7];
  cpu_special_variables["lc3CPU.PC"] = (int16_t *)&cpu.PC;
  cpu_special_variables["lc3CPU.PSR"] = (int16_t *)&cpu.PSR;


#if defined(USE_READLINE)
  using_history();
#endif
  mem[0xFFFE] = mem[0xFFFE] | 0x8000;

  char *cmdline = 0;
  const char *cmd = 0;
  std::string last_cmd;

  UserBreakpoits breakpoints(src_info);
  bool break_on_return;
  bool breakpoint_hit = false;	// Flag set once breakpoint is detected, so that breakpointed instruction can be enabled.
  int temporary_breakpoint = -1;
  int repeat_count = 0;
  const int INT_INFINITY = 0x7FFFFFFF;
  int saved_execution_range_start = 0;
  int saved_execution_range_end = INT_INFINITY;
  uint16_t selected_scope = cpu.PC;

  show_execution_position(cpu, src_info, mem, gui_mode, quiet_mode);

  signal(SIGINT, sigproc);
  break_on_return = false;
  for(;
      cmdline = readline(quiet_mode ? "(gdb) " : "(lc3db) ");
      free(cmdline)) try {
    int instructions_to_run = 0;
    int limit_execution_range_start = 0;
    int limit_execution_range_end = INT_INFINITY;
    int step_over_calls = 0;
    int in_step_over_mode = 0;
    int show_help = 0;

    if (!*cmdline) {
      cmd = last_cmd.c_str();
    } else {
      cmd = cmdline;
#if defined(USE_READLINE)
      add_history(cmd);
#endif
    }

    std::istringstream incmd(cmd);
    std::string cmdstr;

    cmdstr.clear();
    param1.clear();
    param2.clear();
    incmd >> cmdstr;

#warning "HELP is disabled for developement. Enable it before a release!"
//    if (cmdstr == "help" || cmdstr == "h") {
//      show_help = 1;
//      incmd >> cmdstr;
//      // Leave only general list with commands and  split the help message
//      // to commands and print it together with parsing (see "run" command)
//      //printf("%s", HELP);
//    }

    if (cmdstr == "run") {
      if (show_help) {
	printf("Initializes the machine and runs the object file\n"
	    "The arguments are not supported, but simplified input and output redirection can be used:\n"
	    "   run < file_or_terminal\n"
	    "This will redirect both input and output (the \">\" and \">>\" are not allowed.\n");
	continue;
      }
      incmd >> param1 >> param2;
      if (param1 == "<" && !param2.empty()) {
	hw.set_tty(open(param2.c_str(), O_RDWR));
      }

      uint16_t pc = load_prog("lib/los.obj", src_info, mem, NULL);
      if (pc == 0xFFFF) {
	sprintf(sys_string, "%s/lib/lc3db/los.obj", path_ptr);
	printf("Loading %s\n", sys_string);
	pc = load_prog(sys_string, src_info, mem, NULL);
      }
      else {
	printf("Loading lib/los.obj\n");
      }

      // Load executable if specified
      if (exec_file) {
	uint16_t entry;
	printf("Loading %s\n", exec_file);
      	uint16_t start_addr = load_prog(exec_file, src_info, mem, &entry);
	if (0xFFFF == start_addr) {
	  printf("failed to load %s\n", exec_file);
	} else {
	  mem[0x01FE] = start_addr;
	  int bt_id = breakpoints.add(entry, false);
	  //temporary_breakpoint = start_addr;
	}
      }

      if (pc != 0xFFFF) {
	cpu.PC = mem[0x01FF];
	cpu.PSR = 0x0000;
	instructions_to_run = INT_INFINITY;
	mem[0xFFFE] = mem[0xFFFE] | 0x8000;
      } else {
	printf("Could not find los.obj\n");
      }
    } else if (cmdstr == "finish") {
      break_on_return = true;
      instructions_to_run = INT_INFINITY;
    } else if (cmdstr == "set") {
      incmd >> param1;

      if (param1 == "variable") {
	param1.clear();
	incmd >> param1 >> param2;
	if (param2 != "=") {
	  fprintf(stderr, "\"set\" command is only supported for setting simple variables. Syntax:\n"
	      "   set variable VARIABLE_NAME = NEW_VALUE\n"
	      "The VARIABLE_NAME can be C level variable visible in the current scope or CPU registers or assembler level label.\n");
	  continue;
	}
	incmd >> param2;


	VariableInfo* v = find_variable(cpu, mem, src_info, param1.c_str(), selected_scope);
	if (v) {
	  set_variable(v, cpu, mem, src_info, param2);
	} else {
	  fprintf(stderr, "variable \"%s\" not found\n", param1.c_str());
	}
      } else {
	fprintf(stderr, "\"set\" command is only supported for setting simple variables. Syntax:\n"
	    "   set variable VARIABLE_NAME = NEW_VALUE\n"
	    "The VARIABLE_NAME can be C level variable visible in the current scope or CPU registers or assembler level label.\n");
      }
    } else if (cmdstr == "dump" || cmdstr == "d"  || cmdstr == "x") {
      if (cmdstr == "x") {
	incmd >> param2 >> param1;
	if (param2 == "regs" || param1 == "regs") {
	  for (int i = 0; i < 8; i++) {
	    if (i == 4) printf("\n");
	    printf("R%d:  %.4x (%5d)  ", i,
		   cpu.R[i] & 0xFFFF, cpu.R[i] & 0xFFFF);
	  }
	  printf("\n");
	  printf(
      "PC:  %.4x (%5d)  PSR: %.4x (%5d)  MCR: %.4x (%5d)  MCC: %.4x (%5d)\n"
      "Instructions Run: %5d               Mode: %s  Pri: %d  NZP: %c%c%c\n",
		 cpu.PC & 0xFFFF, cpu.PC & 0xFFFF,
		 cpu.PSR & 0xFFFF, cpu.PSR & 0xFFFF,
		 mem[0xFFFE] & 0xFFFF, mem[0xFFFE] & 0xFFFF,
                 mem[0xFFFF] & 0xFFFF, mem[0xFFFF] & 0xFFFF,
		 instruction_count,
		 (cpu.PSR & 0x8000) ? "User  " : "Kernel",
		 (cpu.PSR >> 8) & 0x7, (cpu.PSR&0x4)?'1':'0',
                 (cpu.PSR&0x2)?'1':'0', (cpu.PSR&0x1)?'1':'0');
	  continue;
	}

	x_addr = lexical_cast<uint16_t>(param1);
	x_off = lexical_cast<uint16_t>(param2);
	x_addr = x_addr + x_off;
	sprintf(x_string, "x%x", x_addr);
	param2 = x_string;

      } else {
	incmd >> param1 >> param2;
      }
      off = lexical_cast<uint16_t>(param1);
      uint16_t off2 = lexical_cast<uint16_t>(param2);
      uint16_t counter = 1;

      std::ostringstream myout;
      myout << std::setprecision(4) << std::setw(4) << std::setfill('0')
	    << std::hex << off << ":";
      for(;off<=off2;off++,counter++) {
	myout << " " << std::hex << std::setprecision(4) << std::setw(4)
	      << std::setfill('0') << mem[off] ;
	if(counter%8==0 && off!=off2) {
	  myout << std::endl << std::hex << std::setprecision(4)
		<< std::setw(4) << std::setfill('0') << off+1 << ":";
	  counter=0;
	}
      }
      myout << std::endl;
      printf("%s",myout.str().c_str());
    } else if (cmdstr == "compile") {
      incmd >> param1;
      const char *args[] = { "", param1.c_str(), NULL };
      lc3_asm(2, args);
    } else if (cmdstr == "tty") {
      incmd >> param1;
      hw.set_tty(open(param1.c_str(), O_RDWR));
    } else if (cmdstr == "continue" || cmdstr == "c" || cmdstr=="cont") {
      printf("running\n");
      instructions_to_run = INT_INFINITY;
    } else if (cmdstr == "disassemble" || cmdstr == "dasm") {
      incmd >> param1 >> param2;
      uint16_t start = lexical_cast<uint16_t>(param1);
      uint16_t end  = lexical_cast<uint16_t>(param2);
      for (; start < end; start++) {
	uint16_t IR = mem[start];
	for (std::map<std::string, uint16_t>::iterator i = src_info.symbol.begin();
	     i != src_info.symbol.end(); ++i) {
	  if (i->second == start) {
	    printf("%s:\n", i->first.c_str());
	    break;
	  }
	}
	printf("0x%.4x: %.4x: ", start & 0xFFFF, IR & 0xFFFF);
	cpu.decode(IR);
      }
    } else if (cmdstr == "load" || cmdstr == "l" || cmdstr == "file") {
      incmd >> param1;
      uint16_t entry;
      uint16_t pc = load_prog(param1.c_str(), src_info, mem, &entry);
      if (pc != 0xFFFF) {
	cpu.PC = pc;
	//const char *file = "";
	//if (!mem.debug[cpu.PC].empty()) {
	//  file = mem.debug[cpu.PC].c_str();
	//}
	int bt_id = breakpoints.add(entry, false);
	show_execution_position(cpu, src_info, mem, gui_mode, quiet_mode);
	mem[0xFFFE] = mem[0xFFFE] | 0x8000;
      } else {
	printf("Could not open %s\n", param1.c_str());
      }
    } else if (cmdstr == "stepi" || cmdstr == "si") {
      incmd >> param1;
      try {
	instructions_to_run = lexical_cast<uint16_t>(param1);
      } catch(bad_lexical_cast &e) {
	instructions_to_run = 1;
      }
    } else if (cmdstr == "nexti" || cmdstr == "ni") {
  l_handle_nexti:
      instructions_to_run = 1;
      step_over_calls = 1;
    } else if (cmdstr == "step" || cmdstr == "s") {
      incmd >> param1;
      int repeat;
      try {
	repeat = lexical_cast<uint16_t>(param1);
      } catch(bad_lexical_cast &e) {
	repeat = 1;
      }
      SourceLocation line = src_info.find_source_location_absolute(cpu.PC);
      if (line.lineNo <= 0 || !line.isHLLSource) {
	// No hi level line at current location
	instructions_to_run = repeat;
      } else {
	if (repeat != 1) {
	  fprintf(stderr, "step argument is currently not supported for C language\n");
	}
	limit_execution_range_start = line.firstAddr;
	limit_execution_range_end = line.lastAddr;
	instructions_to_run = INT_INFINITY;
      }
    } else if (cmdstr == "next" || cmdstr == "n") {
      SourceLocation line = src_info.find_source_location_absolute(cpu.PC);
      if (line.lineNo <= 0 || !line.isHLLSource) {
	// No hi level line at current location
	goto l_handle_nexti;
      } else {
	instructions_to_run = INT_INFINITY;
	step_over_calls = 1;
	limit_execution_range_start = line.firstAddr;
	limit_execution_range_end = line.lastAddr;
      }
    } else if (cmdstr == "ignore") {
      incmd >> param1 >> param2;
      int id = lexical_cast<uint16_t>(param1);
      int count = lexical_cast<uint16_t>(param2);
      int res = breakpoints.setIgnoreCount(id, count);
      if (res > 0) {
	printf("Will ignore next %d crossings of breakpoint %d.\n", count, res);
      } else {
	printf("No breakpoint number %d.\n", id);
      }
    } else if (cmdstr == "break" || cmdstr == "b" ||
	       cmdstr == "tbreak" || cmdstr == "tb") {
      uint16_t bp_addr;
      bool bp_valid = false;
      size_t colPos;
      incmd >> param1;
      bool make_temporary = (cmdstr[0] == 't');

      if (src_info.symbol.count(param1)) {
	// Symbol
	bp_addr = src_info.symbol[param1];
	bp_valid = true;
      } else if ((colPos=param1.find(":")) != std::string::npos) {
	// FILENAME:LINE
	std::string fileName = param1.substr(0,colPos);
	try {
	  uint16_t lineNo;
	  lineNo = lexical_cast<uint16_t>(param1.substr(colPos+1));
	  bp_addr = src_info.find_line_start_address(fileName, lineNo);
	  bp_valid = bp_addr != 0;
	} catch(bad_lexical_cast &e) {
	  bp_valid = false;
	}
      } else {
	// try verbatim number
	if (param1[0] == '*') {
	  bp_addr = lexical_cast<uint16_t>(param1.substr(1));
	} else {
	  bp_addr = lexical_cast<uint16_t>(param1);
	}
	bp_valid = true;
      }

      if (bp_valid) {
	int bt_id = breakpoints.add(bp_addr, make_temporary);
	//if (make_temporary && bt_id > 0) {
	//  breakpoints.setEnabled(bt_id, true, true, Delete);
	//}
      } else {
	printf("breakpoint specification [%s] is not valid\n", param1.c_str());
      }


    } else if (cmdstr == "display" || cmdstr == "disp") {
      incmd >> param1;

      if (param1.empty()) {
	// show display list
	print_displays(cpu, mem, src_info, true);
      } else {
	// add to display
	VariableInfo* v = find_variable(cpu, mem, src_info, param1.c_str(), selected_scope);
	if (v) displays.push_back(DisplayInfo(++lastDisplay, v));
      }
    } else if (cmdstr == "print" || cmdstr == "p" || cmdstr == "output") {
      incmd >> param1;
      VariableInfo* v = find_variable(cpu, mem, src_info, param1.c_str(), selected_scope);
      if (v) {
	print_variable(v, cpu, mem, src_info);
      }
    } else if (cmdstr == "exit" || cmdstr == "quit" || cmdstr == "q") {
      free(cmdline);
      cmdline = 0;
      break;
    } else if (cmdstr == "ignore") {
      //param1.clear();
      //param2.clear();
      incmd >> param1 >> param2;
      int id = lexical_cast<uint16_t>(param1);
      int count = lexical_cast<uint16_t>(param1);
      breakpoints.setIgnoreCount(id, count);
    } else if (cmdstr == "delete" || cmdstr == "disable" || cmdstr == "undisplay") {
      bool is_delete_cmd = cmdstr=="delete" || cmdstr=="undisplay";
      bool is_display_cmd = cmdstr=="undisplay";
      param1.clear();
      incmd >> param1;

      if (!is_display_cmd) {
	if (param1 == "display") {
	  param1.clear();
	  incmd >> param1;
	  is_display_cmd = 1;
	} else if (param1 == "breakpoints") {
	  param1.clear();
	  incmd >> param1;
	  is_display_cmd = 0;
	}
      }
      if (param1.empty()) {
	printf("%s id expected.\nTry using the `help' command.\n", is_display_cmd ? "Display" : "Breakpoint");
      } else {
	do{
	  int id = lexical_cast<uint16_t>(param1);
	  if (!is_display_cmd) {
	    if (is_delete_cmd) {
	      breakpoints.erase(id);
	    } else {
	      breakpoints.setEnabled(id, false, false, Keep);
	    }
	  } else {
#warning "handle the remove/enable/disable of lc3CPU variable and it's printing and setting (using lc3CPU.R0, lc3CPU.CC)"
	    int i = find_display(id);
	    if (i == DISPLAY_NOT_FOUND) {
	      printf("No display number %d\n", id);
	    } else {
		if (is_delete_cmd) {
		  displays.erase(displays.begin()+i);
		} else {
		  displays[i].isActive = 0;
		}
	    }
	  }
	  param1.clear();
	  incmd >> param1;
	} while (!param1.empty());
      }
    } else if (cmdstr == "enable") {
      bool is_display_cmd = 0;
      BreakpointDisposition disp = Keep;
      //param1.clear();
      incmd >> param1;
      if (param1 == "display") {
	param1.clear();
	incmd >> param1;
	is_display_cmd = 1;
      } else if (param1 == "breakpoints") {
	param1.clear();
	incmd >> param1;
	is_display_cmd = 0;
      }
      if (!is_display_cmd && param1 == "once") {
	disp = Disable;
	param1.clear();
	incmd >> param1;
      } else if (!is_display_cmd && param1 == "delete") {
	disp = Delete;
	param1.clear();
	incmd >> param1;
      }
      if (param1.empty()) {
	printf("%s id expected.\nTry using the `help' command.\n", is_display_cmd ? "Display" : "Breakpoint");
      } else {
	do{
	  int id = lexical_cast<uint16_t>(param1);
	  if (!is_display_cmd) {
	    breakpoints.setEnabled(id, true, disp != Keep, disp);
	  } else {
	    int i = find_display(id);
	    if (i == DISPLAY_NOT_FOUND) {
	      printf("No display number %d\n", id);
	    } else {
	      displays[i].isActive = 1;
	    }
	  }
	  param1.clear();
	  incmd >> param1;
	} while (!param1.empty());
      }
    } else if (cmdstr == "frame" || cmdstr == "up" || cmdstr == "down") {
	incmd >> param1;
	int N = -1;
	try {
	  N = lexical_cast<uint16_t>(param1);
	} catch(bad_lexical_cast &e) {
	}

	update_backtrace(cpu, mem, src_info);
	int frame_count = backtrace.frames.size();

	if (cmdstr == "frame") {
	  //selected_frame = backtrace.frames.begin() + ((N==-1) ? 0 : N);
	  selected_frame_id = ((N==-1) ? selected_frame_id : N);
	} else if (cmdstr == "up") {
	  selected_frame_id += ((N==-1) ? 1 : N);
	  //selected_frame += (N==-1) ? 1 : N;
	  //selected_frame = backtrace.frames.begin() + (*selected_frame)->id +((N==-1) ? 1 : N);
	} else if (cmdstr == "down") {
	  selected_frame_id -= ((N==-1) ? 1 : N);
	  //selected_frame = backtrace.frames.begin() + (*selected_frame)->id -((N==-1) ? 1 : N);
	  //selected_frame -= (N==-1) ? 1 : N;
	}
	if (selected_frame_id >= frame_count) {
	  selected_frame_id = frame_count-1;
	}
	if (selected_frame_id < 0) {
	  selected_frame_id = 0;
	}
	selected_frame = backtrace.frames.begin() + selected_frame_id;

	//if (selected_frame != backtrace.frames.end()) {
	if (frame_count) {
	  print_frame(*selected_frame, cpu, mem, src_info);
	  selected_scope = (*selected_frame)->scope;
	  show_execution_position(cpu, src_info, mem, gui_mode, quiet_mode, selected_scope);
	}
	continue;
    } else if (cmdstr == "bt" || cmdstr == "backtrace" || cmdstr == "where") {
      update_backtrace(cpu, mem, src_info);

      for (int i=0; i < backtrace.frames.size(); i++) {
	print_frame(backtrace.frames[i], cpu, mem, src_info);
      }
    } else if (cmdstr == "info") {
      incmd >> param1;
#warning "fake command for developement. Remove it before a release!"
      if (param1 == "all") {
	incmd >> param2;
	uint16_t scope, backup;
	try {
	  scope = lexical_cast<uint16_t>(param2) & 0xffff;
	} catch(bad_lexical_cast &e) {
	  scope = cpu.PC;
	}
	printf("Locals:\n");
	print_locals(cpu, mem, src_info, scope);
	printf("Args:\n");
	print_args(cpu, mem, src_info, scope);
	printf("Variables:\n");
	print_globals(cpu, mem, src_info, scope);
      }
      if (param1 == "breakpoints" || param1 == "b") {
	breakpoints.showInfo();
	// FixMe: Todo: add context, to use with "frame" commands, and use it as replacement of cpu.PC
      } else if (param1 == "display") {
	print_displays(cpu, mem, src_info, false);
      } else if (param1 == "locals") {
	print_locals(cpu, mem, src_info, selected_scope);
      } else if (param1 == "args") {
	print_args(cpu, mem, src_info, selected_scope);
      } else if (param1 == "variables") {
	print_globals(cpu, mem, src_info, selected_scope);
      } else if (param1 == "registers" || param1 == "r") {
	  print_registers(cpu, mem, src_info, "\t","\n");
      }
    } else {
      printf("Bad command `%s'\nTry using the `help' command.\n", cmd);
      continue;
    }

    if (instructions_to_run) {
      while (instructions_to_run) {
	//fprintf(stderr, "\nIR: %d \tPC: %04x\n", instructions_to_run, cpu.PC & (0xFFFF));
	instructions_to_run--;

	// Check stoping conditions
	if (!(mem[0xFFFE] & 0x8000)) {
	  fprintf(stderr, "LC3 is halted\n");
	  break;
	}
	// FixMe:
	if (break_on_return) {
	  int i = mem[cpu.PC] & (0xFFFF);
	  if (i == 0xc1c0 || // RET
	      i == 0x8000)   // RTI
	  {
	    fprintf(stderr, "stopped on return\n");
	    break_on_return = false;
	    break;
	  }
	}
	if (signal_received) {
	  signal_received = 0;
	  break;
	}
	if (temporary_breakpoint == cpu.PC) {
	  temporary_breakpoint = -1;
	  if (!in_step_over_mode) {
	    break;
	  } else {
	    limit_execution_range_start = saved_execution_range_start;
	    limit_execution_range_end = saved_execution_range_end;
	    // Reset for next command
	    step_over_calls = 1;
	  }
	}
	if (!breakpoint_hit && breakpoints.check(cpu.PC)) {
	  breakpoint_hit = true;
	  break;
	}

	// next/nexti workaround
	if (step_over_calls) {
	  uint16_t IR_opcode = mem[cpu.PC] & 0xF000;
	  if (IR_opcode == 0xF000 || // TRAP
	      IR_opcode == 0x4000) { // JSR
	    // Next instruction is a call
	    in_step_over_mode = 1;
	    step_over_calls = 0;

	    temporary_breakpoint = cpu.PC + 1;

	    instructions_to_run = INT_INFINITY;
	    saved_execution_range_start = limit_execution_range_start;
	    saved_execution_range_end = limit_execution_range_end;
	    limit_execution_range_start = 0;
	    limit_execution_range_end = INT_INFINITY;
	  }
	}

	// Execute
	cpu.cycle();
	mem.cycle();
	instruction_count++;
	breakpoint_hit = false;

	if (cpu.PC < limit_execution_range_start ||
	    cpu.PC > limit_execution_range_end) {
	  break;
	}

      }
      selected_scope = cpu.PC;
      show_execution_position(cpu, src_info, mem, gui_mode, quiet_mode);
    }

    repeat_count = 0;
  } catch (bad_lexical_cast &e) {
      printf("Bad command `%s'\nTry using the `help' command.\n", cmd);
  }

  return 0;
}

#if !defined(USE_READLINE)
static char*
readline (const char* prompt)
{
    char buf[200];
    char* strip_nl;

    /* Prompt and read a line until successful. */
    while (1) {

#if !defined(USE_READLINE)
	printf ("%s", prompt);
#endif
	/* read a line */
	if (fgets (buf, 200, stdin) != NULL)
	    break;

	/* no more input? */
    	if (feof (stdin))
	    return NULL;

    	/* Otherwise, probably a CTRL-C, so print a blank line and
	   (possibly) another prompt, then try again. */
    	puts ("");
    }

    /* strip carriage returns and linefeeds */
    for (strip_nl = buf + strlen (buf) - 1;
    	 strip_nl >= buf && (*strip_nl == '\n' || *strip_nl == '\r');
	 strip_nl--);
    *++strip_nl = 0;

    return strdup (buf);
}

#endif
// vim: sw=2 si:
