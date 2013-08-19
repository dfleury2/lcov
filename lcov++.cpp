// g++ -O3 -o lcov++ lcov++.cpp

#include "lcov++.h"
#include "demangle.h"

#include <iostream>
#include <vector>
#include <fstream>
#include <algorithm>

using namespace std;

// Storing infos by source
std::map< std::string, Functions > SourceFunctions;
std::map< std::string, Lines > SourceLines;
std::map< std::string, Branches > SourceBranches;

// --------------------------------------------------------------------------
// This is the size of the buffer used to read in source file lines.
#define STRING_SIZE 200

struct function_info;
struct block_info;
struct source_info;

// --------------------------------------------------------------------------
// Describes an arc between two basic blocks.
struct arc_info
{
   // source and destination blocks.
   block_info* src;
   block_info* dst;

   // transition counts.
   gcov_type count;
   // used in cycle search, so that we do not clobber original counts.
   gcov_type cs_count;

   unsigned int count_valid : 1;
   unsigned int on_tree : 1;
   unsigned int fake : 1;
   unsigned int fall_through : 1;

   // Arc is for a function that abnormally returns.
   unsigned int is_call_non_return : 1;

   // Arc is for catch/setjump.
   unsigned int is_nonlocal_return : 1;

   // Is an unconditional branch.
   unsigned int is_unconditional : 1;

   // Loop making arc.
   unsigned int cycle : 1;

   // Next branch on line.
   arc_info* line_next;

   // Links to next arc on src and dst lists.
   arc_info* succ_next;
   arc_info* pred_next;
};

// --------------------------------------------------------------------------
// Describes a basic block. Contains lists of arcs to successor and
// predecessor blocks.
struct block_info
{
   // Chain of exit and entry arcs.
   arc_info* succ;
   arc_info* pred;

   // Number of unprocessed exit and entry arcs.
   gcov_type num_succ;
   gcov_type num_pred;

   // Block execution count.
   gcov_type count;
   unsigned flags : 13;
   unsigned count_valid : 1;
   unsigned valid_chain : 1;
   unsigned invalid_chain : 1;

   // Block is a call instrumenting site.
   unsigned is_call_site : 1; // Does the call.
   unsigned is_call_return : 1; // Is the return.

   // Block is a landing pad for longjmp or throw.
   unsigned is_nonlocal_return : 1;

   union
   {
      struct
      {
         // Array of line numbers and source files. source files are
         // introduced by a linenumber of zero, the next 'line number' is
         // the number of the source file.  Always starts with a source
         // file.
         unsigned* encoding;
         unsigned num;
      } line; // Valid until blocks are linked onto lines
      struct
      {
         // Single line graph cycle workspace.  Used for all-blocks mode.
         arc_info* arc;
         unsigned ident;
      } cycle; // Used in all-blocks mode, after blocks are linked onto lines.
   } u;

   // Temporary chain for solving graph, and for chaining blocks on one line.
   block_info* chain;
};

// --------------------------------------------------------------------------
// Describes a single function. Contains an array of basic blocks.
struct function_info
{
   // Name of function.
   char* name;
   unsigned ident;
   unsigned checksum;

   // Array of basic blocks.
   block_info* blocks;
   unsigned num_blocks;
   unsigned blocks_executed;

   // Raw arc coverage counts.
   gcov_type* counts;
   unsigned num_counts;

   // First line number.
   unsigned line;
   source_info* src;

   // Next function in same source file.
   function_info* line_next;

   // Next function.
   function_info* next;
};

// --------------------------------------------------------------------------
// Describes coverage of a file or function.
struct coverage_info
{
   int lines;
   int lines_executed;

   int branches;
   int branches_executed;
   int branches_taken;

   int calls;
   int calls_executed;

   std::string name;
};

// --------------------------------------------------------------------------
// Describes a single line of source. Contains a chain of basic blocks with code on it.
struct line_info
{
   gcov_type count;           // execution count
   union
   {
      arc_info* branches;      // branches from blocks that end on this
      //  line. Used for branch-counts when not
      //  all-blocks mode.
      block_info* blocks;    // blocks which start on this line.  Used
      // in all-blocks mode.
   } u;
   unsigned exists : 1;
};

// --------------------------------------------------------------------------
// Describes a file mentioned in the block graph.  Contains an array of line info.
struct source_info
{
   source_info() : index(0), lines(0), num_lines(0), functions(0), next(0)
   {}

   // name of source file (include absolute path)
   std::string name;
   unsigned index;

   // Array of line information.
   line_info* lines;
   unsigned num_lines;

   coverage_info coverage;

   // Functions in this source file.  These are in ascending line number order.
   function_info* functions;

   // Next source file.
   source_info* next;
};

// Holds a list of function basic block graphs.
static function_info* functions;

// This points to the head of the sourcefile structure list.
static source_info* sources;

// This holds data summary information.
static struct gcov_summary object_summary;
static unsigned program_count;

// Modification time of graph file.
static time_t bbg_file_time;

// Stamp of the bbg file
static unsigned gcno_stamp;

// Output branch probabilities.
static int flag_branches = 1; //0;

// Show unconditional branches too.
static int flag_unconditional = 0;

// Output count information for every basic block, not merely those
// that contain line number information.
static int flag_all_blocks = 1; //0;

// Output the number of times a branch was taken as opposed to the percentage
// of times it was taken.
static int flag_counts = 1; //0;

// Forward declarations.
static void fnotice(FILE*, const char*, ...);
static void process_file(const std::string&);
static std::string createGCNOfilename(const std::string&);
static source_info* find_source(const char*, const std::string& gcnoFilename);
static int read_graph_file(const std::string& gcnoFilename);
static int read_count_file(const std::string& gcdaFilename);
static void solve_flow_graph(function_info*, const std::string& gcnoFilename);
static void add_branch_counts(coverage_info*, const arc_info*);
static void add_line_counts(function_info*, const std::string& gcnoFilename);
static void function_summary(const coverage_info*, const char*);
static const char* format_gcov(gcov_type, gcov_type, int);
static void accumulate_line_counts(source_info*);
static int output_branch_count(int, const arc_info*, int& branch, gcov_type& taken);
static void output_lines(FILE*, const source_info*, const std::string& gcdaFilename, const std::string& gcnoFilename);
static void aggregate_info(const source_info*);
static std::string make_gcov_file_name(const std::string&);
static void release_structures(void);

// ---------------------------------------------------------------------------
bool IsGCDA(const char* filename)
{
   size_t size = strlen(filename);
   return (size > 5
           && filename[ size - 5 ] == '.'
           && filename[ size - 4 ] == 'g'
           && filename[ size - 3 ] == 'c'
           && filename[ size - 2 ] == 'd'
           && filename[ size - 1 ] == 'a');
}

// ---------------------------------------------------------------------------
std::vector< std::string > ReadDir(const std::string& fullName, const char* shortName = 0, int currentLevel = 0)
{
   std::vector< std::string > filenames;

#ifdef WIN32
   WIN32_FIND_DATAA file;
   std::string filter = fullName + "/*";
   HANDLE hSearch = FindFirstFileA(filter.c_str(), &file);
   if (hSearch != INVALID_HANDLE_VALUE)
   {
      do
      {
         if (file.dwFileAttributes == FILE_ATTRIBUTE_DIRECTORY)
         {
            if (file.cFileName[0] == '.')
               continue;

            std::vector< std::string > tmp = ReadDir(fullName + "/" + file.cFileName, file.cFileName, currentLevel + 3);
            filenames.insert(filenames.end(), tmp.begin(), tmp.end());
         }

         if (IsGCDA(file.cFileName))
            filenames.push_back(fullName + "/" + file.cFileName);
      }
      while (FindNextFileA(hSearch, &file));

      FindClose(hSearch);
   }
#else

   DIR* rep = opendir(fullName.c_str());
   if (!rep)
   {
      cerr << "opendir error [" << errno << "] on [" << fullName << "]"<< endl;
      return filenames;
   }

   dirent* entry;
   while ((entry = readdir(rep)))
   {
      if (entry->d_type == DT_DIR)
      {
         if (entry->d_name[0] == '.')
            continue;

         std::vector< std::string > tmp = ReadDir(fullName + "/" + entry->d_name, entry->d_name, currentLevel + 3);
         filenames.insert(filenames.end(), tmp.begin(), tmp.end());
      }
      else
      {
         // We have a file but is this a gcda file (.gcda)
         if (IsGCDA(entry->d_name))
            filenames.push_back(fullName + "/" + entry->d_name);
      }
   }
   closedir(rep);
#endif

   return filenames;
}

// --------------------------------------------------------------------------
int main(int argc, char* argv[])
{
   std::string directory = (argc > 1 ? argv[1] : ".");

   cout << "Capturing coverage data from " << directory << endl;

   // All filenames for the arc count data.
   cout << "Scanning " << directory << " for .gcda files ..." << endl;
   std::vector< std::string > GCDAFilenames = ReadDir(directory);
   std::sort(GCDAFilenames.begin(), GCDAFilenames.end());
   cout << "Found " << GCDAFilenames.size() << " data files in " << directory << endl;

   // Process all found files
   for (std::vector< std::string >::const_iterator it = GCDAFilenames.begin(); it != GCDAFilenames.end(); ++it)
   {
      cout << "Processing " << (*it) << endl;
      release_structures();

      process_file((*it));
   }

   const char* appInfoFilename = "app.info";

   ofstream file(appInfoFilename);
   for (std::map< std::string, Functions >::const_iterator it = SourceFunctions.begin(); it != SourceFunctions.end(); ++it)
   {
      const Functions& functions = it->second;

      // Header section
      file << "TN:" << endl;
      file << "SF:" << it->first << endl;

      // FN section
      size_t fnf = functions.size(); // function count
      size_t fnh = 0; // function hit
      for (Functions::const_iterator function = functions.begin(); function != functions.end(); ++function)
         file << "FN:" << function->second.line << "," << function->first << endl;

      // FNDA section
      for (Functions::const_iterator function = functions.begin(); function != functions.end(); ++function)
      {
         if (function->second.hit) ++fnh;
         file << "FNDA:" << function->second.hit << "," << function->first << endl;
      }
      file << "FNF:" << fnf << endl;
      file << "FNH:" << fnh << endl;

      // BRDA section
      std::map< std::string, Branches >::const_iterator foundBranches = SourceBranches.find(it->first);
      if (foundBranches != SourceBranches.end())
      {
         const Branches& branches = foundBranches->second;

         int brf = branches.size(); // # of branches found
         int brh = 0; // # of branches hit

         for (std::map< BranchId, gcov_type >::const_iterator jt = branches.begin(); jt != branches.end(); ++jt)
         {
            if (jt->second) ++brh;

            file << "BRDA:" << jt->first.line << "," << jt->first.block << "," << jt->first.branch << ",";
            if (jt->second < 0) file << '-';
            else file << jt->second;
            file << endl;
         }

         file << "BRF:" << brf << endl;
         file << "BRH:" << brh << endl;
      }

      // DA section
      std::map< std::string, Lines >::const_iterator foundLines = SourceLines.find(it->first);
      if (foundLines != SourceLines.end())
      {
         const Lines& lines = foundLines->second;

         int lf = lines.size(); // # of instrumented lines
         int lh = 0; // # of lines with non zero execution count
         for (std::map< int, int >::const_iterator jt = lines.begin(); jt != lines.end(); ++jt)
         {
            if (jt->second > 0) ++lh;

            file << "DA:" << jt->first << "," << jt->second << endl;
         }
         file << "LF:" << lf << endl;
         file << "LH:" << lh << endl;
      }

      // Closing
      file << "end_of_record" << endl;
   }

   cout << "Finished " << appInfoFilename << " creation" << endl;
}

// --------------------------------------------------------------------------
static void
fnotice(FILE* file, const char* cmsgid, ...)
{
   va_list ap;

   va_start(ap, cmsgid);
   vfprintf(file, (cmsgid), ap);
   va_end(ap);
}

// --------------------------------------------------------------------------
// Process a single source file.
static
void process_file(const std::string& gcdaFilename)
{
   // Filename for the basic block graph.
   std::string gcnoFilename = createGCNOfilename(gcdaFilename);
   if (read_graph_file(gcnoFilename))
      return;

   if (!functions)
   {
      fnotice(stderr, "%s:no functions found\n", gcnoFilename.c_str());
      return;
   }

   if (read_count_file(gcdaFilename))
      return;

   for (function_info* fn = functions; fn; fn = fn->next)
      solve_flow_graph(fn, gcnoFilename);

   for (source_info* src = sources; src; src = src->next)
      src->lines = (line_info*)calloc(src->num_lines, sizeof(line_info));

   for (function_info* fn = functions; fn; fn = fn->next)
      add_line_counts(fn, gcnoFilename);

   for (source_info* src = sources; src; src = src->next)
   {
      accumulate_line_counts(src);
      //function_summary (&src->coverage, "File");

      aggregate_info(src);
   }
}

// --------------------------------------------------------------------------
// Release all memory used.
static
void release_structures()
{
   bbg_file_time = 0;
   gcno_stamp = 0;

   source_info* src;
   while ((src = sources))
   {
      sources = src->next;
      free(src->lines);
   }

   function_info* fn;
   while ((fn = functions))
   {
      unsigned ix;
      block_info* block;

      functions = fn->next;
      for (ix = fn->num_blocks, block = fn->blocks; ix--; block++)
      {
         arc_info* arc, *arc_n;

         for (arc = block->succ; arc; arc = arc_n)
         {
            arc_n = arc->succ_next;
            free(arc);
         }
      }
      free(fn->blocks);
      free(fn->counts);
   }
}

// --------------------------------------------------------------------------
// Generate the names of the graph and data files. If OBJECT_DIRECTORY
// is not specified, these are looked for in the current directory,
// and named from the basename of the FILE_NAME sans extension. If
// OBJECT_DIRECTORY is specified and is a directory, the files are in
// that directory, but named from the basename of the FILE_NAME, sans
// extension. Otherwise OBJECT_DIRECTORY is taken to be the name of
// the object *file*, and the data files are named from that.
// --------------------------------------------------------------------------
static
std::string createGCNOfilename(const std::string& gcdaFilename)
{
   size_t extensionPosition = gcdaFilename.rfind(GCOV_DATA_SUFFIX);

   std::string gcnoFilename = gcdaFilename.substr(0, extensionPosition) + GCOV_NOTE_SUFFIX;

   return gcnoFilename;
}

// --------------------------------------------------------------------------
// Find or create a source file structure for FILE_NAME. Copies FILE_NAME on creation
// --------------------------------------------------------------------------
static
source_info* find_source(const char* file_name, const std::string& gcnoFilename)
{
   source_info* src = 0;

   if (!file_name)
      file_name = "<unknown>";

   // Extract the directory from gcno filename
   std::string filename = file_name;
   if (filename[0] != '/')
   {
      // only for not absolute path
      size_t position = gcnoFilename.rfind('/');
      if (position != std::string::npos)
         filename = gcnoFilename.substr(0, position + 1) + filename;
   }

   // Reduce the path
   size_t found;
   while ((found = filename.find("/../")) != std::string::npos)
   {
      size_t before = filename.rfind('/', found - 1);
      if (before != std::string::npos)
         filename.erase(before, found - before + 3);
      else
         break; // Oups ???
   }

   for (src = sources; src; src = src->next)
      if (filename == src->name)
         return src;

   src = new source_info();
   src->name = filename;
   src->coverage.name = src->name;
   src->index = sources ? sources->index + 1 : 1;
   src->next = sources;
   sources = src;

   return src;
}

// --------------------------------------------------------------------------
// Read the graph file. Return nonzero on fatal error.
// --------------------------------------------------------------------------
static
int read_graph_file(const std::string& gcnoFilename)
{
   unsigned version;
   unsigned current_tag = 0;
   struct function_info* fn = NULL;
   source_info* src = NULL;

   if (!gcov_open(gcnoFilename.c_str(), 1))
   {
      fnotice(stderr, "%s:cannot open graph file\n", gcnoFilename.c_str());
      return 1;
   }
   bbg_file_time = gcov_time();
   if (!gcov_magic(gcov_read_unsigned(), GCOV_NOTE_MAGIC))
   {
      fnotice(stderr, "%s:not a gcov graph file\n", gcnoFilename.c_str());
      gcov_close();
      return 1;
   }

   version = gcov_read_unsigned();
   if (version != GCOV_VERSION)
   {
      char v[4], e[4];

      GCOV_UNSIGNED2STRING(v, version);
      GCOV_UNSIGNED2STRING(e, GCOV_VERSION);

      //fnotice (stderr, "%s:version '%.4s', prefer '%.4s'\n", gcnoFilename.c_str(), v, e);
   }
   gcno_stamp = gcov_read_unsigned();

   unsigned tag;
   while ((tag = gcov_read_unsigned()))
   {
      unsigned length = gcov_read_unsigned();
      gcov_position_t base = gcov_position();

      if (tag == GCOV_TAG_FUNCTION)
      {
         unsigned ident = gcov_read_unsigned();
         unsigned checksum = gcov_read_unsigned();
         char* function_name = strdup(gcov_read_string());
         source_info* src = find_source(gcov_read_string(), gcnoFilename);
         unsigned lineno = gcov_read_unsigned();

         fn = (function_info*)calloc(1, sizeof(function_info));
         fn->name = function_name;
         fn->ident = ident;
         fn->checksum = checksum;
         fn->src = src;
         fn->line = lineno;

         fn->next = functions;
         functions = fn;
         current_tag = tag;

         if (lineno >= src->num_lines)
            src->num_lines = lineno + 1;
         // Now insert it into the source file's list of
         // functions. Normally functions will be encountered in
         // ascending order, so a simple scan is quick.
         function_info* probe, * prev;
         for (probe = src->functions, prev = NULL;
               probe && probe->line > lineno;
               prev = probe, probe = probe->line_next)
            continue;
         fn->line_next = probe;
         if (prev)
            prev->line_next = fn;
         else
            src->functions = fn;
      }
      else if (fn && tag == GCOV_TAG_BLOCKS)
      {
         if (fn->blocks)
            fnotice(stderr, "%s:already seen blocks for '%s'\n", gcnoFilename.c_str(), fn->name);
         else
         {
            unsigned num_blocks = GCOV_TAG_BLOCKS_NUM(length);
            fn->num_blocks = num_blocks;

            fn->blocks = (block_info*)calloc(fn->num_blocks, sizeof(block_info));
            for (unsigned ix = 0; ix != num_blocks; ix++)
               fn->blocks[ix].flags = gcov_read_unsigned();
         }
      }
      else if (fn && tag == GCOV_TAG_ARCS)
      {
         unsigned src = gcov_read_unsigned();
         unsigned num_dests = GCOV_TAG_ARCS_NUM(length);

         if (src >= fn->num_blocks || fn->blocks[src].succ)
            goto corrupt;

         while (num_dests--)
         {
            unsigned dest = gcov_read_unsigned();
            unsigned flags = gcov_read_unsigned();

            if (dest >= fn->num_blocks)
               goto corrupt;
            struct arc_info* arc = (arc_info*)calloc(1, sizeof(arc_info));
            arc->dst = &fn->blocks[dest];
            arc->src = &fn->blocks[src];

            arc->count = 0;
            arc->count_valid = 0;
            arc->on_tree = !!(flags & GCOV_ARC_ON_TREE);
            arc->fake = !!(flags & GCOV_ARC_FAKE);
            arc->fall_through = !!(flags & GCOV_ARC_FALLTHROUGH);

            arc->succ_next = fn->blocks[src].succ;
            fn->blocks[src].succ = arc;
            fn->blocks[src].num_succ++;

            arc->pred_next = fn->blocks[dest].pred;
            fn->blocks[dest].pred = arc;
            fn->blocks[dest].num_pred++;

            if (arc->fake)
            {
               if (src)
               {
                  // Exceptional exit from this function, the
                  // source block must be a call.
                  fn->blocks[src].is_call_site = 1;
                  arc->is_call_non_return = 1;
               }
               else
               {
                  // Non-local return from a callee of this
                  // function. The destination block is a catch or
                  // setjmp.
                  arc->is_nonlocal_return = 1;
                  fn->blocks[dest].is_nonlocal_return = 1;
               }
            }

            if (!arc->on_tree)
               fn->num_counts++;
         }
      }
      else if (fn && tag == GCOV_TAG_LINES)
      {
         unsigned blockno = gcov_read_unsigned();
         unsigned* line_nos = (unsigned*)calloc(length - 1, sizeof(unsigned));

         if (blockno >= fn->num_blocks || fn->blocks[blockno].u.line.encoding)
            goto corrupt;

         unsigned ix = 0;
         for (ix = 0; ;)
         {
            unsigned lineno = gcov_read_unsigned();

            if (lineno)
            {
               if (!ix)
               {
                  line_nos[ix++] = 0;
                  line_nos[ix++] = src->index;
               }
               line_nos[ix++] = lineno;
               if (lineno >= src->num_lines)
                  src->num_lines = lineno + 1;
            }
            else
            {
               const char* file_name = gcov_read_string();
               if (!file_name)
                  break;

               src = find_source(file_name, gcnoFilename);

               line_nos[ix++] = 0;
               line_nos[ix++] = src->index;
            }
         }

         fn->blocks[blockno].u.line.encoding = line_nos;
         fn->blocks[blockno].u.line.num = ix;
      }
      else if (current_tag && !GCOV_TAG_IS_SUBTAG(current_tag, tag))
      {
         fn = NULL;
         current_tag = 0;
      }
      gcov_sync(base, length);
      if (gcov_is_error())
      {
corrupt:
         ;
         fnotice(stderr, "%s:corrupted\n", gcnoFilename.c_str());
         gcov_close();
         return 1;
      }
   }
   gcov_close();

   // We built everything backwards, so nreverse them all.

   // Reverse sources. Not strictly necessary, but we'll then process
   // them in the 'expected' order.
   {
      source_info* src, *src_p, *src_n;

      for (src_p = NULL, src = sources; src; src_p = src, src = src_n)
      {
         src_n = src->next;
         src->next = src_p;
      }
      sources =  src_p;
   }

   // Reverse functions.
   {
      function_info* fn, *fn_p, *fn_n;

      for (fn_p = NULL, fn = functions; fn; fn_p = fn, fn = fn_n)
      {
         unsigned ix;

         fn_n = fn->next;
         fn->next = fn_p;

         // Reverse the arcs.
         for (ix = fn->num_blocks; ix--;)
         {
            arc_info* arc, *arc_p, *arc_n;

            for (arc_p = NULL, arc = fn->blocks[ix].succ; arc;
                  arc_p = arc, arc = arc_n)
            {
               arc_n = arc->succ_next;
               arc->succ_next = arc_p;
            }
            fn->blocks[ix].succ = arc_p;

            for (arc_p = NULL, arc = fn->blocks[ix].pred; arc;
                  arc_p = arc, arc = arc_n)
            {
               arc_n = arc->pred_next;
               arc->pred_next = arc_p;
            }
            fn->blocks[ix].pred = arc_p;
         }
      }
      functions = fn_p;
   }
   return 0;
}

// --------------------------------------------------------------------------
// Reads profiles from the count file and attach to each
// function. Return nonzero if fatal error.
// --------------------------------------------------------------------------
static int read_count_file(const std::string& gcnaFilename)
{
   unsigned ix;
   unsigned version;
   unsigned tag;
   function_info* fn = NULL;
   int error = 0;

   if (!gcov_open(gcnaFilename.c_str(), 1))
   {
      fnotice(stderr, "%s:cannot open data file\n", gcnaFilename.c_str());
      return 1;
   }
   if (!gcov_magic(gcov_read_unsigned(), GCOV_DATA_MAGIC))
   {
      fnotice(stderr, "%s:not a gcov data file\n", gcnaFilename.c_str());
cleanup:
      ;
      gcov_close();
      return 1;
   }
   version = gcov_read_unsigned();
   if (version != GCOV_VERSION)
   {
      char v[4], e[4];

      GCOV_UNSIGNED2STRING(v, version);
      GCOV_UNSIGNED2STRING(e, GCOV_VERSION);

      // fnotice( stderr, "%s:version '%.4s', prefer version '%.4s'\n", gcnaFilename.c_str(), v, e );
   }
   tag = gcov_read_unsigned();
   if (tag != gcno_stamp)
   {
      fnotice(stderr, "%s:stamp mismatch with graph file\n", gcnaFilename.c_str());
      goto cleanup;
   }

   while ((tag = gcov_read_unsigned()))
   {
      unsigned length = gcov_read_unsigned();
      unsigned long base = gcov_position();

      if (tag == GCOV_TAG_OBJECT_SUMMARY)
         gcov_read_summary(&object_summary);
      else if (tag == GCOV_TAG_PROGRAM_SUMMARY)
         program_count++;
      else if (tag == GCOV_TAG_FUNCTION)
      {
         unsigned ident = gcov_read_unsigned();
         struct function_info* fn_n = functions;

         for (fn = fn ? fn->next : NULL; ; fn = fn->next)
         {
            if (fn)
               ;
            else if ((fn = fn_n))
               fn_n = NULL;
            else
            {
               fnotice(stderr, "%s:unknown function '%u'\n", gcnaFilename.c_str(), ident);
               break;
            }
            if (fn->ident == ident)
               break;
         }

         if (!fn)
            ;
         else if (gcov_read_unsigned() != fn->checksum)
         {
            fnotice(stderr, "%s:profile mismatch for '%s'\n", gcnaFilename.c_str(), fn->name);
            goto cleanup;
         }
      }
      else if (tag == GCOV_TAG_FOR_COUNTER(GCOV_COUNTER_ARCS) && fn)
      {
         if (length != GCOV_TAG_COUNTER_LENGTH(fn->num_counts))
         {
            fnotice(stderr, "%s:profile mismatch for '%s'\n", gcnaFilename.c_str(), fn->name);
            goto cleanup;
         }

         if (!fn->counts)
            fn->counts = (gcov_type*)calloc(fn->num_counts, sizeof(gcov_type));

         for (ix = 0; ix != fn->num_counts; ix++)
            fn->counts[ix] += gcov_read_counter();
      }
      gcov_sync(base, length);
      if ((error = gcov_is_error()))
      {
         fnotice(stderr, error < 0 ? "%s:overflowed\n" : "%s:corrupted\n", gcnaFilename.c_str());
         goto cleanup;
      }
   }

   gcov_close();
   return 0;
}

// --------------------------------------------------------------------------
// Solve the flow graph. Propagate counts from the instrumented arcs
// to the blocks and the uninstrumented arcs.
// --------------------------------------------------------------------------
static
void solve_flow_graph(function_info* fn, const std::string& gcnoFilename)
{
   unsigned ix;
   arc_info* arc;
   gcov_type* count_ptr = fn->counts;
   block_info* blk;
   block_info* valid_blocks = NULL;    // valid, but unpropagated blocks.
   block_info* invalid_blocks = NULL;  // invalid, but inferable blocks.

   if (fn->num_blocks < 2)
      fnotice(stderr, "%s:'%s' lacks entry and/or exit blocks\n", gcnoFilename.c_str(), fn->name);
   else
   {
      if (fn->blocks[0].num_pred)
         fnotice(stderr, "%s:'%s' has arcs to entry block\n", gcnoFilename.c_str(), fn->name);
      else
         // We can't deduce the entry block counts from the lack of predecessors.
         fn->blocks[0].num_pred = ~(unsigned)0;

      if (fn->blocks[fn->num_blocks - 1].num_succ)
         fnotice(stderr, "%s:'%s' has arcs from exit block\n", gcnoFilename.c_str(), fn->name);
      else
         // Likewise, we can't deduce exit block counts from the lack of its successors.
         fn->blocks[fn->num_blocks - 1].num_succ = ~(unsigned)0;
   }

   // Propagate the measured counts, this must be done in the same order as the code in profile.c
   for (ix = 0, blk = fn->blocks; ix != fn->num_blocks; ix++, blk++)
   {
      block_info const* prev_dst = NULL;
      int out_of_order = 0;
      int non_fake_succ = 0;

      for (arc = blk->succ; arc; arc = arc->succ_next)
      {
         if (!arc->fake)
            non_fake_succ++;

         if (!arc->on_tree)
         {
            if (count_ptr)
               arc->count = *count_ptr++;
            arc->count_valid = 1;
            blk->num_succ--;
            arc->dst->num_pred--;
         }
         if (prev_dst && prev_dst > arc->dst)
            out_of_order = 1;
         prev_dst = arc->dst;
      }
      if (non_fake_succ == 1)
      {
         // If there is only one non-fake exit, it is an unconditional branch.
         for (arc = blk->succ; arc; arc = arc->succ_next)
            if (!arc->fake)
            {
               arc->is_unconditional = 1;
               // If this block is instrumenting a call, it might be
               // an artificial block. It is not artificial if it has
               // a non-fallthrough exit, or the destination of this
               // arc has more than one entry.  Mark the destination
               // block as a return site, if none of those conditions
               // hold.
               if (blk->is_call_site && arc->fall_through && arc->dst->pred == arc && !arc->pred_next)
                  arc->dst->is_call_return = 1;
            }
      }

      // Sort the successor arcs into ascending dst order. profile.c
      // normally produces arcs in the right order, but sometimes with
      // one or two out of order.  We're not using a particularly
      // smart sort.
      if (out_of_order)
      {
         arc_info* start = blk->succ;
         unsigned changes = 1;

         while (changes)
         {
            arc_info* arc, *arc_p, *arc_n;

            changes = 0;
            for (arc_p = NULL, arc = start; (arc_n = arc->succ_next);)
            {
               if (arc->dst > arc_n->dst)
               {
                  changes = 1;
                  if (arc_p)
                     arc_p->succ_next = arc_n;
                  else
                     start = arc_n;
                  arc->succ_next = arc_n->succ_next;
                  arc_n->succ_next = arc;
                  arc_p = arc_n;
               }
               else
               {
                  arc_p = arc;
                  arc = arc_n;
               }
            }
         }
         blk->succ = start;
      }

      // Place it on the invalid chain, it will be ignored if that's wrong.
      blk->invalid_chain = 1;
      blk->chain = invalid_blocks;
      invalid_blocks = blk;
   }

   while (invalid_blocks || valid_blocks)
   {
      while ((blk = invalid_blocks))
      {
         gcov_type total = 0;
         const arc_info* arc;

         invalid_blocks = blk->chain;
         blk->invalid_chain = 0;
         if (!blk->num_succ)
            for (arc = blk->succ; arc; arc = arc->succ_next)
               total += arc->count;
         else if (!blk->num_pred)
            for (arc = blk->pred; arc; arc = arc->pred_next)
               total += arc->count;
         else
            continue;

         blk->count = total;
         blk->count_valid = 1;
         blk->chain = valid_blocks;
         blk->valid_chain = 1;
         valid_blocks = blk;
      }
      while ((blk = valid_blocks))
      {
         gcov_type total;
         arc_info* arc, *inv_arc;

         valid_blocks = blk->chain;
         blk->valid_chain = 0;
         if (blk->num_succ == 1)
         {
            block_info* dst;

            total = blk->count;
            inv_arc = NULL;
            for (arc = blk->succ; arc; arc = arc->succ_next)
            {
               total -= arc->count;
               if (!arc->count_valid)
                  inv_arc = arc;
            }
            dst = inv_arc->dst;
            inv_arc->count_valid = 1;
            inv_arc->count = total;
            blk->num_succ--;
            dst->num_pred--;
            if (dst->count_valid)
            {
               if (dst->num_pred == 1 && !dst->valid_chain)
               {
                  dst->chain = valid_blocks;
                  dst->valid_chain = 1;
                  valid_blocks = dst;
               }
            }
            else
            {
               if (!dst->num_pred && !dst->invalid_chain)
               {
                  dst->chain = invalid_blocks;
                  dst->invalid_chain = 1;
                  invalid_blocks = dst;
               }
            }
         }
         if (blk->num_pred == 1)
         {
            block_info* src;

            total = blk->count;
            inv_arc = NULL;
            for (arc = blk->pred; arc; arc = arc->pred_next)
            {
               total -= arc->count;
               if (!arc->count_valid)
                  inv_arc = arc;
            }
            src = inv_arc->src;
            inv_arc->count_valid = 1;
            inv_arc->count = total;
            blk->num_pred--;
            src->num_succ--;
            if (src->count_valid)
            {
               if (src->num_succ == 1 && !src->valid_chain)
               {
                  src->chain = valid_blocks;
                  src->valid_chain = 1;
                  valid_blocks = src;
               }
            }
            else
            {
               if (!src->num_succ && !src->invalid_chain)
               {
                  src->chain = invalid_blocks;
                  src->invalid_chain = 1;
                  invalid_blocks = src;
               }
            }
         }
      }
   }

   // If the graph has been correctly solved, every block will have a valid count.
   for (ix = 0; ix < fn->num_blocks; ix++)
      if (!fn->blocks[ix].count_valid)
      {
         fnotice(stderr, "%s:graph is unsolvable for '%s'\n",  gcnoFilename.c_str(), fn->name);
         break;
      }
}

// --------------------------------------------------------------------------
// Increment totals in COVERAGE according to arc ARC.
// --------------------------------------------------------------------------
static void
add_branch_counts(coverage_info* coverage, const arc_info* arc)
{
   if (arc->is_call_non_return)
   {
      coverage->calls++;
      if (arc->src->count)
         coverage->calls_executed++;
   }
   else if (!arc->is_unconditional)
   {
      coverage->branches++;
      if (arc->src->count)
         coverage->branches_executed++;
      if (arc->count)
         coverage->branches_taken++;
   }
}

// --------------------------------------------------------------------------
// Format a HOST_WIDE_INT as either a percent ratio, or absolute
// count.  If dp >= 0, format TOP/BOTTOM * 100 to DP decimal places.
// If DP is zero, no decimal point is printed. Only print 100% when
// TOP==BOTTOM and only print 0% when TOP=0.  If dp < 0, then simply
// format TOP.  Return pointer to a static string.
// --------------------------------------------------------------------------
static
char const* format_gcov(gcov_type top, gcov_type bottom, int dp)
{
   static char buffer[20];

   if (dp >= 0)
   {
      float ratio = bottom ? (float)top / bottom : 0;
      int ix;
      unsigned limit = 100;
      unsigned percent;

      for (ix = dp; ix--;)
         limit *= 10;

      percent = (unsigned)(ratio * limit + (float)0.5);
      if (percent <= 0 && top)
         percent = 1;
      else if (percent >= limit && top != bottom)
         percent = limit - 1;
      ix = sprintf(buffer, "%.*u%%", dp + 1, percent);
      if (dp)
      {
         dp++;
         do
         {
            buffer[ix+1] = buffer[ix];
            ix--;
         }
         while (dp--);
         buffer[ix + 1] = '.';
      }
   }
   else
      sprintf(buffer, "%d", (unsigned)top);

   return buffer;
}

// --------------------------------------------------------------------------
// Output summary info for a function.
// --------------------------------------------------------------------------
static
void function_summary(const coverage_info* coverage, const char* title)
{
   //   fnotice (stdout, "%s '%s'\n", title, coverage->name.c_str() );
   //
   //   if (coverage->lines)
   //     fnotice (stdout, "Lines executed:%s of %d\n", format_gcov (coverage->lines_executed, coverage->lines, 2), coverage->lines);
   //   else
   //     fnotice (stdout, "No executable lines\n");
   //
   //   if (flag_branches)
   //   {
   //     if (coverage->branches)
   //     {
   //       fnotice (stdout, "Branches executed:%s of %d\n", format_gcov (coverage->branches_executed, coverage->branches, 2), coverage->branches );
   //       fnotice (stdout, "Taken at least once:%s of %d\n", format_gcov (coverage->branches_taken, coverage->branches, 2 ), coverage->branches );
   //     }
   //     else
   //       fnotice (stdout, "No branches\n");
   //     if (coverage->calls)
   //       fnotice (stdout, "Calls executed:%s of %d\n", format_gcov (coverage->calls_executed, coverage->calls, 2), coverage->calls );
   //     else
   //       fnotice (stdout, "No calls\n");
   //   }
}

// --------------------------------------------------------------------------
// Generate an output file name. LONG_OUTPUT_NAMES and PRESERVE_PATHS
// affect name generation. With preserve_paths we create a filename
// from all path components of the source file, replacing '/' with
// '#', without it we simply take the basename component. With
// long_output_names we prepend the processed name of the input file
// to each output name (except when the current source file is the
// input file, so you don't get a double concatenation). The two
// components are separated by '##'. Also '.' filename components are
// removed and '..'  components are renamed to '^'.  */
// --------------------------------------------------------------------------
static
std::string make_gcov_file_name(const std::string& src_name)
{
   return src_name + ".gcov";
}

// --------------------------------------------------------------------------
// Scan through the bb_data for each line in the block, increment
// the line number execution count indicated by the execution count of
// the appropriate basic block.
// --------------------------------------------------------------------------
static
void add_line_counts(function_info* fn, const std::string& gcnoFilename)
{
   unsigned ix;
   line_info* line = NULL; // This is propagated from one iteration to the next.

   // Scan each basic block.
   for (ix = 0; ix != fn->num_blocks; ix++)
   {
      block_info* block = &fn->blocks[ix];
      unsigned* encoding;
      const source_info* src = NULL;
      unsigned jx;

      if (block->count && ix && ix + 1 != fn->num_blocks)
         fn->blocks_executed++;
      for (jx = 0, encoding = block->u.line.encoding;
            jx != block->u.line.num; jx++, encoding++)
         if (!*encoding)
         {
            unsigned src_n = *++encoding;

            for (src = sources; src->index != src_n; src = src->next)
               continue;
            jx++;
         }
         else
         {
            line = &src->lines[*encoding];
            line->exists = 1;
            line->count += block->count;
         }
      free(block->u.line.encoding);
      block->u.cycle.arc = NULL;
      block->u.cycle.ident = ~0U;

      if (!ix || ix + 1 == fn->num_blocks)
         ; // Entry or exit block
      else if (flag_all_blocks)
      {
         line_info* block_line = line ? line : &fn->src->lines[fn->line];

         block->chain = block_line->u.blocks;
         block_line->u.blocks = block;
      }
      else if (flag_branches)
      {
         arc_info* arc;

         for (arc = block->succ; arc; arc = arc->succ_next)
         {
            arc->line_next = line->u.branches;
            line->u.branches = arc;
         }
      }
   }
   if (!line)
      fnotice(stderr, "%s:no lines for '%s'\n", gcnoFilename.c_str(), fn->name);
}

// --------------------------------------------------------------------------
// Accumulate the line counts of a file.
// --------------------------------------------------------------------------
static
void accumulate_line_counts(source_info* src)
{
   line_info* line;
   function_info* fn, *fn_p, *fn_n;
   unsigned ix;

   // Reverse the function order.
   for (fn = src->functions, fn_p = NULL; fn;
         fn_p = fn, fn = fn_n)
   {
      fn_n = fn->line_next;
      fn->line_next = fn_p;
   }
   src->functions = fn_p;

   for (ix = src->num_lines, line = src->lines; ix--; line++)
   {
      if (!flag_all_blocks)
      {
         arc_info* arc, *arc_p, *arc_n;

         // Total and reverse the branch information.
         for (arc = line->u.branches, arc_p = NULL; arc;
               arc_p = arc, arc = arc_n)
         {
            arc_n = arc->line_next;
            arc->line_next = arc_p;

            add_branch_counts(&src->coverage, arc);
         }
         line->u.branches = arc_p;
      }
      else if (line->u.blocks)
      {
         // The user expects the line count to be the number of times
         // a line has been executed. Simply summing the block count
         // will give an artificially high number.  The Right Thing
         // is to sum the entry counts to the graph of blocks on this
         // line, then find the elementary cycles of the local graph
         // and add the transition counts of those cycles.
         block_info* block, *block_p, *block_n;
         gcov_type count = 0;

         // Reverse the block information.
         for (block = line->u.blocks, block_p = NULL; block;
               block_p = block, block = block_n)
         {
            block_n = block->chain;
            block->chain = block_p;
            block->u.cycle.ident = ix;
         }
         line->u.blocks = block_p;

         // Sum the entry arcs.
         for (block = line->u.blocks; block; block = block->chain)
         {
            arc_info* arc;

            for (arc = block->pred; arc; arc = arc->pred_next)
            {
               if (arc->src->u.cycle.ident != ix)
                  count += arc->count;
               if (flag_branches)
                  add_branch_counts(&src->coverage, arc);
            }

            // Initialize the cs_count.
            for (arc = block->succ; arc; arc = arc->succ_next)
               arc->cs_count = arc->count;
         }

         // Find the loops. This uses the algorithm described in
         // Tiernan 'An Efficient Search Algorithm to Find the
         // Elementary Circuits of a Graph', CACM Dec 1970. We hold
         // the P array by having each block point to the arc that
         // connects to the previous block. The H array is implicitly
         // held because of the arc ordering, and the block's
         // previous arc pointer.

         // Although the algorithm is O(N^3) for highly connected
         // graphs, at worst we'll have O(N^2), as most blocks have
         // only one or two exits. Most graphs will be small.

         // For each loop we find, locate the arc with the smallest
         // transition count, and add that to the cumulative
         // count.  Decrease flow over the cycle and remove the arc
         // from consideration.
         for (block = line->u.blocks; block; block = block->chain)
         {
            block_info* head = block;
            arc_info* arc;

next_vertex:
            ;
            arc = head->succ;
current_vertex:
            ;
            while (arc)
            {
               block_info* dst = arc->dst;
               if (// Already used that arc.
                  arc->cycle
                  // Not to same graph, or before first vertex.
                  || dst->u.cycle.ident != ix
                  // Already in path.
                  || dst->u.cycle.arc)
               {
                  arc = arc->succ_next;
                  continue;
               }

               if (dst == block)
               {
                  // Found a closing arc.
                  gcov_type cycle_count = arc->cs_count;
                  arc_info* cycle_arc = arc;
                  arc_info* probe_arc;

                  // Locate the smallest arc count of the loop.
                  for (dst = head; (probe_arc = dst->u.cycle.arc);
                        dst = probe_arc->src)
                     if (cycle_count > probe_arc->cs_count)
                     {
                        cycle_count = probe_arc->cs_count;
                        cycle_arc = probe_arc;
                     }

                  count += cycle_count;
                  cycle_arc->cycle = 1;

                  // Remove the flow from the cycle.
                  arc->cs_count -= cycle_count;
                  for (dst = head; (probe_arc = dst->u.cycle.arc);
                        dst = probe_arc->src)
                     probe_arc->cs_count -= cycle_count;

                  // Unwind to the cyclic arc.
                  while (head != cycle_arc->src)
                  {
                     arc = head->u.cycle.arc;
                     head->u.cycle.arc = NULL;
                     head = arc->src;
                  }
                  // Move on.
                  arc = arc->succ_next;
                  continue;
               }

               // Add new block to chain.
               dst->u.cycle.arc = arc;
               head = dst;
               goto next_vertex;
            }
            // We could not add another vertex to the path. Remove
            // the last vertex from the list.
            arc = head->u.cycle.arc;
            if (arc)
            {
               // It was not the first vertex. Move onto next arc.
               head->u.cycle.arc = NULL;
               head = arc->src;
               arc = arc->succ_next;
               goto current_vertex;
            }
            // Mark this block as unusable.
            block->u.cycle.ident = ~0U;
         }

         line->count = count;
      }

      if (line->exists)
      {
         src->coverage.lines++;
         if (line->count)
            src->coverage.lines_executed++;
      }
   }
}

// --------------------------------------------------------------------------
// Output information about ARC number IX.  Returns nonzero if
// anything is output.
// --------------------------------------------------------------------------
static
int output_branch_count(int ix, const arc_info* arc, int& branch, gcov_type& taken)
{
   if (arc->is_call_non_return)
   {
      //if (arc->src->count)
      //{
      //  fnotice (gcov_file, "call   %2d returned %s\n", ix, format_gcov (arc->src->count - arc->count, arc->src->count, -flag_counts));
      //}
      //else
      //  fnotice (gcov_file, "call   %2d never executed\n", ix);
   }
   else if (!arc->is_unconditional)
   {
      branch = ix;
      taken = (arc->src->count ? arc->count : -1);
   }
   else if (flag_unconditional && !arc->dst->is_call_return)
   {
      //if (arc->src->count)
      //  fnotice (gcov_file, "unconditional %2d taken %s\n", ix, format_gcov (arc->count, arc->src->count, -flag_counts));
      //else
      //  fnotice (gcov_file, "unconditional %2d never executed\n", ix);
   }
   else
      return 0;
   return 1;
}

// --------------------------------------------------------------------------
// Aggregate the info on the global information
// --------------------------------------------------------------------------
static
void aggregate_info(const source_info* src)
{
   Functions& srcFunctions = SourceFunctions[ src->name ];
   Lines& srcLines = SourceLines[ src->name ];
   Branches& srcBranches = SourceBranches[ src->name ];

   unsigned line_num;         // current line number.
   const line_info* line;     // current line info ptr.
   char const* retval = "";   // status of source file reading.

   function_info* fn = src->functions;

   for (line_num = 1, line = &src->lines[line_num]; line_num < src->num_lines; line_num++, line++)
   {
      for (; fn && fn->line == line_num; fn = fn->line_next)
      {
         arc_info* arc = fn->blocks[fn->num_blocks - 1].pred;
         gcov_type return_count = fn->blocks[fn->num_blocks - 1].count;

         for (; arc; arc = arc->pred_next)
            if (arc->fake)
               return_count -= arc->count;

         std::string functionName = Demangled(fn->name);
         srcFunctions[ fn->name ].line = fn->line;
         srcFunctions[ fn->name ].hit += fn->blocks[0].count;
      }

      // For lines which don't exist in the .bb file, print '-' before
      // the source line.  For lines which exist but were never
      // executed, print '#####' before the source line.  Otherwise,
      // print the execution count before the source line.  There are
      // 16 spaces of indentation added before the source line so that
      // tabs won't be messed up.
      if (line->exists)
         srcLines[ line_num ] += line->count;

      // Looking for all blocks
      block_info* block;

      BranchId currentBranchId;

      int ix, jx;
      for (ix = jx = 0, block = line->u.blocks; block; block = block->chain)
      {
         currentBranchId.line = line_num;
         currentBranchId.block = 9999;

         if (!block->is_call_return)
            currentBranchId.block = ix++;

         for (arc_info* arc = block->succ; arc; arc = arc->succ_next)
         {
            currentBranchId.branch = -1;
            gcov_type taken = -1;
            jx += output_branch_count(jx, arc, currentBranchId.branch, taken);

            if (currentBranchId.branch != -1)
            {
               gcov_type currentTaken = -1;
               if (srcBranches.find(currentBranchId) != srcBranches.end())
                  currentTaken = srcBranches[ currentBranchId ];

               if (taken >= 0)
               {
                  if (currentTaken < 0) currentTaken = taken;
                  else currentTaken += taken;
               }
               srcBranches[ currentBranchId ] = currentTaken;
            }
         }
      }
   }
}
