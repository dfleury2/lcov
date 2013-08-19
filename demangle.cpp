#include <string>

#ifndef WIN32
#include <cxxabi.h>
#include <stdlib.h>
#endif
 
// ---------------------------------------------------------------------------
std::string Demangled(const std::string& functionName)
{
   std::string realName;
#ifdef WIN32
   realName = functionName;
#else
   int status = 0;
   char* realname = abi::__cxa_demangle(functionName.c_str(), 0, 0, &status);
   if (status == 0)
   {
      realName = realname;
      free(realname);
   }
#endif
   return realName;
}
