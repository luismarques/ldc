//===-- tool.cpp ----------------------------------------------------------===//
//
//                         LDC – the LLVM D compiler
//
// This file is distributed under the BSD-style LDC license. See the LICENSE
// file for details.
//
//===----------------------------------------------------------------------===//

#include "driver/tool.h"
#include "mars.h"
#include "driver/exe_path.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#ifdef _WIN32
#include <Windows.h>
#endif

//////////////////////////////////////////////////////////////////////////////

static llvm::cl::opt<std::string>
    gcc("gcc", llvm::cl::desc("GCC to use for assembling and linking"),
        llvm::cl::Hidden, llvm::cl::ZeroOrMore);

//////////////////////////////////////////////////////////////////////////////

static std::string findProgramByName(llvm::StringRef name) {
#if LDC_LLVM_VER >= 306
  llvm::ErrorOr<std::string> res = llvm::sys::findProgramByName(name);
  return res ? res.get() : std::string();
#else
  return llvm::sys::FindProgramByName(name);
#endif
}

//////////////////////////////////////////////////////////////////////////////

std::string getProgram(const char *name, const llvm::cl::opt<std::string> *opt,
                       const char *envVar) {
  std::string path;
  const char *prog = nullptr;

  if (opt && !opt->empty()) {
    path = findProgramByName(opt->c_str());
  }

  if (path.empty() && envVar && (prog = getenv(envVar)) && prog[0] != '\0') {
    path = findProgramByName(prog);
  }

  if (path.empty()) {
    path = findProgramByName(name);
  }

  if (path.empty()) {
    error(Loc(), "failed to locate %s", name);
    fatal();
  }

  return path;
}

////////////////////////////////////////////////////////////////////////////////

std::string getGcc(const llvm::Triple *triple) {
  if(triple != nullptr && triple->getArch() == llvm::Triple::msp430) {
    std::string path = findProgramByName("msp430-gcc");

    if(path.empty()) {
      path = findProgramByName("msp430-elf-gcc");
    }

    if (path.empty()) {
      error(Loc(), "Could not find MSP430 GCC cross-compiler for external assembly");
      fatal();
    }

    return path;
  }

#if defined(__FreeBSD__) && __FreeBSD__ >= 10
  // Default compiler on FreeBSD 10 is clang
  return getProgram("clang", &gcc, "CC");
#else
  return getProgram("gcc", &gcc, "CC");
#endif
}

//////////////////////////////////////////////////////////////////////////////

void createDirectoryForFileOrFail(llvm::StringRef fileName) {
  auto dir = llvm::sys::path::parent_path(fileName);
  if (!dir.empty() && !llvm::sys::fs::exists(dir)) {
    if (auto ec = llvm::sys::fs::create_directories(dir)) {
      error(Loc(), "failed to create path to file: %s\n%s", dir.data(),
            ec.message().c_str());
      fatal();
    }
  }
}

////////////////////////////////////////////////////////////////////////////////

std::vector<const char *> getFullArgs(const std::string &tool,
                                      const std::vector<std::string> &args,
                                      bool printVerbose) {
  std::vector<const char *> fullArgs;
  fullArgs.reserve(args.size() +
                   2); // executeToolAndWait() appends an additional null

  fullArgs.push_back(tool.c_str());
  for (const auto &arg : args)
    fullArgs.push_back(arg.c_str());

  // Print command line if requested
  if (printVerbose) {
    for (auto arg : fullArgs)
      fprintf(global.stdmsg, "%s ", arg);
    fprintf(global.stdmsg, "\n");
    fflush(global.stdmsg);
  }

  return fullArgs;
}

////////////////////////////////////////////////////////////////////////////////

int executeToolAndWait(const std::string &tool_,
                       std::vector<std::string> const &args, bool verbose) {
  const auto tool = findProgramByName(tool_);
  if (tool.empty()) {
    error(Loc(), "failed to locate %s", tool_.c_str());
    return -1;
  }

  // Construct real argument list.
  // First entry is the tool itself, last entry must be NULL.
  auto realargs = getFullArgs(tool, args, verbose);
  realargs.push_back(nullptr);

  // Execute tool.
  std::string errstr;
  if (int status = llvm::sys::ExecuteAndWait(tool, &realargs[0], nullptr,
                                             nullptr, 0, 0, &errstr)) {
    error(Loc(), "%s failed with status: %d", tool.c_str(), status);
    if (!errstr.empty()) {
      error(Loc(), "message: %s", errstr.c_str());
    }
    return status;
  }
  return 0;
}

////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32

namespace windows {

bool needsQuotes(const llvm::StringRef &arg) {
  return // not already quoted
      !(arg.size() > 1 && arg[0] == '"' &&
        arg.back() == '"') && // empty or min 1 space or min 1 double quote
      (arg.empty() || arg.find(' ') != arg.npos || arg.find('"') != arg.npos);
}

size_t countPrecedingBackslashes(llvm::StringRef arg, size_t index) {
  size_t count = 0;

  for (size_t i = index - 1; i >= 0; --i) {
    if (arg[i] != '\\')
      break;
    ++count;
  }

  return count;
}

std::string quoteArg(llvm::StringRef arg) {
  if (!needsQuotes(arg))
    return arg;

  std::string quotedArg;
  quotedArg.reserve(3 + 2 * arg.size()); // worst case

  quotedArg.push_back('"');

  const size_t argLength = arg.size();
  for (size_t i = 0; i < argLength; ++i) {
    if (arg[i] == '"') {
      // Escape all preceding backslashes (if any).
      // Note that we *don't* need to escape runs of backslashes that don't
      // precede a double quote! See MSDN:
      // http://msdn.microsoft.com/en-us/library/17w5ykft%28v=vs.85%29.aspx
      quotedArg.append(countPrecedingBackslashes(arg, i), '\\');

      // Escape the double quote.
      quotedArg.push_back('\\');
    }

    quotedArg.push_back(arg[i]);
  }

  // Make sure our final double quote doesn't get escaped by a trailing
  // backslash.
  quotedArg.append(countPrecedingBackslashes(arg, argLength), '\\');
  quotedArg.push_back('"');

  return quotedArg;
}

int executeAndWait(const char *commandLine) {
  STARTUPINFO si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);

  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  DWORD exitCode;

#if UNICODE
  std::wstring wcommandLine;
  if (!llvm::ConvertUTF8toWide(commandLine, wcommandLine))
    return -3;
  auto cmdline = const_cast<wchar_t *>(wcommandLine.data());
#else
  auto cmdline = const_cast<char *>(commandLine);
#endif
  // according to MSDN, only CreateProcessW (unicode) may modify the passed
  // command line
  if (!CreateProcess(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si,
                     &pi)) {
    exitCode = -1;
  } else {
    if (WaitForSingleObject(pi.hProcess, INFINITE) != 0 ||
        !GetExitCodeProcess(pi.hProcess, &exitCode))
      exitCode = -2;

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }

  return exitCode;
}

bool setupMsvcEnvironmentImpl() {
  if (getenv("VSINSTALLDIR"))
    return true;

  llvm::SmallString<128> tmpFilePath;
  if (llvm::sys::fs::createTemporaryFile("ldc_dumpEnv", "", tmpFilePath))
    return false;

  /* Run `%ComSpec% /s /c "...\dumpEnv.bat <x86|amd64> > <tmpFilePath>"` to dump
   * the MSVC environment to the temporary file.
   *
   * cmd.exe /c treats the following string argument (the command)
   * in a very peculiar way if it starts with a double-quote.
   * By adding /s and enclosing the command in extra double-quotes
   * (WITHOUT additionally escaping the command), the command will
   * be parsed properly.
   */

  auto comspecEnv = getenv("ComSpec");
  if (!comspecEnv) {
    warning(Loc(),
            "'ComSpec' environment variable is not set, assuming 'cmd.exe'.");
    comspecEnv = "cmd.exe";
  }
  std::string cmdExecutable = comspecEnv;
  std::string batchFile = exe_path::prependBinDir("dumpEnv.bat");
  std::string arch =
      global.params.targetTriple->isArch64Bit() ? "amd64" : "x86";

  llvm::SmallString<512> commandLine;
  commandLine += quoteArg(cmdExecutable);
  commandLine += " /s /c \"";
  commandLine += quoteArg(batchFile);
  commandLine += ' ';
  commandLine += arch;
  commandLine += " > ";
  commandLine += quoteArg(tmpFilePath);
  commandLine += '"';

  const int exitCode = executeAndWait(commandLine.c_str());
  if (exitCode != 0) {
    error(Loc(), "`%s` failed with status: %d", commandLine.c_str(), exitCode);
    llvm::sys::fs::remove(tmpFilePath);
    return false;
  }

  auto fileBuffer = llvm::MemoryBuffer::getFile(tmpFilePath);
  llvm::sys::fs::remove(tmpFilePath);
  if (fileBuffer.getError())
    return false;

  const auto contents = (*fileBuffer)->getBuffer();
  const auto size = contents.size();

  // Parse the file.
  std::vector<std::pair<llvm::StringRef, llvm::StringRef>> env;

  size_t i = 0;
  // for each line
  while (i < size) {
    llvm::StringRef key, value;

    for (size_t j = i; j < size; ++j) {
      const char c = contents[j];
      if (c == '=' && key.empty()) {
        key = contents.slice(i, j);
        i = j + 1;
      } else if (c == '\n' || c == '\r' || c == '\0') {
        if (!key.empty()) {
          value = contents.slice(i, j);
        }
        // break and continue with next line
        i = j + 1;
        break;
      }
    }

    if (!key.empty() && !value.empty())
      env.emplace_back(key, value);
  }

  if (global.params.verbose)
    fprintf(global.stdmsg, "Applying environment variables:\n");

  bool haveVsInstallDir = false;

  for (const auto &pair : env) {
    const std::string key = pair.first.str();
    const std::string value = pair.second.str();

    if (global.params.verbose)
      fprintf(global.stdmsg, "  %s=%s\n", key.c_str(), value.c_str());

    SetEnvironmentVariableA(key.c_str(), value.c_str());

    if (key == "VSINSTALLDIR")
      haveVsInstallDir = true;
  }

  return haveVsInstallDir;
}

bool setupMsvcEnvironment() {
  const bool success = setupMsvcEnvironmentImpl();
  if (!success)
    warning(Loc(), "no Visual C++ installation detected");
  return success;
}

} // namespace windows

#endif // _WIN32
