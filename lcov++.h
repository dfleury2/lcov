#ifndef __LCOV_H_INCLUDED__
#define __LCOV_H_INCLUDED__

#include "gcov.h"
#include "gcov-io.h"
#include "gcov-io.c"

#include <map>
#include <string>

// ---------------------------------------------------------------------------
struct FunctionInfo
{
   FunctionInfo() : line(0), hit(0) {}

   int line; // line of the source code
   int hit;  // # of executed
};

// ---------------------------------------------------------------------------
struct BranchId
{
   BranchId() : line(0), block(0), branch(0) {}
   int line;
   int block;
   int branch;
};

// ---------------------------------------------------------------------------
inline
bool operator < (const BranchId& lhs, const BranchId& rhs)
{
   if (lhs.line < rhs.line) return true;
   if (lhs.line > rhs.line) return false;
   if (lhs.block < rhs.block) return true;
   if (lhs.block > rhs.block) return false;
   if (lhs.branch < rhs.branch) return true;
   if (lhs.branch > rhs.branch) return false;
   return false;
}

// ---------------------------------------------------------------------------
// Function informations
typedef std::map< std::string, FunctionInfo > Functions;
// Line execution count
typedef std::map< int, int > Lines;
// Branch execution count
typedef std::map< BranchId, gcov_type > Branches;

// Storing infos by source
extern std::map< std::string, Functions > SourceFunctions;
extern std::map< std::string, Lines > SourceLines;
extern std::map< std::string, Branches > SourceBranches;

#endif

